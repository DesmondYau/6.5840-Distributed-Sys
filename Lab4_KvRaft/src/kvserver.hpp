#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include "rpc/labrpc.hpp"
#include "raft.hpp"   
#include "models.hpp"
#include "json.hpp"


// ---------------------------------------------------------------------------
// Reuse ApplyMsg and ApplyChannel from lab3
// ---------------------------------------------------------------------------
struct ApplyMsg
{
    bool CommandValid;                                      // true if this is a real log entry
    uint64_t  CommandIndex;                                 // the log index of the committed entry
    uint32_t CommandTerm;                                   // the term of the committed entry
    std::string Command;                                    // the actual client command stored in the log

    bool SnapshotValid;                                     // true if this message is a snapshot, false if this message is a normal command
    std::vector<uint8_t> Snapshot;                          // serialized snapshot data
    uint64_t LastIncludedIndex;                             // lastIncludedIndex
    uint32_t lastIncludedTerm;                              // lastIncludedTerm
};

class ApplyChannel
{
public:
    void push(const ApplyMsg& applyMsg)
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_q.push(applyMsg);
        m_cv.notify_one();
    }

    std::optional<ApplyMsg> pop()
    {
        std::unique_lock<std::mutex> lock(m_mu);
        m_cv.wait(lock, [this]() {
            return m_closed || !m_q.empty();
        });

        if (m_closed && m_q.empty())
        {
            return std::nullopt;
        }

        auto element = m_q.front();
        m_q.pop();
        return element;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_closed.store(true);
        m_cv.notify_all();
    }

private:
    std::queue<ApplyMsg> m_q;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::atomic<bool> m_closed {false};
};


// ---------------------------------------------------------------------------
// KVServer class
// ---------------------------------------------------------------------------


class KVServer : public std::enable_shared_from_this<KVServer>
{
public:
    KVServer(int id, int maxRaftState, std::shared_ptr<Persister> persister, const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers);
    ~KVServer();
    void initKVServer(labrpc::Server* labrpcSrv);
    void get(const models::GetArgs& args, models::GetReply& reply);
    void putAppend(const models::PutAppendArgs& args, models::PutAppendReply& reply);
    void kill();
    void applierLoop();
    Raft* getRaft();

private:
    void registerRPCs(labrpc::Server* labrpcSrv);

    int m_id;
    int m_maxRaftState {0};
    std::atomic<bool> m_dead {false};
    std::unique_ptr<Raft> m_raft;
    std::shared_ptr<Logger> m_logger;
    std::shared_ptr<ApplyChannel> m_applyChannel;                     // kvServer read committed logs from raft (which guarantees consistency of log) through Apply Channel 
    std::unordered_map<std::string, std::string> m_kvStore;           // The actual Key Value Store
    std::map<uint64_t, int> m_clientSeqMap;                           // Maps clientID to the highest SeqNum the server has successfull applied for them
    std::map<uint64_t, std::promise<models::Op>> m_waitMap;           // Allows the synchronous network RPC thread to go to sleep and wait for the asynchronous Raft cluster to finish consensus for a specific log entry                                                         // laprpc::Server is owned by Config object (for RPC usage)
    std::thread m_applierThread;
    std::mutex m_mu;
    
};

// Factory function (called by config)
std::shared_ptr<KVServer> startKVServer(int id, int maxRaftState, labrpc::Server* srv, std::shared_ptr<Persister> persister, const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers);