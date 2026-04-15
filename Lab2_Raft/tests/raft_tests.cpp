#include <gtest/gtest.h>
#include "../src/config.hpp"
#include <thread>
#include <chrono>

const std::chrono::milliseconds RaftElectionTimeout(1000);

TEST(RaftTest2A, InitialElection) {
    // Creates a COnfig harness with 3 Raft servers
    Config cfg(3, false);
    cfg.begin("Test (2A): initial election");

    // is a leader elected?
    int leader = cfg.checkOneLeader();
    ASSERT_GE(leader, 0);

    // sleep a bit to avoid racing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int term1 = cfg.checkTerms();
    ASSERT_GE(term1, 1);

    // does the leader+term stay the same if no failures?
    std::this_thread::sleep_for(2 * RaftElectionTimeout);
    int term2 = cfg.checkTerms();
    EXPECT_EQ(term1, term2);

    // there should still be a leader
    cfg.checkOneLeader();

    cfg.end();
}
