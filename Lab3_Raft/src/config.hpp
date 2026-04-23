#pragma once

#include <queue>
#include <vector>
#include <map>
#include <memory>
#include <condition_variable>
#include <string>
#include <mutex>
#include <chrono>
#include "raft.hpp"

class Raft;
class Persister;
class Network;


struct ApplyMsg
{
    bool CommandValid;                                      // true if this is a real log entry
    uint64_t  CommandIndex;                                 // the log index of the committed entry
    std::string Command;                                    // the actual client command stored in the log
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

    ApplyMsg pop()
    {
        std::unique_lock<std::mutex> lock { m_mu };
        m_cv.wait(lock, [this]{ 
            return !m_q.empty(); 
        });
        auto msg = m_q.front();
        m_q.pop();
        return msg;
    }

private:
    std::queue<ApplyMsg> m_q;
    std::mutex m_mu;
    std::condition_variable m_cv;
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

    /*Functions for testing*/
    int checkOneLeader();                                   // Returns leader ID
    int checkTerms();                                       // Returns current term across cluster
    void checkNoLeader();                                   // Asserts no leader exists
    std::pair<int,std::string> nCommitted(int index);                           // returns (#servers committed, command string)
    int one(const std::string& command, int expectedServers, bool retry);       // submit command and wait for commit
                                           

private:
    void checkTimeout();
    

    int m_num;                                               // number of Raft servers
    std::shared_ptr<Network> m_network;                      // simulated RPC network
    std::vector<std::shared_ptr<Raft>> m_rafts;              // Vector of Raft instances
    std::vector<bool> m_connected;                           // Vector of connection status for each Raft instance
    std::vector<std::shared_ptr<Persister>> m_persisters;    // Vector of persisters holding each server’s durable state (term, log, snapshot)
    std::vector<std::map<int,std::string>> m_logs;           // Tracks committed log entries for each server (used in log consistency tests)
    std::vector<std::vector<std::string>> m_endpointNames;   // RPC endpoint names between servers
    std::vector<std::string> m_applyErrors;

    std::chrono::steady_clock::time_point m_startTime;        // Timestamp for tracking duration of the entire test harness lifetime
    std::chrono::steady_clock::time_point m_t0;               // Timestamp for tracking duration of each test 
    int m_rpcs0;                                              // Snapshot of the total RPC count at the beginning of the test
    long m_bytes0;                                            // Snapshot of the total bytes sent at the beginning of the test
    uint64_t m_maxLogIndex {0};                                    // Tracks the highest log index observed across all Raft servers during the test
    uint64_t m_maxLogIndex0 {0};                                   // Baseline value of maxIndex at the start of each test
    std::mutex m_mu;
};
