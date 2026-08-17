#pragma once

#include "ll/api/event/EventBus.h"
#include "mc/world/actor/player/Player.h"

namespace ll::event {

class PlayerJoinEvent : public Event {
public:
    /// Returns a reference to the joining player
    mc::Player& self() { return *m_player; }

private:
    mc::Player* m_player = nullptr;  // stub — never actually set
};

} // namespace ll::event
