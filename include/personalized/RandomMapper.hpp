#pragma once

#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>

namespace personalized {

class RandomMapper {
public:
    RandomMapper(uint64_t seed, size_t size);
    size_t map(size_t i) const;
    size_t unmap(size_t j) const;
    size_t size() const { return m_size; }

    static std::vector<size_t> fisherYatesShuffle(uint64_t seed, size_t n, int passes = 1);

    static std::vector<size_t> partialScramble(
        uint64_t seed, size_t n, double intensity, int passes = 1
    );

private:
    size_t   m_size;
    uint64_t m_seed;
    static constexpr int kRounds = 6;
    uint64_t m_keys[kRounds]{};

    uint64_t feistelF(uint64_t right, uint64_t roundKey) const;
    size_t   feistelEncrypt(size_t index) const;
    size_t   feistelDecrypt(size_t index) const;
};

} // namespace personalized
