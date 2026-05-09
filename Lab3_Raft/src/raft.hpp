
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
#include "threadpool.hpp"


namespace labrpc {
    class Endpoint;
}

class Persister;
class ApplyChannel;
class Logger;

class Raft
{
public:

    struct LogEntry
    {
        std::string command;
        uint32_t term;
    };

    struct AppendEntriesArgs
    {
        uint32_t term;
        int32_t leaderId;
        uint64_t preLogIndex;
        uint32_t preLogTerm;
        std::vector<LogEntry> entries;
        uint64_t leaderCommit;

    };
    
    struct AppendEntriesReply
    {
        uint32_t term;
        bool success;
        uint64_t conflictIndex;
        uint32_t conflictTerm;
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

    struct InstallSnapshotArgs
    {
        uint32_t term;
        int32_t leaderId;
        uint64_t lastIncludedIndex;
        uint32_t lastIncludedTerm;
        int offset;
        std::vector<uint8_t> data;
        bool done;
    };

    struct InstallSnapshotReply
    {
        uint32_t term;
    };

    enum class State
    {
        LEADER,
        CANDIDATE,
        FOLLOWER
    };


    Raft(const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers, int32_t id, std::shared_ptr<Persister> persister, 
        std::shared_ptr<ApplyChannel>applyChannel, std::shared_ptr<Logger> logger);
    ~Raft();

    void startRaft();
    void appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply);
    void requestVote(const RequestVoteArgs& args, RequestVoteReply& reply);
    void installSnapshot(const InstallSnapshotArgs& args, InstallSnapshotReply& reply);
    void broadcastAppendEntries();
    void startElection();
    bool isLeader();
    void snapshot(uint64_t lastIncludedIndex, const std::string& snapshot);
    void kill();
    
    std::tuple<int, int, bool> start(const std::string& command);
    std::pair<uint32_t, State> getTermState();
   


private:
    int helperGenerateTimeout();
    void helperUpdateLeaderCommitIndex();
    void helperPromoteToLeader();
    void helperPromoteToCandidate();
    void helperStepDownToFollower(uint32_t term);
    void helperPersist();
    void helperReadPersist();
    bool helperNeedsSnapshot (size_t peerId);
    void helperTriggerInstallSnapshot(size_t id);
    uint64_t helperGetRelativeIndex(uint64_t absoluteIndex) const;
    std::pair<AppendEntriesArgs, uint64_t> helperBuildAppendEntriesArgs(size_t peerId);
    

    bool sendRequestVoteRPC(int32_t id, const RequestVoteArgs& args, RequestVoteReply& reply);
    bool sendAppendEntriesRPC(int32_t id, const AppendEntriesArgs& args, AppendEntriesReply& reply);
    bool sendInstallSnapshotRPC(int32_t id, const InstallSnapshotArgs& args, InstallSnapshotReply& reply);

    int32_t m_id;                                             
    int32_t m_votedFor { -1 };
    int32_t m_votesGranted { 0 };
    uint32_t m_currentTerm { 0 };  
    uint64_t m_commitIndex { 0 };
    uint64_t m_lastApplied { 0 };
    uint64_t m_lastIncludedIndex { 0 };
    uint32_t m_lastIncludedTerm { 0 };
    State m_state { State::FOLLOWER };                                              // Leader, Candidate, Follower
    std::vector<std::shared_ptr<LogEntry>> m_logs {};
    std::vector<uint64_t> m_nextindex {};
    std::vector<uint64_t> m_matchIndex {};
    std::vector<std::shared_ptr<labrpc::Endpoint>> m_peers {};                      // Vector of RPC endpoint of all peers in the network
    std::shared_ptr<Persister> m_persister {};                                      // Persister object
    std::shared_ptr<ApplyChannel> m_applyChannel {};                                // ApplyChannel for sending ApplyMsg for each newly committed log entry   
    std::shared_ptr<Logger> m_logger {};
    std::chrono::steady_clock::time_point m_lastHeartbeat{} ;                       // Timepoint where we last receive a valid AppendEntries RPC or granting vote
    std::chrono::milliseconds m_electionTimeout {};                                 // Timeout duration in milliseconds
    ThreadPool m_threadPool;
    std::thread m_raftThread {};
    void* m_last_vector_addr = nullptr;
    bool m_newLogArrive {false};
    

    std::atomic<bool> m_dead { false };                                          // Track if raft instance is dead. Atomic since the variable does not coordinate with other variables/state
    std::mutex m_mu;
    std::condition_variable m_cv;

};

