#include <random>
#include <chrono>
#include <tuple>
#include "helper.hpp"
#include "raft.hpp"
#include "config.hpp"
#include "persister.hpp"
#include "logger.hpp"
#include "./rpc/endpoint.hpp"
#include "../include/json.hpp"



Raft::Raft(const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers, int32_t id, std::shared_ptr<Persister> persister, 
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
    , m_nextindex(peers.size(), 0) 
    , m_matchIndex(peers.size(), 0)
    , m_threadPool { m_peers.size() * 2 }
{
    {
        // Initialize last valid heartbeat time to now
        std::lock_guard<std::mutex> lock(m_mu);
        m_lastHeartbeat = std::chrono::steady_clock::now();
        m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());
    
        // Read and restore state from persister when initialized. Also insert dummy log entry
        helperReadPersist();    
    }

    // Start raft thread
    m_raftThread = std::thread(&Raft::startRaft, this);
}

Raft::~Raft()
{
    // 1. Shut down the thread pool (Wait for all workers to join/exit)
    m_threadPool.shutdown();

    // 2. Signal shutdown
    {
        std::lock_guard<std::mutex> lock(m_mu);   
        m_dead.store(true);
    }

    // 3. Join the main raft loop thread
    m_cv.notify_all();   // wake up the main raft thread if it is waiting

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
        /*
        static auto last_log_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - last_log_time > std::chrono::milliseconds(500)) 
        {
            m_logger->log(LogLevel::DEBUG, LogEvent(LogEvent::Type::HEARTBEAT, m_id, m_currentTerm, "Loop Pulse (Active)"));
            last_log_time = now;
        }
        */

        if (m_state == Raft::State::FOLLOWER)
        {
            // Use wait_for instead of wait_until deadline to allow more checking
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return m_dead.load() || (std::chrono::steady_clock::now() > m_lastHeartbeat + m_electionTimeout);
            });

            if (m_dead.load())
                return;
            else if (std::chrono::steady_clock::now() > m_lastHeartbeat + m_electionTimeout)
            {
                helperPromoteToCandidate();

                lock.unlock();
                startElection();
                lock.lock();
            }
        }
        else if (m_state == Raft::State::CANDIDATE)
        {
            // Use wait_for instead of wait_until deadline to allow more checking
            m_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return (m_dead.load() || m_state != State::CANDIDATE || m_votesGranted > m_peers.size()/2);
            });

            if (m_dead.load())
                return;

            if (m_state == State::FOLLOWER)
                continue; 
            else if (m_votesGranted > m_peers.size()/2)
            {
                helperPromoteToLeader();

                // Initialize nextIndex vector and matchIndex vector when first becoming leader. Remember to account for snapshot 
                auto lastLogIndex = m_lastIncludedIndex + static_cast<uint64_t>(m_logs.size()-1);
                std::fill(m_nextindex.begin(), m_nextindex.end(), lastLogIndex + 1);
                std::fill(m_matchIndex.begin(), m_matchIndex.end(), 0);
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
            // Wait for next heartbeat interval, but allow immediate wake-up via notify_all()
            m_cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return m_dead.load() || m_state != State::LEADER || m_newLogArrive;
            });

            // Reset the flag immediately after waking up
            m_newLogArrive = false;

            if (m_dead.load() || m_state != State::LEADER)
                continue;

            lock.unlock();
            broadcastAppendEntries();
            lock.lock();
        }
        else
        {
            LogEvent event(LogEvent::Type::ERROR, m_id, m_currentTerm, "Invalid State!");
            m_logger->logRaft(LogLevel::ERROR, event);
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
        m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());

        LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Starting Election with term " + std::to_string(m_currentTerm));
        m_logger->logRaft(LogLevel::INFO, event);

        helperPersist();
    }
    
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

            uint64_t lastLogIndex = static_cast<uint64_t>(m_logs.size() - 1) + m_lastIncludedIndex;
            uint32_t lastLogTerm  = m_logs.back()->term;
            args = { m_currentTerm, m_id, lastLogIndex, lastLogTerm };
        }

        m_threadPool.enqueue([this, id, args] {
        //std::thread ([this, id, args] {
            RequestVoteReply reply;
            bool received = sendRequestVoteRPC(static_cast<int32_t>(id), args, reply);
            if (received)
            {
    
                std::lock_guard<std::mutex> lock(m_mu);
                if (m_dead.load() || m_state != State::CANDIDATE) 
                    return;

                if (reply.voteGranted)
                {
                    m_votesGranted++;
                    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Received true vote from server " + std::to_string(id));
                    m_logger->logRaft(LogLevel::INFO, event);  
                    
                    m_cv.notify_all();          // ← wake up main thread to check if there is enough vote
                }
                else if (!reply.voteGranted && reply.term > m_currentTerm)
                {
                    // Notice we should reset election timer here. Otherwise, we will immediate convert back to CANDIDATE once  we turn into FOLLOWER since timer was not reset
                    helperStepDownToFollower(reply.term);
                }
            }
        //}).detach(); 
        });       
    }    
}


void Raft::broadcastAppendEntries()
{
    for (size_t id{0}; id<m_peers.size(); id++)
    {
        if (static_cast<int32_t>(id) == m_id)
            continue;
        
        AppendEntriesArgs args;
        uint64_t lastAbsIndexSent {0};
        std::vector<LogEntry> logEntries;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_dead.load() || m_state != State::LEADER)
                return;
            
            if (helperNeedsSnapshot(id))
            {
                helperTriggerInstallSnapshot(id);
                continue;
            }

            
            // 1. Check if the object is already destroyed/dead
            if (m_dead.load()) {
                std::cerr << "!!! Thread still running after m_dead=true" << std::endl;
                return;
            }

            // 2. Sanity check the vector pointer
            // If this address changes unexpectedly between loops, someone is re-allocating it
            void* current_vector_addr = m_nextindex.data();
            if (m_last_vector_addr != nullptr && m_last_vector_addr != current_vector_addr) {
                // This will only trigger if THIS specific node's vector moves
                std::cerr << "!!! REAL CRITICAL: m_nextindex vector moved in memory!" << std::endl;
            }
            m_last_vector_addr = current_vector_addr;

            // 3. Check for obvious garbage
            if (m_nextindex[id] > 10000000) { // Or whatever your max possible log index is
                std::cerr << "!!! DATA CORRUPTION: Found huge nextIndex value " << m_nextindex[id] << " for server " << id << std::endl;
                return;
            }

            // DEBUG CHECK
            if (id >= m_nextindex.size()) {
                // Log the error and immediately exit this iteration
                std::cerr << "!!! OOB ACCESS: id=" << id << " size=" << m_nextindex.size() << std::endl;
                continue; 
            }
            
            // We stored the term of log entry of last inlucded index in dummy term after taking snapshot
            // If relativePrevLogIndex < 0, this means leader has clear log at that position and we need to rely on dummy log entry -> Send InstallSnapshot RPC, covered above
            uint64_t prevLogIndex = m_nextindex[id] - 1;
            auto relPrevLogIndex = helperGetRelativeIndex(prevLogIndex);
            auto relNextIndex = helperGetRelativeIndex(m_nextindex[id]);
            // std::cout << "m_nextIndex " << m_nextindex[id] << " prevLogIndex " << prevLogIndex << " m_lastIncludedIndex " << m_lastIncludedIndex << " relPrevLogIndex " << relPrevLogIndex << std::endl;
            uint32_t prevLogTerm  = m_logs[relPrevLogIndex]->term;  
            
            // We should use size of log entries to determine whether the AppendEntries RPC is a heartbeat (empty) or not (with valid log entries)
            // Need to adjust index by m_lastIncludedIndex after taking snapshot
            for (size_t i = relNextIndex; i < m_logs.size(); i++)
            {
                if (i > 0)
                {
                    logEntries.emplace_back(m_logs[i]->command, m_logs[i]->term);
                    lastAbsIndexSent = std::max(lastAbsIndexSent, i + m_lastIncludedIndex);
                }
            }
                        
            args = { m_currentTerm, m_id, prevLogIndex, prevLogTerm, logEntries, m_commitIndex };
        }
        LogEvent event {
            logEntries.empty() ? LogEvent::Type::HEARTBEAT : LogEvent::Type::REPLICATION,
            m_id,
            m_currentTerm,
            logEntries.empty() ? "Broadcasting Heartbeat to " + std::to_string(id) : "Broadcasting AppendEntries to " + std::to_string(id) + " starting:" + std::to_string(args.preLogIndex + 1) + " ending:" + std::to_string(args.preLogIndex + logEntries.size())
        };
        m_logger->logRaft(LogLevel::DEBUG, event);
        

        uint64_t termStarted;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            termStarted = m_currentTerm;                // term guard
        }
        // Start a new thread to send AppendEntires RPC
        m_threadPool.enqueue([this, id, lastAbsIndexSent, termStarted, args = std::move(args)] {
        //std::thread ([this, id, lastAbsIndexSent, termStarted, args = std::move(args)] {
            // Remember to properly initialize the reply object with {}. Took so long to debug
            AppendEntriesReply reply {};
            bool received = sendAppendEntriesRPC(static_cast<int32_t>(id), args, reply);
            
            if (received)
            {   
                //std::lock_guard<std::mutex> lock(m_mu);
                {
                    // We use lock_guard and small checks inside small scope. 
                    // Not likely causing ABA (i.e. leader becomes follower and becomes leader of later term again in between our check)
            
                    std::lock_guard<std::mutex> lock(m_mu);
                    if (m_dead.load() || m_state != State::LEADER || m_currentTerm != termStarted) 
                        return;
                
                    // Remember first thing is to check if reponse term is greater than current term to step down from leader
                    // Do not forget to reset timer when step down to follower
                    if (reply.term > m_currentTerm)
                    {
                        helperStepDownToFollower(reply.term);
                        return;
                    }
                }

                if (reply.success)
                {
                    std::lock_guard<std::mutex> lock(m_mu); 

                    // Remmember to do state check
                    // Otherwise, When a leader steps down to FOLLOWER, these old detached threads might keep running and continue to read/write m_nextindex and m_matchIndex
                    // Meanwhile the new leader calls m_nextindex.assign(...) and m_matchIndex.assign(...). The old threads then corrupt the vectors → garbage values → wrong prevLogIndex calculations → failed AppendEntries → more elections
                    if (m_dead.load() || m_state != State::LEADER) 
                        return;

                    LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Node " + std::to_string(id) + " ACKED index " + std::to_string(args.preLogIndex + args.entries.size()));
                    m_logger->logRaft(LogLevel::INFO, event);

                    // Update matchIndex and nextindex for each follower. Remeber to move matchIndex by size of vector transmitted
                    // Note that AppendEntries RPC can arrive out of order (e.g. RPC2 arrive before RPC1) which might have updated m_matchIndex in the mean time
                    // We should always update match and next index of follower even for heartbeat. This servers as a check to see if follower have all updated logs as leader
                    uint64_t matchIndex = args.preLogIndex + args.entries.size();
                    m_matchIndex[id] = std::max(m_matchIndex[id], matchIndex);
                    m_nextindex[id] = m_matchIndex[id] + 1;
                    
                    // Update CommitIndex
                    helperUpdateLeaderCommitIndex();

                    // If commitIndex > lastApplied: increment lastApplied, apply log[lastApplied] to state machine
                    while (m_commitIndex > m_lastApplied)
                    {
                        m_lastApplied++;
                        m_applyChannel->push(ApplyMsg {true, m_lastApplied,m_logs[m_lastApplied - m_lastIncludedIndex]->term, m_logs[m_lastApplied - m_lastIncludedIndex]->command, 
                                                    false, {}, 0, 0});

                        LogEvent event(LogEvent::Type::APPLY, m_id, m_currentTerm, "Apply log with index: " + std::to_string(m_lastApplied) + " and entry: " + m_logs[m_lastApplied - m_lastIncludedIndex]->command);
                        m_logger->logRaft(LogLevel::INFO, event);
                    }
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_mu);
                    LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Node " + std::to_string(id) + " REJECT index " + std::to_string(args.preLogIndex + 1));
                    m_logger->logRaft(LogLevel::INFO, event);

                    if (m_dead.load() || m_state != State::LEADER) 
                        return;

                    // If AppendEntries fails because of log inconsistency: decrement nextIndex and retry
                    // Must update nextIndex aggressively and jump to conflicing index directly (or decreases by 1 if the follower didn't provide one))
                    if (reply.conflictIndex > 0) 
                    {
                        m_nextindex[id] = reply.conflictIndex;
                    } 
                    else if (m_nextindex[id] > m_lastIncludedIndex + 1) 
                    {
                        m_nextindex[id]--;                  
                    }

                    // When raft server frequenlty crash and end up very short logs compared to leader, we should be able to trigger install snapshot after reply fails
                    if (m_nextindex[id] <= m_lastIncludedIndex)
                    {
                        helperTriggerInstallSnapshot(id);
                        return;
                    }

                    m_cv.notify_all();
                }
                        
            }
            else
            {
                // THIS IS THE MISSING LOG
                LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "RPC FAILED/TIMEOUT to Node " + std::to_string(id));
                m_logger->logRaft(LogLevel::ERROR, event);
                return;
            }
        //}).detach(); 
        });
    }
}


/*
    Public methods that are called by other raft node
    Using lock_guard to perform locking at all times
*/
void Raft::appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    {
        m_logger->logRaft(LogLevel::INFO, LogEvent(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Attempting to acquire lock for AppendEntries from " + std::to_string(args.leaderId)));
        std::lock_guard<std::mutex> lock(m_mu);
        LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "RECEIVED appendEntries from " + std::to_string(args.leaderId));
        m_logger->logRaft(LogLevel::INFO, event); 

        reply.term = m_currentTerm;
        reply.success = false;

        // Terms in AppendEntries RPC < current term of raft
        if (m_currentTerm > args.term)
        {
            return;
        }

        // Remember first thing is to check if reponse term is greater than current term to step down from leader
        // Read the paper carefully, you do not need to be sure this AppendEntries rpc is from leader before converting to follower. Term > m_currentTerm is all you need
        if (m_currentTerm < args.term)
        {
            // According to paper. we should pass consistency check to confirm this AppendEntries RPC is from current leader before reset election timer
            // However, the strict implementation is causing followers to start election a lot more easily, causing repeated splits vots and cannot converge to one leader
            // Notice we should reset election timer here. Otherwise, we will immediately convert back to CANDIDATE once we turn into FOLLOWER since timer was not reset
            helperStepDownToFollower(args.term);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        
        // Consistency check to verify AppendEntries RPC comes from current leader. 
        // If the leader is lagging or sends an outdated AppendEntries (e.g., trying to replicate an entry that you have already snapshotted), args.preLogIndex will be smaller than m_lastIncludedIndex
        if (args.preLogIndex < m_lastIncludedIndex) {
            // The leader is offering entries that have already been snapshotted.
            // Return success or handle based on your specific snapshot recovery strategy.
            reply.success = false; 
            return;
        }
        uint64_t relPreLogIndex = args.preLogIndex - m_lastIncludedIndex;
        
        // Follower has very few logs or term does not match at the same index
        if (relPreLogIndex >= m_logs.size() || m_logs[relPreLogIndex]->term != args.preLogTerm)
        {
            if (relPreLogIndex >= m_logs.size())
            {
                reply.conflictIndex = (m_logs.size() - 1) + m_lastIncludedIndex;
                reply.conflictTerm = -1;
            }
            else
            {
                reply.conflictTerm = m_logs[relPreLogIndex]->term;
                reply.conflictIndex = args.preLogIndex;
                uint64_t relConflictIndex = reply.conflictIndex - m_lastIncludedIndex;
                // find out 
                while (relConflictIndex > 0 && m_logs[relConflictIndex-1]->term == reply.conflictTerm)
                {
                    std::cout << "follower " << m_id << " stuck in while loop" << std::endl;
                    reply.conflictIndex--;
                    relConflictIndex--;
                }
            }
            reply.success = false;
            
            // Relax reset timeout criteria. Otherwise, very likely to stuck in split vote situation
            //m_lastHeartbeat = std::chrono::steady_clock::now();
            //m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());

            LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Failed log consistency check relPreLogIndex " + std::to_string(relPreLogIndex) + " m_logs.size " + std::to_string(m_logs.size()));
            m_logger->logRaft(LogLevel::INFO, event); 
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);

        //Passed consistency check we can confirm this AppendEntries RPC is from current leader. Reset election timer since 
        reply.success = true;
        m_lastHeartbeat = std::chrono::steady_clock::now();
        m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());

        // If an existing entry conflicts with a new one (same index but different terms), delete the existing entry and all that follow it
        size_t adj { args.preLogIndex + 1 - m_lastIncludedIndex };
        size_t mark { 0 };
        for (size_t i{0}; i < args.entries.size(); i++) 
        {
            if (i + adj < m_logs.size())
            {
                // Compare follower’s entry term vs leader’s entry term at this index
                if (m_logs[i + adj]->term != args.entries[i].term) 
                {
                    m_logs.resize(i + adj); // truncate conflicting suffix
                    mark = i;
                    LogEvent event(LogEvent::Type::DELETION, m_id, m_currentTerm, "Resizing logs to index:" + std::to_string(m_logs.size()-1 + m_lastIncludedIndex));
                    m_logger->logRaft(LogLevel::INFO, event); 

                    break;
                }
                // Since network is unreliable, our AppendEntries RPC reply back to leader can be dropped. We might have appeneded the log but leader does not know
                // In that case, when leader retries with the same log, we move forward so that we would not reappend the same log
                else if (m_logs[i + adj]->term == args.entries[i].term && m_logs[i + adj]->command == args.entries[i].command)
                {
                    mark++;
                }
            }
        }
                
        // Replicate logs
        for (size_t i=mark; i < args.entries.size(); i++)
        {
            m_logs.push_back(std::make_shared<LogEntry>(args.entries[i].command, args.entries[i].term));
            LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, 
                "Append log entry with command:" + m_logs.back()->command + " and term:" + std::to_string(m_logs.back()->term) + " at index " + std::to_string(m_logs.size() - 1 + m_lastIncludedIndex));
            m_logger->logRaft(LogLevel::INFO, event);   
        }

        helperPersist();
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);

        // if leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
        // leaderCommit: leaderCommit value. This is the leader telling the follower: "Everything up to this index is safely committed in the majority of the cluster."
        // lastNewEntryIndex: highest index that the follower just received and appended to its own log in this specific RPC
        if (args.leaderCommit > m_commitIndex)
        {
            uint64_t lastNewEntryIndex = args.preLogIndex + args.entries.size();
            m_commitIndex = std::min(args.leaderCommit, lastNewEntryIndex);
        }
           
                    
                
        // If commitIndex > lastApplied: increment lastApplied, apply log[lastApplied] to state machine
        while (m_commitIndex > m_lastApplied)
        {
            m_lastApplied++;
            m_applyChannel->push(ApplyMsg {true, m_lastApplied, m_logs[m_lastApplied - m_lastIncludedIndex]->term, m_logs[m_lastApplied - m_lastIncludedIndex]->command,
                                        false, {}, 0, 0});

            LogEvent event(LogEvent::Type::APPLY, m_id, m_currentTerm, "Apply log with index:" + std::to_string(m_lastApplied) + " and entry:" + m_logs[m_lastApplied - m_lastIncludedIndex]->command);
            m_logger->logRaft(LogLevel::INFO, event);
        }
    }
            
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
        m_logger->logRaft(LogLevel::DEBUG, event);

        return;
    }
    else
    {
        if (m_currentTerm < args.term)
        {
            // Notice we should reset election timer here. Otherwise, we will immediate convert back to CANDIDATE once  we turn into FOLLOWER since timer was not reset
            helperStepDownToFollower(args.term);
        }
        
        uint64_t lastLogIndex = static_cast<uint64_t>(m_logs.size()-1) + m_lastIncludedIndex;
        uint32_t lastLogTerm = m_logs.back()->term;
        bool logsUpToDate = (args.lastLogTerm > lastLogTerm) || (args.lastLogTerm == lastLogTerm && args.lastLogIndex >= lastLogIndex);
        
        // Forgot logUpToDate need && condition
        if ((m_votedFor == -1 || m_votedFor == args.candidateId) && logsUpToDate)
        {
            reply.voteGranted = true;
            m_votedFor = args.candidateId;
            m_lastHeartbeat = std::chrono::steady_clock::now();
            m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());

            LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Voted for " + std::to_string(m_votedFor));
            m_logger->logRaft(LogLevel::INFO, event);
        } 

        helperPersist();
    }        
}



void Raft::installSnapshot(const InstallSnapshotArgs& args, InstallSnapshotReply& reply)
{
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_dead.load()) 
        return;

    std::cout << "Raft: " << m_id << " installing snapshot" << std::endl;

    reply.term = m_currentTerm;

    // Reply immediately if term < currentTerm
    if (args.term < m_currentTerm)
    {
        return;
    }

    // Higher term -> step down
    if (args.term > m_currentTerm)
    {
        helperStepDownToFollower(args.term);
    }

    // If we already have a newer or equal snapshot, ignore
    if (args.lastIncludedIndex <= m_lastIncludedIndex)
    {
        return;
    }

    // Update metadata
    m_lastIncludedIndex = args.lastIncludedIndex;
    m_lastIncludedTerm = args.lastIncludedTerm;
    std::cout << "Raft: " << m_id << " lastIncludedIndex" << m_lastIncludedIndex << " lastIncludedTerm" << m_lastIncludedTerm << std::endl;

    // Discard entire log before snapshot. When we send InstallSnapshot RPC, leader has already discarded the logs to restore follower
    // Keep only the dummy entry at the beginning with lastIncludedTerm
    m_logs.clear();
    auto dummy = std::make_shared<LogEntry>();   
    dummy->command = ""; dummy->term = m_lastIncludedTerm;         
    m_logs.push_back(dummy);

    // Treat everything up to snapshot as applied
    if (m_lastApplied < m_lastIncludedIndex)
        m_lastApplied = m_lastIncludedIndex;

    // Persist current Raft state, then atomically save both state + snapshot
    helperPersist();
    auto latestRaftState = m_persister->readRaftState();
    m_persister->saveStateAndSnapshot(latestRaftState, args.data);

    // Notify test harness/config.
    m_applyChannel->push(ApplyMsg{
        false,                              // CommandValid = false
        0, 
        0,
        "",
        true,                               // SnapshotValid = true
        args.data,
        static_cast<uint64_t>(m_lastIncludedIndex),
        static_cast<uint32_t>(m_lastIncludedTerm)
    });    

    std::cout << "Raft " << m_id << ": ";
    for (auto& logEntry : m_logs) 
    { 
        std::cout << logEntry->command << " " << logEntry->term << " "; 
    }
    std::cout << std::endl;
    //LogEvent event(LogEvent::Type::SNAPSHOT, m_id, m_currentTerm, "Installed snapshot at index " + std::to_string(m_lastIncludedIndex));
    //m_logger->log(LogLevel::INFO, event);
}



void Raft::snapshot(uint64_t index, const std::string& snapshot)
{
    nlohmann::json j;
    std::lock_guard<std::mutex> lock(m_mu);

    if (index <= m_lastIncludedIndex)
        return;

    // Update m_lastIncludedIndex and m_lastIncludedTerm before clearing old log
    uint64_t relIndex = index - m_lastIncludedIndex;
    if (relIndex >= m_logs.size()) 
        return; 

    uint64_t keepFrom = index - m_lastIncludedIndex + 1;
    m_lastIncludedTerm = m_logs[relIndex]->term;
    m_lastIncludedIndex = index;
    
    // Remove old logs from persister raft state
    if (keepFrom < m_logs.size())
    {
        m_logs.erase(m_logs.begin(), m_logs.begin() + keepFrom);
    }
    else
    {
        m_logs.clear();
    }

    // Insert back dummy log entry with term equals to m_lastIncludedTerm
    auto dummy = std::make_shared<LogEntry>();   
    dummy->command = ""; dummy->term    = m_lastIncludedTerm;         
    m_logs.insert(m_logs.begin(), dummy);
    
    // Update raft state
    j["Term"] = m_currentTerm;
    j["Voted"] = m_votedFor;

    std::vector<nlohmann::json> logVector;
    for (size_t i=1; i<m_logs.size(); i++)
    {
        logVector.push_back({{"Command", m_logs[i]->command}, {"Term", m_logs[i]->term}});
    }
    j["Log"] = logVector;
    auto data = j.dump();

    // Save updated raft state and snapshot
    std::vector<uint8_t> raftState(data.begin(), data.end());
    std::vector<uint8_t> snapshotByte(snapshot.begin(), snapshot.end());
    m_persister->saveStateAndSnapshot(raftState, snapshotByte);

    LogEvent event(LogEvent::Type::SNAPSHOT, m_id, m_currentTerm, "Snapshot taken including index: " + std::to_string(m_lastIncludedIndex) + " snapshot: " + 
                std::string(snapshotByte.begin(), snapshotByte.end()) + " m_logs.size:" + std::to_string(m_logs.size()));
    m_logger->logRaft(LogLevel::INFO, event);

}


std::tuple<int, int, bool> Raft::start(const std::string& command)
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
        m_logger->logRaft(LogLevel::INFO, event);
        
        m_logs.emplace_back(std::make_shared<LogEntry>(command, m_currentTerm));
        m_newLogArrive = true;
    }

    helperPersist();
    m_cv.notify_all();
    return std::tuple<int, int, bool>{m_logs.size()-1 + m_lastIncludedIndex, m_currentTerm, true};
}



bool Raft::isLeader()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_state == State::LEADER;
}

void Raft::kill()
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_dead.store(true);
    m_cv.notify_all();
}

std::pair<uint32_t, Raft::State> Raft::getTermState()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return std::pair<uint32_t, State>{ m_currentTerm, m_state };
}

int Raft::helperGenerateTimeout()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    // time out cannot be set too frequently in unrealiable network. If there is delay/drop in message, re-election easily starts and cannot converge to a leader
    std::uniform_int_distribution<> dist(700, 900);         
    return dist(gen);
}

void Raft::helperUpdateLeaderCommitIndex() 
{
    if (m_dead.load() || m_state != State::LEADER) 
        return;

    // Safety check for empty logs. Defensive programming
    if (m_logs.empty()) return;

    // Scan backwards from the end of the logs (largest possible index). Notice we need to adjust for m_lastIncludedIndex after taking snapshot
    uint64_t lastLogIndex = m_lastIncludedIndex + m_logs.size() - 1;
    for (uint64_t N = lastLogIndex; N > m_commitIndex; --N) 
    {
        uint64_t relIndex = N - m_lastIncludedIndex;
        if (relIndex >= m_logs.size() || relIndex < 0)
            continue;


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

        // Criteria to update commit index: Majority replicated AND entry from current term
        if (replicated > static_cast<int>(m_peers.size() / 2) && m_logs[relIndex]->term == m_currentTerm) 
        {
            m_commitIndex = N;
            LogEvent event(LogEvent::Type::REPLICATION, m_id, m_currentTerm, "Updated CommitIndex to: " + std::to_string(m_commitIndex));
            m_logger->logRaft(LogLevel::INFO, event);
            break; // commitIndex only moves forward once per call
        }
    }
}



void Raft::helperPromoteToLeader()
{
    m_state = State::LEADER;
    m_cv.notify_all();
    
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to LEADER");
    m_logger->logRaft(LogLevel::INFO, event);

}


void Raft::helperPromoteToCandidate()
{
    m_state = Raft::State::CANDIDATE;
    m_cv.notify_all();
    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to CANDIDATE");
    m_logger->logRaft(LogLevel::INFO, event);
}

void Raft::helperStepDownToFollower(uint32_t term)
{
    
    m_currentTerm = term;
    m_votedFor = -1;
    m_state = State::FOLLOWER;
    m_lastHeartbeat = std::chrono::steady_clock::now();
    m_electionTimeout = std::chrono::milliseconds(helperGenerateTimeout());

    LogEvent event(LogEvent::Type::STATECHANGE, m_id, m_currentTerm, "State change to FOLLOWER");
    m_logger->logRaft(LogLevel::INFO, event);
                        
    m_cv.notify_all();          // wake up main thread on state change
    
    helperPersist();
}

void Raft::helperPersist()
{
    nlohmann::json j;
    {
        j["Term"] = m_currentTerm;
        j["VotedFor"] = m_votedFor;
        
        std::vector<nlohmann::json> logVector;
        for (size_t i{1}; i<m_logs.size(); i++)
        {
            logVector.push_back({{"Command", m_logs[i]->command}, {"Term", m_logs[i]->term}});
        }
        j["Log"] = logVector;
    }

    std::string data = j.dump();
    std::vector<uint8_t> state(data.begin(), data.end());

    m_persister->saveRaftState(state);
}

void Raft::helperReadPersist()
{   
    
    std::vector<uint8_t> raftStateBytes = m_persister->readRaftState();
    std::vector<uint8_t> snapshotBytes = m_persister->readSnapshot();
    
    if (raftStateBytes.empty())
    {
        // Fresh start - no previous persisted state. Push in dummy log entry with term 0
        m_lastIncludedIndex = 0;
        m_lastIncludedTerm = 0;
        m_logs.clear();
        auto dummy = std::make_shared<LogEntry>();   
        dummy->command = ""; dummy->term = 0;        
        m_logs.push_back(dummy);
        

        LogEvent event(LogEvent::Type::PERSISTER, m_id, m_currentTerm, "No previous state from persister");
        m_logger->logRaft(LogLevel::INFO, event);
        return;
    }
    else
    {
        std::string raftStateStr(raftStateBytes.begin(), raftStateBytes.end());
        nlohmann::json j = nlohmann::json::parse(raftStateStr);
        
        m_currentTerm = j.value("Term", 0u);
        m_votedFor = j.value("VotedFor", -1);
        m_lastIncludedIndex = 0;
        m_lastIncludedTerm  = 0;

        if (!snapshotBytes.empty())
        {
            try
            {
                std::string snapStr(snapshotBytes.begin(), snapshotBytes.end());
                nlohmann::json snapJ = nlohmann::json::parse(snapStr);
                m_lastIncludedIndex = snapJ.value("LastIncludedIndex", 0ull);
                m_lastIncludedTerm  = snapJ.value("LastIncludedTerm", 0u);
            }
            catch(...)
            {
                LogEvent event(LogEvent::Type::ERROR, m_id, m_currentTerm, "Failed to parse snapshot");
                m_logger->logRaft(LogLevel::INFO, event);
            }            
        }

        m_lastApplied = m_lastIncludedIndex;
        

        m_logs.clear();
        auto dummy = std::make_shared<LogEntry>();  
        dummy->command = ""; dummy->term = m_lastIncludedTerm;       
        m_logs.push_back(dummy);

        for (const auto& jsonEntry : j["Log"])                                      // Restore raft m_logs with logs not included in snapshot (which are stored in raftState)
        {
            auto command = jsonEntry.value("Command", "");
            auto term = jsonEntry.value("Term", 0u);
            m_logs.push_back(std::make_shared<LogEntry>(command, term));
        }            
    
        LogEvent event(LogEvent::Type::PERSISTER, m_id, m_currentTerm, "Restored state from persister lastIncludedIndex: " + std::to_string(m_lastIncludedIndex) + 
                    " lastIncludedTerm: " + std::to_string(m_logs[0]->term) + " last command:" + m_logs.back()->command);
        m_logger->logRaft(LogLevel::INFO, event);
        for (auto& element : m_logs )
        {
            std::cout << element->command << " ";
        }
        std::cout << std::endl;
    }   
}

bool Raft::helperNeedsSnapshot (size_t peerId)
{
    // m_nextindex <= m_lastIncludedIndex, meaning that the logs to recover the follower is in snapshot -> send InstallSnapshot RPC
    return m_nextindex[peerId] <= m_lastIncludedIndex;
}

void Raft::helperTriggerInstallSnapshot(size_t id)
{
    InstallSnapshotArgs args;
    {
        if (m_dead.load() || m_state != State::LEADER)
            return;
        

        args.term              = m_currentTerm;
        args.leaderId          = m_id;
        args.lastIncludedIndex = m_lastIncludedIndex;
        args.lastIncludedTerm  = m_lastIncludedTerm;
        args.offset            = 0;                    // Lab 3D sends full snapshot in one chunk
        args.data              = m_persister->readSnapshot();
        args.done              = true;
    }

    m_threadPool.enqueue([this, id, args]() {
    //std::thread([this, id, args]() mutable {
        InstallSnapshotReply reply;
        bool received = sendInstallSnapshotRPC(static_cast<int32_t>(id), args, reply);

        if (received)
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_dead.load() || m_state != State::LEADER)
                return;

            if (reply.term > m_currentTerm)
            {
                helperStepDownToFollower(reply.term);
            }
            
            // SUCCESS: follower has installed the snapshot
            // Advance nextIndex and matchIndex so leader knows it is caught up
            m_matchIndex[id] = m_lastIncludedIndex;
            m_nextindex[id]  = m_lastIncludedIndex + 1;

            LogEvent event(LogEvent::Type::SNAPSHOT, m_id, m_currentTerm, 
                "Follower " + std::to_string(id) + " installed snapshot, nextIndex: " + std::to_string(m_nextindex[id]) + " args.lastIncludedTerm " + std::to_string(args.lastIncludedTerm));
            m_logger->logRaft(LogLevel::INFO, event);
        }
    //}).detach();
    });
}



uint64_t Raft::helperGetRelativeIndex(uint64_t absoluteIndex) const
{
    return absoluteIndex - m_lastIncludedIndex;
}

bool Raft::sendRequestVoteRPC(int32_t id, const RequestVoteArgs& args, RequestVoteReply& reply)
{
    LogEvent event(LogEvent::Type::ELECTION, m_id, m_currentTerm, "Sending request vote to server " + std::to_string(id));
    m_logger->logRaft(LogLevel::DEBUG, event);

    return m_peers[id]->call("Raft.RequestVote", args, reply);
}

bool Raft::sendAppendEntriesRPC(int32_t id, const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    return m_peers[id]->call("Raft.AppendEntries", args, reply);
}

bool Raft::sendInstallSnapshotRPC(int32_t id, const InstallSnapshotArgs& args, InstallSnapshotReply& reply)
{
    return m_peers[id]->call("Raft.InstallSnapshot", args, reply);
}
