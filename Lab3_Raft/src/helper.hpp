#pragma once

#include <string>
#include "raft.hpp"


void decodeArgs(const std::string& args, Raft::AppendEntriesArgs& a);
void decodeArgs(const std::string& args, Raft::RequestVoteArgs& a);
void decodeArgs(const std::string& args, Raft::InstallSnapshotArgs& a);

std::string encodeReply(const Raft::AppendEntriesReply& r);
std::string encodeReply(const Raft::RequestVoteReply& r);
std::string encodeReply(const Raft::InstallSnapshotReply& r);

// Encode Raft arguments (Raft AppendEntries -> String)
std::string encodeArgs(const Raft::AppendEntriesArgs& a);
std::string encodeArgs(const Raft::RequestVoteArgs& a);
std::string encodeArgs(const Raft::InstallSnapshotArgs& a);

// Decode Raft replies (String -> Raft AppendEntriesReply)
void decodeReply(const std::string& replyStr, Raft::AppendEntriesReply& r);
void decodeReply(const std::string& replyStr, Raft::RequestVoteReply& r);
void decodeReply(const std::string& replyStr, Raft::InstallSnapshotReply& r);