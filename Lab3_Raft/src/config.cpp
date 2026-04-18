#include "config.hpp"
#include "raft.hpp"
#include "helper.hpp"
#include "persister.hpp"
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
    std::cout << description << " ..." << std::endl;
    m_t0 = std::chrono::steady_clock::now();
    m_rpcs0 = m_network->getTotalRPCCount();
    m_bytes0 = m_network->getTotalBytes();
    m_maxLogIndex0 = m_maxLogIndex;
}

void Config::end() {
    checkTimeout();

    auto elapsed { std::chrono::steady_clock::now() - m_t0 };
    int numRPC { m_network->getTotalRPCCount() - m_rpcs0 };
    long numBytes { m_network->getTotalBytes() - m_bytes0 };
    int numCommits { m_maxLogIndex - m_maxLogIndex0 };

    std::cout << "  ... Passed -- " << std::endl;
    std::cout << "Test took " << std::chrono::duration_cast<std::chrono::seconds>(elapsed) << "s" << std::endl;
    std::cout << "Number of Raft peers =" << m_num << std::endl;
    std::cout << "Number of RPCs sent =" << numRPC << std::endl;
    std::cout << "Total number of bytes sent =" << numBytes << std::endl;
    std::cout << "Number of log entires committed =" << numCommits << std::endl;
}

int Config::checkOneLeader() {
    std::cout << "[Test 2A] CheckOneLeader starting..." << std::endl;

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
    std::cout << "[Test 2A] CheckTerms starting..." << std::endl;

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
    for (int i{0}; i < m_num; i++) {
        if (m_connected[i] && m_rafts[i]) {
            auto [_, state] = m_rafts[i]->getTermState();
            if (state == Raft::State::LEADER)
                throw std::runtime_error("expected no leader");
        }
    }
}

std::pair<int,std::string> Config::nCommitted(int index) {
    std::lock_guard<std::mutex> lock(m_mu);

    int count = 0;
    std::string cmd;
    for (int i = 0; i < m_num; i++) {
        if (m_logs[i].contains(index)) {
            count++;
            if (cmd.empty()) {
                cmd = m_logs[i][index];
            } else if (m_logs[i][index] != cmd) {
                throw std::runtime_error("different commands committed at same index");
            }
        }
    }
    return {count, cmd};
}

int Config::one(const std::string& command, int expectedServers, bool retry) {
    // Try to find a leader and submit the command
    int index = -1;
    for (int tries = 0; tries < 5; tries++) {
        int leader = -1;
        for (int i = 0; i < m_num; i++) {
            if (m_connected[i] && m_rafts[i]) {
                auto [term, state] = m_rafts[i]->getTermState();
                if (state == Raft::State::LEADER) {
                    leader = i;
                    break;
                }
            }
        }

        if (leader != -1) {
            bool ok = false;
            index = m_rafts[leader]->start(command, ok);
            if (ok) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (index == -1) throw std::runtime_error("no leader to start command");

    // Wait until the command is committed by expectedServers
    for (int iters = 0; iters < 10; iters++) {
        auto [nd, cmd] = nCommitted(index);
        if (nd >= expectedServers) {
            if (cmd != command) {
                throw std::runtime_error("committed command does not match");
            }
            return index;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!retry) throw std::runtime_error("command not committed in time");
    return -1;
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
    auto raft = std::make_shared<Raft>(endpoints, i, m_persisters[i], applyChannel);
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

    disconnectServer(i);
    m_network->deleteServer(std::to_string(i)); 

    // copy persister state
    if (m_persisters[i]) 
    {
        m_persisters[i] = std::make_shared<Persister>(*m_persisters[i]);
    }

    if (m_rafts[i]) 
    {
        m_rafts[i]->kill();
        m_rafts[i] = nullptr;
    }

    // preserve last persisted state
    if (m_persisters[i]) {
        auto raftState { m_persisters[i]->ReadRaftState() };
        m_persisters[i] = std::make_shared<Persister>();
        m_persisters[i]->SaveRaftState(raftState);
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
