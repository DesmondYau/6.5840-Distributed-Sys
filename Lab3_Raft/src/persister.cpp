#include "persister.hpp"
#include <memory>


Persister::Persister(const Persister& persister) 
{
    // Fresh mutex would be constructed by default
    m_raftstate = persister.m_raftstate;
    m_snapshot = persister.m_snapshot; 
}

void Persister::SaveRaftState(const std::vector<uint8_t>& state) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_raftstate = state;
}

std::vector<uint8_t> Persister::ReadRaftState() {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_raftstate;
}

int Persister::RaftStateSize() {
    std::lock_guard<std::mutex> lock(m_mu);
    return static_cast<int>(m_raftstate.size());
}

void Persister::SaveStateAndSnapshot(const std::vector<uint8_t>& state,
                                     const std::vector<uint8_t>& snap) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_raftstate = state;
    m_snapshot = snap;
}

std::vector<uint8_t> Persister::ReadSnapshot() {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_snapshot;
}

int Persister::SnapshotSize() {
    std::lock_guard<std::mutex> lock(m_mu);
    return static_cast<int>(m_snapshot.size());
}
