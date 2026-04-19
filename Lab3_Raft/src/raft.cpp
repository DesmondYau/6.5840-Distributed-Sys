#include <random>
#include <chrono>
#include "helper.hpp"
#include "raft.hpp"
#include "config.hpp"
#include "persister.hpp"
#include "logger.hpp"
#include "./rpc/endpoint.hpp"


Raft::Raft(const std::vector<std::shared_ptr<Endpoint>>& peers, int32_t id, std::shared_ptr<Persister> persister, 
           std::shared_ptr<ApplyChannel> applyChannel, std::shared_ptr<Logger> logger)
    : m_peers { peers }
    , m_id { id }
    , m_votedFor { -1 }
    , m_currentTerm { 0 }  
    , m_commitIndex { 0 }
    , m_lastApplied { 0 }
    , m_state { State::FOLLOWER }
    , m_persister { persister }
    , m_applyChannel { applyChannel }
    , m_logger { logger }
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(300, 500);
    m_electionTimeout = std::chrono::milliseconds(dist(gen));

    {
        // Initialize last valid heartbeat time to now
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastHeartbeat = std::chrono::steady_clock::now();
    }
    
    // Push in dummy log with term 0
    m_logs.push_back(std::make_shared<LogEntry>("", 0));

    // Start raft thread
    m_raftThread = std::thread(&Raft::startRaft, this);
}

Raft::~Raft()
{
    m_dead.store(true);
    if (m_raftThread.joinable())
    {
        m_raftThread.join();
    }
   
}

/*
    Mainly using unique_lock to lock at all time
    Only unlock when sleep (predicate evaluate to false) or when we call other public methods with locks
*/ 
void Raft::startRaft()
{
    std::unique_lock<std::mutex> lock(m_mu);
    while (!m_dead.load())
    {   
        if (m_state == Raft::State::FOLLOWER)
        {
            m_cv.wait_until(lock, m_lastHeartbeat + m_electionTimeout, [this] {
                return m_dead.load();
            });

            if (m_dead.load())
                return;
            else if (std::chrono::steady_clock::now() > m_lastHeartbeat + m_electionTimeout)
            {
                lock.unlock();
                promoteToCandidate();
                startElection();
                lock.lock();
            }
        }
        else if (m_state == Raft::State::CANDIDATE)
        {
            m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return (m_dead.load() || m_state != State::CANDIDATE || m_votesGranted > m_peers.size()/2);
            });

            if (m_dead.load() || m_state == State::FOLLOWER)
                return;
            else if (m_votesGranted > m_peers.size()/2)
            {
                lock.unlock();
                promoteToLeader();
                lock.lock();
            
                // Initialize nextIndex vector and matchIndex vector when first becoming leader
                auto lastLogIndex = static_cast<uint64_t>(m_logs.size()-1);
                for (size_t i{0}; i<m_peers.size(); i++)
                {
                    m_nextindex.emplace_back(lastLogIndex+1);
                    m_matchIndex.emplace_back(0);
                }
            }
            else if (std::chrono::steady_clock::now() > m_lastHeartbeat + m_electionTimeout)
            {
                lock.unlock();
                startElection();
                lock.lock();
            }
        }
        else if (m_state == Raft::State::LEADER)
        {
            lock.unlock();
            broadcastAppendEntries("");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            lock.lock();
        }
        else
        {
            LogEvent event(LogEvent::Type::ERROR, m_id, m_currentTerm, "Invalid State!");
            m_logger->log(LogLevel::ERROR, event);
            return;
        }
    }
}

/*
    Mainly using lock_guard for scoped based locking when accessing shared and mutable data
    Otherwise, we unblock (especially when we peform send/deliver)
    Notice we perform checking on server death and state when we lock again
*/
void Raft::startElection()
{
    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Starting Election with term " + std::to_string(m_currentTerm+1));
    m_logger->log(LogLevel::INFO, event);

    // Increment current term. vote for self, reset election timer
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_currentTerm++;
        m_votedFor = m_id;
        m_votesGranted = 1;
        m_lastHeartbeat = std::chrono::steady_clock::now();
    }

    uint64_t lastLogIndex = static_cast<uint64_t>(m_logs.size() - 1);
    uint32_t lastLogTerm  = m_logs.back()->term;
    for (size_t id{0}; id<m_peers.size(); id++)
    {
        // Request vote from all peers except itself
        if (static_cast<int32_t>(id) == m_id)
            continue;

        RequestVoteArgs args;
        {
            std::lock_guard<std::mutex> lock(m_mu);

            if (m_dead.load() || m_state != State::CANDIDATE)
                return;
            
            args = { m_currentTerm, m_id, lastLogIndex, lastLogTerm };
        }
        std::thread ([this, id, args] {
            RequestVoteReply reply;
            bool received = sendRequestVote(static_cast<int32_t>(id), args, reply);
            if (received)
            {
                std::lock_guard<std::mutex> lock(m_mu);
                if (m_dead.load() || m_state != State::CANDIDATE)
                    return;
                else if (reply.voteGranted == true)
                {
                    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Received true vote from server " + std::to_string(id));
                    m_logger->log(LogLevel::INFO, event);  

                    m_votesGranted++;
                }
                else if (!reply.voteGranted && reply.term > getTermState().first)
                {
                    m_currentTerm = reply.term;
                    m_votedFor = -1;
                    m_state = State::FOLLOWER;
                    return;
                }
            }
        }).detach();        
    }    
}


void Raft::broadcastAppendEntries(const std::string& logEntry)
{
    LogEvent event
    {
        logEntry.empty() ? LogEvent::Type::HEARTBEAT : LogEvent::Type::REPLICATION,
        m_id,
        m_currentTerm,
        logEntry.empty() ? "Broadcasting Heartbeat" : "Broadcasting AppendEntries with entry: " + logEntry
    };
    m_logger->log(LogLevel::DEBUG, event);

    for (size_t id{0}; id<m_peers.size(); id++)
    {
        if (static_cast<int32_t>(id) == m_id)
            continue;
        
        AppendEntriesArgs args;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            
            if (m_dead.load() || m_state != State::LEADER) 
                return;
            
            uint64_t prevLogIndex = m_nextindex[id] - 1;
            uint32_t prevLogTerm  = m_logs[prevLogIndex]->term;
                           
            args = { m_currentTerm, m_id, prevLogIndex, prevLogTerm, logEntry, m_commitIndex};
        }
        std::thread ([this, id, args] {
            AppendEntriesReply reply;
            bool received = sendAppendEntries(static_cast<int32_t>(id), args, reply);
        }).detach(); 

    }
}

bool Raft::sendAppendEntries(int32_t id, const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    return m_peers[id]->call("Raft.AppendEntries", args, reply);
    
}

/*
    Public methods that are called by other raft node
    Using lock_guard to perform locking at all times
*/
void Raft::appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    std::lock_guard<std::mutex> lock(m_mu);
    reply.term = m_currentTerm;
    reply.success = false;

    // Terms in AppendEntries RPC < current term of raft
    if (m_currentTerm > args.term)
    {
        reply.success = false;
    }
    else
    {
        if (m_currentTerm < args.term)
        {
            m_currentTerm = args.term;
            m_votedFor = -1;
            m_state = State::FOLLOWER;
        }

        if (m_logs[args.preLogIndex]->term != args.preLogTerm)
        {
            reply.success = false;
            // delete the existing entry and all that follow it
        }
        else
        {
            reply.success = true;
            m_lastHeartbeat = std::chrono::steady_clock::now();
            // append new entries not already in the log
            // if leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
        }
            
    }   
}


bool Raft::sendRequestVote(int32_t id, const RequestVoteArgs& args, RequestVoteReply& reply)
{
    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Sending request vote to server " + std::to_string(id));
    m_logger->log(LogLevel::DEBUG, event);

    return m_peers[id]->call("Raft.RequestVote", args, reply);
}

/*
    Public methods that are called by other raft node
    Using lock_guard to perform locking at all times
*/
void Raft::requestVote(const RequestVoteArgs& args, RequestVoteReply& reply)
{
    std::lock_guard<std::mutex> lock(m_mu);
    reply.term = m_currentTerm;
    reply.voteGranted = false;

    if (m_currentTerm > args.term)
    {
        LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Reject requestVote: Candidate has lower term");
        m_logger->log(LogLevel::DEBUG, event);

        reply.term = m_currentTerm;
        reply.voteGranted = false;
    }
    else
    {
        if (m_currentTerm < args.term)
        {
            m_currentTerm = args.term;
            m_votedFor = -1;
            m_state = State::FOLLOWER;
        }
        
        bool logsUpToDate = (args.lastLogTerm < m_logs.back()->term || 
                             args.lastLogTerm == m_logs.back()->term && args.lastLogIndex <= static_cast<uint64_t>(m_logs.size()-1));
        if (m_votedFor == -1 || m_votedFor == args.candidateId || logsUpToDate)
        {
            reply.voteGranted = true;
            m_votedFor = args.candidateId;
            m_lastHeartbeat = std::chrono::steady_clock::now();

            LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Voted for " + std::to_string(m_votedFor));
            m_logger->log(LogLevel::INFO, event);
        }
    }        
}


void Raft::kill()
{
    m_dead.store(true);
    m_cv.notify_all();
}


std::pair<uint32_t, Raft::State> Raft::getTermState()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return std::pair<uint32_t, State>{ m_currentTerm, m_state };
}

void Raft::promoteToLeader()
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_state = State::LEADER;
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to LEADER");
    m_logger->log(LogLevel::INFO, event);

}


void Raft::promoteToCandidate()
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_state = Raft::State::CANDIDATE;
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to CANDIDATE");
    m_logger->log(LogLevel::INFO, event);
}



