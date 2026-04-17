#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>


class Service {
public:
    using Handler = std::function<void(const std::string& args, std::string& reply)>;

    Service(const std::string& name) 
        : m_name { name }
    {}

    void addMethod(const std::string& methodName, Handler handler) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_methods[methodName] = std::move(handler);
    }

    bool dispatch(const std::string& methodName, const std::string& args, std::string& reply) {
        std::lock_guard<std::mutex> lock(m_mu);

        if (!m_methods.contains(methodName))
            return false;
        m_methods[methodName](args, reply);
        return true;
    }

    std::string name() const { return m_name; }

private:
    std::string m_name;
    std::unordered_map<std::string, Handler> m_methods;
    mutable std::mutex m_mu;
};