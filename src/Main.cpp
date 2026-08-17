#include "PersonalizedMod.h"
#include "Config.h"
#include "SeedManager.h"
#include "Utils/Logger.h"
#include "Utils/RandomMapper.h"

#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/world/ActorAddEvent.h"
#include "ll/api/event/server/ServerTickEvent.h"
#include "ll/api/service/Service.h"

#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

#include <numeric>
#include <unordered_map>

namespace personalized {

namespace {
Config config;
bool mobSwapBuilt = false;
bool inventoryShuffled = false;
int tickCounter = 0;

std::unordered_map<std::string, std::string> mobSwapMap;

const std::vector<std::string> mobPool = {
    "minecraft:zombie",
    "minecraft:skeleton",
    "minecraft:pig",
    "minecraft:cow",
    "minecraft:sheep",
    "minecraft:chicken",
    "minecraft:creeper",
    "minecraft:spider",
    "minecraft:blaze",
    "minecraft:enderman",
};

} // namespace

PersonalizedMod& PersonalizedMod::getInstance() {
    static PersonalizedMod instance;
    return instance;
}

bool PersonalizedMod::load() {
    auto& logger = getSelf().getLogger();
    logger.info("Loading Personalized...");

    auto configFilePath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configFilePath.string())) {
        logger.warn("Cannot load config from {}", configFilePath.string());
        logger.info("Saving default config");
        ll::config::saveConfig(config, configFilePath.string());
    }

    return true;
}

bool PersonalizedMod::enable() {
    auto& logger = getSelf().getLogger();
    auto& bus = ll::event::EventBus::getInstance();

    logger.info("Personalized v0.2.0 — UUID-seeded world scrambling mod");

    if (!config.enabled) {
        logger.info("Mod disabled in config");
        return true;
    }

    // ── 1. PlayerJoinEvent: Capture UUID → seed the mod ──
    bus.emplaceListener<ll::event::player::PlayerJoinEvent>(
        [&logger](ll::event::player::PlayerJoinEvent& event) {
            auto& player = event.self();
            auto uuid = player.getUuid().asString();

            logger.info("Player {} joining — UUID: {}", player.getRealName(), uuid);

            if (!SeedManager::instance().isInitialized()) {
                if (config.seedSource == "fixed") {
                    SeedManager::instance().initializeWithFixedSeed(config.fixedSeed);
                } else {
                    SeedManager::instance().initializeWithUUID(uuid);
                }
            }
        }
    );

    // ── 2. ActorAddEvent: Swap mob types on spawn ──
    if (config.mobModelSwapEnabled) {
        bus.emplaceListener<ll::event::ActorAddEvent>(
            [&logger](ll::event::ActorAddEvent& event) {
                if (!SeedManager::instance().isInitialized()) return;
                if (mobSwapMap.empty() && !mobSwapBuilt) {
                    PersonalizedMod::getInstance().buildMobSwapMap();
                    mobSwapBuilt = true;
                }
                if (mobSwapMap.empty()) return;

                auto& actor = event.self();
                if (actor.isPlayer()) return;

                std::string typeId = actor.getTypeName();
                auto it = mobSwapMap.find(typeId);
                if (it == mobSwapMap.end()) return;

                const std::string& replacement = it->second;

                if (config.dryRun) {
                    logger.info("[DRY] Would swap {} -> {}", typeId, replacement);
                    return;
                }

                // Kill original mob and spawn replacement at same position
                auto pos = actor.getPosition();
                auto* dim = actor.getDimension();
                if (!dim) return;

                actor.remove();

                // Spawn replacement entity — client renders new mob's model
                dim->spawnEntity(replacement, pos);

                logger.debug("Swapped {} -> {} at ({:.0f}, {:.0f}, {:.0f})",
                             typeId, replacement, pos.x, pos.y, pos.z);
            }
        );
        logger.info("Mob model swap hook installed");
    }

    // ── 3. ServerTickEvent: Inventory scramble after seed is ready ──
    if (config.inventoryScrambleEnabled) {
        bus.emplaceListener<ll::event::server::ServerTickEvent>(
            [&logger](ll::event::server::ServerTickEvent&) {
                if (inventoryShuffled) return;
                if (!SeedManager::instance().isInitialized()) return;

                ++tickCounter;
                if (tickCounter < 40) return; // Wait 2 seconds for players to join

                auto* level = ll::service::getLevel();
                if (!level) return;

                for (auto& player : level->getPlayers()) {
                    PersonalizedMod::getInstance().shufflePlayerInventory(player);
                }

                inventoryShuffled = true;
                logger.info("Inventory scrambled for all players");
            }
        );
        logger.info("Inventory scramble hook installed");
    }

    logger.info("All hooks active");
    return true;
}

bool PersonalizedMod::disable() {
    getSelf().getLogger().info("Disabling Personalized...");
    mobSwapMap.clear();
    mobSwapBuilt = false;
    inventoryShuffled = false;
    return true;
}

void PersonalizedMod::buildMobSwapMap() {
    if (!SeedManager::instance().isInitialized()) return;

    auto& logger = getSelf().getLogger();
    auto rng = SeedManager::instance().createRNG();
    std::mt19937_64 mobRng(SeedManager::instance().getSeed() ^ 0xB0B0FACEDEADBEEFULL);

    mobSwapMap.clear();

    for (const auto& mobId : mobPool) {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(mobRng) > config.mobSwapIntensity) continue;

        std::string selected;
        int attempts = 0;
        do {
            std::uniform_int_distribution<size_t> idxDist(0, mobPool.size() - 1);
            selected = mobPool[idxDist(mobRng)];
            ++attempts;
        } while (selected == mobId && attempts < 10);

        mobSwapMap[mobId] = selected;
        logger.debug("Mob swap: {} -> {}", mobId, selected);
    }

    logger.info("Mob swap map built: {} entries", mobSwapMap.size());
}

void PersonalizedMod::shufflePlayerInventory(Player& player) {
    if (config.dryRun) {
        getSelf().getLogger().info("[DRY] Would shuffle inventory for {}", player.getRealName());
        return;
    }

    std::mt19937_64 invRng(SeedManager::instance().getSeed() ^ 0xC0FFEE0000000000ULL);

    auto& inventory = player.getInventory();
    int slotCount = inventory.getContainerSize();
    if (slotCount <= 0) return;

    // Fisher-Yates shuffle of inventory slots
    for (int pass = 0; pass < config.inventoryShufflePasses; ++pass) {
        for (int i = slotCount - 1; i > 0; --i) {
            std::uniform_int_distribution<int> dist(0, i);
            int j = dist(invRng);
            if (i != j) {
                auto itemA = inventory.getItem(i);
                auto itemB = inventory.getItem(j);
                inventory.setItem(i, *itemB);
                inventory.setItem(j, *itemA);
            }
        }
    }

    player.refreshInventory();
    getSelf().getLogger().debug("Shuffled inventory for {} ({} slots)",
                                player.getRealName(), slotCount);
}

} // namespace personalized

#include "ll/api/memory/Hook.h"
// LL_REGISTER_MOD: real SDK auto-registers; stub defines it as no-op
LL_REGISTER_MOD(personalized::PersonalizedMod, personalized::PersonalizedMod::getInstance())
