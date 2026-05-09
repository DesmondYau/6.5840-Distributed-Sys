#pragma once

#include <queue>
#include <vector>
#include <map>
#include <memory>
#include <condition_variable>
#include <string>
#include <mutex>
#include <chrono>

#include "rpc/labrpc.hpp"

class Raft;
class Persister;


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
    void push(const ApplyMsg& msg)
    {
        std::lock_guard<std::mutex> lock { m_mu };
        m_q.push(msg);
        m_cv.notify_one();
    }

    std::optional<ApplyMsg> pop()
    {
        std::unique_lock<std::mutex> lock { m_mu };
        m_cv.wait(lock, [this]{ 
            return !m_q.empty() || m_closed;
        });
        
        // If closed and empty, return nullopt so the thread knows to exit
        if (m_q.empty() && m_closed) {
            return std::nullopt;
        }

        auto msg = m_q.front();
        m_q.pop();
        return msg;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock { m_mu };
        m_closed = true;
        m_cv.notify_all();
        
    }

private:
    std::queue<ApplyMsg> m_q;
    std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_closed = false;
};


class Config {
public:
    Config(int servers, bool unreliable);                   // Builds a cluster with <servers> Raft instances. The <unrealiable flag controls whether the simulated network drops/delays RPCs
    ~Config();

    void begin(const std::string& description);             // Begin test case
    void end();

    void startServer(int i);                                // Start/restart Raft server i
    void crashServer(int i);                                // Kill Raft server i but save state
    void connectServer(int i);                              // Attach server i to network
    void disconnectServer(int i);                           // Detach server i from network  
    long bytesTotal();
    std::shared_ptr<Raft> getRaft(int i);
    void setNetworkUnreliable(bool unrel);
    void setNetworkLongReordering(bool reorder);
    void cleanup();  

    /*Functions for snapshots*/
    std::string ingestSnap(int i, const std::vector<uint8_t>& snapshot, int index);     // load the snapshot into the test harness's internal verification data structures so the test can check correctness
    void applierSnap(int i, std::shared_ptr<ApplyChannel> applyChannel);       // Processes snapshot/apply messages coming from Raft and periodically trigger snapshot

    /*Functions for testing*/
    void checkTimeout();
    int checkOneLeader();                                                       // Returns leader ID
    int checkTerms();                                                           // Returns current term across cluster
    void checkNoLeader();                                                       // Asserts no leader exists
    std::pair<int,std::string> nCommitted(int index);                           // returns (#servers committed, command string)
    int one(const std::string& command, int expectedServers, bool retry);       // submit command and wait for commit
    size_t logSize();                                                           // Returns maximum serialized raftstate size across all raft persisters
                                           

private:
    int m_num;                                                  // number of Raft servers
    std::shared_ptr<labrpc::Network> m_network;                 // simulated RPC network
    std::vector<std::shared_ptr<Raft>> m_rafts;                 // Vector of Raft instances
    std::vector<bool> m_connected;                              // Vector of connection status for each Raft instance
    std::vector<std::shared_ptr<Persister>> m_persisters;       // Vector of persisters holding each server’s durable state (term, log, snapshot)
    std::vector<std::map<int,std::string>> m_logs;              // Tracks committed log entries for each server (used in log consistency tests)
    std::vector<std::vector<std::string>> m_endpointNames;      // RPC endpoint names between servers
    std::vector<uint64_t> m_lastApplied;                        // Tracks the highest log index that the test harness believes server i has applied so far
    std::vector<std::string> m_applyErrors;
    std::vector<std::shared_ptr<ApplyChannel>> m_applyChannels;
    std::vector<std::thread> m_applierThreads;
    std::vector<std::atomic<bool>> m_applierStopped;            // Signal applier thread (used for applying and taking snapshot) to stop
    std::mutex m_mu;
    const int SnapShotInterval {10};                             // Interval of how frequently snapshot is taken by config object/test harness

    std::chrono::steady_clock::time_point m_startTime;          // Timestamp for tracking duration of the entire test harness lifetime
    std::chrono::steady_clock::time_point m_t0;                 // Timestamp for tracking duration of each test 
    int m_rpcs0;                                                // Snapshot of the total RPC count at the beginning of the test
    long m_bytes0;                                              // Snapshot of the total bytes sent at the beginning of the test
    uint64_t m_maxLogIndex {0};                                 // Tracks the highest log index observed across all Raft servers during the test
    uint64_t m_maxLogIndex0 {0};                                // Baseline value of maxIndex at the start of each test
    size_t m_maximumRaftStateSize {0};                          // Tracks maximum serialized raftstate size across all raft persisters. Used in test 3D
    
};
