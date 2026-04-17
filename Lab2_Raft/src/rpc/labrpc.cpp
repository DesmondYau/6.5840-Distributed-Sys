#include "labrpc.hpp"
#include "endpoint.hpp"
#include "server.hpp"
#include <thread>
#include <chrono>
#include <future>



Network::Network() 
{
    // Start a network dispatcher thread and listen to 
    m_dispatcher = std::thread([this]() {
        while (!m_done) {
            std::shared_ptr<reqMsg> xreq;
            {
                std::unique_lock<std::mutex> lock(m_queueMu);
                m_cv.wait(lock, [this]() { return m_done || !m_reqQueue.empty(); });
                if (m_done) 
                    break;
                xreq = (m_reqQueue.front());
                m_reqQueue.pop();
            }

            // Track stats
            m_totalRPCCount.fetch_add(1, std::memory_order_relaxed);
            m_totalBytes.fetch_add(xreq->args.size(), std::memory_order_relaxed);

            // Process asynchronously
            std::thread([this, xreq]() {
                ReplyMsg replyMsg;
                this->deliver(xreq->endpointName, xreq->rpcType, xreq->args, replyMsg);
                xreq->prom.set_value(replyMsg);
            }).detach();
        }
    });
}

Network::~Network()
{
    m_done = true;
    m_cv.notify_all();
    if (m_dispatcher.joinable()) {
        m_dispatcher.join();
    }
}

void Network::send(const std::string& endpointName, const std::string& rpcType, const std::string& args, std::promise<ReplyMsg> prom) 
{
    auto req = std::make_shared<reqMsg>();
    req->endpointName = endpointName;
    req->rpcType = rpcType;
    req->args = args;
    req->prom = std::move(prom);

    {
        std::lock_guard<std::mutex> lock(m_queueMu);
        m_reqQueue.push(req);
    }
    m_cv.notify_one();
}


void Network::deliver(const std::string& endpointName, const std::string& rpcType, const std::string& args, ReplyMsg& replyMsg)
{
    /*
        Ensure RPC endpoint is enabled, connected and the server exists    
    */
    std::shared_ptr<Server> server;
    std::string serverName;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_enabledMap[endpointName] && m_connectionMap.contains(endpointName) && m_servers.contains(m_connectionMap[endpointName]))
        {
            serverName = m_connectionMap[endpointName];
            server = m_servers[serverName];
        }
        else
        {
            int ms = m_longDelays ? rand() % 7000 : rand() % 100;
            std::thread([&replyMsg, ms]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                replyMsg = { false, "" };
            }).detach();
            return;
        }
    }

    /*
        Simulate network unrealiability
    */
    // Short delay if unreliable
    if (!m_reliable) {
        int ms = rand() % 27;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    // Drop reqeuest randomly if network is unrealiable
    if (!m_reliable && rand() % 1000 < 100)
    {
        return;
    }
    
    /*
        Track RPC count and bytes sent
    */
    m_totalRPCCount.fetch_add(1, std::memory_order_relaxed);
    m_totalBytes.fetch_add(args.size(), std::memory_order_relaxed);

    /*
        Run RPC handler asynchronously so we can check server death while waiting.
    */
    // Dispatch message in separate thread
    std::promise<ReplyMsg> prom;
    std::future<ReplyMsg> fut = prom.get_future();
    std::thread([this, server, rpcType, args, p = std::move(prom)]() mutable {
        std::string localReply;
        bool ok = server->dispatch(rpcType, args, localReply);
        m_totalBytes += localReply.size();
        p.set_value({ok, localReply});
    }).detach();


    /*
        Poll 100ms. If reply arrives and not empty, mark replyOK. If not, check if server was deleted.
    */    
    bool replyOK { false };                                        // Track whether the background thread has finished and produced a valid reply
    bool serverDead { false };                                     // Tracks whether the server was deleted while we were waiting.
    ReplyMsg localReplyMsg { false, "" };

    // Wait loop: either reply arrives or server dies
    while (!replyOK && !serverDead) {
        if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) 
        {
            localReplyMsg = fut.get();
            replyOK = localReplyMsg.ok;
        } 
        else 
        {
            serverDead = isServerDead(endpointName, serverName, server);
            if (serverDead)
            {
                // Drain the future if it has already produced a value
                if (fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    fut.get(); 
                }
                return;
            }  
        }
    }

    /*
        Final server death check, simulate unreliable network, out-of-order deliver
    */
    serverDead = isServerDead(endpointName, serverName, server);
    if (!replyOK || serverDead) {                                       // server was killed or no reply: simulate timeout
        replyMsg = {false, ""};                                                 
        return;
    } 
    else if (!m_reliable && rand() % 1000 < 100)                        // Simulate unrealiable network. Drop the reply randomly 
    {
        replyMsg = {false, ""};                                 
        return;
    } 
    else if (m_longReordering && rand() % 1000 < 600)                   // Simulate out-of-order delivery. Delay the response for a while
    {
        int ms = 200 + (rand() % (1 + rand() % 2000));
        std::thread([this, localReplyMsg, &replyMsg, ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            replyMsg = localReplyMsg;
        }).detach();
    } 
    else                                                                 // normal reply
    {
        replyMsg = localReplyMsg;
    }
}

bool Network::isServerDead(const std::string& endpointName, const std::string& serverName, const std::shared_ptr<Server>& server) 
{
    std::lock_guard<std::mutex> lock(m_mu);

    // endpoint is disabled 
    if (!m_enabledMap[endpointName]) {
        return true;
    }

    // server removed
    if (!m_servers.contains(serverName)) {
        return true; 
    }

    // server replaced
    if (m_servers[serverName] != server) {
        return true; 
    }

    return false;
}

std::shared_ptr<Endpoint> Network::makeEndpoint(const std::string& endpointName) {
    std::lock_guard<std::mutex> lock(m_mu);

    // Create a new endpoint object
    auto endpoint = std::make_shared<Endpoint>(endpointName, shared_from_this());
    
    // Register in network maps
    m_endpoints[endpointName] = endpoint;
    m_enabledMap[endpointName] = false;
    
    return endpoint;
}

void Network::addServer(const std::string& serverName, std::shared_ptr<Server> server) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_servers[serverName] = server;

    std::cout << "[Labrpc] Added server: " << serverName << std::endl;
}

void Network::deleteServer(const std::string& serverName) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_servers[serverName] = nullptr;

    std::cout << "[Labrpc] Deleted server: " << serverName << std::endl;
}

void Network::connect(const std::string& endpointName, const std::string& serverName) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_connectionMap[endpointName] = serverName;
}

void Network::enable(const std::string& endpointName, bool isEnabled) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_enabledMap[endpointName] = isEnabled;

    if (isEnabled)
        std::cout << "[Labrpc] Endpoint: " << endpointName << " enabled: true" << std::endl;
    else 
        std::cout << "[Labrpc] Endpoint: " << endpointName << " enabled: false" << std::endl;
}


void Network::setReliable(bool yes) { m_reliable = yes; }

void Network::setLongDelays(bool yes) { m_longDelays = yes; }

void Network::setLongReordering(bool yes) { m_longReordering = yes; }

int Network::getRPCCount(const std::string& serverName) {
    if (m_servers.contains(serverName)) 
        return m_servers[serverName]->getCount();
    return 0;
}

int Network::getTotalRPCCount() { return m_totalRPCCount.load(); }

long Network::getTotalBytes() { return m_totalBytes.load(); }

void Network::cleanup() 
{ 
    std::lock_guard<std::mutex> lock(m_mu);
    m_servers.clear(); 
    m_endpoints.clear();
    m_connectionMap.clear(); 
    m_enabledMap.clear(); 
    m_done = true;
    m_cv.notify_all();
}
