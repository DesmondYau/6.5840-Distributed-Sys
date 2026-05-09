#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <thread>

#include "rpc/labrpc.hpp"
#include "models.hpp" 
#include "json.hpp"
#include "kvhelper.hpp"

class Clerk {
public:
    /**
     * @brief Clerk is the cleint that interacts with the distributed Key-Value store backed by the Raft consensus algorithm
     */
    Clerk(labrpc::Network* net, const std::vector<std::string>& servers) 
        : m_net{net}
        , m_servers{servers}
        , m_leaderHint {0}
        , m_seq {0}
    {
        std::random_device rd;
        std::mt19937_64 eng(rd());
        std::uniform_int_distribution<uint64_t> distr;
        m_clientID = distr(eng);
    }

    /**
     * @brief Fetch the current value for a key.
     * Implementation of the loop that tries forever until a leader responds.
     */
    std::string Get(const std::string& key) 
    {
        models::GetArgs arg {key, m_clientID, m_seq};
        std::string strArg = encodeArgs(arg);
        size_t nums = m_servers.size();

        while (true)
        {
            for (size_t i{0}; i<nums; i++)
            {
                // Calculate peer index starting from last known leader
                int peer = (m_leaderHint + i) % nums;
                
                // Set up promise and future
                std::promise<labrpc::ReplyMsg> prom;
                std::future<labrpc::ReplyMsg> fut = prom.get_future();

                // Send the message over labrpc
                m_net->send(m_servers[peer], "KVServer.Get", strArg, std::move(prom));

                // Handle reply
                if (fut.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready)
                {
                    labrpc::ReplyMsg replyMsg;
                    try 
                    {
                        replyMsg = fut.get();
                    }
                    catch (const std::future_error& e) 
                    {
                        // The network destroyed the RPC because the server was crashed
                        // Treat this as a "server not found" and try the next one.
                        continue; 
                    }


                    if (replyMsg.ok)
                    {
                        std::string strReply = replyMsg.reply;
                        models::GetReply reply;
                        decodeReply(strReply, reply);

                        if (reply.m_err == "OK")
                        {
                            m_leaderHint = peer;
                            return reply.m_value;
                        }
                        else if (reply.m_err == "ErrNoKey")
                        {
                            m_leaderHint = peer;
                            return "";
                        }
                        else if (reply.m_err == "ErrWrongLeader")
                        {
                            continue;
                        }
                        else
                        {
                            std::cerr << "[Clerk] Error Get Reply status" << std::endl;
                        }
                    }
                    else
                    {
                        std::cerr << "[Clerk] Error receiving labRPC message" << std::endl;
                    }
                }
                else
                {
                    continue;
                }
            }
            // Pause for a moment to let the servers finish their election before trying again.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    /**
     * @brief Shared logic for Put and Append operations.
     */
    void PutAppend(const std::string& key, const std::string& value, const std::string& op) 
    {
        // Increment the sequence number for the new Put/Append operation
        // Allows kvServer to know which operation is this in case some message was dropped in unrealiable network
        m_seq++;

        models::PutAppendArgs arg {key, value, op, m_clientID, m_seq};
        std::string strArg = encodeArgs(arg);
        size_t nums = m_servers.size();

        // Retry loop
        while (true)
        {
            for (size_t i{0}; i<nums; i++)
            {
                // Calculate peer index starting from last known leader
                int peer = (m_leaderHint + i) % nums;
                
                // Set up promise and future
                std::promise<labrpc::ReplyMsg> prom;
                std::future<labrpc::ReplyMsg> fut = prom.get_future();

                // Send the message over labrpc
                m_net->send(m_servers[peer], "KVServer.PutAppend", strArg, std::move(prom));

                // Handle reply
                if (fut.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready)
                {
                    labrpc::ReplyMsg replyMsg;
                    try 
                    {
                        replyMsg = fut.get();
                    }
                    catch (const std::future_error& e) 
                    {
                        // The network destroyed the RPC because the server was crashed
                        // Treat this as a "server not found" and try the next one.
                        continue; 
                    }

                    if (replyMsg.ok)
                    {
                        std::string strReply = replyMsg.reply;
                        models::PutAppendReply reply;
                        decodeReply(strReply, reply);

                        if (reply.m_err == "OK")
                        {
                            m_seq++;
                            m_leaderHint = peer;
                            return;
                        }
                        else if (reply.m_err == "ErrWrongLeader")
                        {
                            continue;
                        }
                        else
                        {
                            std::cerr << "[Clerk] Error PutAppend Reply status" << std::endl;
                        }
                    }
                    else
                    {
                        std::cerr << "[Clerk] Error receiving labRPC message" << std::endl;
                    }
                }
                else
                {
                    continue;
                }
            }
            // Pause for a moment to let the servers finish their election before trying again.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Wrappers to funnel both Put and Append into the shared RPC endpoint
    void Put(const std::string& key, const std::string& value) 
    {
        std::cout << "[Clerk] ID:" << m_clientID << " calling PUT with key:" << key << " value:" << value << std::endl;
        PutAppend(key, value, "Put");
    }

    void Append(const std::string& key, const std::string& value) 
    {
        std::cout << "[Clerk] ID:" << m_clientID << " calling APPEND with key:" << key << " value:" << value << std::endl;
        PutAppend(key, value, "Append");
    }

private:
    labrpc::Network* m_net;               // The network to send RPCs
    std::vector<std::string> m_servers;   // Server endpoint names
    uint64_t m_clientID;                  // Unique Client ID 
    std::atomic<int> m_seq;               // Sequence number for idempotency
    std::atomic<int> m_leaderHint;        // Cached index of the suspected leader
};

