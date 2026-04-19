#pragma once
#include <iostream>
#include <mutex>
#include <sstream>
#include <iomanip>

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

class LogEvent {
public:
    enum class Type {
        STATECHANGE,
        ELECTION,
        HEARTBEAT,
        REPLICATION,
        ERROR
    };

    LogEvent()
    {}

    LogEvent(Type type, int32_t raftId, uint32_t currentTerm, const std::string& message)
        : m_type(type)
        , m_raftId(raftId)
        , m_currentTerm(currentTerm)
        , m_message(message)
    {}

    std::string toString() const 
    {
        std::stringstream os;
        os  << "Raft: " << m_raftId << " CurrentTerm: " << m_currentTerm
            << " [" << typeToString(m_type) << "] "
            << m_message;
        return os.str();
    }

private:
    Type m_type;
    int32_t m_raftId;
    uint32_t m_currentTerm;
    std::string m_message;

    static const char* typeToString(Type type) 
    {
        switch (type) {
            case Type::STATECHANGE: return "STATECHANGE";
            case Type::ELECTION:    return "ELECTION";
            case Type::HEARTBEAT:   return "HEARTBEAT";
            case Type::REPLICATION: return "REPLICATION";
            case Type::ERROR:       return "ERROR";
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

    void log(LogLevel level, const LogEvent& event) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_out << "[" << levelToString(level) << "] " << event.toString() << "\n";
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