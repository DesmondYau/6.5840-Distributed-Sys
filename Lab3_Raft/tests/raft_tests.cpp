#include <gtest/gtest.h>
#include "../src/config.hpp"
#include <thread>
#include <chrono>

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

    /*
        - We keep on failing this test case because we implemented candidate to reject VoteRequest automatically.
        - When raft 2 (leader) and raft 1 are disconnected, raft 1 becomes a candidate. The same goes for raft 0
        - When raft 1 rejoins, both raft 0 and raft 1 are candidate and would not vote for each other -> split vote
        - Solved by implementing logger
    */
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

/*
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
        int xindex = cfg.one(index * 100, 3, false);

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
    cfg.one(99, 3, false);
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
*/
