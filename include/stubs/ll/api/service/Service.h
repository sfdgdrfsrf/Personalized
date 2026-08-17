#pragma once
#include <vector>
#include "mc/world/actor/player/Player.h"

namespace ll::service {

struct LevelStub {
    std::vector<Player> getPlayers() { return {}; }
};
inline LevelStub* getLevel() { return nullptr; }

} // namespace ll::service
