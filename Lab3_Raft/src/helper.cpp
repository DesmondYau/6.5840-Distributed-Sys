#include <string>
#include "helper.hpp"
#include "raft.hpp"
#include "../include/json.hpp"


void to_json(nlohmann::json& j, const Raft::LogEntry& entry) {
    j = nlohmann::json{{"Command", entry.command}, {"Term", entry.term}};
}

void from_json(const nlohmann::json& j, Raft::LogEntry& entry) {
    j.at("Command").get_to(entry.command);
    j.at("Term").get_to(entry.term);
}

void decodeArgs(const std::string& args, Raft::AppendEntriesArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.term        = j["Term"].get<uint32_t>();
    a.leaderId    = j["LeaderId"].get<uint32_t>();
    a.preLogIndex = j["PreLogIndex"].get<uint64_t>();
    a.preLogTerm  = j["PreLogTerm"].get<uint32_t>();
    a.entries     = j["Entries"].get<std::vector<Raft::LogEntry>>(); 
    a.leaderCommit= j["LeaderCommit"].get<uint64_t>();
}

void decodeArgs(const std::string& args, Raft::RequestVoteArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.term         = j["Term"].get<uint32_t>();
    a.candidateId  = j["CandidateId"].get<uint32_t>();
    a.lastLogIndex = j["LastLogIndex"].get<uint64_t>();
    a.lastLogTerm  = j["LastLogTerm"].get<uint32_t>();
}

// Decode String → InstallSnapshotArgs
void decodeArgs(const std::string& args, Raft::InstallSnapshotArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.term              = j["Term"].get<uint32_t>();
    a.leaderId          = j["LeaderId"].get<int32_t>();
    a.lastIncludedIndex = j["LastIncludedIndex"].get<uint64_t>();
    a.lastIncludedTerm  = j["LastIncludedTerm"].get<uint32_t>();
    a.offset            = j["Offset"].get<int>();          
    a.data              = j["Data"].get<std::vector<uint8_t>>();
    a.done              = j["Done"].get<bool>();
}

std::string encodeReply(const Raft::AppendEntriesReply& r) {
    nlohmann::json j;
    j["Term"] = r.term;
    j["Success"] = r.success;
    j["ConflictIndex"] = r.conflictIndex;
    j["ConflictTerm"] = r.conflictTerm;
    return j.dump();
}

std::string encodeReply(const Raft::RequestVoteReply& r)
{
    nlohmann::json j;
    j["Term"]        = r.term;
    j["VoteGranted"] = r.voteGranted;
    return j.dump();
}

std::string encodeReply(const Raft::InstallSnapshotReply& r)
{
    nlohmann::json j;
    j["Term"] = r.term;
    return j.dump();
}

// Encode Raft arguments (Raft AppendEntries -> String)
std::string encodeArgs(const Raft::AppendEntriesArgs& a) {
    nlohmann::json j;
    j["Term"]        = a.term;
    j["LeaderId"]    = a.leaderId;
    j["PreLogIndex"] = a.preLogIndex;
    j["PreLogTerm"]  = a.preLogTerm;
    j["Entries"]     = a.entries;
    j["LeaderCommit"]= a.leaderCommit;
    return j.dump();
}

// Encode Raft arguments (Raft RequestVoteArgs -> String)
std::string encodeArgs(const Raft::RequestVoteArgs& a) {
    nlohmann::json j;
    j["Term"]         = a.term;
    j["CandidateId"]  = a.candidateId;
    j["LastLogIndex"] = a.lastLogIndex;
    j["LastLogTerm"]  = a.lastLogTerm;
    return j.dump();
}

// Encode InstallSnapshotArgs → String
std::string encodeArgs(const Raft::InstallSnapshotArgs& a)
{
    nlohmann::json j;
    j["Term"]             = a.term;
    j["LeaderId"]         = a.leaderId;
    j["LastIncludedIndex"]= a.lastIncludedIndex;
    j["LastIncludedTerm"] = a.lastIncludedTerm;
    j["Offset"]           = a.offset;          
    j["Data"]             = a.data;             
    j["Done"]             = a.done;
    return j.dump();
}


// Decode Raft replies (String -> Raft AppendEntriesReply)
void decodeReply(const std::string& replyStr, Raft::AppendEntriesReply& r) {
    nlohmann::json j = nlohmann::json::parse(replyStr);
    r.term = j["Term"].get<uint32_t>();
    r.success = j["Success"].get<bool>();
    
    // These fields are only present on failure replies
    r.conflictIndex = j["ConflictIndex"].get<uint64_t>();
    r.conflictTerm  = j["ConflictTerm"].get<uint32_t>();
}

// Decode Raft replies (String -> Raft RequestVoteReply)
void decodeReply(const std::string& replyStr, Raft::RequestVoteReply& r) {
    nlohmann::json j = nlohmann::json::parse(replyStr);
    r.term        = j["Term"].get<uint32_t>();
    r.voteGranted = j["VoteGranted"].get<bool>();
}

void decodeReply(const std::string& replyStr, Raft::InstallSnapshotReply& r)
{
    nlohmann::json j = nlohmann::json::parse(replyStr);
    r.term = j["Term"].get<uint32_t>();
}