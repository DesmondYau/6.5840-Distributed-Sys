#include <random>
#include <chrono>
#include <tuple>
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
    
    m_electionTimeout = std::chrono::milliseconds(generateTimeout());

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
            // Use wait_for instead of wait_until deadline to allow more checking
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return m_dead.load() || (std::chrono::steady_clock::now() > m_lastHeartbeat + m_electionTimeout);
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
            // Use wait_for instead of wait_until deadline to allow more checking
            m_cv.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return (m_dead.load() || m_state != State::CANDIDATE || m_votesGranted > m_peers.size()/2);
            });

            if (m_dead.load())
                return;

            if (m_state == State::FOLLOWER)
                continue; 

            else if (m_votesGranted > m_peers.size()/2)
            {
                lock.unlock();
                promoteToLeader();
                lock.lock();
            
                // Initialize nextIndex vector and matchIndex vector when first becoming leader
                auto lastLogIndex = static_cast<uint64_t>(m_logs.size()-1);
                m_nextindex.assign(m_peers.size(), lastLogIndex+1);
                m_matchIndex.assign(m_peers.size(), 0);
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
            broadcastAppendEntries();
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
    // Increment current term. vote for self, reset election timer
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_dead.load() || m_state != State::CANDIDATE) 
            return;

        m_currentTerm++;
        m_votedFor = m_id;
        m_votesGranted = 1;
        m_lastHeartbeat = std::chrono::steady_clock::now();
        m_electionTimeout = std::chrono::milliseconds(generateTimeout());

        LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Starting Election with term " + std::to_string(m_currentTerm));
        m_logger->log(LogLevel::INFO, event);
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

                else if (reply.voteGranted)
                {
                    m_votesGranted++;
                    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Received true vote from server " + std::to_string(id));
                    m_logger->log(LogLevel::INFO, event);  
                    
                    m_cv.notify_all();          // ← wake up main thread to check if there is enough vote
                }
                else if (!reply.voteGranted && reply.term > m_currentTerm)
                {
                    m_currentTerm = reply.term;
                    m_votedFor = -1;
                    m_state = State::FOLLOWER;
                    // Notice we should reset election timer here. Otherwise, we will immediate convert back to CANDIDATE once  we turn into FOLLOWER since timer was not reset
                    m_lastHeartbeat = std::chrono::steady_clock::now();
                    m_electionTimeout = std::chrono::milliseconds(generateTimeout());

                    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to FOLLOWER");
                    m_logger->log(LogLevel::INFO, event);
                    
                    m_cv.notify_all();          // ← wake up main thread on state change
                }
            }
        }).detach();        
    }    
}


void Raft::broadcastAppendEntries()
{
    for (size_t id{0}; id<m_peers.size(); id++)
    {
        if (static_cast<int32_t>(id) == m_id)
            continue;
        
        
        AppendEntriesArgs args;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_dead.load() || m_state != State::LEADER) 
                return;
            
            // We should use size of log entries to determine whether the AppendEntries RPC is a heartbeat (empty) or not (with valid log entries)
            std::vector<LogEntry> logEntries;
            for (size_t i = m_nextindex[id]; i<m_logs.size(); i++)
            {
                logEntries.emplace_back(m_logs[i]->command, m_logs[i]->term);
            }
            uint64_t prevLogIndex = (m_nextindex[id] == 0 ? 0 : m_nextindex[id] - 1);
            uint32_t prevLogTerm  = m_logs[prevLogIndex]->term;
                           
            args = { m_currentTerm, m_id, prevLogIndex, prevLogTerm, logEntries, m_commitIndex};

            LogEvent event {
                logEntries.empty() ? LogEvent::Type::HEARTBEAT : LogEvent::Type::REPLICATION,
                m_id,
                m_currentTerm,
                logEntries.empty() ? "Broadcasting Heartbeat" : "Broadcasting AppendEntries to " + std::to_string(id) + " starting:" + std::to_string(prevLogIndex + 1) + " ending:" + std::to_string(prevLogIndex + logEntries.size())
            };
            m_logger->log(LogLevel::DEBUG, event);
        }
        
        // Start a new thread to send AppendEntires RPC
        std::thread ([this, id, args = std::move(args)] {
            // Remember to properly initialize the reply object with {}. Took so long to debug
            AppendEntriesReply reply {};
            bool received = sendAppendEntries(static_cast<int32_t>(id), args, reply);
            
            
            if (received)
            {
                //std::lock_guard<std::mutex> lock(m_mu);
                {
                    // We perform use lock_guard and small checks inside small scope. 
                    // Not likely causing ABA (i.e. leader becomes follower and becomes leader of later term again in between our check)
                    std::lock_guard<std::mutex> lock(m_mu);
                    if (m_dead.load() || m_state != State::LEADER) 
                        return;

                    // Remember first thing is to check if reponse term is greater than current term to step down from leader
                    // Do not forget to reset timer when step down to follower
                    if (reply.term > m_currentTerm)
                    {
                        m_currentTerm = reply.term;
                        m_votedFor = -1;
                        m_state = State::FOLLOWER;
                        m_lastHeartbeat = std::chrono::steady_clock::now();
                        m_electionTimeout = std::chrono::milliseconds(generateTimeout());

                        LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to FOLLOWER");
                        m_logger->log(LogLevel::INFO, event);
                        m_cv.notify_all();
                        return;
                    }

                }


                if (reply.success)
                {
                    {   
                        // Remmember to do state check
                        // Otherwise, When a leader steps down to FOLLOWER, these old detached threads might keep running and continue to read/write m_nextindex and m_matchIndex
                        // Meanwhile the new leader calls m_nextindex.assign(...) and m_matchIndex.assign(...). The old threads then corrupt the vectors → garbage values → wrong prevLogIndex calculations → failed AppendEntries → more elections
                        std::lock_guard<std::mutex> lock(m_mu);
                        if (m_dead.load() || m_state != State::LEADER) 
                            return;

                        // Update matchIndex and nextindex for each follower. Remeber to move matchIndex by size of vector transmitted
                        // Note that AppendEntries RPC can arrive out of order (e.g. RPC2 arrive before RPC1) which might have updated m_matchIndex in the mean time
                        // Distinguish Heartbeat from RPC with valid log. We do not need to update matchIndex and nextIndex when sending heartbeat
                        if (!args.entries.empty())
                        {
                            uint64_t lastSent = args.preLogIndex + args.entries.size();
                            m_matchIndex[id] = std::max(m_matchIndex[id], lastSent);
                            m_nextindex[id] = m_matchIndex[id] + 1;
                        }   
                    }

                    // Update CommitIndex
                    updateLeaderCommitIndex();

                    // If commitIndex > lastApplied: increment lastApplied, apply log[lastApplied] to state machine
                    {
                        std::lock_guard<std::mutex> lock(m_mu);
                        if (m_dead.load() || m_state != State::LEADER) 
                            return;

                        while (m_commitIndex > m_lastApplied)
                        {
                            m_lastApplied++;
                            m_applyChannel->push(ApplyMsg {true, m_lastApplied, m_logs[m_lastApplied]->command});

                            LogEvent event(LogEvent::Type::APPLY, m_id, m_currentTerm, "Apply log with index: " + std::to_string(m_lastApplied) + " and entry: " + m_logs[m_lastApplied]->command);
                            m_logger->log(LogLevel::INFO, event);
                        }
                    }
                    
                    
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    if (m_dead.load() || m_state != State::LEADER) 
                        return;

                    // If AppendEntries fails because of log inconsistency: decrement nextIndex and retry
                    if (reply.conflictTerm != -1)
                    {
                        uint64_t newNextIndex = 0;
                        bool found = false;
                        for (uint64_t i = static_cast<uint64_t>(m_logs.size()) - 1; i > 0; i--)
                        {
                            
                            if (m_logs[i-1]->term == reply.conflictTerm)
                            {
                                newNextIndex = i;
                                found = true;
                                break;
                            }
                        }
                        if (found)
                        {
                            m_nextindex[id] = newNextIndex;
                        }
                            
                        else  
                            m_nextindex[id] = reply.conflictIndex;                            
                    }
                    else 
                    {
                        m_nextindex[id] = reply.conflictIndex; 
                    }
                }
                        
            }
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
        return;
    }
    else
    {
        // Remember first thing is to check if reponse term is greater than current term to step down from leader
        // Read the paper carefully, you do not need to be sure this AppendEntries rpc is from leader before converting to follower. Term > m_currentTerm is all you need
        // We only need consistency check for resetting election timer
        if (m_currentTerm < args.term)
        {
            m_currentTerm = args.term;
            m_votedFor = -1;
            m_state = State::FOLLOWER;
            // According to paper. we should pass consistency check to confirm this AppendEntries RPC is from current leader before reset election timer
            // However, the strict implementation is causing followers to start election a lot more easily, causing repeated splits vots and cannot converge to one leader
            // Notice we should reset election timer here. Otherwise, we will immediate convert back to CANDIDATE once  we turn into FOLLOWER since timer was not reset
            reply.success = true;
            m_lastHeartbeat = std::chrono::steady_clock::now();
            m_electionTimeout = std::chrono::milliseconds(generateTimeout());
            
            LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to FOLLOWER");
            m_logger->log(LogLevel::INFO, event);
        }

        // Consistency check to verify AppendEntries RPC comes from current leader. Need to check if preLogIndex out of range (e.g. when follower has very few logs)
        if (args.preLogIndex >= m_logs.size() ||  m_logs[args.preLogIndex]->term != args.preLogTerm)
        {
            if (args.preLogIndex >= m_logs.size())
            {
                reply.conflictIndex = m_logs.size();
                reply.conflictTerm = -1;
            }
            else
            {
                reply.conflictTerm = m_logs[args.preLogIndex]->term;
                reply.conflictIndex = args.preLogIndex;
                while (reply.conflictIndex > 0 && m_logs[reply.conflictIndex-1]->term == reply.conflictTerm)
                {
                    reply.conflictIndex--;
                }
            }
            reply.success = false;
            return;
        }
        else
        {     
            //Passed consistency check we can confirm this AppendEntries RPC is from current leader. Reset election timer since 
            reply.success = true;
            m_lastHeartbeat = std::chrono::steady_clock::now();
            m_electionTimeout = std::chrono::milliseconds(generateTimeout());

            // If an existing entry conflicts with a new one (same index but different terms), delete the existing entry and all that follow it
            size_t adj { args.preLogIndex + 1 };
            size_t mark { 0 };
            for (size_t i{0}; i < args.entries.size(); i++) 
            {
                // Compare follower’s entry term vs leader’s entry term at this index
                if (i + adj < m_logs.size() && m_logs[i + adj]->term != args.entries[i].term) 
                {
                    m_logs.resize(i + adj); // truncate conflicting suffix
                    mark = i;
                    break;
                }
            }

            for (size_t i=mark; i < args.entries.size(); i++)
            {
                m_logs.push_back(std::make_shared<LogEntry>(args.entries[i].command, args.entries[i].term));
                LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Append log entry with command:" + args.entries[i].command + " and term:" + std::to_string(args.term));
                m_logger->log(LogLevel::INFO, event);   
            }
            
            // if leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
            // I am  guessing args.preLogIndex + args.entries.size() works for index of last new entry
            // However, most other implementation use m_logs.size()-1. We may include uncommited entries but we alwasy have m_commitIndex as minimum 
            if (args.leaderCommit > m_commitIndex)
                m_commitIndex = std::min(args.leaderCommit, m_logs.size()-1);
                
            
            // If commitIndex > lastApplied: increment lastApplied, apply log[lastApplied] to state machine
            while (m_commitIndex > m_lastApplied)
            {
                m_lastApplied++;
                m_applyChannel->push(ApplyMsg {true, m_lastApplied, m_logs[m_lastApplied]->command});

                LogEvent event(LogEvent::Type::APPLY, m_id, m_currentTerm, "Apply log with index:" + std::to_string(m_lastApplied) + " and entry:" + m_logs[m_lastApplied]->command);
                m_logger->log(LogLevel::INFO, event);
            }
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

        return;
    }
    else
    {
        if (m_currentTerm < args.term)
        {
            m_currentTerm = args.term;
            m_votedFor = -1;
            m_state = State::FOLLOWER;
            // Notice we should reset election timer here. Otherwise, we will immediate convert back to CANDIDATE once  we turn into FOLLOWER since timer was not reset
            m_lastHeartbeat = std::chrono::steady_clock::now();
            m_electionTimeout = std::chrono::milliseconds(generateTimeout());

            LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to FOLLOWER");
            m_logger->log(LogLevel::INFO, event);
        }
        
        bool logsUpToDate = (args.lastLogTerm > m_logs.back()->term) || 
                            (args.lastLogTerm == m_logs.back()->term && args.lastLogIndex >= static_cast<uint64_t>(m_logs.size()-1));
        // Forgot logUpToDate need && condition
        if ((m_votedFor == -1 || m_votedFor == args.candidateId) && logsUpToDate)
        {
            reply.voteGranted = true;
            m_votedFor = args.candidateId;
            m_lastHeartbeat = std::chrono::steady_clock::now();
            m_electionTimeout = std::chrono::milliseconds(generateTimeout());

            LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Voted for " + std::to_string(m_votedFor));
            m_logger->log(LogLevel::INFO, event);
        }
    }        
}

std::tuple<int, int, bool> Raft::start(const std::string& command)
{
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_state != State::LEADER)
        {
            // If server is not the leader, returns false
            return std::tuple<int, int, bool>{-1, -1, false};
        }   
        else
        {
            // Leader appends the command to its log as a new entry
            LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Appended new log entry with command: " + command);
            m_logger->log(LogLevel::INFO, event);
            m_logs.emplace_back(std::make_shared<LogEntry>(command, m_currentTerm));
        }
    }  

    m_cv.notify_all();
    return std::tuple<int, int, bool>{m_logs.size()-1, m_currentTerm, true};
}


void Raft::updateLeaderCommitIndex() 
{
    std::lock_guard<std::mutex> lock(m_mu);  
    if (m_dead.load() || m_state != State::LEADER) 
        return;

    // Scan backwards from the end of the log
    for (int N = static_cast<int>(m_logs.size()) - 1; N > m_commitIndex; --N) 
    {
        int replicated = 1; // leader itself counts
        for (size_t i = 0; i < m_peers.size(); i++) 
        {
            if (i == m_id) 
                continue;
            if (m_matchIndex[i] >= N) 
            {
                replicated++;
            }
        }

        // Majority replicated AND entry from current term
        if (replicated > static_cast<int>(m_peers.size() / 2) && m_logs[N]->term == m_currentTerm) 
        {
            m_commitIndex = N;
            LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Updated CommitIndex to: " + std::to_string(m_commitIndex));
            m_logger->log(LogLevel::INFO, event);
            break; // commitIndex only moves forward once per call
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
    m_cv.notify_all();
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to LEADER");
    m_logger->log(LogLevel::INFO, event);

}


void Raft::promoteToCandidate()
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_state = Raft::State::CANDIDATE;
    m_cv.notify_all();
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to CANDIDATE");
    m_logger->log(LogLevel::INFO, event);
}

int Raft::generateTimeout()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(300, 500);
    return dist(gen);
}



