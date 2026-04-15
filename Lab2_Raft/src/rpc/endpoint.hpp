#pragma once
#include <memory>
#include "labrpc.hpp"
#include "../raft.hpp"

class Endpoint {
public:
    Endpoint(const std::string& endpointName, std::shared_ptr<Network> network)
        : m_endpointName { endpointName }
        , m_network { network }
    {}

    bool CallAppendEntries(const Raft::AppendEntriesArgs& args, Raft::AppendEntriesReply& reply);

    bool CallRequestVote(const Raft::RequestVoteArgs& args, Raft::RequestVoteReply& reply);

    std::string getEndpointName() const { return m_endpointName; }

private:
    std::string m_endpointName;
    std::weak_ptr<Network> m_network;
};