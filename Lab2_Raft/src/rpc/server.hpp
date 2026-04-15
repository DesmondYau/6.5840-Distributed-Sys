#pragma once
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include "service.hpp"

class Service; // forward declaration


/*
    Act as RPC dispatcher to route RPCs to the service (e.g. lab 2: RAFT, lab3: KV service, lab4: ShardMaster)
*/
class Server {
public:
    Server()
    {}

    /*
        Registers a new service (e.g. Raft, KV) under a given name
    */
    void addService(const std::string& name, std::shared_ptr<Service> svc) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_services[name] = svc;
    }

    // Dispatch RPC call to the right service
    bool dispatch(const std::string& rpcType, const std::string& args, std::string& reply) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_count++;

        // Split "Raft.AppendEntries" into serviceName and methodName
        auto dotPos = rpcType.find('.');
        if (dotPos == std::string::npos) return false;
        std::string serviceName = rpcType.substr(0, dotPos);
        std::string methodName  = rpcType.substr(dotPos + 1);

        auto it = m_services.find(serviceName);
        if (!m_services.contains(serviceName)) 
            return false;
        else 
            return m_services[serviceName]->dispatch(methodName, args, reply);
    }

    int getCount() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_count;
    }

private:
    std::map<std::string, std::shared_ptr<Service>> m_services;         // Map service name to the service object
    mutable std::mutex m_mu;                                            // mutex
    int m_count {0};                                                    // Track how many RPCs have been dispatched
};