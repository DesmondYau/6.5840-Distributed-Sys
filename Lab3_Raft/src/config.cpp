#include "config.hpp"
#include "raft.hpp"
#include "helper.hpp"
#include "persister.hpp"
#include "logger.hpp"
#include "rpc/labrpc.hpp"
#include "rpc/server.hpp"
#include "rpc/service.hpp"
#include <iostream>
#include <thread>
#include <stdexcept>

Config::Config(int num, bool unreliable)
    : m_num { num }
    , m_network (std::make_shared<Network>())
    , m_rafts(num)
    , m_connected(num , false)
    , m_persisters(num)
    , m_logs(num)
    , m_endpointNames(num, std::vector<std::string>{})
    , m_applyErrors(num)
{
    for (int i{0}; i < m_num; i++) {
        startServer(i);
        connectServer(i);
    }

    setNetworkUnreliable(unreliable);
    m_network->setLongDelays(true);

    m_startTime = std::chrono::steady_clock::now();
}

Config::~Config() { cleanup(); }

void Config::begin(const std::string& description) {
    std::cout << std::endl 
              << "---------------------------------"
              << description << " ..." 
              << "---------------------------------"
              << std::endl;
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
    std::cout << "Number of Raft peers =" << m_num << std::endl;
    std::cout << "Number of RPCs sent =" << numRPC << std::endl;
    std::cout << "Total number of bytes sent =" << numBytes << std::endl;
    std::cout << "Number of log entires committed =" << numCommits << std::endl;
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
        if (m_logs[i].contains(index)) {
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

    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - startTime).count() < 10) 
    {
        int index = -1;
        int term = -1;

        // Try all servers in round-robin
        for (int si = 0; si < m_num; si++) {
            starts = (starts + 1) % m_num;
            if (m_connected[starts] && m_rafts[starts]) {
                auto [idx, t, ok] = m_rafts[starts]->start(command);
                if (ok) {
                    index = idx;
                    term = t;
                    break;
                }
            }
        }

        if (index != -1) {
            auto innerStart = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - innerStart).count() < 2) 
            {
                auto [nd, cmd] = nCommitted(index);
                if (nd >= expectedServers && cmd == command) {
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




void Config::startServer(int i) 
{ 
    std::cout << "[Config] Attempting to start server " << i << std::endl;
    
    // Kill any existing instance and preserve state
    crashServer(i); 
    
    // Generate fresh endpoint names for outgoing RPCs
    m_endpointNames[i] = std::vector<std::string>(m_num);
    std::vector<std::shared_ptr<Endpoint>> endpoints(m_num);
    for (int j{0}; j<m_num; j++)
    {
        m_endpointNames[i][j] = "From_" + std::to_string(i) + "_To_" + std::to_string(j);
        auto endpoint = m_network->makeEndpoint(m_endpointNames[i][j]);
        m_network->connect(m_endpointNames[i][j], std::to_string(j));
        endpoints[j] = endpoint;
    }

    // Copy or create persister
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_persisters[i]) 
        {
            m_persisters[i] = std::make_shared<Persister>(*m_persisters[i]);
        } 
        else 
        {
            m_persisters[i] = std::make_shared<Persister>();
        }
    }
    

    // ApplyChannel listens to messages from Raft indicating newly committed messages
    auto applyChannel = std::make_shared<ApplyChannel>();
    auto sharedLogger = std::make_shared<Logger>();
    std::thread([this, i, applyChannel] {
        while (true) {
            ApplyMsg msg = applyChannel->pop();        // blocking queue pop

            std::string errMsg;
            if (!msg.CommandValid) {
                // ignore other types of ApplyMsg
            } else {
                std::string v = msg.Command;
                {
                    std::lock_guard<std::mutex> lock { this->m_mu };
                    // check consistency across servers
                    for (size_t j = 0; j < m_logs.size(); j++) {
                        if (m_logs[j].contains(msg.CommandIndex) && m_logs[j][msg.CommandIndex] != v)
                        {
                            errMsg = "commit index=" + std::to_string(msg.CommandIndex) +
                                    " server=" + std::to_string(i) + " " + msg.Command +
                                    " != server=" + std::to_string(j) + " " + m_logs[j][msg.CommandIndex];
                        }
                    }

                    m_logs[i][msg.CommandIndex] = v;
                    m_maxLogIndex = std::max(m_maxLogIndex, msg.CommandIndex);

                    if (msg.CommandIndex > 1 && !m_logs[i].contains(msg.CommandIndex-1)) {
                        errMsg = "server " + std::to_string(i) +
                                " apply out of order " + std::to_string(msg.CommandIndex);
                    }
                }
            }

            if (!errMsg.empty()) {
                // record error instead of throwing
                std::lock_guard<std::mutex> lock { this->m_mu };
                m_applyErrors[i] = errMsg;
                std::cerr << "apply error: " << errMsg << std::endl;
                // keep reading after error so Raft doesn't block
            }
        }
    }).detach();

    // Create Raft instance
    auto raft = std::make_shared<Raft>(endpoints, i, m_persisters[i], applyChannel, sharedLogger);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_rafts[i] = raft;
    }
    
    // wrap Raft in a Service
    auto raftService = std::make_shared<Service>("Raft");

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

    // Create a Server and attach Service to Server
    auto server = std::make_shared<Server>();
    server->addService("Raft", raftService);

    // Add Server to network
    m_network->addServer(std::to_string(i), server);

    std::cout << "[Config] Server " << i << " added successfully" << std::endl;
}

void Config::crashServer(int i) 
{
    std::cout << "[Config] Attempting to crash server " << i << std::endl;

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
        auto raftState { m_persisters[i]->readRaftState() };
        m_persisters[i] = std::make_shared<Persister>();
        m_persisters[i]->saveRaftState(raftState);
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

void Config::checkTimeout() {
    auto elapsed = std::chrono::steady_clock::now() - m_startTime;
    if (elapsed > std::chrono::minutes(2))
        throw std::runtime_error("test took longer than 120s");
}

void Config::setNetworkUnreliable(bool unreliable) 
{ 
    m_network->setReliable(!unreliable); 
}

void Config::setNetworkLongReordering(bool reorder)
{
    m_network->setLongReordering(reorder);
}
