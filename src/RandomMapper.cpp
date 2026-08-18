#include "personalized/RandomMapper.hpp"
#include <android/log.h>
#include <cassert>

namespace personalized {

RandomMapper::RandomMapper(uint64_t seed, size_t size)
    : m_size(size), m_seed(seed)
{
    std::mt19937_64 rng(seed);
    for (int i = 0; i < kRounds; ++i)
        m_keys[i] = rng();
}

uint64_t RandomMapper::feistelF(uint64_t right, uint64_t roundKey) const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 8; ++i) {
        h ^= (right >> (i * 8)) & 0xFF;
        h *= 0x100000001b3ULL;
    }
    h ^= roundKey;
    h *= 0x100000001b3ULL;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

size_t RandomMapper::feistelEncrypt(size_t index) const {
    if (m_size <= 1) return index;
    int bits = 0;
    { size_t s = m_size - 1; while (s > 0) { s >>= 1; ++bits; } }
    int leftBits  = bits / 2;
    int rightBits = bits - leftBits;
    uint64_t leftMask  = (1ULL << leftBits) - 1;
    uint64_t rightMask = (1ULL << rightBits) - 1;
    uint64_t left  = (index >> rightBits) & leftMask;
    uint64_t right = index & rightMask;
    for (int i = 0; i < kRounds; ++i) {
        uint64_t newLeft = right;
        uint64_t newRight = left ^ (feistelF(right, m_keys[i]) & leftMask);
        left  = newLeft;
        right = newRight;
    }
    return (left << rightBits) | right;
}

size_t RandomMapper::feistelDecrypt(size_t index) const {
    if (m_size <= 1) return index;
    int bits = 0;
    { size_t s = m_size - 1; while (s > 0) { s >>= 1; ++bits; } }
    int leftBits  = bits / 2;
    int rightBits = bits - leftBits;
    uint64_t leftMask  = (1ULL << leftBits) - 1;
    uint64_t rightMask = (1ULL << rightBits) - 1;
    uint64_t left  = (index >> rightBits) & leftMask;
    uint64_t right = index & rightMask;
    for (int i = kRounds - 1; i >= 0; --i) {
        uint64_t newRight = left;
        uint64_t newLeft = right ^ (feistelF(left, m_keys[i]) & leftMask);
        left  = newLeft;
        right = newRight;
    }
    return (left << rightBits) | right;
}

size_t RandomMapper::map(size_t i) const {
    size_t result = feistelEncrypt(i);
    return result < m_size ? result : i;
}

size_t RandomMapper::unmap(size_t j) const {
    size_t result = feistelDecrypt(j);
    return result < m_size ? result : j;
}

std::vector<size_t> RandomMapper::fisherYatesShuffle(uint64_t seed, size_t n, int passes) {
    std::vector<size_t> v(n);
    std::iota(v.begin(), v.end(), size_t{0});
    std::mt19937_64 rng(seed);
    for (int p = 0; p < passes; ++p) {
        for (size_t i = n - 1; i > 0; --i) {
            std::uniform_int_distribution<size_t> dist(0, i);
            size_t j = dist(rng);
            std::swap(v[i], v[j]);
        }
    }
    return v;
}

std::vector<size_t> RandomMapper::partialScramble(
    uint64_t seed, size_t n, double intensity, int passes)
{
    std::vector<size_t> v(n);
    std::iota(v.begin(), v.end(), size_t{0});
    if (intensity <= 0.0) return v;
    std::mt19937_64 rng(seed ^ 0xDEADBEEF);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    for (int p = 0; p < passes; ++p) {
        for (size_t i = n - 1; i > 0; --i) {
            if (dist(rng) > intensity) continue;
            std::uniform_int_distribution<size_t> jDist(0, i);
            size_t j = jDist(rng);
            std::swap(v[i], v[j]);
        }
    }
    return v;
}

} // namespace personalized
