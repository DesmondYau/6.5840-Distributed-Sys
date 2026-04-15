#pragma once

#include <string>
#include <random>
#include "raft.hpp"
#include "../include/json.hpp"

std::string randomString(size_t length) {
    static const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    thread_local static std::mt19937 generator{std::random_device{}()};
    thread_local static std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result.push_back(charset[dist(generator)]);
    }
    return result;
}

void decodeArgs(const std::string& args, Raft::AppendEntriesArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.Term        = j["Term"].get<uint32_t>();
    a.LeaderId    = j["LeaderId"].get<uint32_t>();
    a.PreLogIndex = j["PreLogIndex"].get<uint64_t>();
    a.PreLogTerm  = j["PreLogTerm"].get<uint32_t>();
    a.Entries     = j["Entries"].get<std::string>();
    a.LeaderCommit= j["LeaderCommit"].get<uint64_t>();
}

void decodeArgs(const std::string& args, Raft::RequestVoteArgs & a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.Term         = j["Term"].get<uint32_t>();
    a.CandidateId  = j["CandidateId"].get<uint32_t>();
    a.LastLogIndex = j["LastLogIndex"].get<uint64_t>();
    a.LastLogTerm  = j["LastLogTerm"].get<uint32_t>();
}

std::string encodeReply(const Raft::AppendEntriesReply& r)
{
    nlohmann::json j;
    j["Term"]    = r.Term;
    j["Success"] = r.Success;
    return j.dump();
}

std::string encodeReply(const Raft::RequestVoteReply& r)
{
    nlohmann::json j;
    j["Term"]        = r.Term;
    j["VoteGranted"] = r.VoteGranted;
    return j.dump();
}