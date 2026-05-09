#include <random>
#include <thread>
#include <iomanip>
#include "config.hpp"
#include "kvserver.hpp"
#include "clerk.hpp"
#include "persister.hpp"
#include "rpc/labrpc.hpp"
#include "rpc/server.hpp"

/*
static std::string randstring(int n) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s(n, '0');
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, sizeof(charset)-2);
    for (int i = 0; i < n; ++i) {
        s[i] = charset[dist(gen)];
    }
    return s;
}
*/

Config::Config(int num, bool unreliable, int maxraftstate)
    : m_num {num}
    , m_maxraftstate {maxraftstate}
    , m_start {std::chrono::steady_clock::now()}
    , m_net {std::make_shared<labrpc::Network>()}
{
    m_kvservers.resize(m_num);
    m_persisters.resize(m_num);
    m_endpointNames.resize(m_num, std::vector<std::string>(m_num));

    m_nextClientId = m_num + 1000;

    m_net->setReliable(!unreliable);
}

Config::~Config() 
{
    cleanup();
}

// ===================================================================
// Test control
// ===================================================================

void Config::begin(const std::string& description) 
{
    std::cout << description << " ..." << std::endl;
    m_t0 = std::chrono::steady_clock::now();
    rpcs0 = rpcTotal();
    m_ops = 0;
}

void Config::end() 
{
    cleanup();
    checkTimeout();

    auto duration = std::chrono::steady_clock::now() - m_t0;
    double secs = std::chrono::duration<double>(duration).count();
    int nrpc = rpcTotal() - rpcs0;
    int nops = m_ops.load();

    std::cout << " ... Passed -- "
              << std::fixed << std::setprecision(1) << secs << "s "
              << m_num << " " << nrpc << " " << nops << std::endl;
}

void Config::op() 
{
    m_ops.fetch_add(1, std::memory_order_relaxed);
}

// ===================================================================
// Server Management
// ===================================================================



void Config::startServer(int i) 
{
    std::lock_guard<std::mutex> lock(m_mu);

    // 1. fresh set of labrpc Endpoint names
    for (int j = 0; j < m_num; ++j) 
    {
        m_endpointNames[i][j] = "From_" + std::to_string(i) + "_To_" + std::to_string(j);
    }

    // 2. Create fresh labrpc Endpoints and connect them in labrpc
    std::vector<std::shared_ptr<labrpc::Endpoint>> peers;
    for (int j = 0; j < m_num; ++j) 
    {
        auto endpoint = m_net->makeEndpoint(m_endpointNames[i][j]);
        m_net->connect(m_endpointNames[i][j], std::to_string(j));

        peers.push_back(endpoint);
    }

    // 3. Fresh persister (copy old state if exists)
    if (m_persisters[i]) 
    {
        m_persisters[i] = std::make_shared<Persister>(*m_persisters[i]);
    } 
    else 
    {
        m_persisters[i] = std::make_shared<Persister>();
    }

    // 4. Create the labrpc::Server that will host the KVServer
    auto labrpcServer = std::make_shared<labrpc::Server>();

    // 5. Create the KVServer using factor function
    m_kvservers[i] = startKVServer(i, m_maxraftstate, labrpcServer.get(), m_persisters[i], peers);

    // Register server with network
    m_net->addServer(std::to_string(i), labrpcServer);

    std::cout << "[Config] Started server " << i << std::endl;
}

void Config::shutdownServer(int i) 
{
    std::lock_guard<std::mutex> lock(m_mu);

    // Flips boolean flag for ths server endpoint in labrpc to false
    disconnectUnlocked(i, all());
    // Remove server from the "routing map" in labrpc
    m_net->deleteServer(std::to_string(i));

    
    if (m_persisters[i]) 
    {
        // Copy old persister's content to save the last persisted state in a different part of the computer's memory
        // Any background threads that might make changes after shutdownServer is called would only make changes to the old persister
        m_persisters[i] = std::make_unique<Persister>(*m_persisters[i]);
    }

    if (m_kvservers[i]) 
    {
        // 1. Tell the server to gracefully shut down its threads
        m_kvservers[i]->kill();

        // 2. Destroy the object and set the pointer in the array to nullptr
        m_kvservers[i].reset();
    }

    std::cout << "[Config] Shutedown server " << i << std::endl;
}

// ===================================================================
// Network Connectivity
// ===================================================================

void Config::connectAll() 
{
    std::lock_guard<std::mutex> lock(m_mu);

    for (int i = 0; i < m_num; ++i) 
    {
        for (int j = 0; j < m_num; ++j) 
        {
            if (!m_endpointNames[i].empty()) 
            {
                m_net->enable(m_endpointNames[i][j], true);
            }
        }
    }
}


void Config::partition(const std::vector<int>& p1, const std::vector<int>& p2) 
{
    std::lock_guard<std::mutex> lock(m_mu);
    for (int i : p1) 
    {
        disconnectUnlocked(i, p2);
        connectUnlocked(i, p1);
    }

    for (int i : p2) 
    {
        disconnectUnlocked(i, p1);
        connectUnlocked(i, p2);
    }
}

void Config::connectUnlocked(int i, const std::vector<int>& to) 
{
    for (int j : to) 
    {
        if (!m_endpointNames[i].empty()) 
        {
            m_net->enable(m_endpointNames[i][j], true);
        }
    }
    for (int j : to) 
    {
        if (!m_endpointNames[j].empty()) {
            m_net->enable(m_endpointNames[j][i], true);
        }
    }
}

void Config::disconnectUnlocked(int i, const std::vector<int>& from) 
{
    for (int j : from) 
    {
        if (!m_endpointNames[i].empty()) 
        {
            m_net->enable(m_endpointNames[i][j], false);
        }
    }
    for (int j : from) {
        if (!m_endpointNames[j].empty()) 
        {
            m_net->enable(m_endpointNames[j][i], false);
        }
    }
}

// ===========================================================================
// Client management
// ===========================================================================

Clerk* Config::makeClient(const std::vector<int>& to) 
{
    std::lock_guard<std::mutex> lock(m_mu);

    // Generate fresh unique endpoint names for this client
    std::vector<std::string> endnames_local(m_num);
    for (int j = 0; j < m_num; ++j) 
    {
        endnames_local[j] = "From_Client_" + std::to_string(m_nextClientId) + "_To_" + std::to_string(j);
    }

    // Create and register the endpoints with the network
    for (int j = 0; j < m_num; ++j) 
    {
        m_net->makeEndpoint(endnames_local[j]);
        m_net->connect(endnames_local[j], std::to_string(j));
    }

    // Prepare list of server endpoint names for client to talk to
    std::vector<std::string> servers;
    for (int j = 0; j < m_num; ++j)
    {
        servers.push_back(endnames_local[j]);
    }

    // Shuffle the server list. This tests the Clerk’s leader discovery and retry logic more thoroughly
    // Prevents the client from always contacting servers in the same fixed order (which could hide bugs)
    // In real distributed systems, clients should not assume any particular order of servers
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(servers.begin(), servers.end(), g);


    // Create the Clerk
    auto ck_unique = std::make_unique<Clerk>(m_net.get(), servers);
    Clerk* ck = ck_unique.release();                                        // release ownership to the test caller to avoid dangling pointer

    // Store mapping
    m_clerks[ck] = std::move(endnames_local);

    // Increment counter 
    m_nextClientId++;

    // Connect the client to the requested servers
    connectClientUnlocked(ck, to);

    return ck;
}

void Config::deleteClient(Clerk* ck) 
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_clerks.erase(ck);
    delete ck;                  // Ensure memory is cleaned up
}

void Config::connectClient(Clerk* ck, const std::vector<int>& to) 
{
    std::lock_guard<std::mutex> lock(m_mu);
    connectClientUnlocked(ck, to);
}

void Config::connectClientUnlocked(Clerk* ck, const std::vector<int>& to) 
{
    if (!m_clerks.contains(ck))
    {
        return;
    }

    const auto& names = m_clerks.at(ck); 
    
    for (int j : to) {
        // Enable the network endpoints for the specified servers
        m_net->enable(names[j], true); 
    }
}

void Config::disconnectClient(Clerk* ck, const std::vector<int>& from) 
{
    std::lock_guard<std::mutex> lock(m_mu);
    disconnectClientUnlocked(ck, from);
}

void Config::disconnectClientUnlocked(Clerk* ck, const std::vector<int>& to) 
{
    if (!m_clerks.contains(ck))
    {
        return;
    }

    const auto& names = m_clerks.at(ck); 
    
    for (int j : to) {
        // Enable the network endpoints for the specified servers
        m_net->enable(names[j], false); 
    }
}


// ===========================================================================
// Helper functions
// ===========================================================================

size_t Config::logSize() {
    size_t maxsz = 0;
    for (const auto& p : m_persisters) 
    {
        if (p) 
        {
            maxsz = std::max(maxsz, p->raftStateSize());
        }
    }
    return maxsz;
}

size_t Config::snapshotSize() {
    size_t maxsz = 0;    
    for (const auto& p : m_persisters) 
    {
        if (p)
        {
            maxsz = std::max(maxsz, p->snapshotSize());
        }
    }
    return maxsz;
}

int Config::rpcTotal() {
    return m_net ? m_net->getTotalRPCCount() : 0;
}

void Config::checkTimeout() {
    if (std::chrono::steady_clock::now() - m_start > std::chrono::seconds(120)) 
    {
        std::cerr << "test took longer than 120 seconds" << std::endl;
        exit(1);
    }
}

std::vector<int> Config::all() 
{
    std::vector<int> all(m_num);
    for (int i = 0; i < m_num; ++i)
    {
        all[i] = i;
    }
    return all;
}

std::pair<bool, int> Config::getLeader() 
{
    std::lock_guard<std::mutex> lock(m_mu);
    for (int i = 0; i < m_num; ++i) {
        if (m_kvservers[i]) 
        {
            // Query the underlying Raft instance to check its leadership status
            bool isLeader = m_kvservers[i]->getRaft()->isLeader();  
            if (isLeader) 
                return {true, i};
        }
    }
    return {false, -1};
}

/**
 * Splits the cluster into two groups: p1 (majority) and p2 (minority).
 * Specifically places the current leader into the minority group p2[cite: 6].
 */
std::pair<std::vector<int>, std::vector<int>> Config::make_partition() 
{
    auto [isLeader, leader] = getLeader();
    std::vector<int> p1, p2;
    p1.reserve(m_num/2 + 1); 
    p2.reserve(m_num/2);

    int j = 0;
    for (int i = 0; i < m_num; ++i) 
    { 
        // Skip the leader during the initial distribution
        if (i != leader) 
        {
            // Fill the first group (p1) until it reaches majority capacity
            if (j < static_cast<int>(p1.capacity())) 
            {
                p1.push_back(i);
            } 
            else 
            {
                p2.push_back(i);
            }
            ++j;
        }
    }
    // Add the leader to p2, ensuring it is isolated from the majority p1
    if (leader != -1) 
    {
        p2.push_back(leader);
    }
    return {p1, p2};
}

void Config::cleanup() 
{
    std::lock_guard<std::mutex> lock(m_mu);
    for (auto& srv : m_kvservers) 
    {
        if (srv)
        {
            srv->kill();
        } 
    }
    if (m_net)
    {
        m_net->cleanup();
    } 
    checkTimeout();
}


std::unique_ptr<Config> make_config(int n, bool unreliable, int maxraftstate)
{
    // Check for CPU count
    static std::once_flag ncpu_once;
    std::call_once(ncpu_once, [](){
        if (std::thread::hardware_concurrency() < 2) 
        {
            std::cout << "warning: only one CPU, which may conceal locking bugs" << std::endl;
        }
    });

    // Create the Config Object which is done after config metadata is ready
    auto cfg = std::make_unique<Config>(n, unreliable, maxraftstate);

    // Create a full set of KV servers 
    for (int i = 0; i < n; ++i) 
    {
        cfg->startServer(i);
    }
    
    // Finalize network connectivity
    cfg->connectAll();

    return cfg;
}