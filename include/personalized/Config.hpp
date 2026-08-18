#pragma once

#include <cstdint>
#include <string>

namespace personalized {

struct Config {
    int         version                  = 2;
    bool        enabled                  = true;
    std::string seedSource               = "uuid";
    uint64_t    fixedSeed                = 0xDEADBEEFCAFEBABEULL;
    bool        mobModelSwapEnabled      = true;
    double      mobSwapIntensity         = 0.5;
    bool        inventoryScrambleEnabled = true;
    int         inventoryShufflePasses   = 3;
    bool        textureSwapEnabled       = true;
    double      textureSwapIntensity     = 0.3;
    bool        nametagScrambleEnabled   = true;
    bool        entityScaleEnabled       = true;
    bool        speedModificationEnabled = true;
    bool        verboseLogging           = true;
    bool        dryRun                   = false;
};

} // namespace personalized
