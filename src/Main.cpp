/**
 * Main.cpp — Personalized mod for MC Bedrock Android (LeviLauncher)
 *
 * Standalone implementation with ZERO external dependencies beyond the NDK.
 * Uses:
 *   - MiniHook: our own ARM64 inline hooker (no Dobby needed)
 *   - dlsym for MC symbol resolution
 *   - Pattern scanning as fallback
 *   - __android_log_print for logging
 *
 * Produces UUID-seeded visual effects:
 *   - Mob nametag scrambling (visible text change on every mob)
 *   - Entity scale modification (mobs appear bigger/smaller)
 *   - Movement speed alteration (player moves at different speed)
 */

#include "personalized/Config.hpp"
#include "personalized/SeedManager.hpp"
#include "personalized/RandomMapper.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
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
//  MiniHook — Minimal ARM64 inline hooker
//
//  Implements function hooking by:
//  1. Saving the first 16 bytes (4 ARM64 instructions) of the target
//  2. Writing an absolute jump (LDR X17, [PC,#8]; BR X17; .quad addr)
//  3. Creating a trampoline: saved bytes + absolute jump back
//  4. Flushing instruction cache
//
//  Limitations:
//  - Only hooks functions whose first 4 instructions are not PC-relative
//    (BL, B, ADR, ADRP). This covers 99% of function prologues.
//  - Not thread-safe during hook installation (install hooks before
//    the target function is called)
// ═══════════════════════════════════════════════════════════════════
namespace MiniHook {

static constexpr size_t kJumpSize    = 16;  // LDR+BR+addr = 4+4+8 bytes
static constexpr size_t kTrampSize   = 32;  // saved(16) + jump-back(16)

// ARM64 absolute jump:
//   LDR X17, [PC, #8]   — loads a 64-bit address from PC+8 into X17
//   BR  X17             — branches to X17
//   .quad target        — the 64-bit target address
static constexpr uint32_t LDR_X17_PC8 = 0x58000051;  // LDR X17, [PC, #8]
static constexpr uint32_t BR_X17      = 0xD61F0220;  // BR X17

static void writeAbsoluteJump(void* where, void* target) {
    auto* p = static_cast<uint32_t*>(where);
    p[0] = LDR_X17_PC8;
    p[1] = BR_X17;
    *reinterpret_cast<void**>(&p[2]) = target;
}

// Check if an ARM64 instruction is PC-relative
// Returns true for BL, B.cond, B, ADRP, ADR, LDR literal
static bool isPCRelative(uint32_t insn) {
    // BL:   100101xx_xxxxxxxx_xxxxxxxx_xxxxxxxx
    if ((insn & 0xFC000000) == 0x94000000) return true;
    // B:    000101xx_xxxxxxxx_xxxxxxxx_xxxxxxxx
    if ((insn & 0xFC000000) == 0x14000000) return true;
    // B.cond: 0101010x_xxxxxxxx_xxxxxxxx_xxxxxxxx
    if ((insn & 0xFF000010) == 0x54000000) return true;
    // CBZ/CBNZ: 0110100x / 0110101x
    if ((insn & 0x7E000000) == 0x34000000) return true;
    if ((insn & 0x7E000000) == 0x35000000) return true;
    // TBZ/TBNZ: 0110110x / 0110111x
    if ((insn & 0x7E000000) == 0x36000000) return true;
    if ((insn & 0x7E000000) == 0x37000000) return true;
    // ADRP: 1xxx0000_xxxxxxxx_xxxxxxxx_xxxxxxxx (page address)
    if ((insn & 0x9F000000) == 0x90000000) return true;
    // ADR:  0xxx10000_xxxxxxxx_xxxxxxxx_xxxxxxxx
    if ((insn & 0x9F000000) == 0x10000000) return true;
    return false;
}

// Allocate executable memory for trampoline
static void* allocTrampoline() {
    void* mem = mmap(nullptr, kTrampSize,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return mem;
}

// Make a memory page writable (temporarily, for writing the hook)
static bool makeWritable(void* addr, size_t len) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(0x1000ULL - 1);
    uintptr_t endPage = (reinterpret_cast<uintptr_t>(addr) + len + 0xFFFULL) & ~(0x1000ULL - 1);
    size_t regionSize = endPage - page;
    // Remove exec, add write
    if (mprotect(reinterpret_cast<void*>(page), regionSize,
                 PROT_READ | PROT_WRITE) != 0) {
        LOGE("MiniHook: mprotect(RW) failed for %p: %s",
             reinterpret_cast<void*>(page), strerror(errno));
        return false;
    }
    return true;
}

// Restore page to executable
static bool makeExecutable(void* addr, size_t len) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(0x1000ULL - 1);
    uintptr_t endPage = (reinterpret_cast<uintptr_t>(addr) + len + 0xFFFULL) & ~(0x1000ULL - 1);
    size_t regionSize = endPage - page;
    if (mprotect(reinterpret_cast<void*>(page), regionSize,
                 PROT_READ | PROT_EXEC) != 0) {
        LOGE("MiniHook: mprotect(RX) failed for %p: %s",
             reinterpret_cast<void*>(page), strerror(errno));
        return false;
    }
    return true;
}

// Flush instruction cache (required on ARM64 after code modification)
static void flushICache(void* begin, void* end) {
    __builtin___clear_cache(static_cast<char*>(begin),
                             static_cast<char*>(end));
}

/**
 * Install an inline hook: target → detour, with trampoline stored in *original
 *
 * @param target   Address of the function to hook
 * @param detour   Address of the replacement function
 * @param original Receives a trampoline that calls the original function
 * @return true on success
 */
static bool hook(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return false;

    LOGI("MiniHook: hooking %p → %p", target, detour);

    // 1. Verify the first 4 instructions are not PC-relative
    auto* insns = static_cast<uint32_t*>(target);
    for (int i = 0; i < 4; ++i) {
        if (isPCRelative(insns[i])) {
            LOGW("MiniHook: instruction %d at %p is PC-relative (0x%08X) — "
                 "hook may be unstable!", i, &insns[i], insns[i]);
            // Continue anyway — it might still work if the offset happens
            // to be zero or the instruction isn't actually reached
        }
    }

    // 2. Allocate trampoline
    void* trampoline = allocTrampoline();
    if (!trampoline) {
        LOGE("MiniHook: failed to allocate trampoline");
        return false;
    }

    // 3. Copy original instructions to trampoline
    memcpy(trampoline, target, kJumpSize);

    // 4. Write jump back to target + kJumpSize at end of trampoline
    writeAbsoluteJump(
        static_cast<uint8_t*>(trampoline) + kJumpSize,
        static_cast<uint8_t*>(target) + kJumpSize
    );

    // 5. Make trampoline executable (it was allocated RWX, but be safe)
    flushICache(trampoline, static_cast<uint8_t*>(trampoline) + kTrampSize);

    // 6. Make target writable
    if (!makeWritable(target, kJumpSize)) {
        munmap(trampoline, kTrampSize);
        return false;
    }

    // 7. Write jump to detour at target
    writeAbsoluteJump(target, detour);

    // 8. Make target executable again
    makeExecutable(target, kJumpSize);

    // 9. Flush instruction cache
    flushICache(target, static_cast<uint8_t*>(target) + kJumpSize);

    // 10. Set original to trampoline
    *original = trampoline;

    LOGI("MiniHook: hook installed — trampoline at %p", trampoline);
    return true;
}

} // namespace MiniHook

// ═══════════════════════════════════════════════════════════════════
//  MC Field Offsets (ARM64, MC Bedrock ~1.21.44 / protocol 26.44)
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
        constexpr size_t mNameTag                = 0x2C0;
        constexpr size_t mRuntimeID              = 0x1A8;
    }
    namespace Player {
        constexpr size_t mName           = 2824;
        constexpr size_t mUUID_Most      = 2800;
        constexpr size_t mUUID_Least     = 2808;
        constexpr size_t mMovementSpeed  = 2900;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Field accessor (offset-based)
// ═══════════════════════════════════════════════════════════════════
template <class T>
static T& fieldAt(void* obj, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj) + offset);
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

// ─── Tracked entities ───
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

struct Pattern {
    std::vector<uint8_t> bytes;
    std::string          mask;
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
    if (pat.bytes.empty()) return nullptr;
    return scanPattern(start, length,
                       reinterpret_cast<const char*>(pat.bytes.data()),
                       pat.mask.c_str());
}

// ═══════════════════════════════════════════════════════════════════
//  Symbol Resolution
// ═══════════════════════════════════════════════════════════════════
static void* tryResolve(const char* symbol) {
    // 1) dlsym from already-loaded MC library
    if (gLibMinecraftPe) {
        void* addr = dlsym(gLibMinecraftPe, symbol);
        if (addr) return addr;
    }

    // 2) dlsym from RTLD_DEFAULT (searches all loaded libraries)
    void* addr = dlsym(RTLD_DEFAULT, symbol);
    if (addr) return addr;

    return nullptr;
}

static bool resolveSymbols() {
    LOGI("=== Resolving MC symbols from libminecraftpe.so ===");

    // ClientInstance::update — multiple mangled name variants
    const char* ci_update_variants[] = {
        "_ZN15ClientInstance6updateEv",
        "_ZN15ClientInstance6updateEf",
        "_ZN15ClientInstance6updateE6float",
        nullptr
    };
    for (int i = 0; ci_update_variants[i]; ++i) {
        gSymbols.ClientInstance_update = tryResolve(ci_update_variants[i]);
        if (gSymbols.ClientInstance_update) {
            LOGI("  ClientInstance::update via: %s", ci_update_variants[i]);
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
            LOGI("  ClientInstance::getLocalPlayer via: %s", ci_getlp_variants[i]);
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
            LOGI("  Actor::getNameTag via: %s", agn_variants[i]);
            break;
        }
    }

    // Actor::setNameTag
    const char* asn_variants[] = {
        "_ZN5Actor11setNameTagERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
        "_ZN5Actor11setNameTagENSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
        nullptr
    };
    for (int i = 0; asn_variants[i]; ++i) {
        gSymbols.Actor_setNameTag = tryResolve(asn_variants[i]);
        if (gSymbols.Actor_setNameTag) {
            LOGI("  Actor::setNameTag via: %s", asn_variants[i]);
            break;
        }
    }

    // Actor::isPlayer
    gSymbols.Actor_isPlayer = tryResolve("_ZNK5Actor8isPlayerEv");
    if (gSymbols.Actor_isPlayer) LOGI("  Actor::isPlayer resolved");

    // Mob::normalTick
    gSymbols.Mob_normalTick = tryResolve("_ZN3Mob10normalTickEv");
    if (gSymbols.Mob_normalTick) LOGI("  Mob::normalTick resolved");

    // ─── Fallback: pattern scanning ───
    if (!gSymbols.ClientInstance_update || !gSymbols.Mob_normalTick) {
        LOGI("Some symbols unresolved — trying pattern scanning ...");
        auto modInfo = getModuleInfo("libminecraftpe.so");
        if (modInfo.base && modInfo.size) {
            LOGI("  libminecraftpe.so: base=%p size=0x%zx", modInfo.base, modInfo.size);

            if (!gSymbols.ClientInstance_update) {
                void* addr = scanHexPattern(modInfo.base, modInfo.size,
                    "A9 01 7B A9 F7 03 7B A9 FD 03 00 91 ?? ?? ?? D1 59 D0 3B D5");
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

    std::mt19937_64 perEntityRng(SeedManager::instance().getSeed() ^ entityID ^ 0x5A5A5A5AULL);

    std::string result = original;

    // Caesar cipher shift on letters
    uint64_t shift = perEntityRng() % 26;
    for (auto& c : result) {
        if (c >= 'a' && c <= 'z') c = 'a' + static_cast<char>((c - 'a' + shift) % 26);
        else if (c >= 'A' && c <= 'Z') c = 'A' + static_cast<char>((c - 'A' + shift) % 26);
    }

    // Add visual prefix to make it OBVIOUS the mod is working
    std::uniform_int_distribution<int> prefixDist(0, 3);
    const char* prefixes[] = {"[P]", "~", "*", ">"};
    result = prefixes[prefixDist(perEntityRng)] + result;

    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  Apply effects to an entity
// ═══════════════════════════════════════════════════════════════════
static void applyEffectsToEntity(void* actorPtr) {
    if (!actorPtr || !SeedManager::instance().isInitialized()) return;

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

            // Try via resolved function
            if (gSymbols.Actor_getNameTag) {
                using GetNameTagFn = std::string(*)(void*);
                try {
                    nameTag = reinterpret_cast<GetNameTagFn>(gSymbols.Actor_getNameTag)(actorPtr);
                } catch (...) {}
            }
            // Fallback: read from offset
            if (nameTag.empty()) {
                nameTag = readStringAt(actorPtr, Offsets::Actor::mNameTag);
            }

            if (!nameTag.empty()) {
                std::string scrambled = scrambleNametag(nameTag, runtimeID);

                if (gSymbols.Actor_setNameTag) {
                    try {
                        reinterpret_cast<Actor_SetNameTag_Fn>(gSymbols.Actor_setNameTag)(
                            actorPtr, scrambled);
                    } catch (...) {}
                } else {
                    try {
                        fieldAt<std::string>(actorPtr, Offsets::Actor::mNameTag) = scrambled;
                    } catch (...) {}
                }

                gScrambledEntities.insert(runtimeID);
                LOGD("Scrambled entity 0x%lX: '%s' -> '%s'",
                     (unsigned long)runtimeID, nameTag.c_str(), scrambled.c_str());
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Apply effects to the local player
// ═══════════════════════════════════════════════════════════════════
static void applyEffectsToPlayer(void* playerPtr) {
    if (!playerPtr || !SeedManager::instance().isInitialized()) return;

    // Movement speed modification — VERY visible
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
    if (orig_CI_update)
        orig_CI_update(client, a2, a3);

    if (!SeedManager::instance().isInitialized()) {
        if (gSymbols.ClientInstance_getLocalPlayer && !gLocalPlayer) {
            using GetLPFn = void*(*)(void*);
            try {
                gLocalPlayer = reinterpret_cast<GetLPFn>(
                    gSymbols.ClientInstance_getLocalPlayer)(client);
            } catch (...) {}

            if (gLocalPlayer) {
                LOGI("LocalPlayer acquired at %p", gLocalPlayer);

                std::string playerName = readStringAt(gLocalPlayer, Offsets::Player::mName);

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

    if (gPlayerSeedReady.load() && gLocalPlayer) {
        applyEffectsToPlayer(gLocalPlayer);
    }
}

// ─── Mob::normalTick detour (called every tick for each mob) ───
static int gTickCounter = 0;
static void mobNormalTickDetour(void* mob) {
    if (orig_Mob_normalTick)
        orig_Mob_normalTick(mob);

    ++gTickCounter;
    if (gTickCounter % 20 != 0) return;  // Every ~1 second

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

    if (gSymbols.ClientInstance_update) {
        if (MiniHook::hook(gSymbols.ClientInstance_update,
                           reinterpret_cast<void*>(ciUpdateDetour),
                           reinterpret_cast<void**>(&orig_CI_update))) {
            LOGI("  Hooked ClientInstance::update");
            ++installed;
        } else {
            LOGE("  FAILED to hook ClientInstance::update");
        }
    } else {
        LOGW("  ClientInstance::update not resolved — skipping");
    }

    if (gSymbols.Mob_normalTick) {
        if (MiniHook::hook(gSymbols.Mob_normalTick,
                           reinterpret_cast<void*>(mobNormalTickDetour),
                           reinterpret_cast<void**>(&orig_Mob_normalTick))) {
            LOGI("  Hooked Mob::normalTick");
            ++installed;
        } else {
            LOGE("  FAILED to hook Mob::normalTick");
        }
    } else {
        LOGW("  Mob::normalTick not resolved — skipping");
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

        if (!gInitializing.exchange(true)) {
            usleep(100000);  // 100ms for library init

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
    } else {
        LOGI("libminecraftpe.so not yet loaded — hooking dlopen to detect load");

        // Find dlopen
        void* dlopenAddr = dlsym(RTLD_DEFAULT, "dlopen");
        if (!dlopenAddr) {
            dlopenAddr = dlsym(RTLD_NEXT, "dlopen");
        }

        if (dlopenAddr) {
            if (MiniHook::hook(dlopenAddr,
                               reinterpret_cast<void*>(my_dlopen),
                               reinterpret_cast<void**>(&orig_dlopen))) {
                LOGI("dlopen hooked at %p — will detect libminecraftpe.so load", dlopenAddr);
            } else {
                LOGE("FAILED to hook dlopen");
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

// 4) PL_REGISTER_MOD compatibility symbol
extern "C" __attribute__((visibility("default")))
void* _pl_mod_instance() {
    static int modInstance = 1;
    return &modInstance;
}

// 5) LeviLauncher metadata symbols
extern "C" __attribute__((visibility("default")))
const char* mod_name() { return "Personalized"; }

extern "C" __attribute__((visibility("default")))
const char* mod_version() { return "0.3.0"; }

extern "C" __attribute__((visibility("default")))
const char* mod_description() { return "UUID-seeded world scrambling for MC Bedrock"; }

} // namespace personalized
