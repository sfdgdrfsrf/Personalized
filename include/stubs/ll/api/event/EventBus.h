#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

namespace ll::event {

class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus inst;
        return inst;
    }

    using ListenerPtr = void*;

    template <typename Event, typename Callback>
    ListenerPtr emplaceListener(Callback&&) {
        return nullptr;
    }

    template <typename Event>
    void removeListener(ListenerPtr) {}
};

namespace player {
struct PlayerJoinEvent {
    Player& self() { static Player s; return s; }
};
} // namespace player

namespace world {
struct ActorAddEvent {
    Actor& self() { static Actor s; return s; }
};
} // namespace world

namespace server {
struct ServerTickEvent {};
struct ServerStartingEvent {};
struct ServerStoppingEvent {};
} // namespace server

using PlayerJoinEvent = player::PlayerJoinEvent;
using ActorAddEvent = world::ActorAddEvent;
using ServerTickEvent = server::ServerTickEvent;
using ServerStartingEvent = server::ServerStartingEvent;
using ServerStoppingEvent = server::ServerStoppingEvent;

} // namespace ll::event
