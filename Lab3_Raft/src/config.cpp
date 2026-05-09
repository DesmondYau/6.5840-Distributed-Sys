#include "config.hpp"

// Implementation headers
#include "raft.hpp"
#include "helper.hpp"
#include "persister.hpp"
#include "logger.hpp"
#include "json.hpp"
#include "rpc/server.hpp"
#include "rpc/service.hpp"

// Standard library
#include <iostream>
#include <thread>
#include <stdexcept>
#include <optional>



Config::Config(int num, bool unreliable)
    : m_num { num }
    , m_network (std::make_shared<labrpc::Network>())
    , m_rafts(num)
    , m_connected(num , false)
    , m_persisters(num)
    , m_logs(num)
    , m_lastApplied(num)
    , m_endpointNames(num, std::vector<std::string>{})
    , m_applyErrors(num)
    , m_applyChannels(num)
    , m_applierThreads(num)
    , m_applierStopped(num)
    , m_maxLogIndex { 0 }           
    , m_maxLogIndex0 { 0 }          
    , m_rpcs0 { 0 }                 
    , m_bytes0 { 0 }                
    , m_t0 {}
    , m_startTime { std::chrono::steady_clock::now() }
{
    for (int i{0}; i < m_num; i++) {
        startServer(i);
        connectServer(i);
    }

    setNetworkUnreliable(unreliable);
    m_network->setLongDelays(true);
}

Config::~Config() 
{ 
    // 1. SIGNAL: Tell all threads to stop immediately
    for (int i = 0; i < m_num; i++) 
    {
        m_applierStopped[i] = true;

        // IMPORTANT: Close the channel to unblock the thread
        if (m_applyChannels[i]) {
            m_applyChannels[i]->close();
        }
    }

    // 2. JOIN: Wait for every single thread to exit cleanly.
    // This blocks until the applierSnap loops have finished their last iteration.
    for (int i = 0; i < m_num; i++) 
    {
        if (m_applierThreads[i].joinable()) 
        {
            m_applierThreads[i].join();
        }
    }

    // 3. DESTROY: Now that no threads are running, 
    // it is 100% safe to nullify pointers and clear containers.
    for (int i = 0; i < m_num; i++) 
    {
        m_rafts[i] = nullptr; 
    }
    m_rafts.clear();

    cleanup(); 

    m_network = nullptr;
}

void Config::startServer(int i) 
{ 
    std::cout << "[Config] Attempting to start server " << i << std::endl;
    
    // Kill any existing instance and preserve state
    crashServer(i); 
    
    // Generate fresh endpoint names for outgoing RPCs
    m_endpointNames[i] = std::vector<std::string>(m_num);
    std::vector<std::shared_ptr<labrpc::Endpoint>> endpoints(m_num);
    for (int j{0}; j<m_num; j++)
    {
        m_endpointNames[i][j] = "From_" + std::to_string(i) + "_To_" + std::to_string(j);
        auto endpoint = m_network->makeEndpoint(m_endpointNames[i][j]);
        m_network->connect(m_endpointNames[i][j], std::to_string(j));
        endpoints[j] = endpoint;
    }

    
    {
        std::lock_guard<std::mutex> lock(m_mu);

        // Reset state
        m_lastApplied[i] = 0;
        m_logs[i].clear();          
        m_applyErrors[i] = "";

       
        if (m_persisters[i]) 
        {
            // Make a fresh copy so old instance doesn't overwrite new instance's state
            m_persisters[i] = std::make_shared<Persister>(*m_persisters[i]);
            // Read snapshot from the copied persister
            auto snapshot = m_persisters[i]->readSnapshot();
            std::cout << "[Config] Snapshot for server " << i << " " << std::string(snapshot.begin(), snapshot.end()) << std::endl;

            if (!snapshot.empty())
            {
                // Call ingestSnap with -1 (skip index verification) — exactly as Go does
                std::string err = ingestSnap(i, snapshot, -1);
                if (!err.empty())
                {
                    std::cerr << "[Config] ingestSnap error on restart: " << err << std::endl;
                }
            }
        } 
        else 
        {
            // No previous persister → create a new one
            m_persisters[i] = std::make_shared<Persister>();
        }
    }
    
    
    // Set up Raft instance
    auto applyChannel = std::make_shared<ApplyChannel>();
    m_applyChannels[i] = applyChannel;
    auto sharedLogger = std::make_shared<Logger>();
    auto raft = std::make_shared<Raft>(endpoints, i, m_persisters[i], applyChannel, sharedLogger);

    // Assign to m_rafts
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_rafts[i] = raft;
    }
    
    // Launch the applier thread. It is now guaranteed that m_rafts[i] exists.
    m_applierStopped[i] = false;
    m_applierThreads[i] = std::thread([this, i, applyChannel]() {
        while (!m_applierStopped[i]) 
        {
            applierSnap(i, applyChannel);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

        
    // wrap Raft in a Service
    auto raftService = std::make_shared<labrpc::Service>("Raft");


    // Rgister RPC methods to the Service (which would be called by dispatch method later on)
    raftService->addMethod("AppendEntries",
        [raft](const std::string& args, std::string& reply) {
            Raft::AppendEntriesArgs a;
            Raft::AppendEntriesReply r;
            decodeArgs(args, a);
            raft->appendEntries(a, r);
            reply = encodeReply(r);
        });
    raftService->addMethod("RequestVote",
        [raft](const std::string& args, std::string& reply) {
            Raft::RequestVoteArgs a;
            Raft::RequestVoteReply r;
            decodeArgs(args, a);
            raft->requestVote(a, r);
            reply = encodeReply(r);
        });
    raftService->addMethod("InstallSnapshot",
        [raft](const std::string& args, std::string& reply) {
            Raft::InstallSnapshotArgs a;
            Raft::InstallSnapshotReply r;
            decodeArgs(args, a);
            raft->installSnapshot(a, r);
            reply = encodeReply(r);
        });

    // Create a Server and attach Service to Server
    auto server = std::make_shared<labrpc::Server>();
    server->addService("Raft", raftService);

    // Add Server to network
    m_network->addServer(std::to_string(i), server);

    std::cout << "[Config] Server " << i << " added successfully" << std::endl;
}

void Config::crashServer(int i) 
{
    std::cout << "[Config] Attempting to crash server " << i << std::endl;

    // Signal the background thread to exit immediately
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_applierStopped[i] = true;
    }

    // Unblock the thread for apply channel
    if (m_applyChannels[i]) {
        m_applyChannels[i]->close();
    }

    // JOIN: Wait for the thread to exit. 
    // This blocks until the thread loop finishes its current iteration.
    if (m_applierThreads[i].joinable()) {
        m_applierThreads[i].join();
    }

    // Disconnect server
    disconnectServer(i);
    m_network->deleteServer(std::to_string(i)); 

    // copy persister state after disconnect
    if (m_persisters[i]) 
    {
        m_persisters[i] = std::make_shared<Persister>(*m_persisters[i]);
    }

    // kill and remove raft
    if (m_rafts[i]) 
    {
        m_rafts[i]->kill();
        m_rafts[i] = nullptr;
    }

    // preserve last persisted state
    if (m_persisters[i]) {
        auto raftState = m_persisters[i]->readRaftState();
        auto snapshot  = m_persisters[i]->readSnapshot();
        m_persisters[i] = std::make_shared<Persister>();
        m_persisters[i]->saveStateAndSnapshot(raftState, snapshot);
    }

    std::cout << "[Config] Server " << i << " crashed successfully" << std::endl;
}

void Config::connectServer(int i) 
{
    m_connected[i] = true;

    // outgoing RPC endpoints
    for (int j{0}; j < m_num; j++) {
        if (m_connected[j] && !m_endpointNames[i].empty()) {
            std::string endpointName = m_endpointNames[i][j];
            m_network->enable(endpointName, true);
        }
    }

    // incoming RPC endpoints
    for (int j{0}; j < m_num; j++) {
        if (m_connected[j] && !m_endpointNames[j].empty()) {
            std::string endpointName = m_endpointNames[j][i];
            m_network->enable(endpointName, true);
        }
    }
}

void Config::disconnectServer(int i) 
{ 
    std::cout << "[Config] Attempting to disconnect server " << i << std::endl;
    m_connected[i] = false;

    // outgoing RPC endpoints
    for (int j{0}; j < m_num; j++) {
        if (!m_endpointNames[i].empty()) {
            std::string endpointName = m_endpointNames[i][j];
            m_network->enable(endpointName, false);
        }
    }

    // incoming RPC endpoints
    for (int j{0}; j < m_num; j++) {
        if (!m_endpointNames[j].empty()) {
            std::string endpointName = m_endpointNames[j][i];
            m_network->enable(endpointName, false);
        }
    }

    std::cout << "[Config] Server " << i << " disconnected successfully" << std::endl;
}

std::shared_ptr<Raft> Config::getRaft(int i) { return m_rafts[i]; };

long Config::bytesTotal()
{
    return m_network->getTotalBytes();
}

void Config::setNetworkUnreliable(bool unreliable) 
{ 
    m_network->setReliable(!unreliable); 
}

void Config::setNetworkLongReordering(bool reorder)
{
    m_network->setLongReordering(reorder);
}

void Config::cleanup() 
{ 
    for (auto& raft : m_rafts)
    {
        if (raft)
            raft->kill();
    }
    m_network->cleanup(); 

    checkTimeout(); 
}


void Config::begin(const std::string& description) {
    std::cout << std::endl 
              << "---------------------------------"
              << description << " ..." 
              << "---------------------------------"
              << std::endl;
    m_maxLogIndex = 0;
    for (int i = 0; i < m_num; i++) {
        m_logs[i].clear();
        m_lastApplied[i] = 0;
        m_applyErrors[i] = "";
    }

    m_t0 = std::chrono::steady_clock::now();
    m_rpcs0 = m_network->getTotalRPCCount();
    m_bytes0 = m_network->getTotalBytes();
    m_maxLogIndex0 = m_maxLogIndex;
}

void Config::end() {
    cleanup();
    checkTimeout();

    auto elapsed { std::chrono::steady_clock::now() - m_t0 };
    int numRPC { m_network->getTotalRPCCount() - m_rpcs0 };
    long numBytes { m_network->getTotalBytes() - m_bytes0 };
    uint64_t numCommits { m_maxLogIndex - m_maxLogIndex0 };

    std::cout << "  ... Passed -- " << std::endl;
    std::cout << "Test took " << std::chrono::duration_cast<std::chrono::seconds>(elapsed) << "s" << std::endl;
    std::cout << "Number of Raft peers = " << m_num << std::endl;
    std::cout << "Number of RPCs sent = " << numRPC << std::endl;
    std::cout << "Total number of bytes sent = " << numBytes << std::endl;
    std::cout << "Number of log entires committed = " << numCommits << std::endl;
}

// ==========================================================================================================================
// Functions used for snapshots
// ==========================================================================================================================


// 1. Processes messages coming from Raft through the applyChannel
//    - If the message is a snapshot (m.SnapshotValid == true), it calls ingestSnap to load the snapshot into the m_logs and m_lastApplied
//    - If the message is a normal command, it applies the command, checks for out-of-order application, and updates m_lastApplied[i]
// 2. Periodically triggers snapshot
void Config::applierSnap(int i, std::shared_ptr<ApplyChannel> applyChannel)
{
    while (true) {

        // Before doing anything, check if this server is still alive in the Config
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_rafts[i] == nullptr) {
                // The server was crashed. Stop this thread.
                return; 
            }
        }

        //Check stop signal BEFORE blocking
        if (m_applierStopped[i]) {
            break;
        }

        // Get the optional result
        auto optionalMsg = applyChannel->pop();   

        // Check if the channel was closed (nullopt)
        if (!optionalMsg.has_value()) {
            break; // Exit the loop cleanly
        }

        // Extract the actual message
        ApplyMsg m = optionalMsg.value();

        std::string errMsg;
        // Case 1: This is a snapshot message from Raft
        if (m.SnapshotValid) 
        {
            // Snapshot from Raft InstallSnapshot RPC
            std::lock_guard<std::mutex> lock(m_mu);
            errMsg = ingestSnap(i, m.Snapshot, m.LastIncludedIndex);
        }
        // Case 2: Normal command apply
        else if (m.CommandValid) 
        {
            // Check of out-of-order apply
            if (m.CommandIndex != m_lastApplied[i] + 1) 
            {
                errMsg = "server " + std::to_string(i) +
                         " apply out of order, expected index " +
                         std::to_string(m_lastApplied[i] + 1) +
                         ", got " + std::to_string(m.CommandIndex);
            }

            if (errMsg.empty()) 
            {
                std::lock_guard<std::mutex> lock(m_mu);
                // Use your existing check logic (or implement checkLogs if needed)
                auto it = m_logs[i].find(m.CommandIndex);
                if (it != m_logs[i].end()) 
                {
                    if (it->second != m.Command) 
                    {
                        errMsg = "commit index=" + std::to_string(m.CommandIndex) +
                                 " server=" + std::to_string(i) + " " + m.Command +
                                 " != server=other " + it->second;
                    }
                } 
                else 
                {
                    m_logs[i][m.CommandIndex] = m.Command;
                    if (m.CommandIndex > m_maxLogIndex) 
                    {
                        m_maxLogIndex = m.CommandIndex;
                    }
                }
                m_lastApplied[i] = m.CommandIndex;
            }

            // Periodically trigger snapshot (important part for lab 3D)
            if ((m.CommandIndex) % SnapShotInterval == 0 && m_rafts[i]) 
            {
                // Build snapshot
                nlohmann::json j;
                j["LastIncludedIndex"] = m.CommandIndex;
                j["LastIncludedTerm"] = m.CommandTerm;

                std::vector<nlohmann::json> logVector;
                for (uint64_t idx = 0; idx <= static_cast<uint64_t>(m.CommandIndex); idx++)
                {
                    if (m_logs[i].contains(idx))
                    {
                        logVector.push_back({{"Index", idx}, {"Command", m_logs[i][idx]}});
                    }
                    
                }
                j["Log"] = logVector;

                std::string snapshotData = j.dump();
                m_rafts[i]->snapshot(m.CommandIndex, snapshotData);
                std::cout << "[Config] Snapshot taken at index " << m.CommandIndex << " for server " << i << std::endl;
            }
        }

        if (!errMsg.empty()) 
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_applyErrors[i] = errMsg;
            std::cerr << "apply error: " << errMsg << std::endl;
        }
    }
}

// 
// i: Server ID - which Raft server this snapshot belongs to
// snapshot: Actual snapshot data sent by Raft
// index: Expected lastIncludedIndx for verification
std::string Config::ingestSnap(int i, const std::vector<uint8_t>& snapshot, int index)
{
    if (snapshot.empty()) 
    {
        return "nil snapshot";
    }

    try 
    {
        // Parse the JSON-encoded snapshot sent by Raft
        std::string snapshotStr(snapshot.begin(), snapshot.end());
        nlohmann::json j = nlohmann::json::parse(snapshotStr);

        // Extract the lastIncludedIndex (the highest index covered by this snapshot)
        uint64_t lastIncludedIndex = j.value("LastIncludedIndex", 0ull);

        // -1 to skip verification which is used when loading a snapshot from the Persister when restarting a Raft instance
        // verify that the lastIncludedIndex inside the snapshot payload matches the lastIncludedIndex in the ApplyMsg
        if (index != -1 && lastIncludedIndex != static_cast<uint64_t>(index)) 
        {
            return "server " + std::to_string(i) + " snapshot doesn't match m.SnapshotIndex";
        }

        // Clear previous log for this server and load snapshot data
        m_logs[i].clear();
        if (j.contains("Log") && j["Log"].is_array()) 
        {
            for (const auto& entry : j["Log"]) 
            {
                uint64_t idx = entry["Index"].get<uint64_t>();
                std::string cmd = entry["Command"].get<std::string>();
                m_logs[i][idx] = cmd;
            }
        }

        // Update lastApplied
        m_lastApplied[i] = lastIncludedIndex;
        std::cout << "[Config] Snapshot ingested at index " << lastIncludedIndex << " for server " << i << std::endl;
        return "";
    } 
    catch (...) 
    {
        return "snapshot decode error";
    }
}

// ==========================================================================================================================
// Functions used for testing
// ==========================================================================================================================

void Config::checkTimeout() {
    auto elapsed = std::chrono::steady_clock::now() - m_startTime;
    if (elapsed > std::chrono::minutes(2))
        throw std::runtime_error("test took longer than 120s");
}


int Config::checkOneLeader() {
    std::cout << "\n[Test] CheckOneLeader starting..." << std::endl;

    for (int tries{0}; tries < 10; tries++) 
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::map<int, std::vector<int>> leaders;

        for (int i{0}; i < m_num; i++) {
            if (m_connected[i] && m_rafts[i]) {
                auto [term, state] = m_rafts[i]->getTermState();
                if (state == Raft::State::LEADER) 
                    leaders[term].emplace_back(i);
            }
        }

        int lastTerm = -1;
        for (auto& [term, ids] : leaders) 
        {
            if (ids.size() > 1)
                throw std::runtime_error("multiple leaders in one term!");

            if (term > lastTerm) 
                lastTerm = term;
        }
        if (!leaders.empty()) 
            return leaders[lastTerm][0];
    }
    throw std::runtime_error("expected one leader, got none");
}

int Config::checkTerms() {
    std::cout << "\n[Test] CheckTerms starting..." << std::endl;

    int term = -1;
    for (int i{0}; i < m_num; i++) 
    {
        if (m_connected[i] && m_rafts[i]) 
        {
            auto [t, _] = m_rafts[i]->getTermState();
            if (term == -1) 
                term = t;
            else if (term != t)
                throw std::runtime_error("servers disagree on term");
        }
    }
    return term;
}

void Config::checkNoLeader() {
    std::cout << "\n[Test] CheckNoLeader starting..." << std::endl;

    for (int i{0}; i < m_num; i++) {
        if (m_connected[i] && m_rafts[i]) {
            auto [_, state] = m_rafts[i]->getTermState();
            if (state == Raft::State::LEADER)
                throw std::runtime_error("expected no leader");
        }
    }

}

// It answers two questions at the same time:
// 1. How many servers contain the log entry at index?
// 2. Do all of them have the exact same command at that index
std::pair<int,std::string> Config::nCommitted(int index) {
    std::cout << "\n[Test] nCommitted starting..." << std::endl;
    std::lock_guard<std::mutex> lock(m_mu);
    

    int count = 0;
    std::string cmd;
    for (int i = 0; i < m_num; i++) {
        if (m_logs[i].contains(index)) 
        {
            count++;
            if (cmd.empty()) 
            {
                cmd = m_logs[i][index];
            } 
            else if (m_logs[i][index] != cmd) 
            {
                throw std::runtime_error("different commands committed at same index");
            }
        }
    }

    std::cout << "[Test] Log with Index " << index << " observed in " << count << " servers" << std::endl;
    return {count, cmd};
}

// 1. Finds a current leader (by trying start() on every raft until one accepts the command).
// 2. Submits the command to that leader.
// 3. Waits (polls) until the command has been committed on at least expectedServers servers and the committed value exactly matches the one we submitted.
// (no duplicates, no missing entries, no wrong terms)
int Config::one(const std::string& command, int expectedServers, bool retry) {
    std::cout << "\n[Test] One starting..." << std::endl;
    auto startTime = std::chrono::steady_clock::now();
    int starts = 0;

    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < 10) 
    {
        int index = -1;
        int term = -1;

        // Try all servers in round-robin
        for (int si = 0; si < m_num; si++) 
        {
            starts = (starts + 1) % m_num;
            if (m_connected[starts] && m_rafts[starts]) 
            {
                auto [idx, t, ok] = m_rafts[starts]->start(command);
                if (ok) {
                    index = idx;
                    term = t;
                    break;
                }
            }
        }

        if (index != -1) 
        {
            auto innerStart = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - innerStart).count() < 2) 
            {
                auto [nd, cmd] = nCommitted(index);
                if (nd >= expectedServers && cmd == command) 
                {
                    return index; // committed successfully
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!retry) {
                throw std::runtime_error("one(" + command + ") failed to reach agreement in term " + std::to_string(term));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    throw std::runtime_error("one(" + command + ") failed to reach agreement (timeout)");
}


size_t Config::logSize()
{
    size_t maxSize = 0;
    for (int i = 0; i < m_num; i++) 
    {
        if (m_persisters[i]) 
        {
            size_t size = m_persisters[i]->raftStateSize();
            maxSize = std::max(maxSize, size);
        }
    }
    return maxSize;
}