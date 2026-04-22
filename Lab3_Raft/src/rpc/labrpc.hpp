#pragma once

#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <random>
#include <queue>
#include <future>

class Endpoint;                                                                        // RPC stubs for communication
class Server;                                                                          // Represent a Raft node in the cluser

struct ReplyMsg
{
    bool ok;
    std::string reply;
};

struct reqMsg 
{
    std::string endpointName;
    std::string rpcType;
    std::string args;
    std::promise<ReplyMsg> prom;
};


class Network : public std::enable_shared_from_this<Network> {
public:
    Network();
    ~Network();

    void send(const std::string& endpointName, const std::string& rpcType, const std::string& args, std::promise<ReplyMsg> prom);
    void deliver(const std::string& endpointName, const std::string& rpcType, const std::string& args, std::promise<ReplyMsg> prom);
    bool isServerDead(const std::string& endpointName, const std::string& serverName, const std::shared_ptr<Server>& server);

    /*
        APIs for adding/deleting/connecting/disconnect servers from network
    */
    std::shared_ptr<Endpoint> makeEndpoint(const std::string& endpointName);           // Create a new RPC endpoint in network
    void addServer(const std::string& serverName, std::shared_ptr<Server> server);     // Register a Raft server with the network
    void deleteServer(const std::string& serverName);                                  // Remvoes a Raft server from the network to simulate a crach
    void connect(const std::string& endpointName, const std::string& serverName);      // Associates an endpoint with a server so that RPCs can be delivered
    void enable(const std::string& endpointName, bool isEnabled);                      // Toggles whether an endpoint is active (simulates disconnect/connect)

    /*
        Network Reliability Controls
    */
    void setReliable(bool yes);                                                     // Simulate unreliable network. If false, messages may be dropped
    void setLongDelays(bool yes);                                                   // Simulate long delays. If true, messssages may be delayed
    void setLongReordering(bool yes);                                               // Simulate out-of-order delivery. If true, messages may arrive out of order

    /*
        Statistics
    */
    int getRPCCount(const std::string& serverName);                                 // Get RPC count for one server
    int getTotalRPCCount();                                                         // Get total RPCs sent
    long getTotalBytes();                                                           // Get total byts sent
    void cleanup();

private:
    
    bool m_reliable { true };
    bool m_longDelays { false };
    bool m_longReordering { false };
    bool m_done { false };
    std::queue<std::shared_ptr<reqMsg>> m_reqQueue;                 // queue of pending requests
    std::mutex m_queueMu;                                           // mutex to protect m_reQueue
    std::condition_variable m_cv;                                   // signals new requests
    std::map<std::string,std::shared_ptr<Server>> m_servers;        // Registry of all Raft servers currently alive in the cluster (server name, pointer to Server object)
    std::map<std::string,std::shared_ptr<Endpoint>> m_endpoints;    // Registry of all RPC Endpoints for RPC communication (endpoint name, pointer to Endpoint object)
    std::map<std::string,std::string> m_connectionMap;              // Map RPC endpoint names to the corresponding server name  (endpoint name, server name)
    std::map<std::string,bool> m_enabledMap;                        // Track whether each RPC endpoint is enabled to simulate partitions, dropped connections, disabled stubs (endpoint name, bool)
    std::atomic<int> m_totalRPCCount { 0 };
    std::atomic<long> m_totalBytes { 0 };
    std::mutex m_mu;
    std::thread m_dispatcher;
};