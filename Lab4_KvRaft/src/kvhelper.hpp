#pragma once

#include "json.hpp"
#include "models.hpp"



// =======================================================================
// GET RPC Serialization (KVServer)
// =======================================================================

// Decode String → GetArgs (Used by KVServer to read client request)
inline void decodeArgs(const std::string& args, models::GetArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.m_key      = j["Key"].get<std::string>();
    a.m_clientID = j["ClientID"].get<int64_t>();
    a.m_seq      = j["Seq"].get<int>();
}

// Encode GetArgs → String (Used by Clerk to send request)
inline std::string encodeArgs(const models::GetArgs& a) 
{
    nlohmann::json j;
    j["Key"]      = a.m_key;
    j["ClientID"] = a.m_clientID;
    j["Seq"]      = a.m_seq;
    return j.dump();
}

// Decode String → GetReply (Used by Clerk to read server response)
inline void decodeReply(const std::string& replyStr, models::GetReply& r) 
{
    nlohmann::json j = nlohmann::json::parse(replyStr);
    r.m_err   = j["Err"].get<std::string>();
    r.m_value = j["Value"].get<std::string>();
}

// Encode GetReply → String (Used by KVServer to send response)
inline std::string encodeReply(const models::GetReply& r) 
{
    nlohmann::json j;
    j["Err"]   = r.m_err;
    j["Value"] = r.m_value;
    return j.dump();
}

// =======================================================================
// PUT/APPEND RPC Serialization (KVServer)
// =======================================================================

// Decode String → PutAppendArgs (Used by KVServer to read client request)
inline void decodeArgs(const std::string& args, models::PutAppendArgs& a)
{
    nlohmann::json j = nlohmann::json::parse(args);
    a.m_key       = j["Key"].get<std::string>();
    a.m_value     = j["Value"].get<std::string>();
    a.m_operation = j["Operation"].get<std::string>();
    a.m_clientID  = j["ClientID"].get<int64_t>();
    a.m_seq       = j["Seq"].get<int>();
}

// Encode PutAppendArgs → String (Used by Clerk to send request)
inline std::string encodeArgs(const models::PutAppendArgs& a) 
{
    nlohmann::json j;
    j["Key"]       = a.m_key;
    j["Value"]     = a.m_value;
    j["Operation"] = a.m_operation;
    j["ClientID"]  = a.m_clientID;
    j["Seq"]       = a.m_seq;
    return j.dump();
}

// Decode String → PutAppendReply (Used by Clerk to read server response)
inline void decodeReply(const std::string& replyStr, models::PutAppendReply& r) 
{
    nlohmann::json j = nlohmann::json::parse(replyStr);
    r.m_err = j["Err"].get<std::string>();
}

// Encode PutAppendReply → String (Used by KVServer to send response)
inline std::string encodeReply(const models::PutAppendReply& r) 
{
    nlohmann::json j;
    j["Err"] = r.m_err;
    return j.dump();
}


// =======================================================================
// JSON Serialization & Deserialization Helpers
// =======================================================================
inline std::string serializeOp(const models::Op& op) 
{
    nlohmann::json j;
    j["m_operation"] = op.m_operation;
    j["m_key"]       = op.m_key;
    j["m_value"]     = op.m_value;
    j["m_clientID"]  = op.m_clientID;
    j["m_seq"]       = op.m_seq;;
    
    return j.dump();
}

inline models::Op deserializeOp(const std::string& data) 
{
    models::Op op;
    try 
    {
        nlohmann::json j = nlohmann::json::parse(data);
        
        // Use .value() to provide safe defaults
        op.m_operation = j.value("m_operation", "");
        op.m_key       = j.value("m_key", "");
        op.m_value     = j.value("m_value", "");
        
        // Provide 0ULL (unsigned long long) to match uint64_t
        op.m_clientID  = j.value("m_clientID", 0ULL); 
        op.m_seq       = j.value("m_seq", 0);
    } 
    catch (const nlohmann::json::exception& e) 
    {
        // Protect against corrupted Raft log strings
        std::cerr << "[KVServer] CRITICAL: JSON Decode error in deserializeOp: " 
                << e.what() << "\nData was: " << data << std::endl;
    }
    
    return op;
}