#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <mutex>

namespace personalized {

class SeedManager {
public:
    static SeedManager& instance();

    /// Initialize with a UUID string
    bool initializeWithUUID(const std::string& uuidStr);

    /// Use a fixed seed
    bool initializeWithFixedSeed(uint64_t seed);

    bool isInitialized() const;
    const std::string& getUUIDString() const;
    uint64_t getSeed() const;
    std::mt19937_64 createRNG() const;

private:
    SeedManager() = default;

    mutable std::mutex m_mutex;
    bool        m_initialized = false;
    std::string m_uuidString;
    uint64_t    m_seed = 0;

    static uint64_t deriveSeedFromUUID(const std::string& uuidStr);
};

} // namespace personalized
