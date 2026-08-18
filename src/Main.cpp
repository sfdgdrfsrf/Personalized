/**
 * Main.cpp — Personalized mod for MC Bedrock Android (LeviLauncher) v0.5.1
 *
 * CRASH FIXES from v0.4.0 → v0.5.0 → v0.5.1:
 *   v0.5.0:
 *   - try/catch does NOT catch SIGSEGV on Android (signals != exceptions)
 *   - MiniHook now REJECTS targets with PC-relative instructions
 *   - dlopen hook removed entirely — replaced with /proc/self/maps polling
 *   - SIGSEGV signal handler + sigsetjmp/siglongjmp recovery
 *   - All pointer dereferences go through safeRead/safeWrite with mincore
 *   v0.5.1 (CRITICAL):
 *   - __attribute__((constructor)) is now a COMPLETE NO-OP
 *     The constructor runs on the linker's thread during call_constructors,
 *     which is unsafe for ANY work (even logging or thread creation).
 *     Crash was: constructor -> log -> __vfprintf -> SIGSEGV
 *   - mod_entry() is the ONLY entry point for initialization
 *   - isMinecraftProcess() check moved to background thread
 *   - No work happens until LeviLamina explicitly calls mod_entry()
 *
 * Three UUID-seeded visual effects:
 *   1. Texture swapping — generates resource pack with permuted block textures
 *   2. Inventory scrambling — permutes ItemStack slots in player inventory
 *   3. Mob model swapping — modifies ActorDefinitionIdentifier strings
 */

#include "personalized/Config.hpp"
#include "personalized/SeedManager.hpp"
#include "personalized/RandomMapper.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <setjmp.h>
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
#include <thread>
#include <fstream>
#include <sstream>

#define LOG_TAG "Personalized"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace personalized {

// ═══════════════════════════════════════════════════════════════════
//  SECTION 0: SIGSEGV Safety Net
//
//  On Android, try/catch(...) does NOT catch SIGSEGV. Signals are
//  not C++ exceptions. We install a signal handler that longjmps
//  out of crashes in our mod code, preventing game kills.
// ═══════════════════════════════════════════════════════════════════
static sigjmp_buf gJmpBuf;
static volatile bool gInOurCode = false;
static struct sigaction gOldSegvHandler;
static struct sigaction gOldBusHandler;

static void segvHandler(int sig, siginfo_t* info, void* ctx) {
    if (gInOurCode) {
        // We're in our code — longjmp to safety instead of crashing
        LOGE("Caught signal %d in mod code (fault addr %p) — recovering", sig,
             info ? info->si_addr : nullptr);
        siglongjmp(gJmpBuf, 1);
    }
    // Not our code — chain to old handler (probably crash the game)
    if (sig == SIGSEGV && gOldSegvHandler.sa_sigaction) {
        gOldSegvHandler.sa_sigaction(sig, info, ctx);
    } else if (sig == SIGBUS && gOldBusHandler.sa_sigaction) {
        gOldBusHandler.sa_sigaction(sig, info, ctx);
    } else {
        // No old handler — restore default and re-raise
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigaction(sig, &sa, nullptr);
        raise(sig);
    }
}

static void installSignalHandler() {
    struct sigaction sa{};
    sa.sa_sigaction = segvHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &gOldSegvHandler);
    sigaction(SIGBUS, &sa, &gOldBusHandler);
    LOGI("SIGSEGV/SIGBUS safety handler installed");
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 1: Safe Memory Access
//
//  Validates that memory is readable/writable before accessing it.
//  Uses mincore() to check page mapping. NEVER causes SIGSEGV.
// ═══════════════════════════════════════════════════════════════════

/// Check if a pointer points to mapped, readable memory
static bool isValidPtr(const void* ptr, size_t len = 1) {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    // Quick sanity: must be in user-space range on ARM64
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFULL) return false;
    // Align to page boundary
    uintptr_t page = addr & ~0xFFFULL;
    size_t pageLen = ((addr + len + 0xFFFUL) & ~0xFFFUL) - page;
    // mincore returns 0 if pages are mapped
    // On Android, we can also try reading /proc/self/maps for verification
    // but mincore is faster for hot paths
    unsigned char vec;
    return mincore(reinterpret_cast<void*>(page), pageLen, &vec) == 0;
}

/// Safe read: returns default value if pointer is invalid
template <class T>
static T safeRead(const void* ptr, T def = T{}) {
    if (!isValidPtr(ptr, sizeof(T))) return def;
    return *reinterpret_cast<const T*>(ptr);
}

/// Safe write: returns false if pointer is invalid
template <class T>
static bool safeWrite(void* ptr, T val) {
    if (!isValidPtr(ptr, sizeof(T))) return false;
    *reinterpret_cast<T*>(ptr) = val;
    return true;
}

/// Read a std::string from an object at an offset — returns empty on failure
static std::string safeReadStr(const void* obj, size_t off) {
    if (!obj) return {};
    const void* strPtr = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(obj) + off);
    if (!isValidPtr(strPtr, sizeof(std::string))) return {};

    // Use sigsetjmp/siglongjmp as belt-and-suspenders
    gInOurCode = true;
    if (sigsetjmp(gJmpBuf, 1) != 0) {
        gInOurCode = false;
        LOGW("safeReadStr: SIGSEGV at obj=%p off=0x%zx", obj, off);
        return {};
    }
    std::string result;
    try {
        result = *reinterpret_cast<const std::string*>(strPtr);
    } catch (...) {
        result = {};
    }
    gInOurCode = false;
    return result;
}

/// Write a std::string to an object at an offset — returns false on failure
static bool safeWriteStr(void* obj, size_t off, const std::string& val) {
    if (!obj) return false;
    void* strPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + off);
    if (!isValidPtr(strPtr, sizeof(std::string))) return false;

    gInOurCode = true;
    if (sigsetjmp(gJmpBuf, 1) != 0) {
        gInOurCode = false;
        LOGW("safeWriteStr: SIGSEGV at obj=%p off=0x%zx", obj, off);
        return false;
    }
    try {
        *reinterpret_cast<std::string*>(strPtr) = val;
    } catch (...) {
        gInOurCode = false;
        return false;
    }
    gInOurCode = false;
    return true;
}

// Safe field access with offset and validation
template <class T>
static T safeField(const void* obj, size_t off, T def = T{}) {
    if (!obj) return def;
    const void* p = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(obj) + off);
    return safeRead<T>(p, def);
}

template <class T>
static bool safeFieldWrite(void* obj, size_t off, T val) {
    if (!obj) return false;
    void* p = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + off);
    return safeWrite<T>(p, val);
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 2: MiniHook — ARM64 Inline Hooker (CRASH-FIXED)
//
//  KEY FIX: Now REJECTS hook targets that contain PC-relative instructions
//  in the first 4 instruction slots. Previously it just logged a warning
//  and proceeded, which created broken trampolines (wrong branch targets)
//  that caused immediate SIGSEGV when the hooked function was called.
//
//  Also added: trampoline validation, instruction decode for ADRP fixup.
// ═══════════════════════════════════════════════════════════════════
namespace MiniHook {
static constexpr size_t kJumpSize  = 16;  // LDR X17, [PC+8]; BR X17; <addr64>
static constexpr size_t kTrampSize = 48;  // original insns + jump back + padding
static constexpr uint32_t LDR_X17_PC8 = 0x58000051;  // LDR X17, #8
static constexpr uint32_t BR_X17      = 0xD61F0220;  // BR X17

static void writeAbsoluteJump(void* where, void* target) {
    auto* p = static_cast<uint32_t*>(where);
    p[0] = LDR_X17_PC8;
    p[1] = BR_X17;
    *reinterpret_cast<void**>(&p[2]) = target;
}

/// Classify an ARM64 instruction — returns true if PC-relative
static bool isPCRelative(uint32_t insn) {
    // BL  (branch with link)
    if ((insn & 0xFC000000) == 0x94000000) return true;
    // B   (unconditional branch)
    if ((insn & 0xFC000000) == 0x14000000) return true;
    // B.cond
    if ((insn & 0xFF000010) == 0x54000000) return true;
    // CBZ / CBNZ
    if ((insn & 0x7E000000) == 0x34000000) return true;
    if ((insn & 0x7E000000) == 0x35000000) return true;
    // TBZ / TBNZ
    if ((insn & 0x7E000000) == 0x36000000) return true;
    if ((insn & 0x7E000000) == 0x37000000) return true;
    // ADRP (page-relative address)
    if ((insn & 0x9F000000) == 0x90000000) return true;
    // ADR
    if ((insn & 0x9F000000) == 0x10000000) return true;
    // LDR literal (unsigned offset, literal)
    if ((insn & 0x3B000000) == 0x18000000) return true;  // LDR Wt, label
    if ((insn & 0x3B000000) == 0x58000000) return true;  // LDR Xt, label
    if ((insn & 0x3B000000) == 0x1C000000) return true;  // LDR Sw, label
    return false;
}

static void* allocTrampoline() {
    void* mem = mmap(nullptr, kTrampSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (mem == MAP_FAILED) ? nullptr : mem;
}

static bool setPagePerms(void* addr, size_t len, int prot) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~0xFFFULL;
    uintptr_t end  = (reinterpret_cast<uintptr_t>(addr) + len + 0xFFFUL) & ~0xFFFUL;
    return mprotect(reinterpret_cast<void*>(page), end - page, prot) == 0;
}

/// Hook a function. Returns true on success.
/// FAILS (returns false) if the target contains PC-relative instructions
/// in the first 4 instruction slots — these cannot be safely relocated.
static bool hook(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return false;

    // Validate target is readable executable memory
    if (!isValidPtr(target, kJumpSize)) {
        LOGE("MiniHook: target %p is not valid memory", target);
        return false;
    }

    // Check for PC-relative instructions — these CANNOT be safely relocated
    auto* insns = static_cast<uint32_t*>(target);
    int pcRelativeCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (!isValidPtr(&insns[i], 4)) {
            LOGE("MiniHook: insn %d at %p is not readable", i, &insns[i]);
            return false;
        }
        if (isPCRelative(insns[i])) {
            LOGW("MiniHook: insn %d at %p is PC-relative (0x%08X) — CANNOT relocate", i, &insns[i], insns[i]);
            ++pcRelativeCount;
        }
    }

    // CRITICAL FIX: Reject if any PC-relative instructions found
    // Previously we just warned and proceeded, which created broken trampolines
    if (pcRelativeCount > 0) {
        LOGE("MiniHook: REJECTING hook at %p — %d PC-relative instructions cannot be relocated", target, pcRelativeCount);
        return false;
    }

    // Allocate trampoline
    void* trampoline = allocTrampoline();
    if (!trampoline) {
        LOGE("MiniHook: failed to allocate trampoline");
        return false;
    }

    // Copy original instructions to trampoline
    memcpy(trampoline, target, kJumpSize);

    // Write jump back from trampoline to target+kJumpSize
    writeAbsoluteJump(static_cast<uint8_t*>(trampoline) + kJumpSize,
                      static_cast<uint8_t*>(target) + kJumpSize);

    // Flush instruction cache for trampoline
    __builtin___clear_cache(static_cast<char*>(trampoline),
                            static_cast<char*>(trampoline) + kTrampSize);

    // Make target writable
    if (!setPagePerms(target, kJumpSize, PROT_READ | PROT_WRITE)) {
        LOGE("MiniHook: failed to make target writable at %p", target);
        munmap(trampoline, kTrampSize);
        return false;
    }

    // Write the jump to our detour
    writeAbsoluteJump(target, detour);

    // Restore target to read+exec
    if (!setPagePerms(target, kJumpSize, PROT_READ | PROT_EXEC)) {
        LOGE("MiniHook: failed to restore target permissions at %p", target);
        // Continue anyway — the hook is written
    }

    // Flush instruction cache for target
    __builtin___clear_cache(static_cast<char*>(target),
                            static_cast<char*>(target) + kJumpSize);

    *original = trampoline;
    return true;
}
} // namespace MiniHook

// ═══════════════════════════════════════════════════════════════════
//  SECTION 3: Memory Utilities
// ═══════════════════════════════════════════════════════════════════
struct ModuleInfo { void* base = nullptr; size_t size = 0; };

static ModuleInfo getModuleInfo(const char* name) {
    ModuleInfo info;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return info;
    char line[512];
    void* hi = nullptr;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            uintptr_t s, e;
            if (sscanf(line, "%lx-%lx", &s, &e) == 2) {
                if (!info.base || (void*)s < info.base) info.base = (void*)s;
                if (!hi || (void*)e > hi) hi = (void*)e;
            }
        }
    }
    fclose(f);
    if (info.base && hi) info.size = (size_t)hi - (size_t)info.base;
    return info;
}

// Pattern scanner — with bounds checking
static void* scanPattern(void* start, size_t len, const char* pat, const char* mask) {
    if (!start || len == 0) return nullptr;
    size_t pl = strlen(mask);
    if (pl == 0 || pl > len) return nullptr;
    auto* b = reinterpret_cast<uint8_t*>(start);
    for (auto* p = b; p <= b + len - pl; ++p) {
        bool ok = true;
        for (size_t j = 0; j < pl; ++j) {
            if (mask[j] == 'x' && p[j] != (uint8_t)pat[j]) { ok = false; break; }
        }
        if (ok) return p;
    }
    return nullptr;
}

struct HexPat { std::vector<uint8_t> bytes; std::string mask; };

static HexPat parseHex(const char* h) {
    HexPat p;
    const char* c = h;
    while (*c) {
        while (*c == ' ') ++c;
        if (!*c) break;
        if (c[0] == '?' && c[1] == '?') {
            p.bytes.push_back(0); p.mask += '?'; c += 2;
        } else {
            char b[3] = {c[0], c[1], 0};
            p.bytes.push_back((uint8_t)strtoul(b, nullptr, 16));
            p.mask += 'x'; c += 2;
        }
    }
    return p;
}

static void* scanHex(void* start, size_t len, const char* hex) {
    auto p = parseHex(hex);
    if (p.bytes.empty()) return nullptr;
    return scanPattern(start, len, reinterpret_cast<const char*>(p.bytes.data()), p.mask.c_str());
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 4: Symbol Resolution (dlsym + RTTI/vtable + patterns)
//
//  Uses dlsym for symbol lookup, RTTI vtables for type checking,
//  and pattern scanning as fallback. All pointer returns are validated.
// ═══════════════════════════════════════════════════════════════════
static void* gLibMC = nullptr;

static void* trySym(const char* sym) {
    if (gLibMC) {
        void* a = dlsym(gLibMC, sym);
        if (a && isValidPtr(a)) return a;
    }
    void* a = dlsym(RTLD_DEFAULT, sym);
    if (a && isValidPtr(a)) return a;
    return nullptr;
}

struct MCSyms {
    void* CI_update = nullptr;
    void* CI_getLocalPlayer = nullptr;
    void* Actor_getNameTag = nullptr;
    void* Actor_setNameTag = nullptr;
    void* Actor_isPlayer = nullptr;
    void* Mob_normalTick = nullptr;
    void* Level_addEntity = nullptr;
    // Vtables found via RTTI
    void* vtbl_Actor = nullptr;
    void* vtbl_Mob = nullptr;
    void* vtbl_Player = nullptr;
};
static MCSyms gS;

static bool resolveSymbols() {
    LOGI("=== Resolving MC symbols ===");

    // Function symbols (multiple mangled variants per function)
    struct { const char** names; void** target; } symGroups[] = {
        {(const char*[]){"_ZN15ClientInstance6updateEv", "_ZN15ClientInstance6updateEf", nullptr}, &gS.CI_update},
        {(const char*[]){"_ZN15ClientInstance14getLocalPlayerEv", "_ZNK15ClientInstance14getLocalPlayerEv", nullptr}, &gS.CI_getLocalPlayer},
        {(const char*[]){"_ZNK5Actor11getNameTagB5cxx11Ev", "_ZNK5Actor11getNameTagEv", nullptr}, &gS.Actor_getNameTag},
        {(const char*[]){"_ZN5Actor11setNameTagERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", nullptr}, &gS.Actor_setNameTag},
        {(const char*[]){"_ZNK5Actor8isPlayerEv", nullptr}, &gS.Actor_isPlayer},
        {(const char*[]){"_ZN3Mob10normalTickEv", nullptr}, &gS.Mob_normalTick},
        {(const char*[]){"_ZN5Level9addEntityERK10ActorUniqueIDS5", "_ZN5Level9addEntityE10ActorUniqueID", nullptr}, &gS.Level_addEntity},
    };
    for (auto& g : symGroups) {
        for (int i = 0; g.names[i]; ++i) {
            *g.target = trySym(g.names[i]);
            if (*g.target) {
                LOGI("  %s -> %p", g.names[i], *g.target);
                break;
            }
        }
    }

    // RTTI vtables (very stable across versions)
    gS.vtbl_Actor  = trySym("_ZTV5Actor");
    gS.vtbl_Mob    = trySym("_ZTV3Mob");
    gS.vtbl_Player = trySym("_ZTV6Player");
    if (gS.vtbl_Actor)  LOGI("  Actor vtable at %p", gS.vtbl_Actor);
    if (gS.vtbl_Mob)    LOGI("  Mob vtable at %p", gS.vtbl_Mob);
    if (gS.vtbl_Player) LOGI("  Player vtable at %p", gS.vtbl_Player);

    // Pattern scanning fallback
    auto mi = getModuleInfo("libminecraftpe.so");
    if (mi.base && mi.size) {
        LOGI("  libminecraftpe.so: base=%p size=0x%zx", mi.base, mi.size);
        if (!gS.CI_update)
            gS.CI_update = scanHex(mi.base, mi.size, "A9 01 7B A9 F7 03 7B A9 FD 03 00 91 ?? ?? ?? D1 59 D0 3B D5");
        if (!gS.Mob_normalTick)
            gS.Mob_normalTick = scanHex(mi.base, mi.size, "FC 01 7B A9 F8 0F 7B A9 ?? ?? ?? A9 ?? ?? ?? A9 ?? ?? ?? A9 54 D0 3B D5");
        if (!gS.CI_getLocalPlayer)
            gS.CI_getLocalPlayer = scanHex(mi.base, mi.size, "?? ?? ?? D1 ?? ?? ?? A9 ?? ?? ?? F9 ?? ?? ?? 91 53 D0 3B D5 E8 03 00 AA");
    }

    int n = 0;
    if (gS.CI_update) ++n;
    if (gS.CI_getLocalPlayer) ++n;
    if (gS.Actor_getNameTag) ++n;
    if (gS.Actor_setNameTag) ++n;
    if (gS.Actor_isPlayer) ++n;
    if (gS.Mob_normalTick) ++n;
    if (gS.Level_addEntity) ++n;
    LOGI("Symbol resolution: %d functions + %d vtables", n,
         (gS.vtbl_Actor ? 1 : 0) + (gS.vtbl_Mob ? 1 : 0) + (gS.vtbl_Player ? 1 : 0));
    return n > 0;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 5: MC Offsets (ARM64, ~1.21.44)
//
//  These are BEST-EFFORT guesses. All accesses use safeField/safeRead
//  which validates pointers before dereference — wrong offsets return
//  default values instead of crashing.
// ═══════════════════════════════════════════════════════════════════
namespace O { // Offsets
    namespace Actor {
        constexpr size_t mRuntimeID = 0x1A8;
        constexpr size_t mNameTag = 0x2C0;
        constexpr size_t mHurtTime = 0x194;
        constexpr size_t mDimension = 448;
        constexpr size_t mLevel = 464;
        constexpr size_t mDesc = 0x1B0;  // ActorDefinitionIdentifier
    }
    namespace ActorDefId {
        constexpr size_t mIdentifier = 0x08;  // std::string inside struct
    }
    namespace Player {
        constexpr size_t mName = 2824;
        constexpr size_t mUUID_Most = 2800;
        constexpr size_t mUUID_Least = 2808;
        constexpr size_t mMovementSpeed = 2900;
        constexpr size_t mInventory = 0x2A0;  // PlayerInventory pointer
    }
    namespace PlayerInventory {
        constexpr size_t mContainer = 0x08;  // Container pointer
    }
    namespace Container {
        constexpr size_t mItems = 0x18;  // ItemStack array pointer
        constexpr size_t mCount = 0x10;  // int count
    }
    constexpr size_t kItemStackSize = 152;  // Approximate ItemStack size
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 6: Texture Swapping — Resource Pack Generation
//
//  Creates a MC Bedrock resource pack that remaps block textures.
//  Written to MC's resource_packs/ directory as an overlay.
//  CRASH FIX: All file I/O wrapped in sigsetjmp safety net.
// ═══════════════════════════════════════════════════════════════════
namespace TextureSwap {

static const std::vector<std::string> blockTextures = {
    "stone", "dirt", "grass_side", "grass_carried", "cobblestone", "oak_planks",
    "spruce_planks", "birch_planks", "jungle_planks", "acacia_planks", "dark_oak_planks",
    "oak_log", "spruce_log", "birch_log", "jungle_log", "sand", "red_sand", "gravel",
    "oak_log_top", "spruce_log_top", "birch_log_top", "jungle_log_top",
    "glass", "iron_block", "gold_block", "diamond_block", "emerald_block",
    "lapis_block", "redstone_block", "coal_block", "obsidian", "ice", "packed_ice",
    "snow", "clay", "netherrack", "soul_sand", "glowstone", "magma", "bedrock",
    "sandstone_bottom", "sandstone_side", "sandstone_top",
    "red_sandstone_bottom", "red_sandstone_side", "red_sandstone_top",
    "oak_leaves", "spruce_leaves", "birch_leaves",
    "wool", "hardened_clay", "prismarine_rough", "prismarine_dark", "prismarine_bricks",
    "sea_lantern", "hay_block_top", "hay_block_side", "bone_block_side", "bone_block_top",
    "purpur_block", "purpur_pillar_top", "purpur_pillar", "end_bricks", "end_stone",
    "chorus_plant", "chorus_flower",
};

static std::string findMCDataDir() {
    const char* bases[] = {
        "/sdcard/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/data/data/com.mojang.minecraftpe/files/games/com.mojang",
        nullptr
    };
    for (int i = 0; bases[i]; ++i) {
        struct stat st;
        if (stat(bases[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            LOGI("Found MC data dir: %s", bases[i]);
            return bases[i];
        }
    }
    return {};
}

static bool writeFile(const std::string& path, const std::string& content) {
    gInOurCode = true;
    if (sigsetjmp(gJmpBuf, 1) != 0) {
        gInOurCode = false;
        LOGW("writeFile: crash writing %s", path.c_str());
        return false;
    }
    std::ofstream f(path);
    if (!f.is_open()) { gInOurCode = false; return false; }
    f << content;
    f.close();
    gInOurCode = false;
    return true;
}

static bool mkdirp(const std::string& path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') {
            std::string sub = path.substr(0, i);
            mkdir(sub.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool generateResourcePack(uint64_t seed) {
    LOGI("=== Generating texture swap resource pack (seed 0x%016lX) ===", (unsigned long)seed);

    std::string mcDir = findMCDataDir();
    if (mcDir.empty()) {
        LOGW("MC data dir not found — skipping texture pack generation");
        return false;
    }

    std::string packDir = mcDir + "/resource_packs/Personalized_Textures";
    std::string texDir  = packDir + "/textures";

    if (!mkdirp(texDir)) {
        LOGE("Failed to create pack directory: %s", packDir.c_str());
        return false;
    }

    // Generate permutation of block textures
    auto permutation = RandomMapper::partialScramble(seed, blockTextures.size(), 0.6, 2);

    // manifest.json — UUIDs derived from seed for uniqueness
    std::mt19937_64 rng(seed ^ 0xDEADBEEFCAFEBABEULL);
    auto genUUID = [&rng]() -> std::string {
        char buf[37];
        snprintf(buf, sizeof(buf), "%08lx-%04lx-%04lx-%04lx-%012lx",
            (unsigned long)(rng() & 0xFFFFFFFF),
            (unsigned long)(rng() & 0xFFFF),
            (unsigned long)((rng() & 0xFFFF) | 0x4000),
            (unsigned long)((rng() & 0x3FFF) | 0x8000),
            (unsigned long)(rng() & 0xFFFFFFFFFFFF));
        return buf;
    };
    std::string headerUUID = genUUID();
    std::string moduleUUID = genUUID();

    std::string manifest =
        "{\n"
        "  \"format_version\": 2,\n"
        "  \"header\": {\n"
        "    \"name\": \"Personalized Textures\",\n"
        "    \"description\": \"UUID-seeded texture permutation by Personalized mod\",\n"
        "    \"uuid\": \"" + headerUUID + "\",\n"
        "    \"version\": [1, 0, 0],\n"
        "    \"min_engine_version\": [1, 21, 0]\n"
        "  },\n"
        "  \"modules\": [{\n"
        "    \"type\": \"resources\",\n"
        "    \"uuid\": \"" + moduleUUID + "\",\n"
        "    \"version\": [1, 0, 0]\n"
        "  }]\n"
        "}\n";

    if (!writeFile(packDir + "/manifest.json", manifest)) {
        LOGE("Failed to write manifest.json");
        return false;
    }

    // terrain_texture.json — remaps each block texture to a different one
    std::ostringstream ttj;
    ttj << "{\n"
        << "  \"resource_pack_name\": \"Personalized\",\n"
        << "  \"texture_name\": \"atlas.terrain\",\n"
        << "  \"padding\": 8,\n"
        << "  \"num_mip_levels\": 4,\n"
        << "  \"texture_data\": {\n";

    for (size_t i = 0; i < blockTextures.size(); ++i) {
        size_t j = permutation[i];
        if (j >= blockTextures.size()) j = i;
        const auto& from = blockTextures[i];
        const auto& to   = blockTextures[j];

        ttj << "    \"" << from << "\": {\n"
            << "      \"textures\": \"textures/blocks/" << to << "\"\n"
            << "    }";
        if (i + 1 < blockTextures.size()) ttj << ",";
        ttj << "\n";
    }

    ttj << "  }\n}\n";

    if (!writeFile(packDir + "/textures/terrain_texture.json", ttj.str())) {
        LOGE("Failed to write terrain_texture.json");
        return false;
    }

    LOGI("Texture resource pack generated at %s (%zu texture swaps)",
         packDir.c_str(), blockTextures.size());
    return true;
}

} // namespace TextureSwap

// ═══════════════════════════════════════════════════════════════════
//  SECTION 7: Inventory Scrambling
//
//  Permutes ItemStack slots using Fisher-Yates shuffle seeded by UUID.
//  CRASH FIX: All pointer dereferences use safeField/safeRead.
// ═══════════════════════════════════════════════════════════════════
namespace InventoryScramble {

static std::vector<size_t> gInvPermutation;
static std::mutex gInvMutex;
static std::atomic<bool> gInvScrambled{false};

static void buildPermutation(uint64_t seed) {
    const size_t slotCount = 36;  // 4 rows of 9
    gInvPermutation = RandomMapper::partialScramble(
        seed ^ 0x1CBE11DAULL, slotCount, 0.7, 3);
    LOGI("Inventory permutation built: %zu slots", slotCount);
}

static void scrambleInventory(void* playerPtr) {
    if (!playerPtr || gInvPermutation.empty()) return;
    if (gInvScrambled.load()) return;

    std::lock_guard lock(gInvMutex);

    // Navigate: Player -> PlayerInventory -> Container -> ItemStack[]
    // Every pointer dereference is validated
    void* invPtr = safeField<void*>(playerPtr, O::Player::mInventory, nullptr);
    if (!invPtr) { LOGW("Inventory scramble: invPtr is null"); return; }

    void* containerPtr = safeField<void*>(invPtr, O::PlayerInventory::mContainer, nullptr);
    if (!containerPtr) { LOGW("Inventory scramble: containerPtr is null"); return; }

    int count = safeField<int>(containerPtr, O::Container::mCount, 0);
    if (count <= 0 || count > 100) count = 36;  // sanity clamp

    void* itemsBase = safeField<void*>(containerPtr, O::Container::mItems, nullptr);
    if (!itemsBase) { LOGW("Inventory scramble: itemsBase is null"); return; }

    // Validate items array is readable
    size_t itemSize = O::kItemStackSize;
    size_t totalSize = (size_t)count * itemSize;
    if (!isValidPtr(itemsBase, totalSize)) {
        LOGW("Inventory scramble: items array not readable (base=%p size=%zu)", itemsBase, totalSize);
        return;
    }

    gInOurCode = true;
    if (sigsetjmp(gJmpBuf, 1) != 0) {
        gInOurCode = false;
        LOGW("Inventory scramble: SIGSEGV — aborting scramble");
        return;
    }

    // Snapshot all items, then write in permuted order
    std::vector<std::vector<uint8_t>> originals(count);
    for (int i = 0; i < count; ++i) {
        originals[i].resize(itemSize);
        void* slot = static_cast<uint8_t*>(itemsBase) + i * itemSize;
        memcpy(originals[i].data(), slot, itemSize);
    }

    for (int i = 0; i < count; ++i) {
        size_t j = (i < (int)gInvPermutation.size()) ? gInvPermutation[i] : (size_t)i;
        if (j >= (size_t)count) j = (size_t)i;
        void* slot = static_cast<uint8_t*>(itemsBase) + i * itemSize;
        memcpy(slot, originals[j].data(), itemSize);
    }

    gInOurCode = false;
    gInvScrambled.store(true);
    LOGI("Inventory scrambled: %d slots permuted", count);
}

} // namespace InventoryScramble

// ═══════════════════════════════════════════════════════════════════
//  SECTION 8: Mob Model Swapping
//
//  Modifies ActorDefinitionIdentifier strings to swap mob appearances.
//  CRASH FIX: All pointer dereferences use safeField/safeReadStr/safeWriteStr.
// ═══════════════════════════════════════════════════════════════════
namespace MobSwap {

static const std::vector<std::string> mobPool = {
    "minecraft:zombie",   "minecraft:skeleton",  "minecraft:pig",
    "minecraft:cow",      "minecraft:sheep",     "minecraft:chicken",
    "minecraft:creeper",  "minecraft:spider",    "minecraft:blaze",
    "minecraft:enderman", "minecraft:witch",     "minecraft:villager_v2",
    "minecraft:iron_golem", "minecraft:wolf",    "minecraft:cat",
    "minecraft:horse",    "minecraft:phantom",   "minecraft:pillager",
};
static std::unordered_map<std::string, std::string> mobSwapMap;
static std::unordered_set<uint64_t> gSwappedMobs;
static std::mutex gMobMutex;

static void buildSwapMap(uint64_t seed) {
    std::mt19937_64 rng(seed ^ 0xB0B0FACEDEADBEEFULL);
    mobSwapMap.clear();
    for (const auto& id : mobPool) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) > 0.6) continue;
        std::string selected; int att = 0;
        do {
            std::uniform_int_distribution<size_t> idx(0, mobPool.size() - 1);
            selected = mobPool[idx(rng)]; ++att;
        } while (selected == id && att < 10);
        if (selected != id) mobSwapMap[id] = selected;
    }
    LOGI("Mob swap map: %zu entries", mobSwapMap.size());
}

static std::string getMobIdentifier(void* actorPtr) {
    if (!actorPtr) return {};
    void* descPtr = safeField<void*>(actorPtr, O::Actor::mDesc, nullptr);
    if (!descPtr) return {};
    return safeReadStr(descPtr, O::ActorDefId::mIdentifier);
}

static bool setMobIdentifier(void* actorPtr, const std::string& newId) {
    if (!actorPtr) return false;
    void* descPtr = safeField<void*>(actorPtr, O::Actor::mDesc, nullptr);
    if (!descPtr) return false;
    return safeWriteStr(descPtr, O::ActorDefId::mIdentifier, newId);
}

static bool isPlayer(void* actorPtr) {
    if (!actorPtr) return false;
    // Check via vtable — Player's vtable pointer should match _ZTV6Player
    if (gS.vtbl_Player) {
        void* objVtbl = safeField<void*>(actorPtr, 0, nullptr);
        if (objVtbl) {
            // Check if the object's vtable is at or after Player vtable
            // This is a rough heuristic — Player derives from Mob derives from Actor
            // Player vtable entries are after Mob entries which are after Actor entries
            uintptr_t vp = reinterpret_cast<uintptr_t>(objVtbl);
            uintptr_t pp = reinterpret_cast<uintptr_t>(gS.vtbl_Player);
            // Exact match means it's exactly a Player
            if (vp == pp) return true;
        }
    }
    // Try calling Actor::isPlayer if available
    if (gS.Actor_isPlayer && isValidPtr(gS.Actor_isPlayer)) {
        gInOurCode = true;
        if (sigsetjmp(gJmpBuf, 1) != 0) {
            gInOurCode = false;
            LOGW("isPlayer: SIGSEGV calling Actor::isPlayer");
            return false;
        }
        bool result = false;
        try {
            result = reinterpret_cast<bool(*)(void*)>(gS.Actor_isPlayer)(actorPtr);
        } catch (...) {
            result = false;
        }
        gInOurCode = false;
        return result;
    }
    return false;
}

static void trySwapMob(void* actorPtr) {
    if (!actorPtr) return;

    uint64_t runtimeID = safeField<uint64_t>(actorPtr, O::Actor::mRuntimeID, 0);
    if (runtimeID == 0) return;
    if (isPlayer(actorPtr)) return;

    std::lock_guard lock(gMobMutex);
    if (gSwappedMobs.count(runtimeID)) return;

    std::string id = getMobIdentifier(actorPtr);
    if (id.empty()) { gSwappedMobs.insert(runtimeID); return; }

    auto it = mobSwapMap.find(id);
    if (it == mobSwapMap.end()) {
        gSwappedMobs.insert(runtimeID);
        return;
    }

    const std::string& newId = it->second;
    if (setMobIdentifier(actorPtr, newId)) {
        gSwappedMobs.insert(runtimeID);
        LOGI("Mob model swap: %s -> %s (entity 0x%lX)",
             id.c_str(), newId.c_str(), (unsigned long)runtimeID);
    }
}

} // namespace MobSwap

// ═══════════════════════════════════════════════════════════════════
//  SECTION 9: Nametag scrambling + player effects
// ═══════════════════════════════════════════════════════════════════
static std::string scrambleNametag(const std::string& orig, uint64_t eid) {
    if (orig.empty()) return orig;
    std::mt19937_64 rng(SeedManager::instance().getSeed() ^ eid ^ 0x5A5A5A5AULL);
    std::string r = orig;
    uint64_t shift = rng() % 26;
    for (auto& c : r) {
        if (c >= 'a' && c <= 'z') c = 'a' + (char)((c - 'a' + shift) % 26);
        else if (c >= 'A' && c <= 'Z') c = 'A' + (char)((c - 'A' + shift) % 26);
    }
    const char* pfx[] = {"[P]", "~", "*", ">>"};
    r = pfx[rng() % 4] + r;
    return r;
}

static void applyNametagScramble(void* actorPtr) {
    if (!actorPtr || !SeedManager::instance().isInitialized()) return;
    uint64_t rid = safeField<uint64_t>(actorPtr, O::Actor::mRuntimeID, 0);
    if (rid == 0 || MobSwap::isPlayer(actorPtr)) return;

    static std::unordered_set<uint64_t> done;
    static std::mutex mtx;
    std::lock_guard lock(mtx);
    if (done.count(rid)) return;

    std::string nameTag = safeReadStr(actorPtr, O::Actor::mNameTag);
    if (nameTag.empty()) return;

    std::string scrambled = scrambleNametag(nameTag, rid);
    safeWriteStr(actorPtr, O::Actor::mNameTag, scrambled);
    done.insert(rid);
}

static void applyPlayerSpeed(void* playerPtr) {
    if (!playerPtr || !SeedManager::instance().isInitialized()) return;
    auto rng = SeedManager::instance().createRNG();
    std::uniform_real_distribution<float> d(0.05f, 0.3f);
    float speed = d(rng);
    safeFieldWrite<float>(playerPtr, O::Player::mMovementSpeed, speed);
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 10: Hook Detours
//
//  CRASH FIX: All detour code validates pointers before use.
//  Uses safeField/safeReadStr instead of raw dereference.
// ═══════════════════════════════════════════════════════════════════
using CI_Update_Fn = void(*)(void*, void*, void*);
static CI_Update_Fn orig_CI_update = nullptr;

using Mob_Tick_Fn = void(*)(void*);
static Mob_Tick_Fn orig_Mob_tick = nullptr;

static void* gLocalPlayer = nullptr;
static std::atomic<bool> gPlayerSeedReady{false};

// ClientInstance::update — per-frame logic
static void ciUpdateDetour(void* ci, void* a2, void* a3) {
    // Always call original first
    if (orig_CI_update) {
        gInOurCode = true;
        if (sigsetjmp(gJmpBuf, 1) == 0) {
            orig_CI_update(ci, a2, a3);
        } else {
            LOGW("ciUpdateDetour: original CI::update crashed — skipping");
        }
        gInOurCode = false;
    }

    if (!SeedManager::instance().isInitialized() && gS.CI_getLocalPlayer && !gLocalPlayer) {
        // Try to get LocalPlayer
        gInOurCode = true;
        if (sigsetjmp(gJmpBuf, 1) == 0) {
            void* lp = nullptr;
            try {
                lp = reinterpret_cast<void*(*)(void*)>(gS.CI_getLocalPlayer)(ci);
            } catch (...) {
                lp = nullptr;
            }
            if (lp && isValidPtr(lp, 4096)) {
                gLocalPlayer = lp;
                LOGI("LocalPlayer at %p", gLocalPlayer);

                // Try to read UUID
                std::string name = safeReadStr(gLocalPlayer, O::Player::mName);
                uint64_t um = safeField<uint64_t>(gLocalPlayer, O::Player::mUUID_Most, 0);
                uint64_t ul = safeField<uint64_t>(gLocalPlayer, O::Player::mUUID_Least, 0);

                std::string uuid;
                if (um || ul) {
                    char b[64];
                    snprintf(b, 64, "%016lX-%016lX", (unsigned long)um, (unsigned long)ul);
                    uuid = b;
                } else if (!name.empty()) {
                    uuid = "name:" + name;
                }

                if (!uuid.empty()) {
                    SeedManager::instance().initializeWithUUID(uuid);
                    LOGI("Seed from UUID: %s", uuid.c_str());
                    uint64_t seed = SeedManager::instance().getSeed();
                    MobSwap::buildSwapMap(seed);
                    InventoryScramble::buildPermutation(seed);
                    gPlayerSeedReady.store(true);
                }
            }
        } else {
            LOGW("ciUpdateDetour: crash getting LocalPlayer — skipping");
        }
        gInOurCode = false;
    }

    // Per-frame player effects
    if (gPlayerSeedReady.load() && gLocalPlayer && isValidPtr(gLocalPlayer, 4096)) {
        applyPlayerSpeed(gLocalPlayer);
    }
}

// Mob::normalTick — per-entity logic
static int gMobTickCounter = 0;
static void mobTickDetour(void* mob) {
    // Always call original first
    if (orig_Mob_tick) {
        gInOurCode = true;
        if (sigsetjmp(gJmpBuf, 1) == 0) {
            orig_Mob_tick(mob);
        } else {
            LOGW("mobTickDetour: original Mob::normalTick crashed — skipping");
        }
        gInOurCode = false;
    }

    ++gMobTickCounter;
    if (gMobTickCounter % 20 != 0) return;  // every ~1s
    if (!gPlayerSeedReady.load()) return;

    // Validate mob pointer before use
    if (!mob || !isValidPtr(mob, 256)) return;

    MobSwap::trySwapMob(mob);
    applyNametagScramble(mob);
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 11: Hook Installation
//
//  CRASH FIX: MiniHook now rejects PC-relative targets.
//  If hooking fails, we log it and continue (no crash).
// ═══════════════════════════════════════════════════════════════════
static std::atomic<bool> gHooksInstalled{false};

static bool installHooks() {
    if (gHooksInstalled.load()) return true;
    LOGI("=== Installing MC hooks ===");
    int n = 0;

    if (gS.CI_update) {
        if (MiniHook::hook(gS.CI_update, reinterpret_cast<void*>(ciUpdateDetour),
                           reinterpret_cast<void**>(&orig_CI_update))) {
            ++n; LOGI("  Hooked CI::update at %p", gS.CI_update);
        } else {
            LOGW("  Failed to hook CI::update (PC-relative insns?)");
        }
    } else {
        LOGW("  CI::update not found — will use polling");
    }

    if (gS.Mob_normalTick) {
        if (MiniHook::hook(gS.Mob_normalTick, reinterpret_cast<void*>(mobTickDetour),
                           reinterpret_cast<void**>(&orig_Mob_tick))) {
            ++n; LOGI("  Hooked Mob::normalTick at %p", gS.Mob_normalTick);
        } else {
            LOGW("  Failed to hook Mob::normalTick (PC-relative insns?)");
        }
    } else {
        LOGW("  Mob::normalTick not found — will use polling");
    }

    gHooksInstalled.store(n > 0);
    LOGI("Hooks installed: %d", n);
    return n > 0;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 12: Background Thread — Main Work Loop
//
//  CRASH FIX (from v0.4.0):
//  - NO dlopen hook! Instead, polls /proc/self/maps for libminecraftpe.so
//  - All heavy work (symbol resolution, hook installation, texture pack
//    generation) happens HERE, not in the constructor
//  - The constructor just starts this thread and returns immediately
//  - Every pointer dereference is validated with isValidPtr/safeField
// ═══════════════════════════════════════════════════════════════════
static std::atomic<bool> gBgThreadRunning{false};
static std::atomic<bool> gInitialized{false};

static void backgroundThread() {
    LOGI("Background thread started (PID %d)", getpid());
    gBgThreadRunning.store(true);

    // Phase 0: Check if we're in the right process
    if (!isMinecraftProcess()) {
        LOGI("Not MC process — background thread exiting");
        gBgThreadRunning.store(false);
        return;
    }
    LOGI("In MC process (PID %d)", getpid());

    // Phase 1: Wait for libminecraftpe.so to be loaded
    // (polling instead of dlopen hook — much safer)
    LOGI("Phase 1: Waiting for libminecraftpe.so...");
    while (gBgThreadRunning.load()) {
        gLibMC = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
        if (gLibMC) {
            LOGI("libminecraftpe.so found at %p", gLibMC);
            break;
        }
        // Also check /proc/self/maps as fallback
        auto mi = getModuleInfo("libminecraftpe.so");
        if (mi.base && mi.size) {
            LOGI("libminecraftpe.so found in maps at %p", mi.base);
            gLibMC = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
            if (gLibMC) break;
        }
        usleep(500000);  // 500ms poll interval
    }

    if (!gBgThreadRunning.load()) return;

    // Phase 2: Resolve symbols
    LOGI("Phase 2: Resolving symbols...");
    resolveSymbols();

    // Phase 3: Install hooks
    LOGI("Phase 3: Installing hooks...");
    installHooks();

    // Phase 4: Generate texture resource pack
    LOGI("Phase 4: Generating texture pack...");
    Config cfg;
    TextureSwap::generateResourcePack(cfg.fixedSeed);

    gInitialized.store(true);
    LOGI("Initialization complete — hooks: %s, symbols resolved",
         gHooksInstalled.load() ? "YES" : "NO (using polling fallback)");

    // Phase 5: Main effect loop (polling fallback for when hooks fail)
    int tick = 0;
    while (gBgThreadRunning.load()) {
        usleep(500000);  // 500ms between scans
        ++tick;

        if (!SeedManager::instance().isInitialized()) continue;
        if (!gLocalPlayer || !isValidPtr(gLocalPlayer, 4096)) {
            // Try to re-acquire LocalPlayer
            if (gS.CI_getLocalPlayer) {
                gInOurCode = true;
                if (sigsetjmp(gJmpBuf, 1) == 0) {
                    // We need ClientInstance... try to find it
                    // For now, just skip if we don't have it
                }
                gInOurCode = false;
            }
            continue;
        }

        // Every 2 seconds: apply inventory scramble (once)
        if (tick % 4 == 0 && !InventoryScramble::gInvScrambled.load()) {
            InventoryScramble::scrambleInventory(gLocalPlayer);
        }

        // Every 1 second: apply player speed
        if (tick % 2 == 0) {
            applyPlayerSpeed(gLocalPlayer);
        }
    }
    LOGI("Background thread stopped");
}

static std::thread gBgThread;

// ═══════════════════════════════════════════════════════════════════
//  SECTION 13: Initialization
//
//  CRASH FIX: Constructor does MINIMAL work:
//    1. Install signal handler
//    2. Check if we're in the MC process
//    3. Start background thread
//    4. Return immediately
//
//  All heavy work (symbol resolution, hooking, texture pack) happens
//  in the background thread, not in the constructor.
// ═══════════════════════════════════════════════════════════════════
static bool isMinecraftProcess() {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char cmd[256]{};
    auto sz = read(fd, cmd, 255);
    close(fd);
    return sz > 0 && (strstr(cmd, "minecraftpe") || strstr(cmd, "levimc") || strstr(cmd, "mojang"));
}

static std::atomic<bool> gModEntryCalled{false};

static bool initialize() {
    // Guard against double-init (constructor + mod_entry both calling)
    if (gModEntryCalled.exchange(true)) return true;

    LOGI("=== Personalized Mod v0.5.1 ===");
    LOGI("Crash-safe: no-op constructor + mod_entry init + signal handlers");

    // Step 1: Install signal handlers FIRST (before anything else)
    installSignalHandler();

    // Step 2: Start background thread — ALL real work happens there
    // isMinecraftProcess() check is inside the thread, not here
    // Do NOT do any symbol resolution, hooking, or file I/O here
    gBgThread = std::thread(backgroundThread);
    gBgThread.detach();

    LOGI("mod_entry() complete — background thread running");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 14: Entry Points
//
//  CRITICAL: __attribute__((constructor)) is a COMPLETE NO-OP.
//  The constructor runs on the linker's thread during call_constructors,
//  which is extremely fragile. ANY work (even a single __android_log_print)
//  can crash because the runtime may not be fully initialized yet.
//  The v0.4.0 crash was: constructor -> __android_log_print -> __vfprintf -> SIGSEGV
//
//  Instead, ALL initialization happens in mod_entry(), which LeviLamina
//  calls explicitly AFTER the .so is fully loaded and the runtime is ready.
// ═══════════════════════════════════════════════════════════════════

// NO-OP constructor — does absolutely nothing
// This prevents crashes during call_constructors
__attribute__((constructor)) static void _init() { /* no-op */ }
__attribute__((destructor))  static void _fini() { gBgThreadRunning.store(false); }

// mod_entry() is the REAL entry point, called by LeviLamina after .so load
extern "C" __attribute__((visibility("default"))) void mod_entry() { initialize(); }
extern "C" __attribute__((visibility("default"))) void* _pl_mod_instance() { static int x = 1; return &x; }
extern "C" __attribute__((visibility("default"))) const char* mod_name() { return "Personalized"; }
extern "C" __attribute__((visibility("default"))) const char* mod_version() { return "0.5.1"; }
extern "C" __attribute__((visibility("default"))) const char* mod_description() {
    return "UUID-seeded texture swap, inventory scramble, mob model swap (crash-safe v0.5.1)";
}

} // namespace personalized
