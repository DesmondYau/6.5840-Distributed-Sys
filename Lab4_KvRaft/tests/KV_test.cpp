// kvraft_test.cc
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <random>
#include <atomic>
#include <iostream>
#include <sstream>
#include <mutex>
#include <memory>

#include "porcupine/checker.hpp" 
#include "porcupine/model.hpp"
#include "porcupine/visualization.hpp"
#include "../src/config.hpp"  
#include "../src/models.hpp"   
#include "../src/clerk.hpp"

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Constants & Time Tracking
// ---------------------------------------------------------------------------
const auto electionTimeout = 1s;
const auto linearizabilityCheckTimeout = 1s;

auto t0 = std::chrono::steady_clock::now();

int64_t since_t0() 
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// OpLog for Porcupine
// ---------------------------------------------------------------------------
struct OpLog 
{
    std::vector<porcupine::Operation> operations;
    std::mutex mtx;

    void Append(porcupine::Operation op) 
    {
        std::lock_guard<std::mutex> lock(mtx);
        operations.push_back(op);
    }

    std::vector<porcupine::Operation> Read() 
    {
        std::lock_guard<std::mutex> lock(mtx);
        return operations;
    }
};

// ---------------------------------------------------------------------------
// Helper functions (Get / Put / Append / check)
// ---------------------------------------------------------------------------
std::string Get(Config* cfg, Clerk* ck, const std::string& key, OpLog* log = nullptr, int cli = -1) 
{
    auto start = since_t0();
    std::string v = ck->Get(key);
    auto end = since_t0();
    
    cfg->op(); 
    
    if (log != nullptr) {
        log->Append({
            models::KvInput{0, key, ""},
            models::KvOutput{v},
            start, end, cli
        });
    }
    return v;
}

void Put(Config* cfg, Clerk* ck, const std::string& key, const std::string& value, OpLog* log = nullptr, int cli = -1) 
{
    // Record start time to test for lineraizability
    auto start = since_t0();

    // calls the Put method on the Clerk object
    ck->Put(key, value);

    // Record end time and stats
    auto end = since_t0();
    cfg->op();
    
    if (log != nullptr) 
    {
        log->Append({
            models::KvInput{1, key, value},
            models::KvOutput{""},
            start, end, cli
        });
    }
}

void Append(Config* cfg, Clerk* ck, const std::string& key, const std::string& value, OpLog* log = nullptr, int cli = -1) 
{
    auto start = since_t0();
    ck->Append(key, value);
    auto end = since_t0();
    cfg->op();
    
    if (log != nullptr) {
        log->Append({
            models::KvInput{2, key, value},
            models::KvOutput{""},
            start, end, cli
        });
    }
}

void check(Config* cfg, Clerk* ck, const std::string& key, const std::string& expected) {
    std::string v = Get(cfg, ck, key, nullptr, -1);
    if (v != expected) {
        FAIL() << "Get(" << key << "): expected '" << expected << "', got '" << v << "'";
    }
}

// ---------------------------------------------------------------------------
// Value Verification Checkers
// ---------------------------------------------------------------------------
void checkClntAppends(int clnt, const std::string& v, int count) {
    int lastoff = -1;
    for (int j = 0; j < count; j++) {
        // The string symbols the jth operation of client clnt
        std::string wanted = "x " + std::to_string(clnt) + " " + std::to_string(j) + " y";
        
        // Check if the wanted string is found
        size_t off = v.find(wanted); 
        if (off == std::string::npos) {
            FAIL() << "Client " << clnt << " missing element '" << wanted << "' in result '" << v << "'";
        }
        
        // Check if the wanted string is found exactly once
        size_t off1 = v.rfind(wanted); 
        if (off1 != off) {
            FAIL() << "Duplicate element '" << wanted << "' in result"; 
        }
        
        // Check if operations are in strict order. E.g. x 2 0 y must come before x 2 1 y
        if (static_cast<int>(off) <= lastoff) {
            FAIL() << "Wrong order for element '" << wanted << "' in result"; 
        }
        lastoff = static_cast<int>(off);
    }
}


// Loops through the counts vector to prove lineraizability
// Client histories can be interleaved however the network wants, but each individual client's history must be completely sound
void checkConcurrentAppends(const std::string& v, const std::vector<int>& counts) {
    for (size_t i = 0; i < counts.size(); i++) 
    {
        checkClntAppends(i, v, counts[i]);
    }
}

// ---------------------------------------------------------------------------
// Main Test Engine
// ---------------------------------------------------------------------------
void GenericTest(const std::string& part, int nclients, int nservers, bool unreliable, 
                 bool crash, bool partitions, int maxraftstate, bool randomkeys) 
{
    std::string title = "Test: ";
    if (unreliable) title += "unreliable net, ";
    if (crash)      title += "restarts, ";
    if (partitions) title += "partitions, ";
    if (maxraftstate != -1) title += "snapshots, ";
    if (randomkeys) title += "random keys, ";
    title += (nclients > 1 ? "many clients" : "one client");
    title += " (" + part + ")";

    // Initializes the network and boots up all 'nservers' KV servers,
    auto cfg = make_config(nservers, unreliable, maxraftstate);
    cfg->begin(title);

    // OpLog stores the exact start and end millisecond of every request to test for linearizability
    // The "Master Clerk" used by the main thread purely for end-of-test verification
    OpLog opLog;
    Clerk* ck = cfg->makeClient(cfg->all());

    // Thread synchronization flags to gracefully stop the chaos loops
    std::atomic<int> done_clients{0};
    std::atomic<int> done_partitioner{0};
    std::vector<std::thread> client_threads;
    std::thread partitioner_thread;
    
    // Tracks the exact number of successful appends each individual client achieved
    std::vector<int> client_op_counts(nclients, 0);

    // -----------------------------------------------------------------------
    // 1. THE RETRY LOOP (3 Iterations)
    // Distributed bugs are flaky. Running the 5-second chaos test three 
    // times catches rare race conditions.
    // -----------------------------------------------------------------------
    for (int iter = 0; iter < 3; ++iter) 
    {
        done_clients = 0;
        done_partitioner = 0;
        client_op_counts.assign(nclients, 0); 

        // -------------------------------------------------------------------
        // 2. SPAWN CONCURRENT CLIENTS
        // -------------------------------------------------------------------
        client_threads.clear();
        for (int cli = 0; cli < nclients; ++cli) 
        {
            client_threads.emplace_back([&, cli]() 
            {
                // Each thread gets its own Clerk simulating a unique user
                Clerk* myck = cfg->makeClient(cfg->all());
                std::string last = "";
                
                // Initialize the client's dedicated key if not testing random keys
                // Client 0 uses "0", client 1 uses "1", etc
                if (!randomkeys) 
                {
                    Put(cfg.get(), myck, std::to_string(cli), last, &opLog, cli);
                }
                
                // Local counter for successful Appends
                int j = 0;

                // -----------------------------------------------------------
                // 3. THE REQUEST LOOP
                // Hammer the server with database operations per second until the main thread flips 'done_clients'
                // -----------------------------------------------------------
                while (done_clients == 0) 
                {
                    // If randomkey is true, client picks a random number between 0 and "nclient" - 1
                    // This creates high contention, where multiple clients are desperately trying to overwrite the exact same key at the exact same time
                    std::string key = randomkeys ? std::to_string(rand() % nclients) : std::to_string(cli);

                    // Creating the payload to insert. 
                    // cli is the Client's ID, and j is the number of successful appends this client has done so far.
                    // E.g. Client 3's fifth payload: "x 3 4 y"
                    std::string nv = "x " + std::to_string(cli) + " " + std::to_string(j) + " y";

                    int rand_val = rand() % 1000;

                    // For around 50% of time, perform APPEND operation
                    if (rand_val < 500) 
                    {
                        Append(cfg.get(), myck, key, nv, &opLog, cli);

                        // The "last" variable acts as the client's personal ledger (ground truth)
                        // If client 0 does 3 appends, its local last variable will accurately hold "x 0 0 yx 0 1 yx 0 2 y"
                        if (!randomkeys) last += nv;                            
                        j++;
                    } 
                    // For around 10% of time, perform PUT operation
                    // Put is only used when randomkeys is turned on. In "random keys" tests, the string verification is skipped, 
                    // and the test relies 100% on the mathematical Porcupine checker to prove correctness.
                    else if (randomkeys && rand_val < 600) 
                    {
                        Put(cfg.get(), myck, key, nv, &opLog, cli);
                        j++;
                    } 
                    else 
                    {
                        std::string v = Get(cfg.get(), myck, key, &opLog, cli);

                        // When using non-random keys and performing GET
                        // Client compares string returned by server with local "last" string to check if they are the same
                        if (!randomkeys && v != last) 
                        {
                            FAIL() << "Get wrong value, key " << key << ", wanted:\n" << last << "\nGot:\n" << v;
                        }
                    }
                }
                client_op_counts[cli] = j; 
                cfg->deleteClient(myck); // Clean up the client to prevent memory leaks
            });
        }

        // Every few hundred milliseconds, the cluster is split into two random groups. This forces constant Leader Elections
        // It tests if your Raft can prevent "Split Brain" (two leaders) and
        // if your KVServer correctly handles requests that were sent to a leader that suddenly lost its majority
        if (partitions) 
        {
            std::this_thread::sleep_for(1s); 
            partitioner_thread = std::thread([&]() 
            {
                while (done_partitioner == 0) 
                {
                    std::vector<int> p1, p2;
                    for (int i = 0; i < nservers; ++i) {
                        (rand() % 2 == 0 ? p1 : p2).push_back(i);
                    }
                    cfg->partition(p1, p2);
                    std::this_thread::sleep_for(electionTimeout + std::chrono::milliseconds(rand() % 200));
                }
            });
        }

        std::this_thread::sleep_for(5s);

        done_clients = 1;
        done_partitioner = 1;

        if (partitions) 
        {
            partitioner_thread.join();
            cfg->connectAll(); 
            std::this_thread::sleep_for(electionTimeout); 
        }

        // This kills every single server and then brings them back. This tests your Persistence (Persister). 
        // When the servers reboot, they must recover their logs and KVStore state perfectly.
        if (crash) {
            for (int i = 0; i < nservers; ++i) cfg->shutdownServer(i);
            std::this_thread::sleep_for(electionTimeout);
            for (int i = 0; i < nservers; ++i) cfg->startServer(i); 
            cfg->connectAll();
        }

        for (auto& t : client_threads) t.join();

        for (int i = 0; i < nclients; ++i) {
            std::string key = std::to_string(i);
            std::string v = Get(cfg.get(), ck, key, &opLog, 0);
            if (!randomkeys) {
                checkClntAppends(i, v, client_op_counts[i]);
            }
        }

        if (maxraftstate > 0) 
        {
            size_t sz = cfg->logSize();
            if (sz > 8 * static_cast<size_t>(maxraftstate)) FAIL() << "logs not trimmed";
        }
        if (maxraftstate < 0) 
        {
            size_t ssz = cfg->snapshotSize();
            if (ssz > 0) FAIL() << "snapshot too large when maxraftstate < 0";
        }
    }

    // -----------------------------------------------------------
    // 4. Final Check
    // Porcupine looks at our messy opLog, sorts it by key
    // and tries to find any possible chronological timeline where your server's responses perfectly match the rules of the kvModelInstance.
    // -----------------------------------------------------------
    auto [res, info] = porcupine::CheckOperations(models::kvModelInstance, opLog.Read(), true, linearizabilityCheckTimeout);
    if (res == porcupine::CheckResult::Illegal) 
    {
        // Generate a unique filename based on the test part (e.g., linear_fail_4A.html)
        std::string fileName = "linear_fail_" + part + ".html";
        
        // Generate the visualization file
        // This must be called BEFORE FAIL() because FAIL() terminates the thread
        porcupine::VisualizePath(models::kvModelInstance, info, fileName);
        
        std::cout << "\n==========================================================" << std::endl;
        std::cout << "LINEARIZABILITY ERROR DETECTED!" << std::endl;
        std::cout << "Visualization saved to: " << fileName << std::endl;
        std::cout << "Open this file in a browser to debug the failure point." << std::endl;
        std::cout << "==========================================================\n" << std::endl;

        FAIL() << "history is not linearizable";
    } 
    else if (res == porcupine::CheckResult::Unknown) 
    {
        std::cout << "info: linearizability check timed out, assuming history is ok\n";
    }

    cfg->end();
}

void GenericTestSpeed(const std::string& part, int maxraftstate) {
    const int nservers = 3;
    const int numOps = 1000;

    // Initialize 3 kvservre node in cluster
    auto cfg = make_config(nservers, false, maxraftstate);
    cfg->begin("Test: ops complete fast enough (" + part + ")");

    // The first Get acts as a "Warm-up". 
    // It forces the cluster to elect a leader and establishes the client's connection so that the subsequent loop measures execution time, not election time.
    Clerk* ck = cfg->makeClient(cfg->all());
    ck->Get("x");  

    //  fires $1000$ Append operations sequentially
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < numOps; ++i) {
        ck->Append("x", "x 0 " + std::to_string(i) + " y");
    }
    auto dur = std::chrono::steady_clock::now() - start;

    std::string v = ck->Get("x");
    checkClntAppends(0, v, numOps); 

    // If heartbeats are 100ms, it expects you to process 1000 operations in roughly 33.3s (1000×33.3ms)
    const auto heartbeatInterval = 100ms;
    const auto timePerOp = heartbeatInterval / 3;
    if (dur > numOps * timePerOp) {
        FAIL() << "Operations completed too slowly";
    }

    cfg->end();
}

// ---------------------------------------------------------------------------
// Implemented Missing Tests
// ---------------------------------------------------------------------------


TEST(KvRaftTest, TestUnreliableOneKey4A) 
{
    const int nservers = 3;
    auto cfg = make_config(nservers, true, -1);
    cfg->begin("Test: concurrent append to same key, unreliable (4A)");

    // Create a client and initialize a single key "k" with empty string value
    Clerk* ck = cfg->makeClient(cfg->all());
    Put(cfg.get(), ck, "k", "");

    // Create 5 concurrent client
    const int nclient = 5;
    const int upto = 10;
    std::vector<std::thread> clients;
    
    for (int me = 0; me < nclient; ++me) 
    {
        clients.emplace_back([&, me]() {
            Clerk* myck = cfg->makeClient(cfg->all());
            for (int n = 0; n < upto; ++n) 
            {
                // Each client perform 10 append operation. E.g. Client 2 first append will be "x 2 0 y"
                Append(cfg.get(), myck, "k", "x " + std::to_string(me) + " " + std::to_string(n) + " y");
            }
            cfg->deleteClient(myck);
        });
    }
    for (auto& t : clients) t.join();

    // Fills in a vector of [10, 10, 10, 10, 10]
    // This means the test expects Client 0 to have 10 successful appends, Client 1 to have 10 successful appends," and so on.
    std::vector<int> counts(nclient, upto);
    // Get final value of key "k" which is a long string as a result of above append
    // Note that the exact interleaving of the chunks is totally unpredictable. 
    // Every time you run the test, the string will look different depending on which thread the CPU scheduled first or which network packet arrived fastest
    std::string vx = Get(cfg.get(), ck, "k");
    checkConcurrentAppends(vx, counts); 

    cfg->end();
}


TEST(KvRaftTest, TestOnePartition4A) {

    // Creates 5 kvservers and a master client
    const int nservers = 5;
    auto cfg = make_config(nservers, false, -1);
    Clerk* ck = cfg->makeClient(cfg->all());

    // ONE begin for the whole test
    cfg->begin("Test: One Partition (4A)");

    // Initialize key "1" with value 13
    Put(cfg.get(), ck, "1", "13");

    // Partition kvservers into 2 groups
    std::cout << "\n--- Phase 1: progress in majority ---" << std::endl;
    std::vector<int> p1 = {0, 1, 2}; 
    std::vector<int> p2 = {3, 4};    
    cfg->partition(p1, p2);

    // Assigns client ckp1 to only talk to majority group
    // Assings client ckp2a and ckp2b to only talk to minority group
    Clerk* ckp1 = cfg->makeClient(p1);
    Clerk* ckp2a = cfg->makeClient(p2);
    Clerk* ckp2b = cfg->makeClient(p2);

    // Client ckp1 to talk to majority and update key 1 to value 14
    Put(cfg.get(), ckp1, "1", "14");
    check(cfg.get(), ckp1, "1", "14");

    std::cout << "\n--- Phase 2: no progress in minority ---" << std::endl;
    std::atomic<bool> done0{false}, done1{false};
    
    // Spins up two background thread
    // One tries to update key 1 to value 15 using minority client
    // The other perform a Get request to key 1
    std::thread t0([&]() { Put(cfg.get(), ckp2a, "1", "15"); done0 = true; });
    std::thread t1([&]() { Get(cfg.get(), ckp2b, "1"); done1 = true; });

    // Test fails if we can put or get in minority group
    std::this_thread::sleep_for(1s);
    if (done0) FAIL() << "Put in minority completed";
    if (done1) FAIL() << "Get in minority completed";

    // Update key 1 to value 16 in majority group
    check(cfg.get(), ckp1, "1", "14");
    Put(cfg.get(), ckp1, "1", "16");
    check(cfg.get(), ckp1, "1", "16");

    std::cout << "\n--- Phase 3: completion after heal ---" << std::endl;
    // Receonenct all servers. Reconnect ckp2a and ckp2b to all servers
    // Background thread t0 which has been trying Put ("1". "15") should now work after network heals
    cfg->connectAll();
    cfg->connectClient(ckp2a, cfg->all());
    cfg->connectClient(ckp2b, cfg->all());

    std::this_thread::sleep_for(electionTimeout);

    t0.join(); 
    t1.join();

    // Master client should get value 15 for key 1
    check(cfg.get(), ck, "1", "15");
    cfg->end();
}


/*
TEST(KvRaftTest, TestSnapshotRPC4B) {
    const int nservers = 3;
    int maxraftstate = 1000;
    auto cfg = make_config(nservers, false, maxraftstate);
    Clerk* ck = cfg->makeClient(cfg->all());

    cfg->begin("Test: InstallSnapshot RPC (4B)");
    Put(cfg.get(), ck, "a", "A");
    check(cfg.get(), ck, "a", "A");

    cfg->partition({0, 1}, {2});
    {
        Clerk* ck1 = cfg->makeClient({0, 1});
        for (int i = 0; i < 50; i++) {
            Put(cfg.get(), ck1, std::to_string(i), std::to_string(i));
        }
        std::this_thread::sleep_for(electionTimeout);
        Put(cfg.get(), ck1, "b", "B");
        cfg->deleteClient(ck1);
    }

    size_t sz = cfg->logSize();
    if (sz > 8 * static_cast<size_t>(maxraftstate)) FAIL() << "logs were not trimmed";

    cfg->partition({0, 2}, {1});
    {
        Clerk* ck1 = cfg->makeClient({0, 2});
        Put(cfg.get(), ck1, "c", "C");
        Put(cfg.get(), ck1, "d", "D");
        check(cfg.get(), ck1, "a", "A");
        check(cfg.get(), ck1, "b", "B");
        check(cfg.get(), ck1, "1", "1");
        check(cfg.get(), ck1, "49", "49");
        cfg->deleteClient(ck1);
    }

    cfg->partition({0, 1, 2}, {});
    Put(cfg.get(), ck, "e", "E");
    check(cfg.get(), ck, "c", "C");
    check(cfg.get(), ck, "e", "E");
    check(cfg.get(), ck, "1", "1");
    cfg->end();
}

TEST(KvRaftTest, TestSnapshotSize4B) {
    const int nservers = 3;
    int maxraftstate = 1000;
    int maxsnapshotstate = 500;
    auto cfg = make_config(nservers, false, maxraftstate);
    Clerk* ck = cfg->makeClient(cfg->all());

    cfg->begin("Test: snapshot size is reasonable (4B)");

    for (int i = 0; i < 200; i++) {
        Put(cfg.get(), ck, "x", "0");
        check(cfg.get(), ck, "x", "0");
        Put(cfg.get(), ck, "x", "1");
        check(cfg.get(), ck, "x", "1");
    }

    size_t sz = cfg->logSize();
    if (sz > 8 * static_cast<size_t>(maxraftstate)) FAIL() << "logs were not trimmed";

    size_t ssz = cfg->snapshotSize();
    if (ssz > static_cast<size_t>(maxsnapshotstate)) FAIL() << "snapshot too large (" << ssz << " > " << maxsnapshotstate << ")";

    cfg->end();
}
*/

// ---------------------------------------------------------------------------
// Standard Boilerplate Tests
// ---------------------------------------------------------------------------
// void GenericTest(const std::string& part, int nclients, int nservers, bool unreliable, 
//                  bool crash, bool partitions, int maxraftstate, bool randomkeys) 
TEST(KvRaftTest, TestBasic4A) { GenericTest("4A", 1, 5, false, false, false, -1, false); }
TEST(KvRaftTest, TestSpeed4A) { GenericTestSpeed("4A", -1); }
TEST(KvRaftTest, TestConcurrent4A) { GenericTest("4A", 5, 5, false, false, false, -1, false); }
TEST(KvRaftTest, TestUnreliable4A) { GenericTest("4A", 5, 5, true, false, false, -1, false); }
TEST(KvRaftTest, TestManyPartitionsOneClient4A) { GenericTest("4A", 1, 5, false, false, true, -1, false); }
TEST(KvRaftTest, TestManyPartitionsManyClients4A) { GenericTest("4A", 5, 5, false, false, true, -1, false); }
TEST(KvRaftTest, TestPersistOneClient4A) { GenericTest("4A", 1, 5, false, true, false, -1, false); }
TEST(KvRaftTest, TestPersistConcurrent4A) { GenericTest("4A", 5, 5, false, true, false, -1, false); }
TEST(KvRaftTest, TestPersistConcurrentUnreliable4A) { GenericTest("4A", 5, 5, true, true, false, -1, false); }
TEST(KvRaftTest, TestPersistPartition4A) { GenericTest("4A", 5, 5, false, true, true, -1, false); }
TEST(KvRaftTest, TestPersistPartitionUnreliable4A) { GenericTest("4A", 5, 5, true, true, true, -1, false); }
TEST(KvRaftTest, TestPersistPartitionUnreliableLinearizable4A) { GenericTest("4A", 15, 7, true, true, true, -1, true); }

// TEST(KvRaftTest, TestSpeed4B) { GenericTestSpeed("4B", 1000); }
// TEST(KvRaftTest, TestSnapshotRecover4B) { GenericTest("4B", 1, 5, false, true, false, 1000, false); }
// TEST(KvRaftTest, TestSnapshotRecoverManyClients4B) { GenericTest("4B", 20, 5, false, true, false, 1000, false); }
// TEST(KvRaftTest, TestSnapshotUnreliable4B) { GenericTest("4B", 5, 5, true, false, false, 1000, false); }
// TEST(KvRaftTest, TestSnapshotUnreliableRecover4B) { GenericTest("4B", 5, 5, true, true, false, 1000, false); }
// TEST(KvRaftTest, TestSnapshotUnreliableRecoverConcurrentPartition4B) { GenericTest("4B", 5, 5, true, true, true, 1000, false); }
// SSSTEST(KvRaftTest, TestSnapshotUnreliableRecoverConcurrentPartitionLinearizable4B) { GenericTest("4B", 15, 7, true, true, true, 1000, true); }

int main(int argc, char** argv) 
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
