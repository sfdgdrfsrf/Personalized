#include "personalized/SeedManager.hpp"
#include <android/log.h>

#define LOG_TAG "Personalized"

namespace personalized {

SeedManager& SeedManager::instance() {
    static SeedManager inst;
    return inst;
}

bool SeedManager::initializeWithUUID(const std::string& uuidStr) {
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;
    m_uuidString = uuidStr;
    m_seed = deriveSeedFromUUID(uuidStr);
    m_initialized = true;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "SeedManager ready — UUID: %s, Seed: 0x%016lX", m_uuidString.c_str(), (unsigned long)m_seed);
    return true;
}

bool SeedManager::initializeWithFixedSeed(uint64_t seed) {
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;
    m_seed = seed;
    m_uuidString = "FIXED";
    m_initialized = true;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
        "SeedManager ready — fixed seed: 0x%016lX", (unsigned long)m_seed);
    return true;
}

bool SeedManager::isInitialized() const {
    std::lock_guard lock(m_mutex);
    return m_initialized;
}

const std::string& SeedManager::getUUIDString() const {
    std::lock_guard lock(m_mutex);
    return m_uuidString;
}

uint64_t SeedManager::getSeed() const {
    std::lock_guard lock(m_mutex);
    return m_seed;
}

std::mt19937_64 SeedManager::createRNG() const {
    std::lock_guard lock(m_mutex);
    return std::mt19937_64(m_seed);
}

uint64_t SeedManager::deriveSeedFromUUID(const std::string& uuidStr) {
    // FNV-1a 64-bit hash
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : uuidStr) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ULL;
    }
    // Murmur3 finalizer for avalanche
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

} // namespace personalized
