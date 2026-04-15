#pragma once
#include <vector>
#include <mutex>
#include <memory>

class Persister {
public:
    Persister() = default;

    // Copy constructor (deep copy)
    Persister(const Persister& persister);

    // Raft state
    void SaveRaftState(const std::vector<uint8_t>& state);
    std::vector<uint8_t> ReadRaftState();
    int RaftStateSize();

    // Snapshot
    void SaveStateAndSnapshot(const std::vector<uint8_t>& state,
                              const std::vector<uint8_t>& snapshot);
    std::vector<uint8_t> ReadSnapshot();
    int SnapshotSize();

private:
    std::mutex m_mu;
    std::vector<uint8_t> m_raftstate;
    std::vector<uint8_t> m_snapshot;
};