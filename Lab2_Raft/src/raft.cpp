#include <random>
#include <chrono>
#include "helper.hpp"
#include "raft.hpp"
#include "config.hpp"
#include "persister.hpp"
#include "./rpc/endpoint.hpp"


Raft::Raft(const std::vector<std::shared_ptr<Endpoint>>& peers, int32_t id, std::shared_ptr<Persister> persister, std::shared_ptr<ApplyChannel> applyChannel)
    : m_peers { peers }
    , m_id { id }
    , m_votedFor { -1 }
    , m_currentTerm { 0 }  
    , m_commitIndex { 0 }
    , m_lastApplied { 0 }
    , m_state { State::FOLLOWER }
    , m_persister { persister }
    , m_applyChannel { applyChannel }
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(400, 600);
    m_electionTimeout = std::chrono::milliseconds(dist(gen));

    {
        // Initialize last valid heartbeat time to now
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastHeartbeat = std::chrono::steady_clock::now();
    }
    
    // Start our raft thread
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
                m_state = Raft::State::CANDIDATE;
                lock.unlock();
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
                std::cout << "[Raft" << m_id << "] Became leader!" << std::endl;
                m_state = State::LEADER;
                return;
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
            for (size_t id{0}; id<m_peers.size(); id++)
            {
                if (static_cast<int32_t>(id) == m_id)
                    continue;
                
                AppendEntriesArgs args;
                {
                    std::lock_guard<std::mutex> lock(m_mu);

                    if (m_dead.load() || m_state != State::CANDIDATE)
                        return;
                    
                    args = { m_currentTerm, m_id, static_cast<uint64_t>(m_logs.size()-1), m_logs.back()->term, "", m_commitIndex};
                    std::thread ([this, id, args] {
                        AppendEntriesReply reply;
                        bool received = sendAppendEntries(static_cast<int32_t>(id), args, reply);
                    }).detach(); 
                }
            }
        }
        else
        {
            std::cerr << "[Raft" << m_id << "] Invalid state. Exiting raft..." << std::endl;
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
    std::cout << "[Raft" << m_id << "] Starting election" << std::endl;

    // Increment current term. vote for self, reset election timer
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_currentTerm++;
        m_votedFor = m_id;
        m_lastHeartbeat = std::chrono::steady_clock::now();
        m_votesGranted = 1;
    }

    std::cout << "[Raft" << m_id << "] Sending requestVote RPC with term " << m_currentTerm << std::endl;
    uint64_t lastLogIndex = m_logs.empty() ? 0 : static_cast<uint64_t>(m_logs.size() - 1);
    uint32_t lastLogTerm  = m_logs.empty() ? 0 : static_cast<uint32_t>(m_logs.back()->term);
    
    for (size_t id{0}; id<m_peers.size(); id++)
    {
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
                else if (reply.voteGranted)
                {
                    std::cout << "[Raft" << m_id << "] Received vote from " << id << " " << reply.voteGranted << std::endl;
                    m_votesGranted++;
                }
                else if (reply.voteGranted && reply.term > getTermState().first)
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


bool Raft::sendAppendEntries(int32_t id, const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    m_peers[id]->call("Raft.AppendEntries", args, reply);
    return reply.success;
}

/*
    Public methods that are called by other raft node
    Using lock_guard to perform locking at all times
*/
void Raft::appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    std::lock_guard<std::mutex> lock(m_mu);

    
    // To implement
}


bool Raft::sendRequestVote(int32_t id, const RequestVoteArgs& args, RequestVoteReply& reply)
{
    std::cout << "[Raft" << m_id << "] Sending requestVote RPC to server " << id << std::endl;
    m_peers[id]->call("Raft.RequestVote", args, reply);
    return reply.voteGranted;
}

/*
    Public methods that are called by other raft node
    Using lock_guard to perform locking at all times
*/
void Raft::requestVote(const RequestVoteArgs& args, RequestVoteReply& reply)
{
    std::lock_guard<std::mutex> lock(m_mu);
    reply.term = m_currentTerm;

    if (m_state != State::FOLLOWER || args.term < m_currentTerm)
    {
        reply.voteGranted = false;
        return;
    }
    // Also need to check if logs up to date
    else if (m_votedFor == -1 || m_votedFor == args.candidateId)
    {
        // Grant vote, update term and reset timer
        m_currentTerm = args.term;
        m_votedFor = args.candidateId;
        m_lastHeartbeat = std::chrono::steady_clock::now();
        reply.voteGranted = true;
        std::cout << "[Raft" << m_id << "] State: " << static_cast<int>(m_state) << " voting for " << m_votedFor << std::endl;
        return;
    }
    else
    {
        reply.voteGranted = false;
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


