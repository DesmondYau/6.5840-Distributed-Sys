#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <numeric>   // for std::accumulate in popcnt if needed

class Bitset {
public:
    // Create a bitset that can hold at least 'bits' bits
    explicit Bitset(uint64_t bits = 0) {
        if (bits == 0) {
            data_.resize(1, 0);
            return;
        }
        size_t chunks = (bits + 63) / 64;   // ceiling division
        data_.resize(chunks, 0);
    }

    Bitset(const Bitset&) = default;
    Bitset& operator=(const Bitset&) = default;

    // Clone (same as Go's clone)
    Bitset clone() const {
        return *this;
    }

    // Set the bit at position 'pos'
    void set(uint64_t pos) {
        auto [major, minor] = index(pos);
        if (major >= data_.size()) {
            data_.resize(major + 1, 0);
        }
        data_[major] |= (uint64_t(1) << minor);
    }

    // Clear the bit at position 'pos'
    void clear(uint64_t pos) {
        auto [major, minor] = index(pos);
        if (major < data_.size()) {
            data_[major] &= ~(uint64_t(1) << minor);
        }
    }

    // Get the value of the bit at position 'pos'
    bool get(uint64_t pos) const {
        auto [major, minor] = index(pos);
        if (major >= data_.size()) return false;
        return (data_[major] & (uint64_t(1) << minor)) != 0;
    }

    // Count number of set bits (popcount)
    uint64_t popcnt() const {
        uint64_t total = 0;
        for (uint64_t word : data_) {
            total += __builtin_popcountll(word);   // very fast on gcc/clang
        }
        return total;
    }

    // Simple hash function used by the checker
    uint64_t hash() const {
        uint64_t h = popcnt();
        for (uint64_t word : data_) {
            h ^= word;
        }
        return h;
    }

    // Equality comparison
    bool equals(const Bitset& other) const {
        return data_ == other.data_;
    }

private:
    std::vector<uint64_t> data_;

    // Helper to compute word index and bit position
    static std::pair<uint64_t, uint64_t> index(uint64_t pos) {
        return {pos / 64, pos % 64};
    }
};