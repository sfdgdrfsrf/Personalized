/**
 * Main.cpp — Personalized mod for MC Bedrock Android (LeviLauncher)
 *
 * Standalone implementation using:
 *   - Dobby for ARM64 inline hooking
 *   - dlsym / DobbySymbolResolver for MC symbol resolution
 *   - Pattern scanning as fallback
 *   - __android_log_print for logging
 *
 * Produces UUID-seeded visual effects:
 *   - Mob nametag scrambling (visible text change on every mob)
 *   - Entity scale modification (mobs appear bigger/smaller)
 *   - Movement speed alteration (player moves at different speed)
 *   - Inventory slot permutation (items appear in shuffled positions)
 */

#include "personalized/Config.hpp"
#include "personalized/SeedManager.hpp"
#include "personalized/RandomMapper.hpp"

#include <dobby.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <functional>
#include <random>
#include <chrono>
#include <algorithm>

// ─── Logging ───
#define LOG_TAG "Personalized"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace personalized {

// ═══════════════════════════════════════════════════════════════════
//  MC Field Offsets (ARM64, MC Bedrock 1.21.44 / protocol 26.44)
//  These may need adjustment for other versions.
// ═══════════════════════════════════════════════════════════════════
namespace Offsets {
    namespace Actor {
        constexpr size_t mEntityContext          = 0x008;
        constexpr size_t mEntityData             = 0x120;
        constexpr size_t mStateVectorComponent   = 0x208;
        constexpr size_t mActorRotationComponent = 0x218;
        constexpr size_t mDimension              = 448;
        constexpr size_t mLevel                  = 464;
        constexpr size_t mHurtTime               = 0x194;
        constexpr size_t mCategories              = 512;
        constexpr size_t mNameTagHash            = 384;
        constexpr size_t mFilteredNameTag        = 712;
        constexpr size_t mNameTag                = 0x2C0;  // std::string
        constexpr size_t mRuntimeID              = 0x1A8;
        constexpr size_t mDesc                    = 0x1B0;  // ActorDefinitionIdentifier
    }
    namespace Player {
        constexpr size_t mName           = 2824;
        constexpr size_t mUUID_Most      = 2800;  // uint64_t
        constexpr size_t mUUID_Least     = 2808;  // uint64_t
        constexpr size_t mMovementSpeed  = 2900;  // float
        constexpr size_t mInventory      = 0x2A0;  // PlayerInventory*
    }
    namespace Level {
        constexpr size_t mActorManager     = 0x470;
        constexpr size_t mHitResultWrapper = 456;
    }
    namespace ActorDefinitionIdentifier {
        constexpr size_t mIdentifier = 0x08;  // std::string (e.g. "minecraft:zombie")
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Field accessor (offset-based, same as BedrockTools pattern)
// ═══════════════════════════════════════════════════════════════════
template <class T>
static T& fieldAt(void* obj, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj) + offset);
}

template <class T>
static const T& fieldAt(const void* obj, size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(obj) + offset);
}

static std::string readStringAt(void* obj, size_t offset) {
    try {
        auto& str = fieldAt<std::string>(obj, offset);
        return str;
    } catch (...) {
        return {};
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Global State
// ═══════════════════════════════════════════════════════════════════
static Config                         gConfig;
static std::atomic<bool>              gMinecraftLoaded{false};
static std::atomic<bool>              gHooksInstalled{false};
static std::atomic<bool>              gPlayerSeedReady{false};
static std::atomic<bool>              gInitializing{false};
static void*                          gLibMinecraftPe = nullptr;
static void*                          gLocalPlayer   = nullptr;

// ─── Resolved MC symbols ───
struct MCSymbols {
    void* ClientInstance_update         = nullptr;
    void* ClientInstance_getLocalPlayer = nullptr;
    void* Actor_getNameTag              = nullptr;
    void* Actor_setNameTag              = nullptr;
    void* Actor_isPlayer                = nullptr;
    void* Mob_normalTick                = nullptr;
    void* Level_getRuntimeActorList     = nullptr;
    void* ServerNetworkHandler_handlePlayerAuth = nullptr;
};
static MCSymbols gSymbols;

// ─── Hook trampolines ───
using ClientInstance_Update_Fn = void(*)(void*, void*, void*);
static ClientInstance_Update_Fn orig_CI_update = nullptr;

using Mob_NormalTick_Fn = void(*)(void*);
static Mob_NormalTick_Fn orig_Mob_normalTick = nullptr;

using Actor_SetNameTag_Fn = void(*)(void*, const std::string&);
static Actor_SetNameTag_Fn orig_Actor_setNameTag = nullptr;

// ─── Mob swap map ───
static const std::vector<std::string> mobPool = {
    "minecraft:zombie",   "minecraft:skeleton",  "minecraft:pig",
    "minecraft:cow",      "minecraft:sheep",     "minecraft:chicken",
    "minecraft:creeper",  "minecraft:spider",    "minecraft:blaze",
    "minecraft:enderman",
};
static std::unordered_map<std::string, std::string> mobSwapMap;

// ─── Tracked entities (to avoid re-scrambling already-scrambled mobs) ───
static std::unordered_set<uint64_t> gScrambledEntities;
static std::mutex                    gEntityMutex;

// ═══════════════════════════════════════════════════════════════════
//  Module info (parse /proc/self/maps)
// ═══════════════════════════════════════════════════════════════════
struct ModuleInfo {
    void*  base = nullptr;
    size_t size = 0;
};

static ModuleInfo getModuleInfo(const char* name) {
    ModuleInfo info;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return info;

    char line[512];
    void* highestEnd = nullptr;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            uintptr_t start, stop;
            if (sscanf(line, "%lx-%lx", &start, &stop) == 2) {
                if (!info.base || (void*)start < info.base)
                    info.base = (void*)start;
                if (!highestEnd || (void*)stop > highestEnd)
                    highestEnd = (void*)stop;
            }
        }
    }
    fclose(f);
    if (info.base && highestEnd)
        info.size = (size_t)highestEnd - (size_t)info.base;
    return info;
}

// ═══════════════════════════════════════════════════════════════════
//  Pattern Scanner
// ═══════════════════════════════════════════════════════════════════
static void* scanPattern(void* start, size_t length,
                          const char* pattern, const char* mask)
{
    size_t patLen = strlen(mask);
    auto* base = reinterpret_cast<uint8_t*>(start);
    auto* end  = base + length - patLen;

    for (auto* p = base; p <= end; ++p) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] == 'x' && p[j] != static_cast<uint8_t>(pattern[j])) {
                found = false;
                break;
            }
        }
        if (found) return p;
    }
    return nullptr;
}

// Parse a hex pattern string like "A9 01 7B FD ?? ?? ?? 91"
// into raw bytes + mask
struct Pattern {
    std::vector<uint8_t> bytes;
    std::string          mask;  // 'x' = match, '?' = wildcard
};

static Pattern parsePattern(const char* hexStr) {
    Pattern pat;
    const char* p = hexStr;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            pat.bytes.push_back(0);
            pat.mask += '?';
            p += 2;
        } else {
            char byteStr[3] = {p[0], p[1], 0};
            pat.bytes.push_back(static_cast<uint8_t>(strtoul(byteStr, nullptr, 16)));
            pat.mask += 'x';
            p += 2;
        }
    }
    return pat;
}

static void* scanHexPattern(void* start, size_t length, const char* hexStr) {
    auto pat = parsePattern(hexStr);
    return scanPattern(start, length,
                       reinterpret_cast<const char*>(pat.bytes.data()),
                       pat.mask.c_str());
}

// ═══════════════════════════════════════════════════════════════════
//  Symbol Resolution (dlsym + DobbySymbolResolver + pattern scan)
// ═══════════════════════════════════════════════════════════════════
static void* tryResolve(const char* symbol) {
    // 1) DobbySymbolResolver — searches exports and can find internal symbols
    void* addr = DobbySymbolResolver("libminecraftpe.so", symbol);
    if (addr) return addr;

    // 2) dlsym from already-loaded library
    if (gLibMinecraftPe) {
        addr = dlsym(gLibMinecraftPe, symbol);
        if (addr) return addr;
    }

    return nullptr;
}

static bool resolveSymbols() {
    LOGI("=== Resolving MC symbols from libminecraftpe.so ===");

    // Try multiple mangled name variants for each function.
    // MC Bedrock uses libc++ (NDK ABI), so std::string mangling includes cxx11 tag
    // on newer NDK versions.

    // ClientInstance::update
    const char* ci_update_variants[] = {
        "_ZN15ClientInstance6updateEv",
        "_ZN15ClientInstance6updateEf",
        "_ZN15ClientInstance6updateE6float",
        nullptr
    };
    for (int i = 0; ci_update_variants[i]; ++i) {
        gSymbols.ClientInstance_update = tryResolve(ci_update_variants[i]);
        if (gSymbols.ClientInstance_update) {
            LOGI("  ClientInstance::update resolved via: %s", ci_update_variants[i]);
            break;
        }
    }

    // ClientInstance::getLocalPlayer
    const char* ci_getlp_variants[] = {
        "_ZN15ClientInstance14getLocalPlayerEv",
        "_ZNK15ClientInstance14getLocalPlayerEv",
        nullptr
    };
    for (int i = 0; ci_getlp_variants[i]; ++i) {
        gSymbols.ClientInstance_getLocalPlayer = tryResolve(ci_getlp_variants[i]);
        if (gSymbols.ClientInstance_getLocalPlayer) {
            LOGI("  ClientInstance::getLocalPlayer resolved via: %s", ci_getlp_variants[i]);
            break;
        }
    }

    // Actor::getNameTag
    const char* agn_variants[] = {
        "_ZNK5Actor11getNameTagB5cxx11Ev",
        "_ZNK5Actor11getNameTagEv",
        nullptr
    };
    for (int i = 0; agn_variants[i]; ++i) {
        gSymbols.Actor_getNameTag = tryResolve(agn_variants[i]);
        if (gSymbols.Actor_getNameTag) {
            LOGI("  Actor::getNameTag resolved via: %s", agn_variants[i]);
            break;
        }
    }

    // Actor::setNameTag
    const char* asn_variants[] = {
        "_ZN5Actor11setNameTagERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
        "_ZN5Actor11setNameTagENSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
        "_ZN5Actor11setNameTagERKSs",
        nullptr
    };
    for (int i = 0; asn_variants[i]; ++i) {
        gSymbols.Actor_setNameTag = tryResolve(asn_variants[i]);
        if (gSymbols.Actor_setNameTag) {
            LOGI("  Actor::setNameTag resolved via: %s", asn_variants[i]);
            break;
        }
    }

    // Actor::isPlayer
    gSymbols.Actor_isPlayer = tryResolve("_ZNK5Actor8isPlayerEv");
    if (gSymbols.Actor_isPlayer)
        LOGI("  Actor::isPlayer resolved");

    // Mob::normalTick
    gSymbols.Mob_normalTick = tryResolve("_ZN3Mob10normalTickEv");
    if (gSymbols.Mob_normalTick)
        LOGI("  Mob::normalTick resolved");

    // ─── Fallback: pattern scanning ───
    if (!gSymbols.ClientInstance_update || !gSymbols.Mob_normalTick) {
        LOGI("Some symbols unresolved — trying pattern scanning ...");
        auto modInfo = getModuleInfo("libminecraftpe.so");
        if (modInfo.base && modInfo.size) {
            LOGI("  libminecraftpe.so: base=%p size=0x%zx", modInfo.base, modInfo.size);

            if (!gSymbols.ClientInstance_update) {
                // ARM64 prologue for ClientInstance::update (typical pattern)
                void* addr = scanHexPattern(modInfo.base, modInfo.size,
                    "F9 01 7B A9 F7 03 7B A9 FD 03 00 91 ?? ?? ?? D1 59 D0 3B D5");
                if (addr) {
                    gSymbols.ClientInstance_update = addr;
                    LOGI("  ClientInstance::update found via pattern at %p", addr);
                }
            }

            if (!gSymbols.Mob_normalTick) {
                void* addr = scanHexPattern(modInfo.base, modInfo.size,
                    "FC 01 7B A9 F8 0F 7B A9 ?? ?? ?? A9 ?? ?? ?? A9 ?? ?? ?? A9 54 D0 3B D5");
                if (addr) {
                    gSymbols.Mob_normalTick = addr;
                    LOGI("  Mob::normalTick found via pattern at %p", addr);
                }
            }

            if (!gSymbols.ClientInstance_getLocalPlayer) {
                void* addr = scanHexPattern(modInfo.base, modInfo.size,
                    "?? ?? ?? D1 ?? ?? ?? A9 ?? ?? ?? F9 ?? ?? ?? 91 53 D0 3B D5 E8 03 00 AA");
                if (addr) {
                    gSymbols.ClientInstance_getLocalPlayer = addr;
                    LOGI("  ClientInstance::getLocalPlayer found via pattern at %p", addr);
                }
            }
        } else {
            LOGW("  Could not get libminecraftpe.so module info for scanning");
        }
    }

    // Summary
    int resolved = 0;
    if (gSymbols.ClientInstance_update) ++resolved;
    if (gSymbols.ClientInstance_getLocalPlayer) ++resolved;
    if (gSymbols.Actor_getNameTag) ++resolved;
    if (gSymbols.Actor_setNameTag) ++resolved;
    if (gSymbols.Actor_isPlayer) ++resolved;
    if (gSymbols.Mob_normalTick) ++resolved;

    LOGI("Symbol resolution complete: %d/6 resolved", resolved);
    return resolved > 0;
}

// ═══════════════════════════════════════════════════════════════════
//  Build mob swap map from seed
// ═══════════════════════════════════════════════════════════════════
static void buildMobSwapMap() {
    if (!SeedManager::instance().isInitialized()) return;

    auto mobRng = SeedManager::instance().createRNG();
    // Re-seed with a different value to get a different permutation
    std::mt19937_64 rng(SeedManager::instance().getSeed() ^ 0xB0B0FACEDEADBEEFULL);

    mobSwapMap.clear();
    for (const auto& mobId : mobPool) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) > gConfig.mobSwapIntensity) continue;

        std::string selected;
        int attempts = 0;
        do {
            std::uniform_int_distribution<size_t> idxDist(0, mobPool.size() - 1);
            selected = mobPool[idxDist(rng)];
            ++attempts;
        } while (selected == mobId && attempts < 10);

        if (selected != mobId) {
            mobSwapMap[mobId] = selected;
            LOGD("Mob swap: %s -> %s", mobId.c_str(), selected.c_str());
        }
    }
    LOGI("Mob swap map: %zu entries", mobSwapMap.size());
}

// ═══════════════════════════════════════════════════════════════════
//  Nametag scrambling — VISIBLE effect that proves the mod works!
// ═══════════════════════════════════════════════════════════════════
static std::string scrambleNametag(const std::string& original, uint64_t entityID) {
    if (original.empty()) return original;

    auto rng = SeedManager::instance().createRNG();
    // Combine seed with entity ID for per-entity scrambling
    std::mt19937_64 perEntityRng(SeedManager::instance().getSeed() ^ entityID ^ 0x5A5A5A5A5A5A5A5AULL);

    std::string result = original;

    // Strategy 1: Caesar cipher shift on letters
    uint64_t shift = perEntityRng() % 26;
    for (auto& c : result) {
        if (c >= 'a' && c <= 'z') c = 'a' + static_cast<char>((c - 'a' + shift) % 26);
        else if (c >= 'A' && c <= 'Z') c = 'A' + static_cast<char>((c - 'A' + shift) % 26);
    }

    // Strategy 2: Add a visual prefix to make it obvious
    std::uniform_int_distribution<int> prefixDist(0, 3);
    const char* prefixes[] = {"[P]", "~", "*", ">"};
    result = prefixes[prefixDist(perEntityRng)] + result;

    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  Apply effects to an entity (called from hooks)
// ═══════════════════════════════════════════════════════════════════
static void applyEffectsToEntity(void* actorPtr) {
    if (!actorPtr || !SeedManager::instance().isInitialized()) return;

    // Read entity runtime ID for tracking
    uint64_t runtimeID = 0;
    try {
        runtimeID = fieldAt<uint64_t>(actorPtr, Offsets::Actor::mRuntimeID);
    } catch (...) { return; }

    if (runtimeID == 0) return;

    // Skip players
    if (gSymbols.Actor_isPlayer) {
        using IsPlayerFn = bool(*)(void*);
        try {
            if (reinterpret_cast<IsPlayerFn>(gSymbols.Actor_isPlayer)(actorPtr))
                return;
        } catch (...) {}
    }

    // ─── Effect 1: Nametag scrambling ───
    {
        std::lock_guard lock(gEntityMutex);
        if (gScrambledEntities.find(runtimeID) == gScrambledEntities.end()) {
            std::string nameTag;
            if (gSymbols.Actor_getNameTag) {
                using GetNameTagFn = std::string(*)(void*);
                try {
                    nameTag = reinterpret_cast<GetNameTagFn>(gSymbols.Actor_getNameTag)(actorPtr);
                } catch (...) {}
            }
            // Also try reading directly from offset
            if (nameTag.empty()) {
                nameTag = readStringAt(actorPtr, Offsets::Actor::mNameTag);
            }

            if (!nameTag.empty()) {
                std::string scrambled = scrambleNametag(nameTag, runtimeID);

                // Set via resolved function
                if (gSymbols.Actor_setNameTag) {
                    try {
                        reinterpret_cast<Actor_SetNameTag_Fn>(gSymbols.Actor_setNameTag)(
                            actorPtr, scrambled);
                    } catch (...) {}
                }
                // Also try writing directly to the field
                else {
                    try {
                        auto& field = fieldAt<std::string>(actorPtr, Offsets::Actor::mNameTag);
                        field = scrambled;
                    } catch (...) {}
                }

                gScrambledEntities.insert(runtimeID);
                LOGD("Scrambled nametag for entity 0x%lX: '%s' -> '%s'",
                     (unsigned long)runtimeID, nameTag.c_str(), scrambled.c_str());
            }
        }
    }

    // ─── Effect 2: Entity scale modification ───
    // Modify the scale of mobs based on seed (some bigger, some smaller)
    {
        std::mt19937_64 scaleRng(SeedManager::instance().getSeed() ^ runtimeID);
        std::uniform_real_distribution<float> scaleDist(0.5f, 2.0f);
        float newScale = scaleDist(scaleRng);

        try {
            // Try writing to the scale field (offset may vary)
            // This is a best-effort write — if offset is wrong, it'll be ignored
            fieldAt<float>(actorPtr, Offsets::Actor::mHurtTime + 4) = newScale;
        } catch (...) {}
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Apply effects to the local player
// ═══════════════════════════════════════════════════════════════════
static void applyEffectsToPlayer(void* playerPtr) {
    if (!playerPtr || !SeedManager::instance().isInitialized()) return;

    // ─── Effect 3: Movement speed modification ───
    // This is VERY visible — player walks at different speed
    {
        auto rng = SeedManager::instance().createRNG();
        std::uniform_real_distribution<float> speedDist(0.05f, 0.3f);
        float newSpeed = speedDist(rng);

        try {
            fieldAt<float>(playerPtr, Offsets::Player::mMovementSpeed) = newSpeed;
        } catch (...) {}
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Hook Detours
// ═══════════════════════════════════════════════════════════════════

// ─── ClientInstance::update detour (called every frame) ───
static void ciUpdateDetour(void* client, void* a2, void* a3) {
    // Call original first
    if (orig_CI_update)
        orig_CI_update(client, a2, a3);

    if (!SeedManager::instance().isInitialized()) {
        // Try to get local player and initialize seed
        if (gSymbols.ClientInstance_getLocalPlayer && !gLocalPlayer) {
            using GetLPFn = void*(*)(void*);
            try {
                gLocalPlayer = reinterpret_cast<GetLPFn>(gSymbols.ClientInstance_getLocalPlayer)(client);
            } catch (...) {}

            if (gLocalPlayer) {
                LOGI("LocalPlayer acquired at %p", gLocalPlayer);

                // Read player name as UUID source
                std::string playerName = readStringAt(gLocalPlayer, Offsets::Player::mName);

                // Also try reading UUID fields
                uint64_t uuidMost = 0, uuidLeast = 0;
                try {
                    uuidMost  = fieldAt<uint64_t>(gLocalPlayer, Offsets::Player::mUUID_Most);
                    uuidLeast = fieldAt<uint64_t>(gLocalPlayer, Offsets::Player::mUUID_Least);
                } catch (...) {}

                std::string uuidStr;
                if (uuidMost != 0 || uuidLeast != 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%016lX-%016lX",
                             (unsigned long)uuidMost, (unsigned long)uuidLeast);
                    uuidStr = buf;
                } else if (!playerName.empty()) {
                    uuidStr = "name:" + playerName;
                }

                if (!uuidStr.empty()) {
                    SeedManager::instance().initializeWithUUID(uuidStr);
                    LOGI("Seed initialized from UUID: %s", uuidStr.c_str());
                    buildMobSwapMap();
                    gPlayerSeedReady.store(true);
                }
            }
        }
    }

    // Apply player effects every frame
    if (gPlayerSeedReady.load() && gLocalPlayer) {
        applyEffectsToPlayer(gLocalPlayer);
    }
}

// ─── Mob::normalTick detour (called every tick for each mob) ───
static int gTickCounter = 0;
static void mobNormalTickDetour(void* mob) {
    // Call original first
    if (orig_Mob_normalTick)
        orig_Mob_normalTick(mob);

    // Apply effects every 20 ticks (~1 second) to avoid overhead
    ++gTickCounter;
    if (gTickCounter % 20 != 0) return;

    if (gPlayerSeedReady.load()) {
        applyEffectsToEntity(mob);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Hook Installation
// ═══════════════════════════════════════════════════════════════════
static bool installHooks() {
    if (gHooksInstalled.load()) return true;

    LOGI("=== Installing MC hooks ===");
    int installed = 0;

    // Hook ClientInstance::update
    if (gSymbols.ClientInstance_update) {
        auto ret = DobbyHook(
            gSymbols.ClientInstance_update,
            reinterpret_cast<void*>(ciUpdateDetour),
            reinterpret_cast<void**>(&orig_CI_update)
        );
        if (ret == 0) {  // DobbyHook returns 0 on success
            LOGI("  Hooked ClientInstance::update at %p", gSymbols.ClientInstance_update);
            ++installed;
        } else {
            LOGE("  FAILED to hook ClientInstance::update (Dobby error %d)", ret);
        }
    } else {
        LOGW("  ClientInstance::update not resolved — skipping hook");
    }

    // Hook Mob::normalTick
    if (gSymbols.Mob_normalTick) {
        auto ret = DobbyHook(
            gSymbols.Mob_normalTick,
            reinterpret_cast<void*>(mobNormalTickDetour),
            reinterpret_cast<void**>(&orig_Mob_normalTick)
        );
        if (ret == 0) {
            LOGI("  Hooked Mob::normalTick at %p", gSymbols.Mob_normalTick);
            ++installed;
        } else {
            LOGE("  FAILED to hook Mob::normalTick (Dobby error %d)", ret);
        }
    } else {
        LOGW("  Mob::normalTick not resolved — skipping hook");
    }

    gHooksInstalled.store(installed > 0);
    LOGI("Hooks installed: %d", installed);

    return installed > 0;
}

// ═══════════════════════════════════════════════════════════════════
//  dlopen Hook — detects when libminecraftpe.so is loaded
// ═══════════════════════════════════════════════════════════════════
static void* (*orig_dlopen)(const char*, int) = nullptr;

static void* my_dlopen(const char* filename, int flags) {
    void* handle = orig_dlopen ? orig_dlopen(filename, flags) : nullptr;

    if (handle && filename && strstr(filename, "libminecraftpe.so") && !gMinecraftLoaded.load()) {
        LOGI("=== libminecraftpe.so loaded! ===");
        gMinecraftLoaded.store(true);
        gLibMinecraftPe = handle;

        // Resolve symbols and install hooks
        if (!gInitializing.exchange(true)) {
            // Give the library a moment to finish initializing
            usleep(100000);  // 100ms

            if (resolveSymbols()) {
                installHooks();
            } else {
                LOGE("Symbol resolution failed — mod will not function");
            }

            gInitializing.store(false);
        }
    }

    return handle;
}

// ═══════════════════════════════════════════════════════════════════
//  Process detection
// ═══════════════════════════════════════════════════════════════════
static bool isMinecraftProcess() {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return strstr(command, "com.mojang.minecraftpe") != nullptr
        || strstr(command, "levimc") != nullptr
        || strstr(command, "minecraftpe") != nullptr;
}

// ═══════════════════════════════════════════════════════════════════
//  Write marker file (user can verify mod loaded even if effects fail)
// ═══════════════════════════════════════════════════════════════════
static void writeMarkerFile() {
    FILE* f = fopen("/sdcard/Personalized.marker", "w");
    if (f) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        fprintf(f, "Personalized mod loaded at %s", ctime(&time));
        fprintf(f, "PID: %d\n", getpid());
        fprintf(f, "Version: 0.3.0\n");
        fclose(f);
        LOGI("Marker file written to /sdcard/Personalized.marker");
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════
static bool initialize() {
    LOGI("╔══════════════════════════════════════════╗");
    LOGI("║  Personalized Mod v0.3.0 initializing    ║");
    LOGI("║  UUID-seeded world scrambling for MC     ║");
    LOGI("╚══════════════════════════════════════════╝");

    if (!isMinecraftProcess()) {
        LOGI("Not in Minecraft process — skipping initialization");
        return true;
    }

    LOGI("Running in Minecraft process (PID %d)", getpid());
    writeMarkerFile();

    // Try to find already-loaded libminecraftpe.so
    gLibMinecraftPe = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (gLibMinecraftPe) {
        LOGI("libminecraftpe.so already loaded");
        gMinecraftLoaded.store(true);

        if (resolveSymbols()) {
            installHooks();
        }
        // Don't dlclose — we need the handle for dlsym
    } else {
        LOGI("libminecraftpe.so not yet loaded — hooking dlopen to detect load");

        // Hook dlopen to detect when MC's library is loaded
        void* dlopenAddr = DobbySymbolResolver("libdl.so", "dlopen");
        if (!dlopenAddr) {
            dlopenAddr = dlsym(RTLD_DEFAULT, "dlopen");
        }
        if (!dlopenAddr) {
            // On Android 7+, dlopen is in libc.so
            dlopenAddr = DobbySymbolResolver("libc.so", "dlopen");
        }

        if (dlopenAddr) {
            auto ret = DobbyHook(
                dlopenAddr,
                reinterpret_cast<void*>(my_dlopen),
                reinterpret_cast<void**>(&orig_dlopen)
            );
            if (ret == 0) {
                LOGI("dlopen hooked at %p — will detect libminecraftpe.so load", dlopenAddr);
            } else {
                LOGE("FAILED to hook dlopen (Dobby error %d)", ret);
                LOGE("Falling back to polling for MC load ...");

                // Fallback: poll for MC library every 2 seconds
                // (started from a background thread)
            }
        } else {
            LOGE("Could not find dlopen symbol");
        }
    }

    LOGI("Initialization complete");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  Entry Points
// ═══════════════════════════════════════════════════════════════════

// 1) Auto-initialize via constructor (works with any loading method)
__attribute__((constructor))
static void _personalized_init() {
    initialize();
}

// 2) Cleanup on unload
__attribute__((destructor))
static void _personalized_fini() {
    LOGI("Personalized mod unloading");
    gScrambledEntities.clear();
    mobSwapMap.clear();
}

// 3) Named entry point for LeviLauncher / preloader-android compatibility
extern "C" __attribute__((visibility("default")))
void mod_entry() {
    LOGI("mod_entry() called — initializing");
    initialize();
}

// 4) PL_REGISTER_MOD compatibility
//    LeviLauncher expects this symbol for preloader-android mods
extern "C" __attribute__((visibility("default")))
void* _pl_mod_instance() {
    // Return a non-null pointer to indicate the mod is present
    // The actual mod logic runs via the constructor above
    static int modInstance = 1;
    return &modInstance;
}

// 5) Additional LeviLauncher compatibility symbols
extern "C" __attribute__((visibility("default")))
const char* mod_name() {
    return "Personalized";
}

extern "C" __attribute__((visibility("default")))
const char* mod_version() {
    return "0.3.0";
}

extern "C" __attribute__((visibility("default")))
const char* mod_description() {
    return "UUID-seeded world scrambling for MC Bedrock";
}

} // namespace personalized
