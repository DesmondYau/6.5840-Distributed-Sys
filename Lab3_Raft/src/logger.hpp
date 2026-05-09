#pragma once
#include <iostream>
#include <mutex>
#include <sstream>
#include <iomanip>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class LogEvent {
public:
    enum class Type {
        // Raft specific
        STATECHANGE,
        ELECTION,
        HEARTBEAT,
        REPLICATION,
        DELETION,
        APPLY,
        PERSISTER,
        SNAPSHOT,
        ERROR,

        // KVServer specific
        KV_REQUEST,       // When receiving Get/PutAppend from Clerk
        KV_REPLY,         // When replying back to Clerk
        KV_STATEMACHINE,  // When modifying the actual std::unordered_map
        KV_ERROR
    };

    LogEvent()
    {}

    // Constructor for Raft events
    LogEvent(Type type, int32_t raftId, uint32_t currentTerm, const std::string& message)
        : m_type(type)
        , m_id(raftId)
        , m_currentTerm(currentTerm)
        , m_message(message)
        , m_isRaft(true)
    {}

    // Constructor for KVServer events (Omit currentTerm)
    LogEvent(Type type, int32_t serverId, const std::string& message)
        : m_type(type)
        , m_id(serverId)
        , m_currentTerm(0)
        , m_message(message)
        , m_isRaft(false)
    {}

    std::string toString() const 
    {
        std::stringstream os;
        if (m_isRaft) 
        {
            os  << "Raft: " << m_id << " CurrentTerm: " << m_currentTerm
                << " [" << typeToString(m_type) << "] "
                << m_message;
        } 
        else 
        {
            // Formatting specifically for KVServer logs
            os  << "Server: " << m_id 
                << " [" << typeToString(m_type) << "] "
                << m_message;
        }
        return os.str();
    }

private:
    Type m_type;
    int32_t m_id;
    uint32_t m_currentTerm;
    std::string m_message;
    bool m_isRaft; // Flag to determine how to format toString()

    static const char* typeToString(Type type) 
    {
        switch (type) {
            // Raft types
            case Type::STATECHANGE: return "STATECHANGE";
            case Type::ELECTION:    return "ELECTION";
            case Type::HEARTBEAT:   return "HEARTBEAT";
            case Type::REPLICATION: return "REPLICATION";
            case Type::DELETION:    return "DELETION";
            case Type::APPLY:       return "APPLY";
            case Type::PERSISTER:   return "PERSISTER";
            case Type::SNAPSHOT:    return "SNAPSHOT";
            case Type::ERROR:       return "ERROR";

            // KVServer types
            case Type::KV_REQUEST:      return "KV_REQ";
            case Type::KV_REPLY:        return "KV_REP";
            case Type::KV_STATEMACHINE: return "KV_STATEMACHINE";
            case Type::KV_ERROR:        return "KV_ERROR";
        }
        return "UNKNOWN";
    }
};

class Logger
{
public:
    Logger(std::ostream& out = std::cout, LogLevel minLevel = LogLevel::DEBUG)
        : m_out(out)
    {}

    void logRaft(LogLevel level, const LogEvent& event) 
    {
        /*
        std::lock_guard<std::mutex> lock(m_mu);
        if (level != LogLevel::DEBUG)
        {
            m_out << "[Raft] [" << levelToString(level) << "] " << event.toString() << "\n";
        }
        */
    }

    void logKVServer(LogLevel level, const LogEvent& event) 
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (level != LogLevel::DEBUG)
        {
            m_out << "[KVServer] [" << levelToString(level) << "] " << event.toString() << "\n";
        }
    }

private:
    std::ostream& m_out;
    std::mutex m_mu;

    static const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }
};