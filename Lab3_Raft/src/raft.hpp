
#pragma once
#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>

class Persister;
class Endpoint;
class ApplyChannel;
class Logger;

class Raft
{
public:
    struct AppendEntriesArgs
    {
        uint32_t term;
        int32_t leaderId;
        uint64_t preLogIndex;
        uint32_t preLogTerm;
        std::string entries;
        uint64_t leaderCommit;

    };
    
    struct AppendEntriesReply
    {
        uint32_t term;
        bool success;
    };

    struct RequestVoteArgs
    {
        uint32_t term;
        int32_t candidateId;
        uint64_t lastLogIndex;
        uint32_t lastLogTerm;
    };

    struct RequestVoteReply
    {
        uint32_t term;
        bool voteGranted;
    };


    struct LogEntry
    {
        std::string command;
        uint32_t term;
    };

    enum class State
    {
        LEADER,
        CANDIDATE,
        FOLLOWER
    };


    Raft(const std::vector<std::shared_ptr<Endpoint>>& peers, int32_t id, std::shared_ptr<Persister> persister, 
        std::shared_ptr<ApplyChannel>applyChannel, std::shared_ptr<Logger> logger);
    ~Raft();

    void startRaft();
    void appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply);
    void requestVote(const RequestVoteArgs& args, RequestVoteReply& reply);
    void startElection();
    void broadcastAppendEntries(const std::string& logEntry);
    void kill();
    std::pair<uint32_t, State> getTermState();
   


private:
    void promoteToLeader();
    void promoteToCandidate();
    bool sendRequestVote(int32_t id, const RequestVoteArgs& args, RequestVoteReply& reply);
    bool sendAppendEntries(int32_t id, const AppendEntriesArgs& args, AppendEntriesReply& reply);

    int32_t m_id;                                             
    int32_t m_votedFor { -1 };
    int32_t m_votesGranted;
    uint32_t m_currentTerm { 0 };  
    uint64_t m_commitIndex { 0 };
    uint64_t m_lastApplied { 0 };
    State m_state { State::FOLLOWER };                                           // Leader, Candidate, Follower
    std::vector<std::shared_ptr<LogEntry>> m_logs;
    std::vector<uint64_t> m_nextindex;
    std::vector<uint64_t> m_matchIndex;
    std::vector<std::shared_ptr<Endpoint>> m_peers;                              // Vector of RPC endpoint of all peers in the network
    std::shared_ptr<Persister> m_persister;                                      // Persister
    std::shared_ptr<ApplyChannel> m_applyChannel;                                // ApplyChannel tfor sending ApplyMsg for each newly committed log entry   
    std::shared_ptr<Logger> m_logger;
    std::chrono::steady_clock::time_point m_lastHeartbeat;                       // Timepoint where we last receive a valid AppendEntries RPC or granting vote
    std::chrono::milliseconds m_electionTimeout;                                 // Timeout duration in milliseconds
    std::thread m_raftThread;
    

    std::atomic<bool> m_dead { false };                                          // Track if raft instance is dead. Atomic since the variable does not coordinate with other variables/state
    std::mutex m_mu;
    std::condition_variable m_cv;

};

