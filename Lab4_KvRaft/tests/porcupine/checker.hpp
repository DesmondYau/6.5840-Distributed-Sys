#pragma once

#include <vector>
#include <map>
#include <memory>
#include <any>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <future>
#include <string>
#include <set>

namespace porcupine {

// --- Core Types ---

enum class EntryKind { Call, Return };
enum class EventKind { Call, Return };
enum class CheckResult { Ok, Illegal, Unknown };

/**
 * Entry represents a single event in the history (either a Call or a Return).
 * It corresponds to the 'entry' struct in checker.go[cite: 4].
 */
struct Entry {
    EntryKind kind;
    std::any value;
    int id;
    int64_t time;
    int clientId;
};

/**
 * Operation represents a completed request-response pair.
 */
struct Operation {
    std::any Input;
    std::any Output;
    int64_t Call;
    int64_t Return;
    int ClientId;
};

/**
 * Event represents an unpaired call or return in a flat log.
 * It matches the Event struct used in checkEvents[cite: 4].
 */
struct Event {
    int ClientId;
    EventKind Kind;
    std::any Value;
    int Id;
};

/**
 * LinearizationInfo stores the results of the check, including the longest 
 * valid prefixes if the history is found to be illegal[cite: 4].
 */
struct LinearizationInfo {
    std::vector<std::vector<Entry>> history;
    std::vector<std::vector<std::vector<int>>> partialLinearizations;
};

// --- Bitset for Cache ---

struct Bitset {
    std::vector<uint64_t> bits;
    Bitset(size_t n = 0) { bits.resize((n + 63) / 64, 0); }

    void set(size_t i) { bits[i / 64] |= (1ULL << (i % 64)); }
    void clear(size_t i) { bits[i / 64] &= ~(1ULL << (i % 64)); }
    
    bool operator==(const Bitset& other) const { return bits == other.bits; }
    bool operator<(const Bitset& other) const { return bits < other.bits; }
    
    size_t hash() const {
        size_t h = 0;
        for (auto b : bits) h ^= std::hash<uint64_t>{}(b) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// --- Model Interface ---

struct Model {
    virtual std::any Init() const = 0;
    virtual std::pair<bool, std::any> Step(const std::any& state, const std::any& input, const std::any& output) const = 0;

    virtual std::string DescribeOperation(const std::any& input, const std::any& output) const { return ""; }
    virtual std::string DescribeState(const std::any& state) const { return ""; }
    
    // Default to Shallow/True Equality if not overridden, mirroring fillDefault
    virtual bool Equal(const std::any& s1, const std::any& s2) const { return true; } 
    
    // Default partition strategy: everything in one partition
    virtual std::vector<std::vector<Operation>> Partition(const std::vector<Operation>& history) const {
        return {history};
    }
    
    // Partition strategy for raw events
    virtual std::vector<std::vector<Event>> PartitionEvent(const std::vector<Event>& history) const {
        return {history};
    }
    virtual ~Model() = default;
};

// --- Linked List Nodes ---

struct Node {
    std::any value;
    Node* match; 
    int id;
    Node *next = nullptr, *prev = nullptr;
};

inline void lift(Node* entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    Node* match = entry->match;
    match->prev->next = match->next;
    if (match->next) match->next->prev = match->prev;
}

inline void unlift(Node* entry) {
    Node* match = entry->match;
    match->prev->next = match;
    if (match->next) match->next->prev = match;
    entry->prev->next = entry;
    entry->next->prev = entry;
}

// --- Internal Search Logic ---

/**
 * Converts paired Operations into Call/Return entries and sorts them by time.
 * Includes tie-breaking logic to ensure Calls precede Returns[cite: 4].
 */
inline std::vector<Entry> makeEntries(const std::vector<Operation>& history) {
    std::vector<Entry> entries;
    int id = 0;
    for (const auto& op : history) {
        entries.push_back({EntryKind::Call, op.Input, id, op.Call, op.ClientId});
        entries.push_back({EntryKind::Return, op.Output, id, op.Return, op.ClientId});
        id++;
    }
    
    // Match the exact sorting logic of the Go version[cite: 4]
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        // Tie-breaker: Calls must be ordered before Returns[cite: 4]
        return a.kind == EntryKind::Call && b.kind == EntryKind::Return;
    });
    return entries;
}

/**
 * Renumbers and converts raw events into structured entries[cite: 4].
 */
inline std::vector<Entry> convertEntries(const std::vector<Event>& events) {
    std::vector<Entry> entries;
    std::map<int, int> idMapping;
    int nextId = 0;
    
    for (size_t i = 0; i < events.size(); ++i) {
        int internalId;
        if (idMapping.find(events[i].Id) == idMapping.end()) {
            internalId = nextId++;
            idMapping[events[i].Id] = internalId;
        } else {
            internalId = idMapping[events[i].Id];
        }
        
        EntryKind kind = (events[i].Kind == EventKind::Return) ? EntryKind::Return : EntryKind::Call;
        entries.push_back({kind, events[i].Value, internalId, static_cast<int64_t>(i), events[i].ClientId});
    }
    return entries;
}

inline Node* makeLinkedEntries(const std::vector<Entry>& entries, std::vector<std::unique_ptr<Node>>& arena) {
    Node* root = nullptr;
    std::map<int, Node*> matchMap;
    for (int i = entries.size() - 1; i >= 0; --i) {
        auto& e = entries[i];
        auto n = std::make_unique<Node>();
        n->value = e.value;
        n->id = e.id;
        if (e.kind == EntryKind::Return) {
            n->match = nullptr;
            matchMap[e.id] = n.get();
        } else {
            n->match = matchMap[e.id];
        }
        if (root) {
            n->next = root;
            root->prev = n.get();
        }
        root = n.get();
        arena.push_back(std::move(n));
    }
    return root;
}

/**
 * The core backtracking search. Now includes partial prefix tracking[cite: 4].
 */
inline std::pair<bool, std::vector<std::vector<int>>> checkSingle(
    const Model& model, const std::vector<Entry>& history, bool computePartial, std::atomic<int32_t>& kill) 
{
    std::vector<std::unique_ptr<Node>> arena;
    Node* entry = makeLinkedEntries(history, arena);
    int n = history.size() / 2;
    Bitset linearized(n);
    
    struct CacheEntry { Bitset lin; std::any state; };
    std::map<size_t, std::vector<CacheEntry>> cache;
    
    struct CallsEntry { Node* node; std::any state; };
    std::vector<CallsEntry> calls;
    
    // Tracks the longest linearizable prefix for debugging[cite: 4]
    std::vector<std::vector<int>> longest(n);
    
    std::any state = model.Init();
    auto dummy = std::make_unique<Node>();
    dummy->next = entry;
    entry->prev = dummy.get();
    
    Node* head = dummy.get();
    Node* curr = head->next;

    while (head->next != nullptr) {
        if (kill.load() != 0) return {false, longest};

        if (curr->match != nullptr) { // Call entry
            auto [ok, newState] = model.Step(state, curr->value, curr->match->value);
            if (ok) {
                Bitset nextLin = linearized;
                nextLin.set(curr->id);
                size_t h = nextLin.hash();
                
                bool seen = false;
                for (auto& ce : cache[h]) {
                    if (ce.lin == nextLin && model.Equal(ce.state, newState)) { seen = true; break; }
                }

                if (!seen) {
                    cache[h].push_back({nextLin, newState});
                    calls.push_back({curr, state});
                    state = std::move(newState);
                    linearized.set(curr->id);
                    lift(curr);
                    curr = head->next;
                    continue;
                }
            }
        }

        if (curr->next != nullptr) {
            curr = curr->next;
        } else {
            if (calls.empty()) return {false, longest};
            
            // If requested, save the longest valid sequence discovered so far[cite: 4]
            if (computePartial) {
                size_t callsLen = calls.size();
                std::vector<int> seq;
                for (auto& v : calls) {
                    if (longest[v.node->id].empty() || callsLen > longest[v.node->id].size()) {
                        if (seq.empty()) {
                            for (auto& c : calls) seq.push_back(c.node->id);
                        }
                        longest[v.node->id] = seq;
                    }
                }
            }
            
            // Backtrack
            auto& top = calls.back();
            curr = top.node;
            state = std::move(top.state);
            linearized.clear(curr->id);
            unlift(curr);
            calls.pop_back();
            curr = curr->next;
        }
    }
    
    // Complete linearization sequence found[cite: 4]
    std::vector<int> seq;
    for (auto& c : calls) seq.push_back(c.node->id);
    for (int i = 0; i < n; i++) longest[i] = seq;
    
    return {true, longest};
}

// --- Parallel Check Logic ---

/**
 * Replaces previous CheckOperations implementation to support Parallel execution
 * and returning LinearizationInfo matching the Go API[cite: 4].
 */
inline std::pair<CheckResult, LinearizationInfo> checkParallel(
    const Model& model, const std::vector<std::vector<Entry>>& history, bool computeInfo, std::chrono::milliseconds timeout) 
{
    std::atomic<int32_t> kill(0);
    std::vector<std::future<std::pair<bool, std::vector<std::vector<int>>>>> futures;

    for (size_t i = 0; i < history.size(); i++) {
        futures.push_back(std::async(std::launch::async, [&model, &history, i, computeInfo, &kill]() {
            return checkSingle(model, history[i], computeInfo, kill);
        }));
    }

    auto start = std::chrono::steady_clock::now();
    bool total_ok = true;
    bool timedOut = false;

    std::vector<std::vector<std::vector<int>>> allLongest(history.size());

    for (size_t i = 0; i < futures.size(); i++) {
        if (timeout.count() > 0) {
            auto remaining = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            if (remaining.count() <= 0 || futures[i].wait_for(remaining) == std::future_status::timeout) {
                timedOut = true;
                kill.store(1);
                break;
            }
        }
        
        auto result = futures[i].get();
        allLongest[i] = result.second;
        
        if (!result.first) {
            total_ok = false;
            if (!computeInfo) {
                kill.store(1); // Stop other partitions if we don't need debug info[cite: 4]
            }
        }
    }

    LinearizationInfo info;
    if (computeInfo) {
        info.history = history;
        // Format partial linearizations into unique sets[cite: 4]
        for (size_t i = 0; i < history.size(); ++i) {
            std::set<std::vector<int>> unique_partials;
            for (const auto& seq : allLongest[i]) {
                if (!seq.empty()) unique_partials.insert(seq);
            }
            std::vector<std::vector<int>> partials(unique_partials.begin(), unique_partials.end());
            info.partialLinearizations.push_back(partials);
        }
    }

    CheckResult res = CheckResult::Ok;
    if (!total_ok) res = CheckResult::Illegal;
    else if (timedOut) res = CheckResult::Unknown;

    return {res, info};
}

// --- Public API Wrappers ---

inline std::pair<CheckResult, LinearizationInfo> CheckOperations(
    const Model& model, const std::vector<Operation>& history, bool verbose = false, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) 
{
    auto partitions = model.Partition(history);
    std::vector<std::vector<Entry>> mappedPartitions;
    for (const auto& part : partitions) {
        mappedPartitions.push_back(makeEntries(part));
    }
    return checkParallel(model, mappedPartitions, verbose, timeout);
}

inline std::pair<CheckResult, LinearizationInfo> CheckEvents(
    const Model& model, const std::vector<Event>& history, bool verbose = false, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) 
{
    auto partitions = model.PartitionEvent(history);
    std::vector<std::vector<Entry>> mappedPartitions;
    for (const auto& part : partitions) {
        mappedPartitions.push_back(convertEntries(part));
    }
    return checkParallel(model, mappedPartitions, verbose, timeout);
}

} // namespace porcupine