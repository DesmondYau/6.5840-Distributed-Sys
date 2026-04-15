
#pragma once
#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

class Persister;
class Endpoint;
class ApplyChannel;



class Raft
{
public:
    struct AppendEntriesArgs
    {
        uint32_t Term;
        uint32_t LeaderId;
        uint64_t PreLogIndex;
        uint32_t PreLogTerm;
        std::string Entries;
        uint64_t LeaderCommit;

    };
    
    struct AppendEntriesReply
    {
        uint32_t Term;
        bool Success;
    };

    struct RequestVoteArgs
    {
        uint32_t Term;
        uint32_t CandidateId;
        uint64_t LastLogIndex;
        uint32_t LastLogTerm;
    };

    struct RequestVoteReply
    {
        uint32_t Term;
        bool VoteGranted;
    };

    enum class State
    {
        LEADER,
        CANDIDATE,
        FOLLOWER
    };

    Raft(const std::vector<std::shared_ptr<Endpoint>>& m_peers, uint32_t id, std::shared_ptr<Persister> persister, std::shared_ptr<ApplyChannel>);

    void appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply);

    void requestVote(const RequestVoteArgs& args, RequestVoteReply& reply);

    void kill();

    std::pair<uint32_t, bool> getState() const;


private:
    uint32_t m_id;
    uint32_t m_currentTerm { 0 };
    uint32_t m_votedFor { UINT32_MAX };
    uint64_t m_commitIndex { 0 };
    uint64_t m_lastApplied { 0 };
    std::vector<uint64_t> m_nextindex;
    std::vector<uint64_t> m_matchIndex;
    std::vector<std::shared_ptr<Endpoint>> m_peers;             // Vector of RPC endpoint of all peers in the network
    std::shared_ptr<Persister> m_persister;                     // Persister
    std::shared_ptr<ApplyChannel> m_applyChannel;               // ApplyChannel tfor sending ApplyMsg for each newly committed log entry   
    State state { State::FOLLOWER };                            // Leader, Candidate, Follower

};

