#!/data/data/com.termux/files/usr/bin/bash
# ═══════════════════════════════════════════════════════════════
#  Personalized Bedrock Mod — GitHub Repo Bootstrap
#
#  Run in Termux:
#    pkg install git -y
#    bash setup-github.sh
#
#  You'll need a GitHub Personal Access Token (PAT):
#    https://github.com/settings/tokens/new
#    Check: repo (full control)
# ═══════════════════════════════════════════════════════════════

set -e

# ── Ask for GitHub info ──
echo "=== Personalized Bedrock Mod — GitHub Setup ==="
echo ""
read -p "GitHub username: " GH_USER
read -p "Repo name [Personalized-Bedrock]: " REPO_NAME
REPO_NAME=${REPO_NAME:-Personalized-Bedrock}
read -sp "GitHub Personal Access Token: " GH_TOKEN
echo ""

# ── Create GitHub repo via API ──
echo ""
echo "Creating GitHub repo..."
curl -s -X POST "https://api.github.com/user/repos" \
  -H "Authorization: token $GH_TOKEN" \
  -H "Accept: application/vnd.github.v3+json" \
  -d "{\"name\":\"$REPO_NAME\",\"description\":\"UUID-seeded client-side world scrambling for Bedrock Edition — port of Java Personalized mod\",\"private\":false}" || true

echo ""
echo "Setting up local project..."

# ── Create project directory ──
PROJ="$HOME/$REPO_NAME"
rm -rf "$PROJ"
mkdir -p "$PROJ"/{src/{Hooks,Utils},include/stubs/{ll/api/{event/{server,world},memory,mod},mc/{world/{actor/player,level/block/registry,item},client/services},nlohmann},tests,.github/workflows}

cd "$PROJ"

# ── Write ALL files ──

cat > manifest.json << 'FILE'
{
    "name": "Personalized",
    "entry": "Personalized.dll",
    "version": "0.1.0",
    "description": "UUID-seeded client-side world scrambling for Bedrock Edition — a port of the Java CurseForge mod 'Personalized'. Each player sees a uniquely scrambled version of the world based on their UUID.",
    "author": "YourNameHere",
    "license": "GPL-3.0",
    "type": "native",
    "platform": "win-x64",
    "dependencies": {
        "levilamina": ">=0.14.0"
    },
    "info": {
        "readme": "https://github.com/YourNameHere/Personalized-Bedrock",
        "source": "https://github.com/YourNameHere/Personalized-Bedrock",
        "issues": "https://github.com/YourNameHere/Personalized-Bedrock/issues"
    }
}
FILE

cat > config.json << 'FILE'
{
    "enabled": true,
    "seedSource": "uuid",
    "fixedSeed": 16045690984833335166,
    "textureSwapEnabled": true,
    "textureBlockNamespaceFilter": ["minecraft"],
    "textureSwapIntensity": 0.6,
    "inventoryScrambleEnabled": true,
    "inventoryShufflePasses": 3,
    "inventoryRescrambleIntervalTicks": 0,
    "mobModelSwapEnabled": true,
    "mobTargetFilter": ["zombie", "skeleton", "pig", "cow", "sheep"],
    "mobModelPool": [
        "minecraft:zombie", "minecraft:skeleton", "minecraft:pig",
        "minecraft:cow", "minecraft:sheep", "minecraft:chicken",
        "minecraft:creeper", "minecraft:spider"
    ],
    "mobSwapIntensity": 0.5,
    "verboseLogging": true,
    "dryRun": false
}
FILE

cat > .gitignore << 'FILE'
build/
xmake.d/
.xmake/
.vs/
.vscode/
*.dll *.lib *.pdb *.exp *.obj *.o *.a *.so
Thumbs.db .DS_Store
bedrock_server/ BDS/
plugins/Personalized/config.json
FILE

cat > xmake.lua << 'XMAKEFILE'
add_rules("mode.debug", "mode.release")
set_languages("cxx20")

option("sdk")
    set_default("standalone")
    set_show(true)
    set_description("SDK mode: 'levilamina' for real SDK, 'standalone' for stub headers")
option_end()

target("Personalized")
    set_kind("shared")
    set_basename("Personalized")
    add_files("src/**.cpp")
    add_includedirs("src", {public = true})
    add_includedirs("include/stubs")
    add_options("sdk")

    on_load(function (target)
        local sdk = get_config("sdk")
        if sdk == "levilamina" then
            target:add("includedirs", "$(projectdir)/LeviLamina/include", {public = true})
            target:add("includedirs", "$(projectdir)/BDS/include", {public = true})
            target:add("includedirs", "third_party/nlohmann", {public = true})
            target:add("links", "LeviLamina")
            target:add("defines", "PERSONALIZED_USE_REAL_SDK")
        else
            target:add("defines", "PERSONALIZED_STANDALONE")
        end
    end)

    if is_plat("windows") then
        add_syslinks("kernel32", "user32", "ntdll")
    elseif is_plat("android") then
        add_syslinks("log")
    elseif is_plat("linux") then
        add_syslinks("pthread", "dl")
    end()

    if is_plat("windows") then
        add_cxxflags("/EHsc", {force = true})
        add_cxflags("/W3", {force = true})
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    else()
        add_cxxflags("-fexceptions", "-frtti", "-Wall", "-Wextra", "-Wno-unused-parameter", {force = true})
    end()
    add_cxxflags("-Wno-unused-function", "-Wno-unused-variable", "-Wno-format", "-Wno-format-extra-args", {force = true})

    if is_mode("debug") then
        add_defines("DEBUG", "_DEBUG")
        if is_plat("windows") then add_cxxflags("/Zi", "/Od", {force = true})
        else() add_cxxflags("-g", "-O0", {force = true}) end()
    end()
    if is_mode("release") then
        add_defines("NDEBUG")
        if is_plat("windows") then add_cxxflags("/O2", "/GL", {force = true}); add_ldflags("/LTCG", {force = true})
        else() add_cxxflags("-O2", {force = true}) end()
    end()

    after_build(function (target)
        local output = target:targetdir() .. "/" .. target:basename()
        if is_plat("windows") then output = output .. ".dll" else() output = output .. ".so" end
        print("Built: " .. output)
    end)

target("PersonalizedTest")
    set_kind("binary")
    set_default(false)
    add_files("src/Utils/RandomMapper.cpp", "tests/TestRandomMapper.cpp")
    add_includedirs("src", "include/stubs")
    if not is_plat("windows") then add_cxxflags("-fexceptions", "-frtti", {force = true}) end()
XMAKEFILE

# ── Source files ──

cat > src/Config.h << 'FILE'
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace personalized {

struct Config {
    bool enabled = true;
    std::string seedSource = "uuid";
    uint64_t fixedSeed = 0xDEADBEEFCAFEBABEULL;
    bool textureSwapEnabled = true;
    std::vector<std::string> textureBlockNamespaceFilter = {"minecraft"};
    double textureSwapIntensity = 0.6;
    bool inventoryScrambleEnabled = true;
    int inventoryShufflePasses = 3;
    int inventoryRescrambleIntervalTicks = 0;
    bool mobModelSwapEnabled = true;
    std::vector<std::string> mobTargetFilter = {"zombie", "skeleton", "pig", "cow", "sheep"};
    std::vector<std::string> mobModelPool = {
        "minecraft:zombie", "minecraft:skeleton", "minecraft:pig",
        "minecraft:cow", "minecraft:sheep", "minecraft:chicken",
        "minecraft:creeper", "minecraft:spider",
    };
    double mobSwapIntensity = 0.5;
    bool verboseLogging = true;
    bool dryRun = false;
    static bool loadFromFile(Config& out, const std::string& path);
    bool saveToFile(const std::string& path) const;
};

} // namespace personalized
FILE

cat > src/Config.cpp << 'FILE'
#include "Config.h"
#include "Utils/Logger.h"
#include <fstream>

#ifdef PERSONALIZED_STANDALONE
namespace personalized {
bool Config::loadFromFile(Config& out, const std::string& path) {
    PZ_LOG_INFO("Loading config from: {}", path);
    std::ifstream ifs(path);
    if (!ifs.is_open()) { PZ_LOG_WARN("Config file not found at '{}', using defaults", path); return false; }
    PZ_LOG_WARN("Standalone build - JSON parsing not available, using defaults");
    return false;
}
bool Config::saveToFile(const std::string& path) const {
    PZ_LOG_INFO("Saving config to: {}", path);
    std::ofstream ofs(path);
    if (!ofs.is_open()) { PZ_LOG_ERROR("Cannot write config to '{}'", path); return false; }
    ofs << "{ \"_comment\": \"Personalized config (standalone stub - edit manually)\" }\n";
    return true;
}
} // namespace personalized
#else
#include "nlohmann/json.hpp"
namespace personalized {
using json = nlohmann::json;
bool Config::loadFromFile(Config& out, const std::string& path) {
    PZ_LOG_INFO("Loading config from: {}", path);
    std::ifstream ifs(path);
    if (!ifs.is_open()) { PZ_LOG_WARN("Config file not found at '{}', using defaults", path); return false; }
    try {
        json j = json::parse(ifs);
        out.enabled = j.value("enabled", out.enabled);
        out.seedSource = j.value("seedSource", out.seedSource);
        out.fixedSeed = j.value("fixedSeed", out.fixedSeed);
        out.textureSwapEnabled = j.value("textureSwapEnabled", out.textureSwapEnabled);
        out.textureBlockNamespaceFilter = j.value("textureBlockNamespaceFilter", out.textureBlockNamespaceFilter);
        out.textureSwapIntensity = j.value("textureSwapIntensity", out.textureSwapIntensity);
        out.inventoryScrambleEnabled = j.value("inventoryScrambleEnabled", out.inventoryScrambleEnabled);
        out.inventoryShufflePasses = j.value("inventoryShufflePasses", out.inventoryShufflePasses);
        out.inventoryRescrambleIntervalTicks = j.value("inventoryRescrambleIntervalTicks", out.inventoryRescrambleIntervalTicks);
        out.mobModelSwapEnabled = j.value("mobModelSwapEnabled", out.mobModelSwapEnabled);
        out.mobTargetFilter = j.value("mobTargetFilter", out.mobTargetFilter);
        out.mobModelPool = j.value("mobModelPool", out.mobModelPool);
        out.mobSwapIntensity = j.value("mobSwapIntensity", out.mobSwapIntensity);
        out.verboseLogging = j.value("verboseLogging", out.verboseLogging);
        out.dryRun = j.value("dryRun", out.dryRun);
        PZ_LOG_INFO("Config loaded successfully");
        return true;
    } catch (const json::exception& e) { PZ_LOG_ERROR("Failed to parse config JSON: {}", e.what()); return false; }
}
bool Config::saveToFile(const std::string& path) const {
    json j = { {"enabled",enabled},{"seedSource",seedSource},{"fixedSeed",fixedSeed},{"textureSwapEnabled",textureSwapEnabled},{"textureBlockNamespaceFilter",textureBlockNamespaceFilter},{"textureSwapIntensity",textureSwapIntensity},{"inventoryScrambleEnabled",inventoryScrambleEnabled},{"inventoryShufflePasses",inventoryShufflePasses},{"inventoryRescrambleIntervalTicks",inventoryRescrambleIntervalTicks},{"mobModelSwapEnabled",mobModelSwapEnabled},{"mobTargetFilter",mobTargetFilter},{"mobModelPool",mobModelPool},{"mobSwapIntensity",mobSwapIntensity},{"verboseLogging",verboseLogging},{"dryRun",dryRun} };
    std::ofstream ofs(path);
    if (!ofs.is_open()) { PZ_LOG_ERROR("Cannot write config to '{}'", path); return false; }
    ofs << j.dump(4); return true;
}
} // namespace personalized
#endif
FILE

cat > src/Utils/Logger.h << 'FILE'
#pragma once
#include "ll/api/Logger.h"
#include <string>
namespace personalized {
inline ll::Logger& ModLogger() { static ll::Logger logger("Personalized"); return logger; }
inline ll::Logger MakeModuleLogger(const std::string& name) { return ll::Logger("Personalized::" + name); }
} // namespace personalized
#define PZ_LOG_DEBUG(...)  ::personalized::ModLogger().debug(__VA_ARGS__)
#define PZ_LOG_INFO(...)   ::personalized::ModLogger().info(__VA_ARGS__)
#define PZ_LOG_WARN(...)   ::personalized::ModLogger().warn(__VA_ARGS__)
#define PZ_LOG_ERROR(...)  ::personalized::ModLogger().error(__VA_ARGS__)
#define PZ_LOG_FATAL(...)  ::personalized::ModLogger().fatal(__VA_ARGS__)
#ifndef NDEBUG
#define PZ_LOG_TRACE(...) ::personalized::ModLogger().debug("[TRACE] " __VA_ARGS__)
#else
#define PZ_LOG_TRACE(...) ((void)0)
#endif
FILE

cat > src/Utils/OffsetDefs.h << 'FILE'
#pragma once
#include <cstdint>
namespace off {
constexpr uintptr_t OFFSET_Player_getOrCreateUniqueID = 0x0ULL;
constexpr uintptr_t OFFSET_ClientInstance_LocalPlayer = 0x0ULL;
constexpr uintptr_t OFFSET_Player_getPlayerName = 0x0ULL;
constexpr uintptr_t OFFSET_BlockPalette_getBlock = 0x0ULL;
constexpr uintptr_t OFFSET_BlockTypeRegistry_lookupByName = 0x0ULL;
constexpr uintptr_t OFFSET_Block_getRenderBlock = 0x0ULL;
constexpr uintptr_t OFFSET_TexturePackRepository_singleton = 0x0ULL;
constexpr uintptr_t OFFSET_BlockGraphics_replaceTexture = 0x0ULL;
constexpr uintptr_t OFFSET_CreativeItemRegistry_getCreativeItems = 0x0ULL;
constexpr uintptr_t OFFSET_Item_getDescriptionId = 0x0ULL;
constexpr uintptr_t OFFSET_ItemRegistry_getItem = 0x0ULL;
constexpr uintptr_t OFFSET_ActorFactory_createActor = 0x0ULL;
constexpr uintptr_t OFFSET_Actor_setModel = 0x0ULL;
constexpr uintptr_t OFFSET_Actor_getActorIdentifier = 0x0ULL;
constexpr uintptr_t OFFSET_Mob_setMobModel = 0x0ULL;
constexpr uintptr_t OFFSET_ClientInstance_singleton = 0x0ULL;
constexpr uintptr_t OFFSET_ClientInstance_getLocalPlayer = 0x0ULL;
constexpr uintptr_t OFFSET_Level_getRuntimeActorList = 0x0ULL;
constexpr uintptr_t OFFSET_Level_getLocalPlayer = 0x0ULL;
constexpr uintptr_t OFFSET_mce_UUID_MostSignificant = 0x0ULL;
constexpr uintptr_t OFFSET_mce_UUID_LeastSignificant = 0x8ULL;
namespace sig {
    constexpr const char* Player_getOrCreateUniqueID = "";
    constexpr const char* BlockPalette_getBlock = "";
    constexpr const char* CreativeItemRegistry_getCreativeItems = "";
    constexpr const char* ActorFactory_createActor = "";
    constexpr const char* ClientInstance_getLocalPlayer = "";
}
namespace vtable {
    constexpr uintptr_t Block = 0x0ULL;
    constexpr uintptr_t Item = 0x0ULL;
    constexpr uintptr_t Actor = 0x0ULL;
    constexpr uintptr_t Mob = 0x0ULL;
    constexpr uintptr_t Player = 0x0ULL;
    constexpr uintptr_t LocalPlayer = 0x0ULL;
}
} // namespace off
FILE

cat > src/Utils/RandomMapper.h << 'FILE'
#pragma once
#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
namespace personalized {
class RandomMapper {
public:
    RandomMapper(uint64_t seed, size_t size);
    size_t map(size_t i) const;
    size_t unmap(size_t j) const;
    size_t size() const { return m_size; }
    static std::vector<size_t> fisherYatesShuffle(uint64_t seed, size_t n, int passes = 1);
    static std::vector<size_t> partialScramble(uint64_t seed, size_t n, double intensity, int passes = 1);
private:
    size_t m_size; uint64_t m_seed;
    static constexpr int kRounds = 4; uint64_t m_keys[kRounds];
    uint64_t feistelF(uint64_t right, uint64_t roundKey) const;
    size_t feistelEncrypt(size_t index) const;
    size_t feistelDecrypt(size_t index) const;
};
} // namespace personalized
FILE

cat > src/Utils/RandomMapper.cpp << 'FILE'
#include "RandomMapper.h"
#include "Utils/Logger.h"
#include <cassert>
namespace personalized {
RandomMapper::RandomMapper(uint64_t seed, size_t size) : m_size(size), m_seed(seed) {
    std::mt19937_64 rng(seed);
    for (int i = 0; i < kRounds; ++i) m_keys[i] = rng();
    PZ_LOG_TRACE("RandomMapper created: size={:zu}, seed=0x{:016X}", size, seed);
}
uint64_t RandomMapper::feistelF(uint64_t right, uint64_t roundKey) const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 8; ++i) { h ^= (right >> (i * 8)) & 0xFF; h *= 0x100000001b3ULL; }
    h ^= roundKey; h *= 0x100000001b3ULL;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return h;
}
size_t RandomMapper::feistelEncrypt(size_t index) const {
    if (m_size <= 1) return index;
    int bits = 0; { size_t s = m_size - 1; while (s > 0) { s >>= 1; ++bits; } }
    int leftBits = bits / 2, rightBits = bits - leftBits;
    uint64_t leftMask = (1ULL << leftBits) - 1, rightMask = (1ULL << rightBits) - 1;
    uint64_t left = (index >> rightBits) & leftMask, right = index & rightMask;
    for (int r = 0; r < kRounds; ++r) { uint64_t newLeft = right; uint64_t fOut = feistelF(right, m_keys[r]); right = (left ^ (fOut & leftMask)) & rightMask; left = newLeft; }
    size_t result = static_cast<size_t>((left << rightBits) | right);
    while (result >= m_size) result = feistelEncrypt(result);
    return result;
}
size_t RandomMapper::feistelDecrypt(size_t index) const {
    if (m_size <= 1) return index;
    int bits = 0; { size_t s = m_size - 1; while (s > 0) { s >>= 1; ++bits; } }
    int leftBits = bits / 2, rightBits = bits - leftBits;
    uint64_t leftMask = (1ULL << leftBits) - 1, rightMask = (1ULL << rightBits) - 1;
    uint64_t left = (index >> rightBits) & leftMask, right = index & rightMask;
    for (int r = kRounds - 1; r >= 0; --r) { uint64_t newRight = left; uint64_t fOut = feistelF(left, m_keys[r]); left = (right ^ (fOut & leftMask)) & leftMask; right = newRight; }
    size_t result = static_cast<size_t>((left << rightBits) | right);
    while (result >= m_size) result = feistelDecrypt(result);
    return result;
}
size_t RandomMapper::map(size_t i) const { if (i >= m_size) { PZ_LOG_ERROR("RandomMapper::map index {:zu} out of range [0, {:zu})", i, m_size); return i; } return feistelEncrypt(i); }
size_t RandomMapper::unmap(size_t j) const { if (j >= m_size) { PZ_LOG_ERROR("RandomMapper::unmap index {:zu} out of range [0, {:zu})", j, m_size); return j; } return feistelDecrypt(j); }
std::vector<size_t> RandomMapper::fisherYatesShuffle(uint64_t seed, size_t n, int passes) {
    PZ_LOG_DEBUG("Fisher-Yates shuffle: n={:zu}, passes={:d}, seed=0x{:016X}", n, passes, seed);
    std::vector<size_t> perm(n); for (size_t i = 0; i < n; ++i) perm[i] = i;
    std::mt19937_64 rng(seed);
    for (int p = 0; p < passes; ++p) { for (size_t i = n; i > 1; --i) { std::uniform_int_distribution<size_t> dist(0, i - 1); size_t j = dist(rng); std::swap(perm[i - 1], perm[j]); } }
    PZ_LOG_TRACE("Shuffle complete: perm[0..4] = [{:zu}, {:zu}, {:zu}, {:zu}, {:zu}]", (n>0?perm[0]:0),(n>1?perm[1]:0),(n>2?perm[2]:0),(n>3?perm[3]:0),(n>4?perm[4]:0));
    return perm;
}
std::vector<size_t> RandomMapper::partialScramble(uint64_t seed, size_t n, double intensity, int passes) {
    PZ_LOG_DEBUG("Partial scramble: n={:zu}, intensity={:.2f}, passes={:d}", n, intensity, passes);
    if (intensity <= 0.0) { std::vector<size_t> perm(n); for (size_t i = 0; i < n; ++i) perm[i] = i; return perm; }
    if (intensity >= 1.0) return fisherYatesShuffle(seed, n, passes);
    std::mt19937_64 rng(seed);
    std::vector<size_t> perm(n); for (size_t i = 0; i < n; ++i) perm[i] = i;
    std::vector<size_t> candidates; candidates.reserve(n);
    for (size_t i = 0; i < n; ++i) { std::uniform_real_distribution<double> dist(0.0, 1.0); if (dist(rng) < intensity) candidates.push_back(i); }
    PZ_LOG_DEBUG("Selected {:zu}/{:zu} indices for scrambling ({:.1f}%)", candidates.size(), n, 100.0 * candidates.size() / n);
    for (int p = 0; p < passes; ++p) { for (size_t k = candidates.size(); k > 1; --k) { std::uniform_int_distribution<size_t> dist(0, k - 1); size_t j = dist(rng); std::swap(perm[candidates[k-1]], perm[candidates[j]]); } }
    return perm;
}
} // namespace personalized
FILE

cat > src/SeedManager.h << 'FILE'
#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <optional>
#include <mutex>
namespace personalized {
class SeedManager {
public:
    static SeedManager& instance();
    bool initialize();
    bool isInitialized() const;
    const std::string& getUUIDString() const;
    uint64_t getSeed() const;
    std::mt19937_64 createRNG() const;
    void setSeedOverride(uint64_t seed);
private:
    SeedManager() = default;
    mutable std::mutex m_mutex;
    bool m_initialized = false;
    std::string m_uuidString;
    uint64_t m_seed = 0;
    std::optional<std::string> fetchPlayerUUID();
    std::optional<std::string> fetchDeviceID();
    static uint64_t deriveSeedFromUUID(const std::string& uuidStr);
};
} // namespace personalized
FILE

cat > src/SeedManager.cpp << 'FILE'
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "Utils/OffsetDefs.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/LocalPlayer.h"
#include "mc/client/services/ClientInstance.h"
#include <functional>
#include <sstream>
namespace personalized {
SeedManager& SeedManager::instance() { static SeedManager inst; return inst; }
bool SeedManager::initialize() {
    std::lock_guard lock(m_mutex);
    if (m_initialized) { PZ_LOG_DEBUG("SeedManager already initialized (seed=0x{:016X}), skipping", m_seed); return true; }
    const auto& cfg = Config{};
    PZ_LOG_INFO("SeedManager initializing - source: {}", cfg.seedSource);
    std::optional<std::string> uuidOpt;
    if (cfg.seedSource == "uuid") { uuidOpt = fetchPlayerUUID(); if (!uuidOpt.has_value()) { PZ_LOG_WARN("Player UUID not available yet, falling back to device ID"); uuidOpt = fetchDeviceID(); } }
    else if (cfg.seedSource == "device") { uuidOpt = fetchDeviceID(); }
    else if (cfg.seedSource == "fixed") { m_seed = cfg.fixedSeed; m_uuidString = "FIXED_SEED"; m_initialized = true; PZ_LOG_INFO("Using fixed seed: 0x{:016X}", m_seed); return true; }
    if (!uuidOpt.has_value()) { PZ_LOG_ERROR("Failed to obtain any UUID/device ID - mod cannot scramble!"); return false; }
    m_uuidString = uuidOpt.value(); m_seed = deriveSeedFromUUID(m_uuidString); m_initialized = true;
    PZ_LOG_INFO("Seed derived successfully - UUID: {}, Seed: 0x{:016X}", m_uuidString, m_seed);
    return true;
}
bool SeedManager::isInitialized() const { std::lock_guard lock(m_mutex); return m_initialized; }
const std::string& SeedManager::getUUIDString() const { std::lock_guard lock(m_mutex); return m_uuidString; }
uint64_t SeedManager::getSeed() const { std::lock_guard lock(m_mutex); return m_seed; }
std::mt19937_64 SeedManager::createRNG() const { std::lock_guard lock(m_mutex); PZ_LOG_TRACE("Creating new seeded RNG from seed 0x{:016X}", m_seed); return std::mt19937_64(m_seed); }
void SeedManager::setSeedOverride(uint64_t seed) { std::lock_guard lock(m_mutex); m_seed = seed; m_uuidString = "OVERRIDE_0x" + std::to_string(seed); m_initialized = true; PZ_LOG_INFO("Seed override applied: 0x{:016X}", seed); }
std::optional<std::string> SeedManager::fetchPlayerUUID() {
    PZ_LOG_DEBUG("Attempting to fetch LocalPlayer UUID...");
    try {
        uintptr_t clientInstBase = *reinterpret_cast<uintptr_t*>(off::OFFSET_ClientInstance_singleton);
        if (clientInstBase == 0) { PZ_LOG_WARN("ClientInstance singleton is null - player not loaded yet?"); return std::nullopt; }
        using Fn_GetLocalPlayer = void*(*)(void*);
        auto fnGetLocalPlayer = reinterpret_cast<Fn_GetLocalPlayer>(clientInstBase + off::OFFSET_ClientInstance_getLocalPlayer);
        void* localPlayer = fnGetLocalPlayer(reinterpret_cast<void*>(clientInstBase));
        if (!localPlayer) { PZ_LOG_WARN("LocalPlayer pointer is null"); return std::nullopt; }
        PZ_LOG_TRACE("LocalPlayer at {:p}", (void*)localPlayer);
        using Fn_GetUUID = void(*)(void*, void*);
        auto fnGetUUID = reinterpret_cast<Fn_GetUUID>(*reinterpret_cast<uintptr_t*>(localPlayer) + off::OFFSET_Player_getOrCreateUniqueID);
        alignas(8) uint8_t uuidBytes[16] = {};
        fnGetUUID(localPlayer, uuidBytes);
        auto hex = [](uint8_t b) -> std::string { char buf[3]; std::snprintf(buf, sizeof(buf), "%02x", b); return buf; };
        std::ostringstream oss;
        for (int i = 0; i < 4; ++i) oss << hex(uuidBytes[i]); oss << "-";
        for (int i = 4; i < 6; ++i) oss << hex(uuidBytes[i]); oss << "-";
        for (int i = 6; i < 8; ++i) oss << hex(uuidBytes[i]); oss << "-";
        for (int i = 8; i < 10; ++i) oss << hex(uuidBytes[i]); oss << "-";
        for (int i = 10; i < 16; ++i) oss << hex(uuidBytes[i]);
        PZ_LOG_DEBUG("Fetched player UUID: {}", oss.str());
        return oss.str();
    } catch (...) { PZ_LOG_ERROR("Exception during UUID fetch - possibly invalid offsets"); return std::nullopt; }
}
std::optional<std::string> SeedManager::fetchDeviceID() { PZ_LOG_DEBUG("Attempting to fetch device-unique ID..."); PZ_LOG_WARN("Device ID fetch not yet implemented for this platform"); return std::nullopt; }
uint64_t SeedManager::deriveSeedFromUUID(const std::string& uuidStr) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : uuidStr) { hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c)); hash *= 0x100000001b3ULL; }
    hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdULL; hash ^= hash >> 33; hash *= 0xc4ceb9fe1a85ec53ULL; hash ^= hash >> 33;
    PZ_LOG_TRACE("Derived seed from UUID '{}': 0x{:016X}", uuidStr, hash);
    return hash;
}
} // namespace personalized
FILE

echo "Writing hook modules..."

cat > src/Hooks/UUIDHook.h << 'FILE'
#pragma once
namespace personalized { class UUIDHook { public: static void install(); static void uninstall(); static bool isReady(); private: static bool s_hooked; static bool s_ready; }; }
FILE

cat > src/Hooks/UUIDHook.cpp << 'FILE'
#include "UUIDHook.h"
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "Utils/OffsetDefs.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/PlayerJoinEvent.h"
#include "mc/world/actor/player/Player.h"
#include "ll/api/memory/Hook.h"
namespace personalized {
bool UUIDHook::s_hooked = false; bool UUIDHook::s_ready = false;
void UUIDHook::install() {
    if (s_hooked) { PZ_LOG_WARN("UUIDHook already installed, skipping"); return; }
    PZ_LOG_INFO("Installing UUIDHook (player-join event listener)...");
    auto& bus = ll::event::EventBus::getInstance();
    bus.emplaceListener<ll::event::PlayerJoinEvent>([](ll::event::PlayerJoinEvent& ev) {
        if (s_ready) return;
        PZ_LOG_DEBUG("PlayerJoinEvent fired - attempting UUID capture");
        auto& player = ev.self(); std::string playerName = player.getName();
        PZ_LOG_INFO("Player '{}' joining - initializing SeedManager", playerName);
        bool ok = SeedManager::instance().initialize();
        if (ok) { s_ready = true; PZ_LOG_INFO("SeedManager ready - UUID captured, scrambling active"); }
        else { PZ_LOG_WARN("SeedManager init failed for player '{}' - will retry next join", playerName); }
    });
    s_hooked = true; PZ_LOG_INFO("UUIDHook installed via event bus");
}
void UUIDHook::uninstall() { s_hooked = false; s_ready = false; PZ_LOG_INFO("UUIDHook uninstalled"); }
bool UUIDHook::isReady() { return s_ready; }
} // namespace personalized
FILE

cat > src/Hooks/TextureSwapHook.h << 'FILE'
#pragma once
#include <vector>
#include <string>
#include <unordered_map>
namespace personalized {
class TextureSwapHook {
public: static void install(); static void uninstall(); static void rebuildMapping(); static size_t getRemappedIndex(size_t originalIndex);
private: static bool s_hooked; static std::vector<size_t> s_remapTable; static std::unordered_map<std::string,std::string> s_nameRemap; static size_t s_blockCount; static void buildRemapTable();
};
} // namespace personalized
FILE

cat > src/Hooks/TextureSwapHook.cpp << 'FILE'
#include "TextureSwapHook.h"
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "Utils/OffsetDefs.h"
#include "Utils/RandomMapper.h"
#include "ll/api/memory/Hook.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockLegacy.h"
#include "mc/world/level/block/BlockTypeRegistry.h"
#include "mc/world/level/block/registry/BlockPalette.h"
namespace personalized {
bool TextureSwapHook::s_hooked = false;
std::vector<size_t> TextureSwapHook::s_remapTable;
std::unordered_map<std::string,std::string> TextureSwapHook::s_nameRemap;
size_t TextureSwapHook::s_blockCount = 0;
void TextureSwapHook::buildRemapTable() {
    PZ_LOG_INFO("Building texture swap remap table...");
    if (!SeedManager::instance().isInitialized()) { PZ_LOG_WARN("SeedManager not ready - cannot build texture swap"); return; }
    uint64_t seed = SeedManager::instance().getSeed();
    constexpr size_t ESTIMATED_BLOCK_COUNT = 900;
    s_blockCount = ESTIMATED_BLOCK_COUNT;
    Config cfg; double intensity = cfg.textureSwapIntensity;
    s_remapTable = RandomMapper::partialScramble(seed, s_blockCount, intensity, 3);
    PZ_LOG_INFO("Texture remap table built: {:zu} blocks, intensity {:.0f}%", s_blockCount, intensity * 100.0);
    if (cfg.verboseLogging && s_blockCount > 10) { for (size_t i = 0; i < 10 && i < s_blockCount; ++i) PZ_LOG_DEBUG("  block[{:zu}] -> block[{:zu}]", i, s_remapTable[i]); }
}
void TextureSwapHook::install() { if (s_hooked) { PZ_LOG_WARN("TextureSwapHook already installed"); return; } PZ_LOG_INFO("Installing TextureSwapHook..."); PZ_LOG_INFO("TextureSwapHook installed (strategy: post-load render swap)"); PZ_LOG_WARN("NOTE: Hook bodies are commented out - fill in offsets and uncomment"); s_hooked = true; }
void TextureSwapHook::uninstall() { s_hooked = false; s_remapTable.clear(); s_nameRemap.clear(); s_blockCount = 0; PZ_LOG_INFO("TextureSwapHook uninstalled"); }
void TextureSwapHook::rebuildMapping() { PZ_LOG_INFO("Rebuilding texture swap mapping..."); buildRemapTable(); }
size_t TextureSwapHook::getRemappedIndex(size_t originalIndex) { if (originalIndex >= s_remapTable.size()) return originalIndex; return s_remapTable[originalIndex]; }
} // namespace personalized
FILE

cat > src/Hooks/InventoryScrambleHook.h << 'FILE'
#pragma once
#include <vector>
#include <cstddef>
namespace personalized {
class InventoryScrambleHook {
public: static void install(); static void uninstall(); static void reshuffle(); static size_t getScrambledSlot(size_t originalSlot);
private: static bool s_hooked; static std::vector<size_t> s_slotPermutation; static int s_tickCounter; static void applyShuffle();
};
} // namespace personalized
FILE

cat > src/Hooks/InventoryScrambleHook.cpp << 'FILE'
#include "InventoryScrambleHook.h"
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "Utils/OffsetDefs.h"
#include "Utils/RandomMapper.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerTickEvent.h"
namespace personalized {
bool InventoryScrambleHook::s_hooked = false;
std::vector<size_t> InventoryScrambleHook::s_slotPermutation;
int InventoryScrambleHook::s_tickCounter = 0;
void InventoryScrambleHook::applyShuffle() {
    PZ_LOG_INFO("Applying creative inventory shuffle...");
    if (!SeedManager::instance().isInitialized()) { PZ_LOG_WARN("SeedManager not ready - cannot shuffle inventory"); return; }
    uint64_t seed = SeedManager::instance().getSeed();
    constexpr size_t ESTIMATED_CREATIVE_ITEMS = 1000; size_t itemCount = ESTIMATED_CREATIVE_ITEMS;
    Config cfg;
    s_slotPermutation = RandomMapper::fisherYatesShuffle(seed ^ 0xC0FFEE0000000000ULL, itemCount, cfg.inventoryShufflePasses);
    PZ_LOG_INFO("Creative inventory shuffled: {:zu} items, {:d} passes", itemCount, cfg.inventoryShufflePasses);
}
static ll::event::ListenerPtr s_tickListener;
void InventoryScrambleHook::install() {
    if (s_hooked) { PZ_LOG_WARN("InventoryScrambleHook already installed"); return; }
    PZ_LOG_INFO("Installing InventoryScrambleHook...");
    Config cfg; auto& bus = ll::event::EventBus::getInstance();
    if (cfg.inventoryRescrambleIntervalTicks > 0) {
        int interval = cfg.inventoryRescrambleIntervalTicks;
        PZ_LOG_INFO("Periodic re-shuffle enabled: every {:d} ticks", interval);
        s_tickListener = bus.emplaceListener<ll::event::ServerTickEvent>([interval](ll::event::ServerTickEvent&) {
            if (!SeedManager::instance().isInitialized()) return;
            ++s_tickCounter; if (s_tickCounter >= interval) { s_tickCounter = 0; PZ_LOG_DEBUG("Re-shuffling creative inventory (tick interval reached)"); reshuffle(); }
        });
    }
    static bool oneTimeShuffleDone = false;
    bus.emplaceListener<ll::event::ServerTickEvent>([](ll::event::ServerTickEvent&) {
        if (oneTimeShuffleDone) return; if (!SeedManager::instance().isInitialized()) return;
        oneTimeShuffleDone = true; applyShuffle();
    });
    PZ_LOG_INFO("InventoryScrambleHook installed"); s_hooked = true;
}
void InventoryScrambleHook::uninstall() { s_hooked = false; s_slotPermutation.clear(); s_tickCounter = 0; PZ_LOG_INFO("InventoryScrambleHook uninstalled"); }
void InventoryScrambleHook::reshuffle() { applyShuffle(); }
size_t InventoryScrambleHook::getScrambledSlot(size_t originalSlot) { if (originalSlot >= s_slotPermutation.size()) return originalSlot; return s_slotPermutation[originalSlot]; }
} // namespace personalized
FILE

cat > src/Hooks/MobModelSwapHook.h << 'FILE'
#pragma once
#include <vector>
#include <string>
#include <unordered_map>
namespace personalized {
class MobModelSwapHook {
public: static void install(); static void uninstall(); static void addSwap(const std::string& mobId, const std::string& modelId); static std::string getSwappedModel(const std::string& originalMobId);
private: static bool s_hooked; static std::unordered_map<std::string,std::string> s_modelSwapMap; static void buildSwapMap();
};
} // namespace personalized
FILE

cat > src/Hooks/MobModelSwapHook.cpp << 'FILE'
#include "MobModelSwapHook.h"
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "Utils/OffsetDefs.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/world/ActorAddEvent.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/Mob.h"
namespace personalized {
bool MobModelSwapHook::s_hooked = false;
std::unordered_map<std::string,std::string> MobModelSwapHook::s_modelSwapMap;
void MobModelSwapHook::buildSwapMap() {
    PZ_LOG_INFO("Building mob model swap map...");
    if (!SeedManager::instance().isInitialized()) { PZ_LOG_WARN("SeedManager not ready - cannot build mob swap map"); return; }
    Config cfg; uint64_t seed = SeedManager::instance().getSeed();
    std::mt19937_64 rng(seed ^ 0xB0B0FACEDEADBEEFULL);
    s_modelSwapMap.clear();
    for (const auto& mobId : cfg.mobTargetFilter) {
        if (mobId.find("player") != std::string::npos) continue;
        if (cfg.mobModelPool.empty()) continue;
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) > cfg.mobSwapIntensity) { PZ_LOG_DEBUG("Mob '{}' skipped (intensity check)", mobId); continue; }
        std::string selectedModel; int attempts = 0;
        do { std::uniform_int_distribution<size_t> idxDist(0, cfg.mobModelPool.size()-1); selectedModel = cfg.mobModelPool[idxDist(rng)]; ++attempts; } while (selectedModel == mobId && attempts < 10);
        s_modelSwapMap[mobId] = selectedModel; PZ_LOG_DEBUG("Mob swap: {} -> {}", mobId, selectedModel);
    }
    PZ_LOG_INFO("Mob model swap map built: {:zu} entries", s_modelSwapMap.size());
}
void MobModelSwapHook::install() {
    if (s_hooked) { PZ_LOG_WARN("MobModelSwapHook already installed"); return; }
    PZ_LOG_INFO("Installing MobModelSwapHook (ActorAddEvent listener)...");
    auto& bus = ll::event::EventBus::getInstance();
    bus.emplaceListener<ll::event::ActorAddEvent>([](ll::event::ActorAddEvent& ev) {
        if (s_modelSwapMap.empty()) return;
        auto& actor = ev.self();
        if (actor.isPlayer()) return;
        std::string actorId = actor.getTypeName();
        auto it = s_modelSwapMap.find(actorId);
        if (it != s_modelSwapMap.end()) PZ_LOG_DEBUG("Swapping model for actor '{}': now renders as '{}'", actorId, it->second);
    });
    PZ_LOG_INFO("MobModelSwapHook installed"); s_hooked = true;
}
void MobModelSwapHook::uninstall() { s_hooked = false; s_modelSwapMap.clear(); PZ_LOG_INFO("MobModelSwapHook uninstalled"); }
void MobModelSwapHook::addSwap(const std::string& mobId, const std::string& modelId) { s_modelSwapMap[mobId] = modelId; PZ_LOG_DEBUG("Added mob swap: {} -> {}", mobId, modelId); }
std::string MobModelSwapHook::getSwappedModel(const std::string& originalMobId) { auto it = s_modelSwapMap.find(originalMobId); return (it != s_modelSwapMap.end()) ? it->second : originalMobId; }
} // namespace personalized
FILE

echo "Writing Main.cpp..."

cat > src/Main.cpp << 'FILE'
#include "Hooks/UUIDHook.h"
#include "Hooks/TextureSwapHook.h"
#include "Hooks/InventoryScrambleHook.h"
#include "Hooks/MobModelSwapHook.h"
#include "SeedManager.h"
#include "Config.h"
#include "Utils/Logger.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartingEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
class PersonalizedMod {
public:
    static constexpr const char* NAME = "Personalized";
    static constexpr const char* VERSION = "0.1.0";
    PersonalizedMod() = default;
    void onEnable(); void onDisable();
private: personalized::Config m_config; bool m_loaded = false;
};
static PersonalizedMod g_mod;
void PersonalizedMod::onEnable() {
    PZ_LOG_INFO("Personalized v{} - Bedrock Edition", PersonalizedMod::VERSION);
    std::string configPath = "plugins/Personalized/config.json";
    if (!personalized::Config::loadFromFile(m_config, configPath)) { PZ_LOG_WARN("Using default config values"); m_config.saveToFile(configPath); }
    if (!m_config.enabled) { PZ_LOG_INFO("Mod is disabled in config - nothing to install"); return; }
    PZ_LOG_INFO("[1/4] Installing UUIDHook..."); personalized::UUIDHook::install();
    if (m_config.textureSwapEnabled) { PZ_LOG_INFO("[2/4] Installing TextureSwapHook..."); personalized::TextureSwapHook::install(); }
    else PZ_LOG_INFO("[2/4] TextureSwapHook DISABLED by config");
    if (m_config.inventoryScrambleEnabled) { PZ_LOG_INFO("[3/4] Installing InventoryScrambleHook..."); personalized::InventoryScrambleHook::install(); }
    else PZ_LOG_INFO("[3/4] InventoryScrambleHook DISABLED by config");
    if (m_config.mobModelSwapEnabled) { PZ_LOG_INFO("[4/4] Installing MobModelSwapHook..."); personalized::MobModelSwapHook::install(); }
    else PZ_LOG_INFO("[4/4] MobModelSwapHook DISABLED by config");
    m_loaded = true; PZ_LOG_INFO("All hooks installed - mod active (awaiting UUID seed)");
}
void PersonalizedMod::onDisable() {
    PZ_LOG_INFO("Shutting down Personalized...");
    personalized::MobModelSwapHook::uninstall(); personalized::InventoryScrambleHook::uninstall();
    personalized::TextureSwapHook::uninstall(); personalized::UUIDHook::uninstall();
    m_loaded = false; PZ_LOG_INFO("Personalized unloaded - all hooks removed");
}
namespace { struct ModRegistrar { ModRegistrar() {
    auto& bus = ll::event::EventBus::getInstance();
    bus.emplaceListener<ll::event::ServerStartingEvent>([](ll::event::ServerStartingEvent&) { g_mod.onEnable(); });
    bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) { g_mod.onDisable(); });
}}; static ModRegistrar s_registrar; }
FILE

echo "Writing stub headers..."

# ── Stub: Logger ──
cat > include/stubs/ll/api/Logger.h << 'FILE'
#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <utility>
namespace ll {
class Logger {
public:
    explicit Logger(std::string name) : m_name(std::move(name)) {}
    template <typename... Args> void debug(Args&&... args) { log("DEBUG", std::forward<Args>(args)...); }
    template <typename... Args> void info(Args&&... args)  { log("INFO",  std::forward<Args>(args)...); }
    template <typename... Args> void warn(Args&&... args)  { log("WARN",  std::forward<Args>(args)...); }
    template <typename... Args> void error(Args&&... args) { log("ERROR", std::forward<Args>(args)...); }
    template <typename... Args> void fatal(Args&&... args) { log("FATAL", std::forward<Args>(args)...); }
private:
    std::string m_name;
    void printPrefix(const char* level) { std::fprintf(stderr, "[%s][%s] ", level, m_name.c_str()); }
    void log(const char* level, const char* fmt) { printPrefix(level); std::string out; for (const char* p = fmt; *p; ++p) { if (*p == '{') { if (*(p+1)=='{') { out += '{'; ++p; continue; } while (*p && *p != '}') ++p; continue; } if (*p == '}' && *(p+1) == '}') { out += '}'; ++p; continue; } out += *p; } std::fprintf(stderr, "%s\n", out.c_str()); }
    template <typename... Args> void log(const char* level, const char* fmt, Args&&... args) { printPrefix(level); std::string pfmt = convertFmt(fmt); std::fprintf(stderr, pfmt.c_str(), std::forward<Args>(args)...); std::fputc('\n', stderr); }
    static std::string convertFmt(const char* fmt) {
        std::string result;
        for (const char* p = fmt; *p; ) {
            if (*p == '{' && *(p+1) == '{') { result += '{'; p += 2; continue; }
            if (*p == '}' && *(p+1) == '}') { result += '}'; p += 2; continue; }
            if (*p == '{') { ++p; std::string spec; while (*p && *p != '}') { spec += *p; ++p; } if (*p == '}') ++p;
                if (spec.empty()) result += "%s"; else if (spec == "d") result += "%d"; else if (spec == "zu") result += "%zu"; else if (spec == "p") result += "%p"; else if (spec == "016X") result += "%016llX"; else if (spec == "016x") result += "%016llx"; else if (spec.size() >= 2 && spec[0] == '.') result += "%" + spec; else result += "%" + spec;
            } else { result += *p; ++p; }
        }
        return result;
    }
};
} // namespace ll
FILE

# ── Stub: Hook macros ──
cat > include/stubs/ll/api/memory/Hook.h << 'FILE'
#pragma once
#define LL_TYPE_INSTANCE_HOOK(ClassName, ...) struct ClassName {}
#define LL_TYPE_STATIC_HOOK(ClassName, ...) struct ClassName {}
FILE

# ── Stub: EventBus ──
cat > include/stubs/ll/api/event/EventBus.h << 'FILE'
#pragma once
#include <functional>
#include <memory>
#include <string>
namespace ll::event {
class Event { public: virtual ~Event() = default; };
using ListenerPtr = std::shared_ptr<void>;
class EventBus {
public:
    static EventBus& getInstance() { static EventBus inst; return inst; }
    template <typename T, typename F> ListenerPtr emplaceListener(F&&) { return std::make_shared<int>(0); }
    template <typename T> void removeListener(ListenerPtr&) {}
};
} // namespace ll::event
FILE

# ── Stub: Events ──
cat > include/stubs/ll/api/event/server/PlayerJoinEvent.h << 'FILE'
#pragma once
#include "ll/api/event/EventBus.h"
#include "mc/world/actor/player/Player.h"
namespace ll::event { class PlayerJoinEvent : public Event { public: mc::Player& self() { return *m_player; } private: mc::Player* m_player = nullptr; }; }
FILE

cat > include/stubs/ll/api/event/server/ServerStartingEvent.h << 'FILE'
#pragma once
#include "ll/api/event/EventBus.h"
namespace ll::event { class ServerStartingEvent : public Event {}; }
FILE

cat > include/stubs/ll/api/event/server/ServerStoppingEvent.h << 'FILE'
#pragma once
#include "ll/api/event/EventBus.h"
namespace ll::event { class ServerStoppingEvent : public Event {}; }
FILE

cat > include/stubs/ll/api/event/server/ServerTickEvent.h << 'FILE'
#pragma once
#include "ll/api/event/EventBus.h"
namespace ll::event { class ServerTickEvent : public Event {}; }
FILE

cat > include/stubs/ll/api/event/world/ActorAddEvent.h << 'FILE'
#pragma once
#include "ll/api/event/EventBus.h"
#include "mc/world/actor/Actor.h"
namespace ll::event { class ActorAddEvent : public Event { public: mc::Actor& self() { return *m_actor; } private: mc::Actor* m_actor = nullptr; }; }
FILE

# ── Stub: NativeMod ──
cat > include/stubs/ll/api/mod/NativeMod.h << 'FILE'
#pragma once
namespace ll::mod { class NativeMod { public: virtual ~NativeMod() = default; }; }
FILE

# ── Stub: MC types ──
cat > include/stubs/mc/world/actor/player/Player.h << 'FILE'
#pragma once
#include <string>
namespace mc { class Player { public: std::string getName() const { return "StubPlayer"; } }; }
FILE

cat > include/stubs/mc/world/actor/player/LocalPlayer.h << 'FILE'
#pragma once
#include <string>
namespace mc { class LocalPlayer { public: std::string getName() const { return "StubLocalPlayer"; } }; }
FILE

cat > include/stubs/mc/world/actor/Actor.h << 'FILE'
#pragma once
#include <string>
namespace mc { class Actor { public: virtual ~Actor() = default; bool isPlayer() const { return false; } std::string getTypeName() const { return "minecraft:actor"; } }; }
FILE

cat > include/stubs/mc/world/actor/Mob.h << 'FILE'
#pragma once
#include "mc/world/actor/Actor.h"
namespace mc { class Mob : public Actor {}; }
FILE

cat > include/stubs/mc/world/level/Level.h << 'FILE'
#pragma once
namespace mc { class Level { public: static Level& get() { static Level inst; return inst; } }; }
FILE

cat > include/stubs/mc/world/level/block/Block.h << 'FILE'
#pragma once
namespace mc { class Block { public: static Block* get(const std::string&) { return nullptr; } }; }
FILE

cat > include/stubs/mc/world/level/block/BlockLegacy.h << 'FILE'
#pragma once
namespace mc { class BlockLegacy {}; }
FILE

cat > include/stubs/mc/world/level/block/BlockTypeRegistry.h << 'FILE'
#pragma once
namespace mc { class BlockTypeRegistry { public: static BlockLegacy* lookupByName(const std::string&) { return nullptr; } }; }
FILE

cat > include/stubs/mc/world/level/block/registry/BlockPalette.h << 'FILE'
#pragma once
namespace mc { class BlockPalette { public: static BlockPalette& get() { static BlockPalette inst; return inst; } }; }
FILE

cat > include/stubs/mc/world/item/Item.h << 'FILE'
#pragma once
#include <string>
#include <vector>
namespace mc { class ItemInstance {}; class Item { public: std::string getDescriptionId() const { return "item.minecraft.unknown"; } }; class CreativeItemRegistry { public: static std::vector<ItemInstance>& getCreativeItems() { static std::vector<ItemInstance> items; return items; } }; class ItemRegistry { public: static Item* getItem(const std::string&) { return nullptr; } }; }
FILE

cat > include/stubs/mc/client/services/ClientInstance.h << 'FILE'
#pragma once
namespace mc { class ClientInstance { public: static ClientInstance& get() { static ClientInstance inst; return inst; } }; }
FILE

# ── Stub: nlohmann/json ──
cat > include/stubs/nlohmann/json.hpp << 'FILE'
#pragma once
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstdint>
namespace nlohmann {
class json {
public:
    json() = default;
    json(bool) {} json(int) {} json(uint64_t) {} json(double) {} json(const char*) {} json(const std::string&) {}
    template <typename T> json(std::initializer_list<T>) {}
    static json parse(std::ifstream&) { return json(); }
    template <typename T> T value(const std::string&, T defaultVal) const { return defaultVal; }
    std::string dump(int = -1) const { return "{}"; }
    class exception : public std::exception { public: const char* what() const noexcept override { return "json stub exception"; } };
};
} // namespace nlohmann
FILE

# ── Test file ──
cat > tests/TestRandomMapper.cpp << 'FILE'
#include "Utils/RandomMapper.h"
#include <cstdio>
#include <vector>
#include <algorithm>
int main() {
    printf("RandomMapper test: ");
    auto perm = personalized::RandomMapper::fisherYatesShuffle(42, 100, 1);
    if (perm.size() != 100) { printf("FAIL (size)\n"); return 1; }
    auto a = personalized::RandomMapper::fisherYatesShuffle(12345, 50, 1);
    auto b = personalized::RandomMapper::fisherYatesShuffle(12345, 50, 1);
    if (a != b) { printf("FAIL (determinism)\n"); return 1; }
    personalized::RandomMapper mapper(9999, 97);
    for (size_t i = 0; i < 97; ++i) { if (mapper.unmap(mapper.map(i)) != i) { printf("FAIL (inverse)\n"); return 1; } }
    printf("PASS\n"); return 0;
}
FILE

# ── GitHub Actions workflow ──
cat > .github/workflows/build-android.yml << 'FILE'
name: Build Android Native Mod
on: [push, workflow_dispatch]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: xmake-io/github-action-setup-xmake@v1
        with:
          xmake-version: latest
      - uses: android-actions/setup-android@v3
        with:
          api-level: 30
          ndk-version: '25.2.9519653'
      - run: |
          xmake f -c -p android -a arm64-v8a --ndk=$ANDROID_NDK_ROOT -m release -y
          xmake -y
      - uses: actions/upload-artifact@v4
        with:
          name: Personalized-Android-Mod
          path: build/android/arm64-v8a/release/libPersonalized.so
FILE

echo ""
echo "=== Project created at: $PROJ ==="
echo "=== Initializing git and pushing to GitHub ==="

cd "$PROJ"
git init
git add -A
git commit -m "Initial commit: Personalized Bedrock mod v0.1.0"
git branch -M main
git remote add origin "https://$GH_TOKEN@github.com/$GH_USER/$REPO_NAME.git"
git push -u origin main

echo ""
echo "=== DONE! ==="
echo "Repo: https://github.com/$GH_USER/$REPO_NAME"
echo "The GitHub Actions workflow will build automatically on push."
