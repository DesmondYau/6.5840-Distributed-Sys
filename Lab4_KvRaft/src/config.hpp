#pragma once

#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <iostream>

#include "rpc/labrpc.hpp"
#include "raft.hpp"       
//#include <gtest/gtest.h>

class Clerk;
class KVServer;

class Config 
{
public:
    Config(int n, bool unreliable, int maxraftstate);
    ~Config();

    // Test Control
    void begin(const std::string& description);
    void end();
    void op();                          

    // Server Lifecycle Management 
    void shutdownServer(int i);
    void startServer(int i);

    // Network Conectivity
    void connectAll();
    void partition(const std::vector<int>& p1, const std::vector<int>& p2);
    Clerk* makeClient(const std::vector<int>& to);
    void deleteClient(Clerk* ck);
    void connectClient(Clerk* ck, const std::vector<int>& to);
    void disconnectClient(Clerk* ck, const std::vector<int>& from);

    // Query/Statistics
    std::vector<int> all();                                                 // returns a vector containing the IDs of every single server currently in the cluster.
    size_t snapshotSize();
    size_t logSize();
    int rpcTotal();
    std::pair<bool, int> getLeader(); 
    std::pair<std::vector<int>, std::vector<int>> make_partition();

    // Clean up
    void cleanup();

    

private:
    int m_num {0};                                                           // Number of servers in the cluster
    int m_nextClientId {0};                                                  // Counter used to generate unique IDs for Clerks. Starts at 0 and is incremented
    int m_maxraftstate {-1};                                                 // Maximum allowed size of Raft’s log (in bytes) before it must snapshot. -1 means “no limit” (used in early tests)
    std::mutex m_mu;                                                         // mutex
    std::shared_ptr<labrpc::Network> m_net;                                  // The simulated RPC network. Created in the constructor.
    std::vector<std::shared_ptr<KVServer>> m_kvservers;                      // Vector of all KVServer instances
    std::vector<std::shared_ptr<Persister>> m_persisters;                    // Vector of persisters for each raft instance
    std::vector<std::vector<std::string>> m_endpointNames;                   // endpointNnames[i][j] = name of client end from i to j
    std::map<Clerk*, std::vector<std::string>> m_clerks;                     // Maps each Clerk ptr to the list of servers it is currently connected to. Used by ConnectClient / DisconnectClient

    // Statistics
    std::chrono::steady_clock::time_point m_start;                              // Records the overall start time of the entire Config object
    std::chrono::steady_clock::time_point m_t0;                                 // Timestamp that marks the start of the current test case
    int rpcs0 {0};                                                              // A baseline (snapshot) of the total number of RPCs sent before the current test started.
    std::atomic<int32_t> m_ops {0};                                             // Counts how many client operations (Get, Put, Append) have been performed during the current test

    void checkTimeout();
    void connectUnlocked(int i, const std::vector<int>& to);
    void disconnectUnlocked(int i, const std::vector<int>& from);
    void connectClientUnlocked(Clerk* ck, const std::vector<int>& to);
    void disconnectClientUnlocked(Clerk* ck, const std::vector<int>& from);
};

// Factory function. Maybe use constructor
std::unique_ptr<Config> make_config(int n, bool unreliable, int maxraftstate);