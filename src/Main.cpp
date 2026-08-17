/**
 * Main.cpp — Personalized mod entry point for LeviLauncher (Android).
 *
 * Uses preloader-android API (PL_REGISTER_MOD, pl::memory::hook, etc.)
 * to hook into Minecraft Bedrock and apply UUID-seeded visual scrambling.
 */

#include "personalized/Config.hpp"
#include "personalized/SeedManager.hpp"
#include "personalized/Signatures.hpp"
#include "personalized/SDK.hpp"

#include <pl/Mod.hpp>
#include <pl/Logger.hpp>
#include <pl/memory/Hook.hpp>
#include <pl/memory/Signature.hpp>
#include <pl/Config.hpp>

#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace personalized {

namespace {

// ── Globals ──
Config                          gConfig;
std::atomic<bool>               gEnabled{false};
std::atomic<bool>               gResolved{false};
std::mutex                      gResolveMutex;
void*                           gLocalPlayerNative = nullptr;
bool                            gHooksInstalled = false;

// Resolved function pointers
using GetLocalPlayerFn = void*(*)(void*);
GetLocalPlayerFn                gGetLocalPlayerFn = nullptr;

using GetNameTagFn = std::string(*)(void*);
GetNameTagFn                    gGetNameTagFn = nullptr;

using SetNameTagFn = void(*)(void*, const std::string&);
SetNameTagFn                    gSetNameTagFn = nullptr;

using IsPlayerFn = bool(*)(void*);
IsPlayerFn                      gIsPlayerFn = nullptr;

// dlopen hook (detects when libminecraftpe.so is loaded)
void* (*gDlopenOriginal)(const char*, int) = nullptr;
pl::memory::HookHandle gDlopenHook;

// ── Signature table (filled at runtime) ──
std::unordered_map<sigs::Id, std::uintptr_t> gSigAddresses;

// ── Mob swap map ──
const std::vector<std::string> mobPool = {
    "minecraft:zombie",   "minecraft:skeleton",  "minecraft:pig",
    "minecraft:cow",      "minecraft:sheep",     "minecraft:chicken",
    "minecraft:creeper",  "minecraft:spider",    "minecraft:blaze",
    "minecraft:enderman",
};
std::unordered_map<std::string, std::string> mobSwapMap;

// ── Logger shortcut ──
auto& logger() {
    static auto* log = &pl::log::Logger::getOrCreate("Personalized");
    return *log;
}

// ─────────────────────────────────────────────
//  Signature resolution
// ─────────────────────────────────────────────
bool resolveSignatures() {
    std::lock_guard lock(gResolveMutex);
    if (gResolved.load(std::memory_order_acquire)) return true;

    logger().info("Resolving signatures in libminecraftpe.so ...");

    for (const auto& def : sigs::definitions) {
        auto addr = pl::memory::resolveSignature(def.pattern, "libminecraftpe.so");
        if (addr) {
            gSigAddresses[def.id] = addr;
            logger().debug("  Resolved sig {} -> 0x{:X}",
                           static_cast<int>(def.id), addr);
        } else {
            logger().warn("  FAILED to resolve sig {}", static_cast<int>(def.id));
        }
    }

    // Cache commonly-used function pointers
    if (auto it = gSigAddresses.find(sigs::Id::ClientInstanceGetLocalPlayer); it != gSigAddresses.end())
        gGetLocalPlayerFn = reinterpret_cast<GetLocalPlayerFn>(it->second);

    if (auto it = gSigAddresses.find(sigs::Id::ActorGetNameTag); it != gSigAddresses.end())
        gGetNameTagFn = reinterpret_cast<GetNameTagFn>(it->second);

    if (auto it = gSigAddresses.find(sigs::Id::ActorSetNameTag); it != gSigAddresses.end())
        gSetNameTagFn = reinterpret_cast<SetNameTagFn>(it->second);

    if (auto it = gSigAddresses.find(sigs::Id::ActorIsPlayer); it != gSigAddresses.end())
        gIsPlayerFn = reinterpret_cast<IsPlayerFn>(it->second);

    bool ok = !gSigAddresses.empty();
    gResolved.store(ok, std::memory_order_release);
    logger().info("Signature resolution {} ({} of {} resolved)",
                  ok ? "OK" : "FAILED", gSigAddresses.size(), sigs::Count);
    return ok;
}

// ─────────────────────────────────────────────
//  dlopen detour — detects MC library load
// ─────────────────────────────────────────────
void* dlopenDetour(const char* filename, int flags) {
    void* handle = gDlopenOriginal ? gDlopenOriginal(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so")) {
        logger().info("libminecraftpe.so loaded — resolving signatures");
        resolveSignatures();
    }
    return handle;
}

// ─────────────────────────────────────────────
//  Build mob swap map from seed
// ─────────────────────────────────────────────
void buildMobSwapMap() {
    if (!SeedManager::instance().isInitialized()) return;

    auto rng = SeedManager::instance().createRNG();
    std::mt19937_64 mobRng(SeedManager::instance().getSeed() ^ 0xB0B0FACEDEADBEEFULL);

    mobSwapMap.clear();
    for (const auto& mobId : mobPool) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(mobRng) > gConfig.mobSwapIntensity) continue;

        std::string selected;
        int attempts = 0;
        do {
            std::uniform_int_distribution<size_t> idxDist(0, mobPool.size() - 1);
            selected = mobPool[idxDist(mobRng)];
            ++attempts;
        } while (selected == mobId && attempts < 10);

        if (selected != mobId) {
            mobSwapMap[mobId] = selected;
            logger().debug("Mob swap: {} -> {}", mobId, selected);
        }
    }
    logger().info("Mob swap map: {} entries", mobSwapMap.size());
}

// ─────────────────────────────────────────────
//  Fetch UUID from LocalPlayer object
// ─────────────────────────────────────────────
std::string fetchPlayerUUID(void* playerNative) {
    if (!playerNative) return {};

    // Player name is at offset Player::mName — use it as a unique identifier
    // For a real UUID, we'd need to find the mce::UUID field offset
    // For now, read the player name and hash it as a pseudo-UUID
    try {
        auto& name = sdk::field<std::string>(playerNative, offsets::Player::mName);
        if (!name.empty()) {
            // Use name as seed source for now
            // A real implementation would read the actual UUID field
            return "name:" + name;
        }
    } catch (...) {}

    return {};
}

// ─────────────────────────────────────────────
//  ClientInstance::update detour — runs every frame
// ─────────────────────────────────────────────
using ClientInstanceUpdateFn = void(*)(void*, void*, void*);
ClientInstanceUpdateFn gClientInstanceUpdateOriginal = nullptr;

void clientInstanceUpdateDetour(void* client, void* a2, void* a3) {
    // Call original first
    if (gClientInstanceUpdateOriginal)
        gClientInstanceUpdateOriginal(client, a2, a3);

    if (!gEnabled.load()) return;

    // Get local player
    if (gGetLocalPlayerFn && !gLocalPlayerNative) {
        gLocalPlayerNative = gGetLocalPlayerFn(client);
        if (gLocalPlayerNative) {
            logger().info("LocalPlayer acquired at {}", gLocalPlayerNative);

            // Fetch UUID and initialize seed
            auto uuid = fetchPlayerUUID(gLocalPlayerNative);
            if (!uuid.empty() && !SeedManager::instance().isInitialized()) {
                SeedManager::instance().initializeWithUUID(uuid);
                logger().info("Seed initialized from UUID: {}", uuid);
                buildMobSwapMap();
            }
        }
    }
}

// ─────────────────────────────────────────────
//  Apply nametag scrambling (visible effect!)
// ─────────────────────────────────────────────
void applyNametagScramble(void* actorNative) {
    if (!actorNative || !SeedManager::instance().isInitialized()) return;

    // Skip players
    if (gIsPlayerFn && gIsPlayerFn(actorNative)) return;

    // Read current nametag
    std::string nameTag;
    if (gGetNameTagFn) {
        try {
            nameTag = gGetNameTagFn(actorNative);
        } catch (...) { return; }
    }
    if (nameTag.empty()) return;

    // Scramble the name tag using our seed
    // This is a VISIBLE effect that proves the mod is working!
    auto rng = SeedManager::instance().createRNG();
    std::string scrambled = nameTag;
    // Simple permutation: shift each character
    uint64_t shift = rng() % 26;
    for (auto& c : scrambled) {
        if (c >= 'a' && c <= 'z') c = 'a' + (c - 'a' + shift) % 26;
        else if (c >= 'A' && c <= 'Z') c = 'A' + (c - 'A' + shift) % 26;
    }

    // Set the scrambled nametag
    if (gSetNameTagFn && scrambled != nameTag) {
        try {
            gSetNameTagFn(actorNative, scrambled);
        } catch (...) {}
    }
}

// ─────────────────────────────────────────────
//  Install hooks into MC functions
// ─────────────────────────────────────────────
bool installHooks() {
    if (gHooksInstalled) return true;

    logger().info("Installing hooks ...");

    int installed = 0;

    // Hook ClientInstance::update
    if (auto it = gSigAddresses.find(sigs::Id::ClientInstanceUpdate); it != gSigAddresses.end()) {
        auto handle = pl::memory::HookHandle(
            reinterpret_cast<pl::memory::FuncPtr>(it->second),
            reinterpret_cast<pl::memory::FuncPtr>(clientInstanceUpdateDetour),
            reinterpret_cast<pl::memory::FuncPtr*>(&gClientInstanceUpdateOriginal)
        );
        if (handle.installed()) {
            logger().info("  Hooked ClientInstance::update");
            ++installed;
        } else {
            logger().warn("  Failed to hook ClientInstance::update");
        }
    }

    gHooksInstalled = (installed > 0);
    logger().info("Hooks installed: {}", installed);
    return gHooksInstalled;
}

// ─────────────────────────────────────────────
//  Check if we're in the Minecraft process
// ─────────────────────────────────────────────
bool isMinecraftProcess() {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    char command[256]{};
    auto size = read(fd, command, sizeof(command) - 1);
    close(fd);
    if (size <= 0) return false;
    return std::strcmp(command, "com.mojang.minecraftpe") == 0
        || std::strstr(command, "levimc") != nullptr
        || std::strstr(command, "minecraftpe") != nullptr;
}

// ─────────────────────────────────────────────
//  Load config
// ─────────────────────────────────────────────
void loadConfig(const std::filesystem::path& configDir) {
    auto configPath = configDir / "config.json";
    try {
        auto j = pl::config::loadConfig(configPath,
            pl::reflection::serialize(gConfig));
        pl::reflection::deserializeTo(gConfig, j);
        logger().info("Config loaded from {}", configPath.string());
    } catch (const std::exception& e) {
        logger().warn("Config load failed: {} — using defaults", e.what());
    }
}

} // namespace

// ═══════════════════════════════════════════
//  Mod class
// ═══════════════════════════════════════════

class PersonalizedMod {
public:
    static PersonalizedMod& instance() {
        static PersonalizedMod mod;
        return mod;
    }

    bool load(pl::mod::ModContext& ctx) {
        logger().info("Personalized mod loading ...");
        logger().info("  Mod dir: {}", ctx.modRootPath().string());

        // Load config
        loadConfig(ctx.configDir());

        if (!isMinecraftProcess()) {
            logger().info("Not in MC process — skipping hook installation");
            return true;
        }

        // Try to resolve signatures if MC is already loaded
        void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
        if (minecraft) {
            resolveSignatures();
            dlclose(minecraft);
            return true;
        }

        // MC not loaded yet — hook dlopen to detect when it loads
        void* libdl = dlopen("libdl.so", RTLD_NOW);
        if (libdl) {
            void* dlopenSym = dlsym(libdl, "dlopen");
            if (dlopenSym) {
                gDlopenHook = pl::memory::HookHandle(
                    dlopenSym,
                    reinterpret_cast<pl::memory::FuncPtr>(dlopenDetour),
                    reinterpret_cast<pl::memory::FuncPtr*>(&gDlopenOriginal)
                );
                if (gDlopenHook.installed()) {
                    logger().info("Hooked dlopen — will detect libminecraftpe.so load");
                }
            }
            dlclose(libdl);
        }

        return true;
    }

    bool enable(pl::mod::ModContext& ctx) {
        gEnabled.store(true);
        logger().info("Personalized mod enabled");

        if (!isMinecraftProcess()) return true;

        // Try to resolve if not done yet
        if (!gResolved.load()) {
            void* minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
            if (minecraft) {
                resolveSignatures();
                dlclose(minecraft);
            }
        }

        // Install hooks once signatures are resolved
        if (gResolved.load()) {
            installHooks();
        }

        return true;
    }

    bool disable(pl::mod::ModContext&) {
        gEnabled.store(false);
        gLocalPlayerNative = nullptr;
        logger().info("Personalized mod disabled");
        return true;
    }

    bool unload(pl::mod::ModContext&) {
        gEnabled.store(false);
        gDlopenHook.reset();
        logger().info("Personalized mod unloaded");
        return true;
    }
};

} // namespace personalized

// ── Register with preloader ──
PL_REGISTER_MOD(personalized::PersonalizedMod, personalized::PersonalizedMod::instance())
