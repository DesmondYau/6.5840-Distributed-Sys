#include "raft.hpp"
#include "config.hpp"
#include "persister.hpp"
#include "./rpc/endpoint.hpp"


Raft::Raft(const std::vector<std::shared_ptr<Endpoint>>& peers, uint32_t id, std::shared_ptr<Persister> persister, std::shared_ptr<ApplyChannel> applyChannel)
    : m_peers { peers }
    , m_id { id }
    , m_persister { persister }
    , m_applyChannel { applyChannel }
{}

void Raft::appendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply)
{

}


void Raft::requestVote(const RequestVoteArgs& args, RequestVoteReply& reply)
{

}

void Raft::kill()
{

}

std::pair<uint32_t, bool> Raft::getState() const
{
    return {};
}

