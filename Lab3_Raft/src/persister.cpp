#include "persister.hpp"
#include <memory>


Persister::Persister(const Persister& persister)
{
    std::lock_guard<std::mutex> lock1(persister.m_mu);
    std::lock_guard<std::mutex> lock(m_mu);
    m_raftstate = persister.m_raftstate;        // vector performs deep copy when using = operator by default;
    m_snapshot = persister.m_snapshot;
}

Persister& Persister::operator=(const Persister& persister)
{
    if (this != &persister)
    {
        std::lock_guard<std::mutex> lock2(persister.m_mu);
        std::lock_guard<std::mutex> lock1(m_mu);
        m_raftstate = persister.m_raftstate;
        m_snapshot = persister.m_snapshot;
    }
    return *this;
}

void Persister::saveRaftState(const std::vector<uint8_t>& state)
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_raftstate = state;
}

std::vector<uint8_t> Persister::readRaftState()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_raftstate;
}

void Persister::saveStateAndSnapshot(const std::vector<uint8_t>& state, const std::vector<uint8_t>& snapshot)
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_raftstate = state;
    m_snapshot = snapshot;
}

std::vector<uint8_t> Persister::readSnapshot()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_snapshot;
}

size_t Persister::raftStateSize()
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_raftstate.size();
}

size_t Persister::snapshotSize() 
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_snapshot.size();
}


/*
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
*/