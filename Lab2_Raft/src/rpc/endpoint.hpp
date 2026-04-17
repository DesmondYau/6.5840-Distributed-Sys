#pragma once
#include <memory>
#include <string>
#include "labrpc.hpp"
#include "../helper.hpp"
#include "../raft.hpp"
#include "../../include/json.hpp"


class Endpoint {
public:
    Endpoint(const std::string& endpointName, std::shared_ptr<Network> network)
        : m_endpointName(endpointName)
        , m_network(network)
    {}

    // Generic Call method, similar to Go's ClientEnd.Call
    template<typename Args, typename Reply>
    bool call(const std::string& rpcType, const Args& args, Reply& reply)
    {
        if (auto net = m_network.lock()) 
        {
            // Encode args
            std::string encodedArgs = encodeArgs(args);

            // Deliver request through the network
            std::promise<ReplyMsg> prom;
            std::future<ReplyMsg> fut = prom.get_future();
            net->send(m_endpointName, rpcType, encodedArgs, std::move(prom));
            ReplyMsg replyMsg;

            // Wait for 100 milliseconds for reply
            if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
                replyMsg = fut.get();
                if (replyMsg.ok) {
                    try 
                    {
                        decodeReply(replyMsg.reply, reply);
                        return true;
                    } 
                    catch (const std::exception& e) 
                    {
                        // Log or handle JSON parse error
                        return false;
                    }
                }
                return false;
            } else {
                // Timeout: peer didn’t respond
                return false;
            }
        }
        return false;
    }

    std::string getEndpointName() const { return m_endpointName; }

private:
    std::string m_endpointName;
    std::weak_ptr<Network> m_network;
};
