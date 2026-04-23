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

    void saveRaftState(const std::vector<uint8_t>& state);
    std::vector<uint8_t> readRaftState();

    /*
    int RaftStateSize();

    
    void SaveStateAndSnapshot(const std::vector<uint8_t>& state,
                              const std::vector<uint8_t>& snapshot);
    std::vector<uint8_t> ReadSnapshot();
    int SnapshotSize();
    */

private:
    mutable std::mutex m_mu;
    std::vector<uint8_t> m_raftstate;
    std::vector<uint8_t> m_snapshot;
};