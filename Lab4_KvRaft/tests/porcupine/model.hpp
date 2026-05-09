#pragma once

#include <any>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include "../src/models.hpp"   
#include "checker.hpp"         // Abstract Model definition

namespace models {

/**
 * @brief Concrete implementation of the Porcupine Model.
 * This class translates generic Porcupine operations (using std::any) into 
 * strongly-typed KV operations defined in models.hpp[cite: 8, 9].
 */
class KvModelWrapper : public porcupine::Model {
public:
    /**
     * @brief Returns the initial state of the system.
     * Maps to Go's Init func() interface{}[cite: 8].
     */
    std::any Init() const override {
        return KvModel::Init(); // Returns empty string
    }

    /**
     * @brief System step function[cite: 8].
     * Logic: (state, input, output) -> (is_valid, next_state)[cite: 8].
     */
    std::pair<bool, std::any> Step(const std::any& state, const std::any& input, const std::any& output) const override {
        // C++ equivalent of Go's type assertions[cite: 8, 9]
        auto st = std::any_cast<std::string>(state);
        auto inp = std::any_cast<KvInput>(input);
        auto out = std::any_cast<KvOutput>(output);
        
        auto [ok, next_state] = KvModel::Step(st, inp, out);
        return {ok, std::any(next_state)};
    }

    bool Equal(const std::any& state1, const std::any& state2) const override {
        // Cast the std::any states back to strings and compare them
        auto s1 = std::any_cast<std::string>(state1);
        auto s2 = std::any_cast<std::string>(state2);
        return s1 == s2;
    }

    /**
     * @brief Partitions the history by key for performance[cite: 8].
     * Porcupine checks each partition independently[cite: 8].
     */
    std::vector<std::vector<porcupine::Operation>> Partition(const std::vector<porcupine::Operation>& history) const override {
        std::map<std::string, std::vector<porcupine::Operation>> groups;
        for (const auto& op : history) {
            // Accessing the 'Input' member from the Operation struct defined in checker.hpp
            auto inp = std::any_cast<KvInput>(op.Input); 
            groups[inp.m_key].push_back(op);
        }
        
        std::vector<std::vector<porcupine::Operation>> result;
        for (auto const& [key, ops] : groups) {
            result.push_back(ops);
        }
        return result;
    }

    /**
     * @brief Optional visualization helper[cite: 8].
     * Note: If checker.hpp does not define this as virtual, remove 'override'.
     */
    std::string DescribeOperation(const std::any& input, const std::any& output) const override {
        auto inp = std::any_cast<KvInput>(input);
        auto out = std::any_cast<KvOutput>(output);
        return KvModel::DescribeOperation(inp, out);
    }

    virtual std::string DescribeState(const std::any& state) const override {
        auto st = std::any_cast<std::string>(state);
        return st; // For KV stores, the state is usually a string representation of the map
    }
};

/**
 * @brief Global instance for the linearizability checker.
 */
inline KvModelWrapper kvModelInstance;

} // namespace models