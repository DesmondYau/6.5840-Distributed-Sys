#pragma once

#include "../include/buttonrpc-master/buttonrpc.hpp"
#include "../include/json.hpp"

enum class TaskState { 
    UNASSIGNED, 
    ASSIGNED, 
    COMPLETED 
};

enum class TaskType { 
    MAPTASK, 
    REDUCETASK, 
    EMPTYTASK, 
    COMPLETETASK
};

class Task
{
public:
    using ptr = std::shared_ptr<Task>;

    /**
     * @brief Default constructor for RPC
     */
    Task()
        : m_taskId { -1 }
        , m_fileName { "" }
        , m_taskType { TaskType::EMPTYTASK }
    {}

    /**
     * @brief Constructor
     */
    Task(int taskId, std::string fileName, TaskType TaskType)
        : m_taskId { taskId }
        , m_fileName { fileName }
        , m_taskType { TaskType }
    {}

    /**
     * @brief Implementation for buttonrpc
     */
    friend Serializer& operator>> (Serializer& in, Task& d) {
		in >> d.m_taskId >> d.m_fileName >> d.m_taskType >> d.m_taskState >> d.m_deadline;
		return in;
	}

    /**
     * @brief Implementation for buttonrpc
     */
	friend Serializer& operator<< (Serializer& out, Task& d) {
		out << d.m_taskId << d.m_fileName << d.m_taskType << d.m_taskState << d.m_deadline;
		return out;
	}

    /**
     * @brief Return task id
     */
    int getTaskId() const { return m_taskId; }

    /**
     * @brief Return filename of task
     */
    std::string getFileName() const { return m_fileName; }

    /**
     * @brief Return task type
     */
    TaskType getTaskType() const { return m_taskType; }

    /**
     * @brief Return task state
     */
    TaskState getTaskState() const { return m_taskState; }

    /**
     * @brief Set task state
     */
    void setTaskState(TaskState taskState) { m_taskState = taskState; }

    /**
     * @brief Return task deadline
     */
    std::chrono::steady_clock::time_point getDeadline() const { return m_deadline; }

    /**
     * @brief Set task deadline
     */
    void setTaskDeadline(std::chrono::steady_clock::time_point deadline) { m_deadline = deadline; }


private:
    int m_taskId {};
    std::string m_fileName {};
    TaskType m_taskType {};
    TaskState m_taskState { TaskState::UNASSIGNED };
    std::chrono::steady_clock::time_point m_deadline { std::chrono::steady_clock::time_point::min() };
};


struct KeyValuePair
{
    std::string key_;
    std::string value_;
};

using MapFunc = std::vector<KeyValuePair>(*)(const std::string& filename, const std::string& contents);
using ReduceFunc = std::string(*)(const std::string& key, const std::vector<std::string> values);