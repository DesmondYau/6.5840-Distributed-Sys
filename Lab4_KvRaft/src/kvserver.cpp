#include <iostream>
#include "kvserver.hpp"
#include "kvhelper.hpp"
#include "models.hpp"
#include "logger.hpp"
#include "persister.hpp"
#include "helper.hpp"
#include "rpc/service.hpp"
#include "rpc/server.hpp"



// ---------------------------------------------------------------------------
// KVServer Class
// ---------------------------------------------------------------------------
KVServer::KVServer(int32_t id, int maxRaftState, std::shared_ptr<Persister> persister, const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers)
    : m_id {id}
    , m_maxRaftState {maxRaftState}
    , m_dead {false}
{
    // Create Apply Channel and Logger used by raft
    auto applyChannel = std::make_shared<ApplyChannel>();
    auto logger = std::make_shared<Logger>();

    // Store logger in KVServer for KV-specific logging
    m_logger = logger;

    // Initialize raft for kvServer
    m_raft = std::make_unique<Raft>(peers, m_id, persister, applyChannel, logger);

    // Initialize applyChannel for kvServer (kvServer reads commited logs from applyChannel)
    m_applyChannel = applyChannel;

}

void KVServer::initKVServer(labrpc::Server* labrpcSrv)
{
    // Start background applierThread for the apply channel first before we add service (which is the public interface to client)
    // Otherwise, there is a risk of having full applyChannel with a lot of undrained messages (which might lead to other problem)
    // When passing into pointer of member function, we also need to pass in this since member function needs an object to run
    m_applierThread = std::thread(&KVServer::applierLoop, this);

    // REGISTER KV-SERVER RPCs
    // Note that Config::shutdownServer calls m_kvservers[i].reset() when killing server
    // We need a weakptr and upgrade to sharedptr in the function (shared_from_this) to the point to the object to avoid user after free       
    auto weakSelf = this->weak_from_this();                        
    auto kvService = std::make_shared<labrpc::Service>("KVServer");
    kvService->addMethod("Get", [weakSelf](const std::string& args, std::string& reply) {
            // Upgrade weak_ptr to shared_ptr
            if (auto self = weakSelf.lock())
            {
                models::GetArgs a;
                models::GetReply r;
                decodeArgs(args, a);
                self->get(a, r);
                reply = encodeReply(r);
            }
        });

    kvService->addMethod("PutAppend", [weakSelf](const std::string& args, std::string& reply) {
            if (auto self = weakSelf.lock())
            {
                models::PutAppendArgs a;
                models::PutAppendReply r;
                decodeArgs(args, a);
                self->putAppend(a, r);
                reply = encodeReply(r);
            }
        });
    labrpcSrv->addService("KVServer", kvService);


    // REGISTER RAFT RPCs 
    auto raftService = std::make_shared<labrpc::Service>("Raft");
    raftService->addMethod("AppendEntries", [weakSelf](const std::string& args, std::string& reply) {
        if (auto self = weakSelf.lock())
        {
            Raft::AppendEntriesArgs a; Raft::AppendEntriesReply r;
            decodeArgs(args, a);
            self->m_raft->appendEntries(a, r); // Pass through to m_raft
            reply = encodeReply(r);
        }
    });

    raftService->addMethod("RequestVote", [weakSelf](const std::string& args, std::string& reply) {
        if (auto self = weakSelf.lock())
        {
            Raft::RequestVoteArgs a; Raft::RequestVoteReply r;
            decodeArgs(args, a);
            self->m_raft->requestVote(a, r); // Pass through to m_raft
            reply = encodeReply(r);
        }
    });
    labrpcSrv->addService("Raft", raftService);

    
}


KVServer::~KVServer()
{
    kill();
}

void KVServer::applierLoop()
{
    while (!m_dead)
    {
        // pop() goes to sleep until Raft commits something and Apply Channel becomes not empty
        auto msgOpt = m_applyChannel->pop();

        // Recall pop method returns std::optional<ApplyMsg> and returns optnull when channel is closed and queue is empty
        if (!msgOpt.has_value())
        {
            break;
        }

        // Extract ApplyMsg
        auto applyMsg = msgOpt.value();
        

        if (applyMsg.CommandValid)
        {
            // Lock mutex
            std::lock_guard<std::mutex> lock{m_mu};

            // The apply message has CommandValid -> safe to deserialize applyMsg back to op
            models::Op op = deserializeOp(applyMsg.Command);
        
            // Update key value store (The actual State Machine)
            if (op.m_operation == "Get")
            {
                if (m_kvStore.contains(op.m_key))
                {
                    op.m_value = m_kvStore[op.m_key];
                }
                else
                {
                    op.m_value = "";                    // key not found
                }
            }
            else if (op.m_operation == "Append")
            {
                // Check if this is a duplicate operation from the same client.
                // If not, perform operation and update sequence number for the client
                if (!(m_clientSeqMap.contains(op.m_clientID) && m_clientSeqMap[op.m_clientID] >= op.m_seq))
                {
                    m_kvStore[op.m_key] += op.m_value;
                    m_clientSeqMap[op.m_clientID] = op.m_seq;

                    m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_STATEMACHINE, m_id, 
                        "Applied Append. Key: " + op.m_key + " Current Value: " + m_kvStore[op.m_key]));
                }
            }
            else if (op.m_operation == "Put")
            {
                if (!(m_clientSeqMap.contains(op.m_clientID) && m_clientSeqMap[op.m_clientID] >= op.m_seq))
                {
                    m_kvStore[op.m_key] = op.m_value;
                    m_clientSeqMap[op.m_clientID] = op.m_seq;

                    m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_STATEMACHINE, m_id, 
                        "Applied Put. Key: " + op.m_key + " New Value: " + op.m_value));
                }
            }

            // If we are the leader, we need to fulfill the promise from client for GET/PUT/APPEND operation
            if (m_waitMap.contains(applyMsg.CommandIndex))
            {
                m_logger->logKVServer(LogLevel::DEBUG, LogEvent(LogEvent::Type::KV_STATEMACHINE, m_id, 
                    "Fulfilling promise at log index: " + std::to_string(applyMsg.CommandIndex)));

                m_waitMap[applyMsg.CommandIndex].set_value(op);

                // Remember to erase shared pointer after fufilling the promise. Otherwise, our map will keep growing in memory
                m_waitMap.erase(applyMsg.CommandIndex);
            }
        }
        else
        {

        }     
    }
}

// Note the the get method must also go through raft to avoid the 'Stale Read' trap. E.g.
// 1. Server 0 is the Leader. m_kvStore["x"] = 10
// 2. A network partition happens. Server 0 is trapped in a room by itself. The rest of the cluster elects Server 1 as the new Leader.
// 3. A client tells Server 1: Put("x", 100). The cluster commits it. The real value of "x" is now 100.
// 4. If you do not go through raft, you will get the old value 10 for x
void KVServer::get(const models::GetArgs& args, models::GetReply& reply)
{
    std::unique_lock<std::mutex> lock(m_mu);

    m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_REQUEST, m_id, 
        "Received GET request for key: " + args.m_key + " from Client: " + std::to_string(args.m_clientID) + " Seq: " + std::to_string(args.m_seq)));

    models::Op op {"Get", args.m_key, "", args.m_clientID, args.m_seq};
    auto [idx, term, isLeader] = m_raft->start(serializeOp(op));

    if (!isLeader)
    {
        reply.m_err = "ErrWrongLeader";
        return;
    }   

    // 1. Create the promise and future locally (on the STACK)
    std::promise<models::Op> prom;
    auto fut = prom.get_future();

    // 2. MOVE the promise into the map
    // The promise is now owned by the KVServer's m_waitMap (the HEAP).
    // The local thread no longer has any way to fulfill this promise.
    m_waitMap[idx] = std::move(prom);

    // Unlock the mutex before going to sleep so that our background (applier) thread can fulfill the future
    lock.unlock();
    auto status = fut.wait_for(std::chrono::milliseconds(500));
    lock.lock();

    // Clean up promise if not yet cleaned up by applierloop
    if (m_waitMap.contains(idx))
    {
        m_waitMap.erase(idx);
    }

    // Server is crashed, return
    if (m_dead == true)
    {
        reply.m_err = "ErrWrongLeader";
        return; 
    }

    if (status == std::future_status::ready)
    {
        models::Op committedOp;
        try
        {
            committedOp = fut.get();
        }
        catch(const std::exception& e)
        {
            reply.m_err = "ErrWrongLeader";
            return;
        }
         
        if (committedOp.m_clientID == args.m_clientID && committedOp.m_seq == args.m_seq)
        {
            if (committedOp.m_value != "")
            {
                // Notice you should use the value returned instead of reading from kv Store again to avoid any race condition
                // Only allow backgroun applier thread to directly work with m_kvserver
                reply.m_value = committedOp.m_value;        
                reply.m_err = "OK";
                m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_REPLY, m_id, "Replied OK to GET key: " + args.m_key));
            }
            else
            {
                reply.m_err = "ErrNoKey";
                m_logger->logKVServer(LogLevel::WARN, LogEvent(LogEvent::Type::KV_REPLY, m_id, "Replied ErrNoKey to GET key: " + args.m_key));
            }
        }
        else
        {
            reply.m_err = "ErrWrongLeader";
            m_logger->logKVServer(LogLevel::WARN, LogEvent(LogEvent::Type::KV_ERROR, m_id, "Wrong Command Trap caught during GET! Expected Seq: " + std::to_string(args.m_seq)));
        }
    }
    else
    {
        // Timeout 500 ms passed and raft never reached consensus
        reply.m_err = "ErrWrongLeader";
        m_logger->logKVServer(LogLevel::ERROR, LogEvent(LogEvent::Type::KV_ERROR, m_id, "Timeout waiting for Raft consensus on GET key: " + args.m_key));
    }
}

void KVServer::putAppend(const models::PutAppendArgs& args, models::PutAppendReply& reply)
{
    std::unique_lock<std::mutex> lock(m_mu);

    m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_REQUEST, m_id, 
        "Received " + args.m_operation + " request for key: " + args.m_key + " from Client: " + std::to_string(args.m_clientID) + " Seq: " + std::to_string(args.m_seq)));

    // 1. Create Op struct and call raft start()
    models::Op op {args.m_operation, args.m_key, args.m_value, args.m_clientID, args.m_seq};
    auto [idx, term, isLeader] = m_raft->start(serializeOp(op));
       
    // 2. If server is not leader, return
    if (!isLeader)
    {
        reply.m_err = "ErrWrongLeader";
        return;
    }

    // Check sequence number and see if operation has been applied already. 
    // Note that we perform the check after confirming this kvserver is the leader
    if (m_clientSeqMap.contains(args.m_clientID) && m_clientSeqMap[args.m_clientID] >= args.m_seq)
    {
        m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_REPLY, m_id, "Duplicate request detected. Returning early OK."));
        reply.m_err = "OK";
        return;
    }

    // 4. Create a promise that will eventually hold the 'Op'
    // We expect the background thread to hand us back an 'Op'
    // Notice we actually send op to Raft before we set up the promise & future because we need the idx variable
    // Since the background (applier) thread needs mutex to perform set_value, there would not be race condition
    std::promise<models::Op> prom;
    auto fut = prom.get_future();
    m_waitMap[idx] = std::move(prom);                       // We store on idx of waitMap so that our backgoud (applier) thread can find the corresponding promise and set_value
   

    // 5. Unlock the mutex before going to sleep so that our background (applier) thread can fulfill the future
    lock.unlock();
    auto status = fut.wait_for(std::chrono::milliseconds(500));
    lock.lock();

    // Clean up promise if not yet cleaned up by applierloop
    if (m_waitMap.contains(idx))
    {
        m_waitMap.erase(idx);
    }

    // Server is crashed, return
    if (m_dead == true)
    {
        reply.m_err = "ErrWrongLeader";
        return; 
    }

    // 6. Wait for the background thread to fulfill the promise (e.g., 500ms timeout)
    if (status == std::future_status::ready)
    {
        models::Op committedOp;
        try
        {
            committedOp = fut.get();
        }
        catch(const std::exception& e)
        {
            // KVServer is a varaiable inside the heap, so is its member variaable m_waitMap
            // When our kvserver is crashed, m_waitMap together with the promise would be destroyed (since we moved promise)
            // The above Future variable is a local stack varaibales (which is not destroyed) cannot be fulfilled
            // When promis is destroyed, future status becomes ready and enter this loop
            reply.m_err = "ErrWrongLeader";
            return;
        }

        // Need to perform checking to prevent "Wrong Command Trap". E.g.
        // 1. Client A sends Put("A", "Apple") which is put on index 50
        // 2. Due to partition, server 0 is isolated and cannot commit index 50
        // 3. server 1 becomes leader and commit Put("B", "Banana") sent by another client
        // 4. Backgroun thread would actually return a different value on the same index 50 in this case
        if (committedOp.m_clientID == args.m_clientID && committedOp.m_seq == args.m_seq)
        {
            reply.m_err = "OK";
            m_logger->logKVServer(LogLevel::INFO, LogEvent(LogEvent::Type::KV_REPLY, m_id, "Replied OK to " + args.m_operation + " key: " + args.m_key));
        }
        else
        {
            // Another leader overwrote the log entry
            reply.m_err = "ErrWrongLeader";
            m_logger->logKVServer(LogLevel::WARN, LogEvent(LogEvent::Type::KV_ERROR, m_id, "Wrong Command Trap caught during " + args.m_operation + "!"));
        }
    }   
    else
    {
        // Timeout 500 ms passed and raft never reached consensus
        reply.m_err = "ErrWrongLeader";
        m_logger->logKVServer(LogLevel::ERROR, LogEvent(LogEvent::Type::KV_ERROR, m_id, "Timeout waiting for Raft consensus on " + args.m_operation + " key: " + args.m_key));

    }
}

void KVServer::kill()
{
    // We should restrict scope of lock and release before joining applierThread
    // Otherwise, kill() wiill hold the mutex and wait for applierThread to finish and join
    // while applieThread cannot grab the mutex and finish the operation
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_dead.store(true);
    }

    // Close Apply Channel. Apply Channel wakes up and returns nullopt
    // applierThread breaks out of loop
    m_applyChannel->close();

    // Now applierThread breaks out of the loop and can join applierThread
    if (m_applierThread.joinable())
        m_applierThread.join();
}

Raft* KVServer::getRaft()
{
    return m_raft.get();
}


std::shared_ptr<KVServer> startKVServer(int id, int maxRaftState, labrpc::Server* srv, std::shared_ptr<Persister> persister, const std::vector<std::shared_ptr<labrpc::Endpoint>>& peers)
{
    auto kvServer = std::make_shared<KVServer>(id, maxRaftState, persister, peers);

    kvServer->initKVServer(srv);

    return kvServer;
}

