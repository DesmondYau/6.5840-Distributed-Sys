#pragma once

#include "model.hpp"
#include "checker.hpp"
#include "visualization.hpp" // Ensure visualization is visible to tests
#include <chrono>

namespace porcupine {

/**
 * @brief Main entry point for checking linearizability.
 * Matches the 4-argument signature used in lab4_test.cpp:
 * CheckOperations(model, history, verbose_flag, timeout)
 */
inline std::pair<CheckResult, LinearizationInfo> CheckOperations(
    const Model& model, 
    const std::vector<Operation>& history, 
    bool verbose = false, 
    std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) 
{
    // 1. Partition the history by key
    auto partitions = model.Partition(history);
    std::vector<std::vector<Entry>> mappedPartitions;
    
    // 2. Convert high-level Operations into Call/Return entries
    for (const auto& part : partitions) {
        mappedPartitions.push_back(makeEntries(part));
    }
    
    // 3. Run the parallel backtracking search (from checker.hpp)
    return checkParallel(model, mappedPartitions, verbose, timeout);
}

/**
 * @brief Convenience wrapper for simple true/false boolean checks.
 */
inline bool IsLinearizable(const Model& model, const std::vector<Operation>& history) {
    auto [res, _] = CheckOperations(model, history, false, std::chrono::milliseconds(0));
    return res == CheckResult::Ok;
}

// ---------------------------------------------------------------------------
// Event-based versions (For flat logs instead of paired operations)
// ---------------------------------------------------------------------------

inline std::pair<CheckResult, LinearizationInfo> CheckEvents(
    const Model& model, 
    const std::vector<Event>& history, 
    bool verbose = false, 
    std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) 
{
    auto partitions = model.PartitionEvent(history);
    std::vector<std::vector<Entry>> mappedPartitions;
    
    for (const auto& part : partitions) {
        mappedPartitions.push_back(convertEntries(part));
    }
    
    return checkParallel(model, mappedPartitions, verbose, timeout);
}

} // namespace porcupine