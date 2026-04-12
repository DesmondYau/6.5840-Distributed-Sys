#include <iostream>
#include <chrono>
#include <thread>
#include "master.hpp"

Master::Master(int mapCount, int reduceCount, const std::vector<std::string>& fileNames)
    : m_mapRemaining { mapCount }
    , m_reduceRemaining { reduceCount }
    , m_mapCount { mapCount }
    , m_reduceCount { reduceCount }
{
    for (int i=0; i<mapCount; i++)
    {
        m_mapTasks.emplace_back(std::make_shared<Task>(i, fileNames[i], TaskType::MAPTASK));
    }

    for (int i=0; i<reduceCount; i++)
    {
        m_reduceTasks.emplace_back(std::make_shared<Task>(i, "", TaskType::REDUCETASK));
    }
}


Task Master::getTaskForWorker()
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);
    Task::ptr taskPtr;

    if (m_mapRemaining > 0)
    {
        taskPtr = selectMapTask();
    }
    else if (m_reduceRemaining > 0)
    {
        taskPtr = selectReduceTask() ;
    }
    else
    {
        taskPtr = std::make_shared<Task>(-1, "", TaskType::COMPLETETASK);
    }
    return *taskPtr;
}

Task::ptr Master::selectMapTask()
{
    
    for (auto& i : m_mapTasks)
    {
        if (i->getTaskState() == TaskState::UNASSIGNED)
        {
            i->setTaskState(TaskState::ASSIGNED);
            i->setTaskDeadline(std::chrono::steady_clock::now() + std::chrono::seconds(TASK_TIME_OUT_SEC));
            return i;
        }
    }
    return std::make_shared<Task>(-1, "", TaskType::EMPTYTASK);
}

Task::ptr Master::selectReduceTask()
{    
    for (auto& i : m_reduceTasks)
    {
        if (i->getTaskState() == TaskState::UNASSIGNED)
        {
            i->setTaskState(TaskState::ASSIGNED);
            i->setTaskDeadline(std::chrono::steady_clock::now() + std::chrono::seconds(TASK_TIME_OUT_SEC));
            return i;
        }
    }
    return std::make_shared<Task>(-1, "", TaskType::EMPTYTASK);
}

void Master::reportMapComplete(int taskId)
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    for (auto& i : m_mapTasks)
    {
        if (i->getTaskId() == taskId)
        {
            i->setTaskState(TaskState::COMPLETED);
            m_mapRemaining--;
            std::cout << "Map task " << taskId << " completed." << std::endl;
            std::cout << "Map tasks remained: " << this->getMapRemaining() << std::endl;
        }
    }
}

void Master::reportReduceComplete(int taskId)
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    for (auto& i : m_reduceTasks)
    {
        if (i->getTaskId() == taskId)
        {
            i->setTaskState(TaskState::COMPLETED);
            m_reduceRemaining--;
            std::cout << "Reduce task " << taskId << " completed." << std::endl;
            std::cout << "Reduce tasks remained: " << this->getReduceRemaining() << std::endl;
        }
    }
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cout << "Missing parameter! Input format is './master <number of reduce tasks> ../tests/pg*.txt'" << std::endl;
        return 1;
    }

    /*
        Parse arguments
    */ 
    int mapCount { argc - 2 };
    int reduceCount { std::stoi(argv[1])};

    std::vector<std::string> fileNames;
    for (int i=2; i<argc; i++) {
        fileNames.push_back(argv[i]);
    }


    /*
        Initialize master and register RPC functions
    */ 
    Master master { mapCount, reduceCount, fileNames };
    buttonrpc server;
    server.as_server(5555);
    server.bind("getTaskForWorker", &Master::getTaskForWorker, &master); 
    server.bind("getReduceCount", &Master::getReduceCount, &master); 
    server.bind("reportMapComplete", &Master::reportMapComplete, &master); 
    server.bind("getMapCount", &Master::getMapCount, &master); 
    server.bind("reportReduceComplete", &Master::reportReduceComplete, &master); 

    /*
        Start a separate thread to monitor when to exit master
    */
    std::thread monitorThread([&master]() {
        while (true) {
            if (master.getMapRemaining() == 0 && master.getReduceRemaining())
            {
                std::cout << "All map and reduce tasks completed. Master exiting" << std::endl;
                std::exit(0);
            }
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });


    std::cout << "Starting server on RPC server on port 5555" << std::endl;
    server.run();

    monitorThread.join();
    return 0;
}

// g++-13 -std=c++23 master.cpp -o master -lzmq 
// ./master <number of reduce tasks> ../tests/pg*.txt