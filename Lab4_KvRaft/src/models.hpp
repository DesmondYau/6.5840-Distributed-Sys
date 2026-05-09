#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include "json.hpp"

namespace models 
{

// ===========================================================================
// RPC Argument and Reply Structures (Need Implementation)
// ===========================================================================

/**
 * @brief Arguments for the Get RPC.
 * Each request must include a ClientId and Seq number for duplicate detection.
 */
struct GetArgs 
{
    std::string m_key;
    uint64_t m_clientID;
    int m_seq;
};


struct GetReply 
{
    std::string m_value;
    std::string m_err;
};


/**
 * @brief Arguments for Put and Append RPCs.
 */
struct PutAppendArgs 
{
    std::string m_key;
    std::string m_value;
    std::string m_operation;
    uint64_t m_clientID;
    int m_seq;
};

struct PutAppendReply 
{
    std::string m_value;
    std::string m_err;
};


/**
 * @brief The command that gets stored in Raft's log
 */
struct Op 
{
    std::string m_operation;
    std::string m_key;
    std::string m_value;
    uint64_t m_clientID;
    int m_seq;
};


// ===========================================================================
// Porcupine Linearizability Model
// ===========================================================================

/**
 * @brief Input representation for the Porcupine checker.
 * This represents what hte client attempted to do
 */
struct KvInput 
{
    uint8_t m_operation; // 0 => Get, 1 => Put, 2 => Append
    std::string m_key;
    std::string m_value;
};

/**
 * @brief Output representation for the Porcupine checker.
 * Records exactly what the Raft cluster returned to the client at the end of the operation
 */
struct KvOutput 
{
    std::string m_value;
};

/**
 * @brief The Key-Value Model definition.
 * While our Raft cluster is a complex, multi-server system that can crash or have network delays,
 * the KvModel represents a perfect, single-threaded version of that same database
 * The linearizability checker (Porcupine) uses this model to verify if your distributed Raft cluster is behaving correctly by comparing its actual output to this idealized model
 */
struct KvModel 
{
    
    // 1. Init: Every key starts as an empty string ("")
    static std::string Init() 
    {
        return "";
    }

    // 2. Step function: The mathematical definition of correctness
    // Porcupine feeds it the state (the current string value of the key, Ground Truth), the input (the operation the client requested), and the output (what the server actually returned).
    // It returns a pair: 1) a boolean indicating if the transition is mathematically valid, and 2) the resulting new state
    static std::pair<bool, std::string> Step(const std::string& state, const KvInput& input, const KvOutput& output) 
    {
        if (input.m_operation == 0) 
        {
            // GET: The output value MUST match the current state.
            // State remains unchanged[cite: 1].
            return {output.m_value == state, state};
        } 
        else if (input.m_operation == 1) 
        {
            // PUT: Always valid. The new state becomes the input value
            return {true, input.m_value};
        } 
        else if (input.m_operation == 2) 
        {
            // APPEND: Always valid. New state = old state + input value
            return {true, state + input.m_value};
        } 
        else 
        {
            // APPEND with return value (matches the Go template's else block)
            return {output.m_value == state, state + input.m_value};
        }
    }

    // 3. DescribeOperation: Used by Porcupine to print a readable visual 
    // timeline if the linearizability check fails.
    static std::string DescribeOperation(const KvInput& input, const KvOutput& output) 
    {
        std::ostringstream oss;
        switch (input.m_operation) 
        {
            case 0:
                oss << "get('" << input.m_key << "') -> '" << output.m_value << "'";
                break;
            case 1:
                oss << "put('" << input.m_key << "', '" << input.m_value << "')";
                break;
            case 2:
                oss << "append('" << input.m_key << "', '" << input.m_value << "')";
                break;
            default:
                oss << "<invalid>";
                break;
        }
        return oss.str();
    }

    // 4. Partition: Splits the history of operations by Key
    // Porcupine can check each key's linearizability independently to save time
    template <typename OpType>
    static std::vector<std::vector<OpType>> Partition(const std::vector<OpType>& history) 
    {
        // std::map automatically sorts its elements by the Key, 
        // which completely replaces the need for Go's sort.Strings(keys)
        std::map<std::string, std::vector<OpType>> grouped_history;
        
        for (const auto& op : history) {
            grouped_history[op.m_input.m_key].push_back(op);
        }
        
        std::vector<std::vector<OpType>> ret;
        ret.reserve(grouped_history.size());
        
        for (auto const& [key, ops] : grouped_history) {
            ret.push_back(ops);
        }
        
        return ret;
    }
};

} // namespace models