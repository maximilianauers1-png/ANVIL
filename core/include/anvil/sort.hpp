// LSD radix sort of (key, value) pairs: O(n * bits/11), stable, no hashing.
// Used for edge pairing (validation, feature edges) and Morton-ordered BVHs.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace anvil {

/// Sorts `keys` ascending and permutes `vals` alongside. Only the lowest
/// `key_bits` bits of the keys are considered.
inline void radix_sort_pairs(std::vector<std::uint64_t>& keys, std::vector<std::uint32_t>& vals, int key_bits) {
    constexpr int kDigit = 11;
    constexpr std::size_t kBuckets = std::size_t(1) << kDigit;
    const std::size_t n = keys.size();
    std::vector<std::uint64_t> k2(n);
    std::vector<std::uint32_t> v2(n);
    for (int shift = 0; shift < key_bits; shift += kDigit) {
        std::array<std::size_t, kBuckets> count{};
        for (std::size_t i = 0; i < n; ++i) ++count[(keys[i] >> shift) & (kBuckets - 1)];
        std::size_t sum = 0;
        for (auto& c : count) {
            std::size_t t = c;
            c = sum;
            sum += t;
        }
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t d = (keys[i] >> shift) & (kBuckets - 1);
            std::size_t pos = count[d]++;
            k2[pos] = keys[i];
            v2[pos] = vals[i];
        }
        keys.swap(k2);
        vals.swap(v2);
    }
}

/// Number of bits needed to represent values < n (at least 1).
inline int bits_for(std::uint64_t n) {
    int b = 1;
    while (b < 64 && (std::uint64_t(1) << b) < n) ++b;
    return b;
}

}  // namespace anvil
