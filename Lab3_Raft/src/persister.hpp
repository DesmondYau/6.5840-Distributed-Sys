#pragma once
#include <vector>
#include <mutex>
#include <memory>

class Persister {
public:
    Persister() = default;

    // Copy constructor (deep copy)
    Persister(const Persister& persister);


    Persister& operator=(const Persister& persister);

    /*Operations for Raft state*/
    void saveRaftState(const std::vector<uint8_t>& state);
    std::vector<uint8_t> readRaftState();
    size_t raftStateSize();

    /* Operations for snapshots*/
    void saveStateAndSnapshot(const std::vector<uint8_t>& state, const std::vector<uint8_t>& snapshot);
    std::vector<uint8_t> readSnapshot();
    size_t snapshotSize();
    

private:
    mutable std::mutex m_mu;
    std::vector<uint8_t> m_raftstate;
    std::vector<uint8_t> m_snapshot;
};