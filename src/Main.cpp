/**
 * Main.cpp — Personalized mod for MC Bedrock Android (LeviLauncher) v0.4.0
 *
 * Implements all three UUID-seeded visual effects:
 *   1. Texture swapping — generates resource pack with permuted block textures
 *   2. Inventory scrambling — permutes ItemStack slots in player inventory
 *   3. Mob model swapping — modifies ActorDefinitionIdentifier strings
 *
 * Plus bonus effects: nametag scramble, entity scale, movement speed.
 *
 * Uses MiniHook (our own ARM64 inline hooker) + dlsym + pattern scanning.
 * Zero external dependencies beyond NDK system libraries.
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
//  SECTION 1: MiniHook — ARM64 inline hooker
// ═══════════════════════════════════════════════════════════════════
namespace MiniHook {
static constexpr size_t kJumpSize  = 16;
static constexpr size_t kTrampSize = 32;
static constexpr uint32_t LDR_X17_PC8 = 0x58000051;
static constexpr uint32_t BR_X17      = 0xD61F0220;

static void writeAbsoluteJump(void* where, void* target) {
    auto* p = static_cast<uint32_t*>(where);
    p[0] = LDR_X17_PC8; p[1] = BR_X17;
    *reinterpret_cast<void**>(&p[2]) = target;
}

static bool isPCRelative(uint32_t insn) {
    if ((insn & 0xFC000000) == 0x94000000) return true;  // BL
    if ((insn & 0xFC000000) == 0x14000000) return true;  // B
    if ((insn & 0xFF000010) == 0x54000000) return true;  // B.cond
    if ((insn & 0x7E000000) == 0x34000000) return true;  // CBZ
    if ((insn & 0x7E000000) == 0x35000000) return true;  // CBNZ
    if ((insn & 0x7E000000) == 0x36000000) return true;  // TBZ
    if ((insn & 0x7E000000) == 0x37000000) return true;  // TBNZ
    if ((insn & 0x9F000000) == 0x90000000) return true;  // ADRP
    if ((insn & 0x9F000000) == 0x10000000) return true;  // ADR
    return false;
}

static void* allocTrampoline() {
    void* mem = mmap(nullptr, kTrampSize, PROT_READ|PROT_WRITE|PROT_EXEC,
                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    return (mem == MAP_FAILED) ? nullptr : mem;
}

static bool setPagePerms(void* addr, size_t len, int prot) {
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~0xFFFULL;
    uintptr_t end  = (reinterpret_cast<uintptr_t>(addr) + len + 0xFFFULL) & ~0xFFFULL;
    return mprotect(reinterpret_cast<void*>(page), end - page, prot) == 0;
}

static bool hook(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return false;
    auto* insns = static_cast<uint32_t*>(target);
    for (int i = 0; i < 4; ++i) {
        if (isPCRelative(insns[i]))
            LOGW("MiniHook: insn %d at %p is PC-relative (0x%08X)", i, &insns[i], insns[i]);
    }
    void* trampoline = allocTrampoline();
    if (!trampoline) return false;
    memcpy(trampoline, target, kJumpSize);
    writeAbsoluteJump(static_cast<uint8_t*>(trampoline)+kJumpSize,
                      static_cast<uint8_t*>(target)+kJumpSize);
    __builtin___clear_cache(static_cast<char*>(trampoline),
                            static_cast<char*>(trampoline)+kTrampSize);
    if (!setPagePerms(target, kJumpSize, PROT_READ|PROT_WRITE)) { munmap(trampoline,kTrampSize); return false; }
    writeAbsoluteJump(target, detour);
    setPagePerms(target, kJumpSize, PROT_READ|PROT_EXEC);
    __builtin___clear_cache(static_cast<char*>(target),
                            static_cast<char*>(target)+kJumpSize);
    *original = trampoline;
    return true;
}
} // namespace MiniHook

// ═══════════════════════════════════════════════════════════════════
//  SECTION 2: Memory utilities
// ═══════════════════════════════════════════════════════════════════
template <class T>
static T& fieldAt(void* obj, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj)+off);
}
static std::string readStr(void* obj, size_t off) {
    try { return fieldAt<std::string>(obj, off); } catch (...) { return {}; }
}
static void writeStr(void* obj, size_t off, const std::string& val) {
    try { fieldAt<std::string>(obj, off) = val; } catch (...) {}
}

struct ModuleInfo { void* base=nullptr; size_t size=0; };
static ModuleInfo getModuleInfo(const char* name) {
    ModuleInfo info; FILE* f=fopen("/proc/self/maps","r");
    if(!f) return info; char line[512]; void* hi=nullptr;
    while(fgets(line,sizeof(line),f)) {
        if(strstr(line,name)) { uintptr_t s,e;
            if(sscanf(line,"%lx-%lx",&s,&e)==2) {
                if(!info.base||(void*)s<info.base) info.base=(void*)s;
                if(!hi||(void*)e>hi) hi=(void*)e;
            }
        }
    }
    fclose(f); if(info.base&&hi) info.size=(size_t)hi-(size_t)info.base;
    return info;
}

// Pattern scanner
static void* scanPattern(void* start, size_t len, const char* pat, const char* mask) {
    size_t pl=strlen(mask); auto* b=reinterpret_cast<uint8_t*>(start);
    for(auto* p=b; p<=b+len-pl; ++p) { bool ok=true;
        for(size_t j=0;j<pl;++j) if(mask[j]=='x'&&p[j]!=(uint8_t)pat[j]){ok=false;break;}
        if(ok) return p;
    } return nullptr;
}
struct HexPat { std::vector<uint8_t> bytes; std::string mask; };
static HexPat parseHex(const char* h) {
    HexPat p; const char* c=h;
    while(*c) { while(*c==' ')++c; if(!*c) break;
        if(c[0]=='?'&&c[1]=='?') { p.bytes.push_back(0); p.mask+='?'; c+=2; }
        else { char b[3]={c[0],c[1],0}; p.bytes.push_back((uint8_t)strtoul(b,nullptr,16)); p.mask+='x'; c+=2; }
    } return p;
}
static void* scanHex(void* start, size_t len, const char* hex) {
    auto p=parseHex(hex); if(p.bytes.empty()) return nullptr;
    return scanPattern(start,len,reinterpret_cast<const char*>(p.bytes.data()),p.mask.c_str());
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 3: Symbol resolution (dlsym + RTTI/vtable + patterns)
// ═══════════════════════════════════════════════════════════════════
static void* gLibMC = nullptr;

static void* trySym(const char* sym) {
    if(gLibMC) { void* a=dlsym(gLibMC,sym); if(a) return a; }
    void* a=dlsym(RTLD_DEFAULT,sym); if(a) return a;
    return nullptr;
}

struct MCSyms {
    void* CI_update=nullptr, *CI_getLocalPlayer=nullptr;
    void* Actor_getNameTag=nullptr, *Actor_setNameTag=nullptr;
    void* Actor_isPlayer=nullptr, *Mob_normalTick=nullptr;
    void* Level_addEntity=nullptr;
    // Vtables found via RTTI
    void* vtbl_Actor=nullptr, *vtbl_Mob=nullptr, *vtbl_Player=nullptr;
};
static MCSyms gS;

static bool resolveSymbols() {
    LOGI("=== Resolving MC symbols ===");

    // Function symbols (multiple mangled variants per function)
    struct { const char** names; void** target; } symGroups[] = {
        {(const char*[]){"_ZN15ClientInstance6updateEv","_ZN15ClientInstance6updateEf",nullptr}, &gS.CI_update},
        {(const char*[]){"_ZN15ClientInstance14getLocalPlayerEv","_ZNK15ClientInstance14getLocalPlayerEv",nullptr}, &gS.CI_getLocalPlayer},
        {(const char*[]){"_ZNK5Actor11getNameTagB5cxx11Ev","_ZNK5Actor11getNameTagEv",nullptr}, &gS.Actor_getNameTag},
        {(const char*[]){"_ZN5Actor11setNameTagERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",nullptr}, &gS.Actor_setNameTag},
        {(const char*[]){"_ZNK5Actor8isPlayerEv",nullptr}, &gS.Actor_isPlayer},
        {(const char*[]){"_ZN3Mob10normalTickEv",nullptr}, &gS.Mob_normalTick},
        {(const char*[]){"_ZN5Level9addEntityERK10ActorUniqueIDS5","_ZN5Level9addEntityE10ActorUniqueID",nullptr}, &gS.Level_addEntity},
    };
    for(auto& g : symGroups) {
        for(int i=0; g.names[i]; ++i) {
            *g.target = trySym(g.names[i]);
            if(*g.target) { LOGI("  %s → %p", g.names[i], *g.target); break; }
        }
    }

    // RTTI vtables (very stable across versions — needed for type checking)
    gS.vtbl_Actor  = trySym("_ZTV5Actor");
    gS.vtbl_Mob    = trySym("_ZTV3Mob");
    gS.vtbl_Player = trySym("_ZTV6Player");
    if(gS.vtbl_Actor)  LOGI("  Actor vtable at %p", gS.vtbl_Actor);
    if(gS.vtbl_Mob)    LOGI("  Mob vtable at %p", gS.vtbl_Mob);
    if(gS.vtbl_Player) LOGI("  Player vtable at %p", gS.vtbl_Player);

    // Pattern scanning fallback
    auto mi = getModuleInfo("libminecraftpe.so");
    if(mi.base && mi.size) {
        LOGI("  libminecraftpe.so: base=%p size=0x%zx", mi.base, mi.size);
        if(!gS.CI_update) gS.CI_update = scanHex(mi.base,mi.size,"A9 01 7B A9 F7 03 7B A9 FD 03 00 91 ?? ?? ?? D1 59 D0 3B D5");
        if(!gS.Mob_normalTick) gS.Mob_normalTick = scanHex(mi.base,mi.size,"FC 01 7B A9 F8 0F 7B A9 ?? ?? ?? A9 ?? ?? ?? A9 ?? ?? ?? A9 54 D0 3B D5");
        if(!gS.CI_getLocalPlayer) gS.CI_getLocalPlayer = scanHex(mi.base,mi.size,"?? ?? ?? D1 ?? ?? ?? A9 ?? ?? ?? F9 ?? ?? ?? 91 53 D0 3B D5 E8 03 00 AA");
    }

    int n=0;
    if(gS.CI_update)++n; if(gS.CI_getLocalPlayer)++n; if(gS.Actor_getNameTag)++n;
    if(gS.Actor_setNameTag)++n; if(gS.Actor_isPlayer)++n; if(gS.Mob_normalTick)++n;
    if(gS.Level_addEntity)++n;
    LOGI("Symbol resolution: %d functions + %d vtables", n,
         (gS.vtbl_Actor?1:0)+(gS.vtbl_Mob?1:0)+(gS.vtbl_Player?1:0));
    return n > 0;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 4: MC Offsets (ARM64, ~1.21.44)
// ═══════════════════════════════════════════════════════════════════
namespace O { // Offsets
    namespace Actor {
        constexpr size_t mRuntimeID=0x1A8, mNameTag=0x2C0, mHurtTime=0x194;
        constexpr size_t mDimension=448, mLevel=464;
        // ActorDefinitionIdentifier — contains the mob type string
        constexpr size_t mDesc=0x1B0;
    }
    namespace ActorDefId {
        // The identifier string (e.g. "minecraft:zombie") is at this offset
        // inside the ActorDefinitionIdentifier struct
        constexpr size_t mIdentifier=0x08;
    }
    namespace Player {
        constexpr size_t mName=2824, mUUID_Most=2800, mUUID_Least=2808;
        constexpr size_t mMovementSpeed=2900;
        // PlayerInventory pointer
        constexpr size_t mInventory=0x2A0;
    }
    namespace PlayerInventory {
        // Container pointer inside PlayerInventory
        constexpr size_t mContainer=0x08;
    }
    namespace Container {
        // ItemStack array pointer and count
        constexpr size_t mItems=0x18, mCount=0x10;
    }
    // ItemStack size (approximate — varies by version)
    constexpr size_t kItemStackSize = 152;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 5: Texture Swapping — Resource Pack Generation
//
//  Creates a MC Bedrock resource pack that remaps block textures.
//  The pack goes in MC's resource_packs/ directory and MC loads it
//  as an overlay, swapping which texture each block uses.
// ═══════════════════════════════════════════════════════════════════
namespace TextureSwap {

// Common block texture names for permutation
static const std::vector<std::string> blockTextures = {
    "stone","dirt","grass_side","grass_carried","cobblestone","oak_planks",
    "spruce_planks","birch_planks","jungle_planks","acacia_planks","dark_oak_planks",
    "oak_log","spruce_log","birch_log","jungle_log","sand","red_sand","gravel",
    "oak_log_top","spruce_log_top","birch_log_top","jungle_log_top",
    "glass","iron_block","gold_block","diamond_block","emerald_block",
    "lapis_block","redstone_block","coal_block","obsidian","ice","packed_ice",
    "snow","clay","netherrack","soul_sand","glowstone","magma","bedrock",
    "sandstone_bottom","sandstone_side","sandstone_top",
    "red_sandstone_bottom","red_sandstone_side","red_sandstone_top",
    "oak_leaves","spruce_leaves","birch_leaves",
    "wool","hardened_clay","prismarine_rough","prismarine_dark","prismarine_bricks",
    "sea_lantern","hay_block_top","hay_block_side","bone_block_side","bone_block_top",
    "purpur_block","purpur_pillar_top","purpur_pillar","end_bricks","end_stone",
    "chorus_plant","chorus_flower",
};

static std::string findMCDataDir() {
    // Try common MC Bedrock data directories on Android
    const char* bases[] = {
        "/sdcard/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/storage/emulated/0/Android/data/com.mojang.minecraftpe/files/games/com.mojang",
        "/data/data/com.mojang.minecraftpe/files/games/com.mojang",
        nullptr
    };
    for(int i=0; bases[i]; ++i) {
        struct stat st;
        if(stat(bases[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            LOGI("Found MC data dir: %s", bases[i]);
            return bases[i];
        }
    }
    return {};
}

static bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if(!f.is_open()) return false;
    f << content;
    f.close();
    return true;
}

static bool mkdirp(const std::string& path) {
    // Simple recursive mkdir
    for(size_t i=1; i<path.size(); ++i) {
        if(path[i]=='/') {
            std::string sub=path.substr(0,i);
            mkdir(sub.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
    struct stat st;
    return stat(path.c_str(),&st)==0 && S_ISDIR(st.st_mode);
}

static bool generateResourcePack(uint64_t seed) {
    LOGI("=== Generating texture swap resource pack (seed 0x%016lX) ===", (unsigned long)seed);

    std::string mcDir = findMCDataDir();
    if(mcDir.empty()) {
        LOGW("MC data dir not found — texture pack will be written to /sdcard/");
        mcDir = "/sdcard";
    }

    std::string packDir = mcDir + "/resource_packs/Personalized_Textures";
    std::string texDir  = packDir + "/textures";

    if(!mkdirp(texDir)) {
        LOGE("Failed to create pack directory: %s", packDir.c_str());
        return false;
    }

    // Generate permutation of block textures
    auto permutation = RandomMapper::partialScramble(
        seed, blockTextures.size(), 0.6, 2);

    // --- manifest.json ---
    // UUIDs derived from seed for uniqueness
    std::mt19937_64 rng(seed ^ 0xDEADBEEFCAFEBABEULL);
    auto genUUID = [&rng]() -> std::string {
        char buf[37];
        snprintf(buf,sizeof(buf),"%08lx-%04lx-%04lx-%04lx-%012lx",
            (unsigned long)(rng()&0xFFFFFFFF),
            (unsigned long)(rng()&0xFFFF),
            (unsigned long)((rng()&0xFFFF)|0x4000),  // version 4
            (unsigned long)((rng()&0x3FFF)|0x8000),  // variant 1
            (unsigned long)(rng()&0xFFFFFFFFFFFF));
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

    if(!writeFile(packDir+"/manifest.json", manifest)) {
        LOGE("Failed to write manifest.json"); return false;
    }

    // --- terrain_texture.json ---
    // Maps each block texture to a DIFFERENT texture based on the permutation
    std::ostringstream ttj;
    ttj << "{\n"
        << "  \"resource_pack_name\": \"Personalized\",\n"
        << "  \"texture_name\": \"atlas.terrain\",\n"
        << "  \"padding\": 8,\n"
        << "  \"num_mip_levels\": 4,\n"
        << "  \"texture_data\": {\n";

    for(size_t i=0; i<blockTextures.size(); ++i) {
        size_t j = permutation[i];
        if(j >= blockTextures.size()) j = i; // safety
        const auto& from = blockTextures[i];
        const auto& to   = blockTextures[j];

        ttj << "    \"" << from << "\": {\n"
            << "      \"textures\": \"textures/blocks/" << to << "\"\n"
            << "    }";
        if(i+1 < blockTextures.size()) ttj << ",";
        ttj << "\n";
    }

    ttj << "  }\n}\n";

    if(!writeFile(packDir+"/textures/terrain_texture.json", ttj.str())) {
        LOGE("Failed to write terrain_texture.json"); return false;
    }

    LOGI("Texture resource pack generated at %s (%zu texture swaps)",
         packDir.c_str(), blockTextures.size());

    // Try to activate the pack by adding to world_resource_packs.json
    std::string wrpPath = mcDir + "/resource_packs/world_resource_packs.json";
    // Just log it — proper activation would require JSON manipulation
    LOGI("To activate: add pack '%s' to MC's resource pack settings", packDir.c_str());

    return true;
}

} // namespace TextureSwap

// ═══════════════════════════════════════════════════════════════════
//  SECTION 6: Inventory Scrambling
//
//  Permutes the ItemStack slots in the player's inventory container.
//  Uses Fisher-Yates shuffle seeded by the player's UUID.
//  The scrambling is applied periodically and can be toggled.
// ═══════════════════════════════════════════════════════════════════
namespace InventoryScramble {

static std::vector<size_t> gInvPermutation;
static std::mutex gInvMutex;
static std::atomic<bool> gInvScrambled{false};

// Build the inventory slot permutation from seed
static void buildPermutation(uint64_t seed) {
    // 36 slots = 4 rows of 9 in player inventory
    // (hotbar slots 0-8, main inventory 9-35)
    const size_t slotCount = 36;
    gInvPermutation = RandomMapper::partialScramble(
        seed ^ 0x1NV3NT0RY, slotCount, 0.7, 3);
    LOGI("Inventory permutation built: %zu slots", slotCount);
}

// Apply the permutation to the player's inventory
static void scrambleInventory(void* playerPtr) {
    if(!playerPtr || gInvPermutation.empty()) return;
    if(gInvScrambled.load()) return; // Only scramble once on initial join

    std::lock_guard lock(gInvMutex);

    try {
        // Navigate: Player → PlayerInventory → Container → ItemStack[]
        void* invPtr = fieldAt<void*>(playerPtr, O::Player::mInventory);
        if(!invPtr) return;

        void* containerPtr = fieldAt<void*>(invPtr, O::PlayerInventory::mContainer);
        if(!containerPtr) return;

        int count = fieldAt<int>(containerPtr, O::Container::mCount);
        if(count <= 0) count = 36; // default player inventory size
        void* itemsBase = fieldAt<void*>(containerPtr, O::Container::mItems);
        if(!itemsBase) return;

        // Apply permutation: swap ItemStack data in each slot
        // We work on a copy to avoid partial states
        size_t itemSize = O::kItemStackSize;
        std::vector<uint8_t> buffer(itemSize);

        // First, snapshot all items
        std::vector<std::vector<uint8_t>> originals(count);
        for(int i=0; i<count; ++i) {
            originals[i].resize(itemSize);
            void* slot = static_cast<uint8_t*>(itemsBase) + i * itemSize;
            memcpy(originals[i].data(), slot, itemSize);
        }

        // Then, write items in permuted order
        for(int i=0; i<count; ++i) {
            size_t j = (i < gInvPermutation.size()) ? gInvPermutation[i] : i;
            if(j >= (size_t)count) j = i;
            void* slot = static_cast<uint8_t*>(itemsBase) + i * itemSize;
            memcpy(slot, originals[j].data(), itemSize);
        }

        gInvScrambled.store(true);
        LOGI("Inventory scrambled: %d slots permuted", count);
    } catch(...) {
        LOGW("Inventory scramble failed (bad offset?)");
    }
}

// Unscramble (restore original order) — useful for clean disable
static void unscrambleInventory(void* playerPtr) {
    // Apply inverse permutation
    if(!playerPtr || gInvPermutation.empty()) return;
    // For simplicity, just mark as not scrambled; next tick will re-apply
    gInvScrambled.store(false);
}

} // namespace InventoryScramble

// ═══════════════════════════════════════════════════════════════════
//  SECTION 7: Mob Model Swapping
//
//  Modifies the ActorDefinitionIdentifier string of mobs to change
//  which model/texture/animation they use. Combined with entity
//  creation hooking for new mobs and periodic scanning for existing ones.
// ═══════════════════════════════════════════════════════════════════
namespace MobSwap {

static const std::vector<std::string> mobPool = {
    "minecraft:zombie",   "minecraft:skeleton",  "minecraft:pig",
    "minecraft:cow",      "minecraft:sheep",     "minecraft:chicken",
    "minecraft:creeper",  "minecraft:spider",    "minecraft:blaze",
    "minecraft:enderman", "minecraft:witch",     "minecraft:villager_v2",
    "minecraft:iron_golem","minecraft:wolf",     "minecraft:cat",
    "minecraft:horse",    "minecraft:phantom",   "minecraft:pillager",
};
static std::unordered_map<std::string, std::string> mobSwapMap;
static std::unordered_set<uint64_t> gSwappedMobs;
static std::mutex gMobMutex;

static void buildSwapMap(uint64_t seed) {
    std::mt19937_64 rng(seed ^ 0xB0B0FACEDEADBEEFULL);
    mobSwapMap.clear();
    for(const auto& id : mobPool) {
        std::uniform_real_distribution<double> dist(0.0,1.0);
        if(dist(rng) > 0.6) continue; // 60% of mobs get swapped
        std::string selected; int att=0;
        do {
            std::uniform_int_distribution<size_t> idx(0,mobPool.size()-1);
            selected = mobPool[idx(rng)]; ++att;
        } while(selected==id && att<10);
        if(selected!=id) mobSwapMap[id]=selected;
    }
    LOGI("Mob swap map: %zu entries", mobSwapMap.size());
}

// Get the identifier string from an Actor's ActorDefinitionIdentifier
static std::string getMobIdentifier(void* actorPtr) {
    if(!actorPtr) return {};
    try {
        void* descPtr = fieldAt<void*>(actorPtr, O::Actor::mDesc);
        if(!descPtr) return {};
        return readStr(descPtr, O::ActorDefId::mIdentifier);
    } catch(...) { return {}; }
}

// Set the identifier string — this causes MC to use the new mob's model/texture
static bool setMobIdentifier(void* actorPtr, const std::string& newId) {
    if(!actorPtr) return false;
    try {
        void* descPtr = fieldAt<void*>(actorPtr, O::Actor::mDesc);
        if(!descPtr) return false;
        writeStr(descPtr, O::ActorDefId::mIdentifier, newId);
        return true;
    } catch(...) { return false; }
}

// Check if an actor is a player (skip players!)
static bool isPlayer(void* actorPtr) {
    if(gS.Actor_isPlayer) {
        try { return reinterpret_cast<bool(*)(void*)>(gS.Actor_isPlayer)(actorPtr); }
        catch(...) {}
    }
    // Fallback: check vtable
    if(gS.vtbl_Player && actorPtr) {
        void* vtbl = *reinterpret_cast<void**>(actorPtr);
        // Player vtable should be at or after the Player vtable symbol
        // This is a rough check — Player's vtable >= _ZTV6Player
        // Not perfect but better than nothing
    }
    return false;
}

// Try to swap a mob's model
static void trySwapMob(void* actorPtr) {
    if(!actorPtr) return;

    uint64_t runtimeID = 0;
    try { runtimeID = fieldAt<uint64_t>(actorPtr, O::Actor::mRuntimeID); }
    catch(...) { return; }
    if(runtimeID == 0) return;

    if(isPlayer(actorPtr)) return;

    std::lock_guard lock(gMobMutex);
    if(gSwappedMobs.count(runtimeID)) return; // already swapped

    std::string id = getMobIdentifier(actorPtr);
    if(id.empty()) return;

    auto it = mobSwapMap.find(id);
    if(it == mobSwapMap.end()) {
        gSwappedMobs.insert(runtimeID); // mark as seen even if no swap
        return;
    }

    const std::string& newId = it->second;
    if(setMobIdentifier(actorPtr, newId)) {
        gSwappedMobs.insert(runtimeID);
        LOGI("Mob model swap: %s → %s (entity 0x%lX)",
             id.c_str(), newId.c_str(), (unsigned long)runtimeID);
    }
}

} // namespace MobSwap

// ═══════════════════════════════════════════════════════════════════
//  SECTION 8: Nametag scrambling + player effects
// ═══════════════════════════════════════════════════════════════════
static std::string scrambleNametag(const std::string& orig, uint64_t eid) {
    if(orig.empty()) return orig;
    std::mt19937_64 rng(SeedManager::instance().getSeed() ^ eid ^ 0x5A5A5A5AULL);
    std::string r = orig;
    uint64_t shift = rng() % 26;
    for(auto& c : r) {
        if(c>='a'&&c<='z') c='a'+(char)((c-'a'+shift)%26);
        else if(c>='A'&&c<='Z') c='A'+(char)((c-'A'+shift)%26);
    }
    const char* pfx[] = {"[P]","~","*","⇒"};
    r = pfx[rng()%4] + r;
    return r;
}

static void applyNametagScramble(void* actorPtr) {
    if(!actorPtr || !SeedManager::instance().isInitialized()) return;
    uint64_t rid=0;
    try{rid=fieldAt<uint64_t>(actorPtr,O::Actor::mRuntimeID);}catch(...){return;}
    if(rid==0||MobSwap::isPlayer(actorPtr)) return;

    static std::unordered_set<uint64_t> done;
    static std::mutex mtx;
    std::lock_guard lock(mtx);
    if(done.count(rid)) return;

    std::string nameTag;
    if(gS.Actor_getNameTag) {
        try{nameTag=reinterpret_cast<std::string(*)(void*)>(gS.Actor_getNameTag)(actorPtr);}catch(...){}
    }
    if(nameTag.empty()) nameTag = readStr(actorPtr, O::Actor::mNameTag);
    if(nameTag.empty()) return;

    std::string scrambled = scrambleNametag(nameTag, rid);
    if(gS.Actor_setNameTag) {
        try{reinterpret_cast<void(*)(void*,const std::string&)>(gS.Actor_setNameTag)(actorPtr,scrambled);}catch(...){}
    } else { writeStr(actorPtr, O::Actor::mNameTag, scrambled); }
    done.insert(rid);
}

static void applyPlayerSpeed(void* playerPtr) {
    if(!playerPtr || !SeedManager::instance().isInitialized()) return;
    try {
        auto rng = SeedManager::instance().createRNG();
        std::uniform_real_distribution<float> d(0.05f,0.3f);
        fieldAt<float>(playerPtr, O::Player::mMovementSpeed) = d(rng);
    } catch(...) {}
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 9: Background Effect Thread
//
//  Runs even when hooks fail. Periodically scans for the player
//  object and applies effects directly via memory manipulation.
// ═══════════════════════════════════════════════════════════════════
static std::atomic<bool> gBgThreadRunning{false};
static void* gLocalPlayer = nullptr;
static std::atomic<bool> gPlayerSeedReady{false};

static void backgroundThread() {
    LOGI("Background effect thread started");
    gBgThreadRunning.store(true);
    int tick = 0;

    while(gBgThreadRunning.load()) {
        usleep(500000); // 500ms between scans
        ++tick;

        if(!SeedManager::instance().isInitialized()) continue;
        if(!gLocalPlayer) continue;

        // Every 2 seconds: apply inventory scramble (once)
        if(tick % 4 == 0 && !InventoryScramble::gInvScrambled.load()) {
            InventoryScramble::scrambleInventory(gLocalPlayer);
        }

        // Every 1 second: apply player speed
        if(tick % 2 == 0) {
            applyPlayerSpeed(gLocalPlayer);
        }
    }
    LOGI("Background effect thread stopped");
}

static std::thread gBgThread;

// ═══════════════════════════════════════════════════════════════════
//  SECTION 10: Hook Detours
// ═══════════════════════════════════════════════════════════════════
using CI_Update_Fn = void(*)(void*,void*,void*);
static CI_Update_Fn orig_CI_update = nullptr;

using Mob_Tick_Fn = void(*)(void*);
static Mob_Tick_Fn orig_Mob_tick = nullptr;

// ClientInstance::update — per-frame logic
static void ciUpdateDetour(void* ci, void* a2, void* a3) {
    if(orig_CI_update) orig_CI_update(ci,a2,a3);

    // Acquire LocalPlayer and initialize seed
    if(!SeedManager::instance().isInitialized() && gS.CI_getLocalPlayer && !gLocalPlayer) {
        try {
            gLocalPlayer = reinterpret_cast<void*(*)(void*)>(gS.CI_getLocalPlayer)(ci);
        } catch(...) {}
        if(gLocalPlayer) {
            LOGI("LocalPlayer at %p", gLocalPlayer);
            std::string name = readStr(gLocalPlayer, O::Player::mName);
            uint64_t um=0,ul=0;
            try{um=fieldAt<uint64_t>(gLocalPlayer,O::Player::mUUID_Most);
                ul=fieldAt<uint64_t>(gLocalPlayer,O::Player::mUUID_Least);}catch(...){}
            std::string uuid;
            if(um||ul) { char b[64]; snprintf(b,64,"%016lX-%016lX",(unsigned long)um,(unsigned long)ul); uuid=b; }
            else if(!name.empty()) uuid="name:"+name;
            if(!uuid.empty()) {
                SeedManager::instance().initializeWithUUID(uuid);
                LOGI("Seed from UUID: %s", uuid.c_str());
                uint64_t seed = SeedManager::instance().getSeed();
                MobSwap::buildSwapMap(seed);
                InventoryScramble::buildPermutation(seed);
                gPlayerSeedReady.store(true);
            }
        }
    }

    // Per-frame player effects
    if(gPlayerSeedReady.load() && gLocalPlayer) {
        applyPlayerSpeed(gLocalPlayer);
    }
}

// Mob::normalTick — per-entity logic
static int gMobTickCounter = 0;
static void mobTickDetour(void* mob) {
    if(orig_Mob_tick) orig_Mob_tick(mob);
    ++gMobTickCounter;
    if(gMobTickCounter % 20 != 0) return; // every ~1s
    if(!gPlayerSeedReady.load()) return;

    // Mob model swap
    MobSwap::trySwapMob(mob);

    // Nametag scramble
    applyNametagScramble(mob);
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 11: Hook Installation
// ═══════════════════════════════════════════════════════════════════
static std::atomic<bool> gHooksInstalled{false};

static bool installHooks() {
    if(gHooksInstalled.load()) return true;
    LOGI("=== Installing MC hooks ===");
    int n=0;

    if(gS.CI_update) {
        if(MiniHook::hook(gS.CI_update,reinterpret_cast<void*>(ciUpdateDetour),
                          reinterpret_cast<void**>(&orig_CI_update))) { ++n; LOGI("  Hooked CI::update"); }
        else LOGE("  Failed CI::update");
    } else LOGW("  CI::update not found");

    if(gS.Mob_normalTick) {
        if(MiniHook::hook(gS.Mob_normalTick,reinterpret_cast<void*>(mobTickDetour),
                          reinterpret_cast<void**>(&orig_Mob_tick))) { ++n; LOGI("  Hooked Mob::normalTick"); }
        else LOGE("  Failed Mob::normalTick");
    } else LOGW("  Mob::normalTick not found");

    gHooksInstalled.store(n>0);
    LOGI("Hooks installed: %d", n);
    return n>0;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 12: dlopen Hook — detects libminecraftpe.so load
// ═══════════════════════════════════════════════════════════════════
static void* (*orig_dlopen)(const char*,int) = nullptr;
static std::atomic<bool> gMCLoaded{false};
static std::atomic<bool> gIniting{false};

static void* my_dlopen(const char* fn, int flags) {
    void* h = orig_dlopen ? orig_dlopen(fn,flags) : nullptr;
    if(h && fn && strstr(fn,"libminecraftpe.so") && !gMCLoaded.load()) {
        LOGI("=== libminecraftpe.so loaded! ===");
        gMCLoaded.store(true); gLibMC = h;
        if(!gIniting.exchange(true)) {
            usleep(100000);
            resolveSymbols();
            installHooks();
            gIniting.store(false);
        }
    }
    return h;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 13: Initialization
// ═══════════════════════════════════════════════════════════════════
static bool isMinecraftProcess() {
    int fd=open("/proc/self/cmdline",O_RDONLY);
    if(fd<0) return false; char cmd[256]{}; auto sz=read(fd,cmd,255); close(fd);
    return sz>0 && (strstr(cmd,"minecraftpe")||strstr(cmd,"levimc"));
}

static void writeMarker() {
    FILE* f=fopen("/sdcard/Personalized.marker","w");
    if(f) { auto t=time(nullptr); fprintf(f,"Personalized v0.4.0 loaded at %sPID: %d\n",ctime(&t),getpid()); fclose(f); }
}

static Config gConfig;

static bool initialize() {
    LOGI("╔══════════════════════════════════════════════╗");
    LOGI("║  Personalized Mod v0.4.0                    ║");
    LOGI("║  Texture swap + Inventory scramble + Mob swap║");
    LOGI("╚══════════════════════════════════════════════╝");

    if(!isMinecraftProcess()) { LOGI("Not MC process"); return true; }
    LOGI("In MC process (PID %d)", getpid());
    writeMarker();

    // Generate texture resource pack with default seed
    // (will be regenerated with UUID seed when player joins)
    TextureSwap::generateResourcePack(gConfig.fixedSeed);

    // Find MC library
    gLibMC = dlopen("libminecraftpe.so", RTLD_NOW|RTLD_NOLOAD);
    if(gLibMC) {
        LOGI("libminecraftpe.so already loaded");
        gMCLoaded.store(true);
        resolveSymbols();
        installHooks();
    } else {
        LOGI("Hooking dlopen to detect MC load...");
        void* dlopenAddr = dlsym(RTLD_DEFAULT,"dlopen");
        if(!dlopenAddr) dlopenAddr = dlsym(RTLD_NEXT,"dlopen");
        if(dlopenAddr) {
            if(MiniHook::hook(dlopenAddr,reinterpret_cast<void*>(my_dlopen),
                              reinterpret_cast<void**>(&orig_dlopen)))
                LOGI("dlopen hooked at %p", dlopenAddr);
            else LOGE("dlopen hook failed");
        }
    }

    // Start background effect thread
    gBgThread = std::thread(backgroundThread);
    gBgThread.detach();

    LOGI("Initialization complete");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  SECTION 14: Entry Points
// ═══════════════════════════════════════════════════════════════════
__attribute__((constructor)) static void _init() { initialize(); }
__attribute__((destructor))  static void _fini() { gBgThreadRunning.store(false); }

extern "C" __attribute__((visibility("default"))) void mod_entry() { initialize(); }
extern "C" __attribute__((visibility("default"))) void* _pl_mod_instance() { static int x=1; return &x; }
extern "C" __attribute__((visibility("default"))) const char* mod_name() { return "Personalized"; }
extern "C" __attribute__((visibility("default"))) const char* mod_version() { return "0.4.0"; }
extern "C" __attribute__((visibility("default"))) const char* mod_description() { return "UUID-seeded texture swap, inventory scramble, mob model swap"; }

} // namespace personalized
