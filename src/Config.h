#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace personalized {

struct Config {
    int version = 1;

    // Global toggle
    bool enabled = true;

    // Seed source: "uuid", "fixed"
    std::string seedSource = "uuid";
    uint64_t fixedSeed = 0xDEADBEEFCAFEBABEULL;

    // Mob model swapping
    bool mobModelSwapEnabled = true;
    double mobSwapIntensity = 0.5;

    // Inventory scrambling
    bool inventoryScrambleEnabled = true;
    int inventoryShufflePasses = 3;

    // Debug
    bool verboseLogging = true;
    bool dryRun = false;
};

} // namespace personalized
