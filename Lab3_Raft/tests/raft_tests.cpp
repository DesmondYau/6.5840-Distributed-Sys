#include <gtest/gtest.h>
#include "../src/config.hpp"
#include <thread>
#include <chrono>
#include <future>

const std::chrono::milliseconds RaftElectionTimeout(1000);


TEST(RaftTest3A, InitialElection) {
    // Creates a COnfig harness with 3 Raft servers
    Config cfg(3, false);
    cfg.begin("Test (3A): initial election");

    // Step 1: Check if a leader is elected.
    // Returns the leader ID, should be >= 0.
    int leader = cfg.checkOneLeader();
    ASSERT_GE(leader, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Step 2: Check that all servers agree on the same term.
    // The term must be at least 1, meaning an election has occurred
    int term1 = cfg.checkTerms();
    ASSERT_GE(term1, 1);


    // Sleep for 2× election timeout to ensure stability when no failures occur.
    std::this_thread::sleep_for(2 * RaftElectionTimeout);

    // Step 3: Check that the term has not changed.
    // If term1 != term2, it means a spurious election happened.
    int term2 = cfg.checkTerms();
    EXPECT_EQ(term1, term2);

    // Step 4: Verify that there is still a leader after the wait.
    cfg.checkOneLeader();

    cfg.end();
}


TEST(RaftTest3A, ReElection) {
    Config cfg(3, false);
    cfg.begin("Test (3A): election after network failure");

    int leader1 = cfg.checkOneLeader();

    // Disconnect current leader → new leader should be elected
    cfg.disconnectServer(leader1);
    int leader2 = cfg.checkOneLeader();
    ASSERT_NE(leader1, leader2);

    // Reconnect old leader → should become follower, not disturb new leader
    cfg.connectServer(leader1);
    int leader3 = cfg.checkOneLeader();
    ASSERT_EQ(leader2, leader3);

    // Break quorum → no leader should exist
    cfg.disconnectServer(leader2);
    cfg.disconnectServer((leader2 + 1) % 3);
    std::this_thread::sleep_for(2 * RaftElectionTimeout);
    cfg.checkNoLeader();


    // - We keep on failing this test case because we implemented candidate to reject VoteRequest automatically.
    // - When raft 2 (leader) and raft 1 are disconnected, raft 1 becomes a candidate. The same goes for raft 0
    // - When raft 1 rejoins, both raft 0 and raft 1 are candidate and would not vote for each other -> split vote
    // - Solved by implementing logger
    // Restore quorum → leader should be elected
    cfg.connectServer((leader2 + 1) % 3);
    cfg.checkOneLeader();

    // Reconnect last node → leader should still exist
    cfg.connectServer(leader2);
    cfg.checkOneLeader();

    cfg.end();
}


TEST(RaftTest3A, ManyElections) {
    Config cfg(7, false);
    cfg.begin("Test (3A): multiple elections");

    cfg.checkOneLeader();

    int iters = 10;
    for (int ii = 1; ii < iters; ii++) {
        // Randomly disconnect three nodes
        int i1 = rand() % 7;
        int i2 = rand() % 7;
        int i3 = rand() % 7;
        cfg.disconnectServer(i1);
        cfg.disconnectServer(i2);
        cfg.disconnectServer(i3);

        // Either current leader survives or new one is elected
        cfg.checkOneLeader();

        // Reconnect nodes
        cfg.connectServer(i1);
        cfg.connectServer(i2);
        cfg.connectServer(i3);
    }

    cfg.checkOneLeader();
    cfg.end();
}


TEST(RaftTest3B, BasicAgreement) {
    // Create a cluster of 3 Raft servers with reliable network
    Config cfg(3, false);
    cfg.begin("Test (3B): basic agreement");

    int iters = 3;
    for (int index = 1; index <= iters; index++) {
        // Step 1: Ensure no command is committed before Start()
        auto [nd, _] = cfg.nCommitted(index);
        ASSERT_EQ(nd, 0) << "Some servers committed before Start()";

        // Step 2: Submit a command (index*100) to the leader
        int xindex = cfg.one(std::to_string(index * 100), 3, false);

        // Step 3: Verify the committed log index matches expectation
        ASSERT_EQ(xindex, index) << "Got index " << xindex << " but expected " << index;
    }

    cfg.end();
}


TEST(RaftTest3B, RPCByteCount) {
    // Create a cluster of 3 Raft servers with reliable network
    Config cfg(3, false);
    cfg.begin("Test (3B): RPC byte count");

    // Step 1: Submit an initial command to establish baseline
    cfg.one(std::to_string(99), 3, false);
    int64_t bytes0 = cfg.bytesTotal();

    // Step 2: Send multiple large commands (simulate heavy payloads)
    int iters = 10;
    int64_t sent = 0;
    for (int index = 2; index < iters + 2; index++) {
        std::string cmd(5000, 'x'); // 5000-byte string
        int xindex = cfg.one(cmd, 3, false);
        ASSERT_EQ(xindex, index) << "Got index " << xindex << " but expected " << index;
        sent += cmd.size();
    }

    // Step 3: Measure total RPC bytes sent
    int64_t bytes1 = cfg.bytesTotal();
    int64_t got = bytes1 - bytes0;
    int64_t expected = static_cast<int64_t>(3) * sent;

    // Step 4: Allow small overhead but check against excessive RPC traffic
    ASSERT_LE(got, expected + 50000) << "Too many RPC bytes; got " << got << ", expected " << expected;

    cfg.end();
}

TEST(RaftTest3B, FollowerFailure) {
    Config cfg(3, false);
    cfg.begin("Test (3B): progressive failure of followers");

    cfg.one(std::to_string(101), 3, false);

    // Disconnect one follower
    int leader1 = cfg.checkOneLeader();
    cfg.disconnectServer((leader1 + 1) % 3);

    // Remaining two should agree
    cfg.one(std::to_string(102), 2, false);
    std::this_thread::sleep_for(RaftElectionTimeout);
    cfg.one(std::to_string(103), 2, false);

    // Disconnect remaining follower
    int leader2 = cfg.checkOneLeader();
    cfg.disconnectServer((leader2 + 1) % 3);
    cfg.disconnectServer((leader2 + 2) % 3);

    // Submit command
    auto [index, _, ok] = cfg.getRaft(leader2)->start(std::to_string(104));
    ASSERT_TRUE(ok) << "Leader rejected Start()";
    ASSERT_EQ(index, 4);

    std::this_thread::sleep_for(2 * RaftElectionTimeout);

    // Cannot replicate to any other server, it cannot achieve a majority.
    auto [n, __] = cfg.nCommitted(index);
    ASSERT_EQ(n, 0) << n << " committed but no majority";

    cfg.end();
}


TEST(RaftTest3B, LeaderFailure) {
    Config cfg(3, false);
    cfg.begin("Test (3B): failure of leaders");

    cfg.one(std::to_string(101), 3, false);

    // Disconnect first leader
    int leader1 = cfg.checkOneLeader();
    cfg.disconnectServer(leader1);

    // Remaining followers elect new leader
    cfg.one(std::to_string(102), 2, false);
    std::this_thread::sleep_for(RaftElectionTimeout);
    cfg.one(std::to_string(103), 2, false);

    // Disconnect new leader
    int leader2 = cfg.checkOneLeader();
    cfg.disconnectServer(leader2);

    // Submit command to all servers
    for (int i = 0; i < 3; i++) {
        cfg.getRaft(i)->start(std::to_string(104));
    }

    std::this_thread::sleep_for(2 * RaftElectionTimeout);

    auto [n, __] = cfg.nCommitted(4);
    ASSERT_EQ(n, 0) << n << " committed but no majority";

    cfg.end();
}


TEST(RaftTest3B, FailAgree) {
    Config cfg(3, false);
    cfg.begin("Test (3B): agreement after follower reconnects");

    cfg.one(std::to_string(101), 3, false);

    int leader = cfg.checkOneLeader();
    cfg.disconnectServer((leader + 1) % 3);

    cfg.one(std::to_string(102), 2, false);
    cfg.one(std::to_string(103), 2, false);
    std::this_thread::sleep_for(RaftElectionTimeout);
    cfg.one(std::to_string(104), 2, false);
    cfg.one(std::to_string(105), 2, false);

    // Reconnect follower
    cfg.connectServer((leader + 1) % 3);

    // Full cluster should agree again
    cfg.one(std::to_string(106), 3, true);
    std::this_thread::sleep_for(RaftElectionTimeout);
    cfg.one(std::to_string(107), 3, true);

    cfg.end();
}


TEST(RaftTest3B, FailNoAgree) {
    Config cfg(5, false);
    cfg.begin("Test (3B): no agreement if too many followers disconnect");

    cfg.one(std::to_string(10), 5, false);

    int leader = cfg.checkOneLeader();
    cfg.disconnectServer((leader + 1) % 5);
    cfg.disconnectServer((leader + 2) % 5);
    cfg.disconnectServer((leader + 3) % 5);

    auto [index, _, ok] = cfg.getRaft(leader)->start(std::to_string(20));
    ASSERT_TRUE(ok);
    ASSERT_EQ(index, 2);

    std::this_thread::sleep_for(2 * RaftElectionTimeout);

    auto [n, __] = cfg.nCommitted(index);
    ASSERT_EQ(n, 0) << n << " committed but no majority";

    // Repair
    cfg.connectServer((leader + 1) % 5);
    cfg.connectServer((leader + 2) % 5);
    cfg.connectServer((leader + 3) % 5);

    int leader2 = cfg.checkOneLeader();
    auto [index2, ___, ok2] = cfg.getRaft(leader2)->start(std::to_string(30));
    ASSERT_TRUE(ok2);
    ASSERT_GE(index2, 2);
    ASSERT_LE(index2, 3);

    cfg.one(std::to_string(1000), 5, true);

    cfg.end();
}

TEST(RaftTest3C, Persist1) {
    Config cfg(3, false);
    cfg.begin("Test (3C): basic persistence");

    // Step 1: Commit a command normally.
    // This creates a committed entry at index 1 that should be persisted.
    cfg.one(std::to_string(11), 3, true);

    // Step 2: Crash and restart ALL servers at the same time.
    // This is the core of the test — we are forcing a full cluster restart.
    // After this, servers should reload their log/term from Persister.
    for (int i = 0; i < 3; i++) {
        cfg.crashServer(i);
        cfg.startServer(i);
        cfg.connectServer(i);
    }

    // Step 3: Try to commit a new command (12) after full restart. This is the most important check.
    // The new leader must send preLogIndex=1. Followers must accept it.
    // This only succeeds if every server correctly restored the old log (including command 11 at index 1 with the correct term).
    cfg.one(std::to_string(12), 3, true);

    // Step 4: Test single server crash/restart
    int leader1 = cfg.checkOneLeader();
    cfg.disconnectServer(leader1);
    cfg.startServer(leader1);
    cfg.connectServer(leader1);
    cfg.one(std::to_string(13), 3, true);

    // Step 5: Test another leader crash/restart + partial majority
    int leader2 = cfg.checkOneLeader();
    cfg.disconnectServer(leader2);
    cfg.one(std::to_string(14), 2, true);
    cfg.startServer(leader2);
    cfg.connectServer(leader2);

    // Step 6: Test yet another server crash/restart
    std::this_thread::sleep_for(RaftElectionTimeout);
    int i3 = (cfg.checkOneLeader() + 1) % 3;
    cfg.disconnectServer(i3);
    cfg.one(std::to_string(15), 2, true);
    cfg.startServer(i3);
    cfg.connectServer(i3);
    cfg.one(std::to_string(16), 3, true);

    cfg.end();
}


TEST(RaftTest3C, Persist2) {
    Config cfg(5, false);
    cfg.begin("Test (3C): more persistence");

    int index = 1;
    for (int iters = 0; iters < 5; iters++) {

        // Commit a command on all 5 servers (full quorum)
        cfg.one(std::to_string(10 + index), 5, true);
        index++;

        int leader1 = cfg.checkOneLeader();

        // Disconnect 2 followers → now only 3 servers left (still a majority)
        cfg.disconnectServer((leader1 + 1) % 5);
        cfg.disconnectServer((leader1 + 2) % 5);

        // Commit next command with only 3 live servers
        // This tests that the system still makes progress with a partial majority
        cfg.one(std::to_string(10 + index), 3, true);
        index++;

        // Now disconnect the current leader + 2 more followers. No more servers are alive
        cfg.disconnectServer((leader1 + 0) % 5);    // disconnect the current leader
        cfg.disconnectServer((leader1 + 3) % 5);
        cfg.disconnectServer((leader1 + 4) % 5);

        // Create 2 completely new raft instances
        // These are brand-new Raft objects (not the old ones)
        cfg.startServer((leader1 + 1) % 5);
        cfg.startServer((leader1 + 2) % 5);
        cfg.connectServer((leader1 + 1) % 5);
        cfg.connectServer((leader1 + 2) % 5);


        std::this_thread::sleep_for(RaftElectionTimeout);   // // give them time to elect a leader

        // Restart one more new Raft instance
        cfg.startServer((leader1 + 3) % 5);
        cfg.connectServer((leader1 + 3) % 5);
        cfg.one(std::to_string(10 + index), 3, true);
        index++;


        // Bring two previously discoonected servers back
        cfg.connectServer((leader1 + 4) % 5);
        cfg.connectServer((leader1 + 0) % 5);
    }

    cfg.one(std::to_string(1000), 5, true);
    cfg.end();
}


TEST(RaftTest3C, Persist3) {
    Config cfg(3, false);
    cfg.begin("Test (3C): partitioned leader and one follower crash, leader restarts");

    // 1. Normal commit (101) with full cluster (3/3 alive)
    cfg.one(std::to_string(101), 3, true);

    // 2. Disconnect one follower (server 2)
    // Commit a new entry (102) while one follower is partitioned
    int leader = cfg.checkOneLeader();
    cfg.disconnectServer((leader + 2) % 3);
    cfg.one(std::to_string(102), 2, true);

    // 3. Crash the current leader (server 0) and the remaining follower (server 1)
    // This is the critical failure scenario: the leader itself crashes while partitioned
    cfg.crashServer((leader + 0) % 3);
    cfg.crashServer((leader + 1) % 3);

    // 4. Reconnect the previously partitioned follower (server 2)
    // Now 1 server is alive and connected
    cfg.connectServer((leader + 2) % 3);

    // 5. Restart the old leader (server 0) as a brand-new Raft instance
    // This new Raft will call readPersist() and must correctly restore its log
    // The restarted leader must have correctly restored the previous entries (101 and 102) so it can successfully replicate the new entry
    cfg.startServer((leader + 0) % 3);
    cfg.connectServer((leader + 0) % 3);
    cfg.one(std::to_string(103), 2, true);

    // 6. Restart the last crashed follower (server 1)
    cfg.startServer((leader + 1) % 3);
    cfg.connectServer((leader + 1) % 3);
    cfg.one(std::to_string(104), 3, true);

    cfg.end();
}


TEST(RaftTest3C, Figure8) {
    Config cfg(5, false);
    cfg.begin("Test (3C): Figure 8");

    // Initial commit with only 1 server to establish a baseline log entry.
    // This forces at least one entry to be persisted before the stress begins.
    cfg.one(std::to_string(rand() % 10000), 1, true);

    int nup = 5;    // number of currently alive (up) Raft instances

    // Run 1000 iterations of random leader crashes and restarts.
    // This is the core stress test that mimics Figure 8 in the extended Raft paper.
    for (int iters = 0; iters < 1000; iters++) {
        int leader = -1;

        // Try to submit a random command from ANY currently alive Raft.
        // If a Raft is the leader, it will succeed and return ok = true.
        for (int i = 0; i < 5; i++) 
        {
            if (cfg.getRaft(i)) {
                auto [idx, term, ok] = cfg.getRaft(i)->start(std::to_string(rand() % 10000));
                if (ok) 
                    leader = i;     // remember which server is currently the leader
            }
        }

        // Occasional longer sleep (with 10% probability) to allow elections / heartbeats
        if ((rand() % 1000) < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(13));
        }

        // If we found a leader this iteration → crash it
        // This simulates a leader failing at any moment (very common in real systems)
        if (leader != -1) {
            cfg.crashServer(leader);
            nup--;
        }

        // Guarantee we always have at least a majority (3) alive.
        // If we drop below 3, randomly pick a dead server and bring it back up.
        // This forces the system to constantly recover from crashes while still ma
        if (nup < 3) {
            int s = rand() % 5;
            if (!cfg.getRaft(s)) {
                cfg.startServer(s);
                cfg.connectServer(s);
                nup++;
            }
        }
    }

    // After the 1000 random crash/restart cycles, bring ALL 5 servers back online.
    // This ensures the final state is consistent across the whole cluster.
    for (int i = 0; i < 5; i++) {
        if (!cfg.getRaft(i)) {
            cfg.startServer(i);
            cfg.connectServer(i);
        }
    }

    cfg.one(std::to_string(rand() % 10000), 5, true);
    cfg.end();
}


TEST(RaftTest3C, UnreliableAgree) {

    // Create a 5-server cluster with unreliable network (drops, delays, reordering)
    // but no long reordering yet.
    Config cfg(5, true);           // true = unreliable
    cfg.begin("Test (3C): unreliable agreement");

    // We will fire many concurrent commands while the network is unreliable.
    // The test verifies that Raft can still reach agreement even under packet loss.
    for (int iters = 1; iters < 50; iters++) 
    {

        // 4 background commands submitted concurrently (simulates high load)
        // Each is sent with expectedServers = 1 because the network is unreliable.
        for (int j = 0; j < 4; j++) 
        {
            // In Go this was a goroutine; here we just call sequentially
            // (the test harness already handles concurrency inside one()).
            cfg.one(std::to_string(100 * iters + j), 1, true);
        }

        // One more command per iteration with 1 server.
        cfg.one(std::to_string(iters), 1, true);
    }

    // After all the unreliable traffic, turn the network back to reliable.
    // This lets the final commit be checked cleanly.
    cfg.setNetworkUnreliable(false);

    // Final commit that must be accepted by all 5 servers.
    cfg.one(std::to_string(100), 5, true);

    cfg.end();
}


TEST(RaftTest3C, Figure8Unreliable) {
    // 5-server cluster with unreliable network from the beginning.
    Config cfg(5, true);           // true = unreliable (drops, delays, reordering)
    cfg.begin("Test (3C): Figure 8 (unreliable)");

    // Seed the log with one entry on a single server.
    cfg.one(std::to_string(rand() % 10000), 1, true);

    int nup = 5;   // number of currently alive Raft instances

    // Run 1000 iterations of random leader crashes under unreliable network.
    // This is the classic "Figure 8" stress test from the Raft paper,
    // but now with packet loss, delays, and (later) long reordering.
    for (int iters = 0; iters < 1000; iters++) {

        // After 200 iterations, enable long reordering (very harsh condition).
        if (iters == 200) {
            cfg.setNetworkLongReordering(true);
        }

        int leader = -1;

        // Try to submit a random command from any alive Raft.
        // If a Raft is the current leader, it will return ok = true.
        for (int i = 0; i < 5; i++) {
            if (cfg.getRaft(i)) {
                auto [idx, term, ok] = cfg.getRaft(i)->start(std::to_string(rand() % 10000));
                if (ok) {
                    leader = i;
                }
            }
        }

        // Random short sleep (most of the time ~13ms, occasionally ~500ms)
        // to simulate timing variation and give elections/heartbeats time.
        if ((rand() % 1000) < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(13));
        }

        // With some probability, crash the current leader.
        if (leader != -1 && (rand() % 1000) < (int)RaftElectionTimeout.count() / 2) {
            cfg.crashServer(leader);
            nup--;
        }

        // Guarantee we always have at least a majority (3) alive.
        if (nup < 3) {
            int s = rand() % 5;
            if (!cfg.getRaft(s)) {
                cfg.startServer(s);
                cfg.connectServer(s);
                nup++;
            }
        }
    }

    // After 1000 crash/restart cycles, bring every server back online.
    for (int i = 0; i < 5; i++) {
        if (!cfg.getRaft(i)) {
            cfg.startServer(i);
            cfg.connectServer(i);
        }
    }

    // Final commit with full quorum to verify the cluster recovered correctly.
    cfg.one(std::to_string(rand() % 10000), 5, true);

    cfg.end();
}


// internalChurn is a helper function used by both ReliableChurn and UnreliableChurn.
// It runs a stress test with continuous server failures (churn) while clients
// keep submitting commands.
static void internalChurn(Config& cfg, bool unreliable) {
    if (unreliable) {
        cfg.begin("Test (3C): unreliable churn");
    } else {
        cfg.begin("Test (3C): churn");
    }

    std::atomic<int> stop{0}; // signal to stop the client threads

    // 1. Client function: each client continuously submits random commands
    // and records the values it successfully started.
    auto clientFunc = [&](int me) -> std::vector<int> {
        std::vector<int> values;
        while (stop.load() == 0) {
            int x = rand() % 1000000;

            int index = -1;
            bool ok = false;

            // Try every server until one accepts the command (i.e. it is leader).
            for (int i = 0; i < 5; i++) {
                if (cfg.getRaft(i)) {
                    auto [idx, term, accepted] = cfg.getRaft(i)->start(std::to_string(x));
                    if (accepted) {
                        ok = true;
                        index = idx;
                        break;
                    }
                }
            }

            if (ok) {
                // Wait for the command to be committed (with backoff).
                for (int to : {10, 20, 50, 100, 200}) {
                    auto [nd, cmd] = cfg.nCommitted(index);
                    if (nd > 0) {
                        if (cmd == std::to_string(x)) {
                            values.push_back(x);    // remember this value was committed
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(to));
                }
            } else {
                // If no leader right now → back off a bit.
                std::this_thread::sleep_for(std::chrono::milliseconds(79 + me * 17));
            }
        }
        return values;
    };

    // 2. Launch 3 concurrent client threads
    const int ncli = 3;
    std::vector<std::thread> clients;
    std::vector<std::future<std::vector<int>>> futures;

    for (int i = 0; i < ncli; i++) {
        std::promise<std::vector<int>> prom;
        futures.push_back(prom.get_future());
        clients.emplace_back([&, i, p = std::move(prom)]() mutable {
            p.set_value(clientFunc(i));
        });
    }

    // 3. Main churn loop — 20 iterations of random failures
    for (int iters = 0; iters < 20; iters++) {
        if ((rand() % 1000) < 200) {
            int i = rand() % 5;
            cfg.disconnectServer(i);
        }
        if ((rand() % 1000) < 500) {
            int i = rand() % 5;
            if (!cfg.getRaft(i)) {
                cfg.startServer(i);
            }
            cfg.connectServer(i);
        }
        if ((rand() % 1000) < 200) {
            int i = rand() % 5;
            if (cfg.getRaft(i)) {
                cfg.crashServer(i);
            }
        }

        // Sleep a bit less than election timeout so the cluster stays lively 
        // Gives the Raft cluster some time to reaact but not long enough for the system to become completely stable.
        std::this_thread::sleep_for(RaftElectionTimeout * 7 / 10);
    }

    // 4. Stabilize the cluster, test gives the cluster one full election timeout to settle down.
    std::this_thread::sleep_for(RaftElectionTimeout);

    // 5. Switch to reliable network for the final check.
    cfg.setNetworkUnreliable(false);

    // Bring all servers back online.
    for (int i = 0; i < 5; i++) {
        if (!cfg.getRaft(i)) {
            cfg.startServer(i);
        }
        cfg.connectServer(i);
    }

    stop.store(1);

    // Collect all values submitted by the clients.
    std::vector<int> allValues;
    for (auto& f : futures) {
        auto v = f.get();
        allValues.insert(allValues.end(), v.begin(), v.end());
    }

    // Final command to make sure everything is committed.
    std::this_thread::sleep_for(RaftElectionTimeout);
    int lastIndex = cfg.one(std::to_string(rand() % 1000000), 5, true);

    // Verify that every value submitted by clients was eventually committed.
    for (int v1 : allValues) {
        bool found = false;
        for (int index = 1; index <= lastIndex; index++) {
            auto [nd, cmd] = cfg.nCommitted(index);
            if (nd > 0 && cmd == std::to_string(v1)) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("didn't find a value");
        }
    }

    cfg.end();

    // Join all client threads to prevent shutdown crash
    for (auto& t : clients) {
        if (t.joinable()) t.join();
    }
}

TEST(RaftTest3C, ReliableChurn) {
    Config cfg(5, false);   // reliable network
    internalChurn(cfg, false);
}

TEST(RaftTest3C, UnreliableChurn) {
    Config cfg(5, true);    // unreliable network
    internalChurn(cfg, true);
}
