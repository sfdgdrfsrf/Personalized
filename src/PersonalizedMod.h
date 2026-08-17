#pragma once

#include "ll/api/mod/NativeMod.h"
#include "mc/world/actor/player/Player.h"

namespace personalized {

class PersonalizedMod {
public:
    static PersonalizedMod& getInstance();

    PersonalizedMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    void buildMobSwapMap();
    void shufflePlayerInventory(Player& player);

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace personalized
