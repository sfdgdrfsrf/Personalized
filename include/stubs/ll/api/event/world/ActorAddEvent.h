#pragma once

#include "ll/api/event/EventBus.h"
#include "mc/world/actor/Actor.h"

namespace ll::event {

class ActorAddEvent : public Event {
public:
    mc::Actor& self() { return *m_actor; }

private:
    mc::Actor* m_actor = nullptr;  // stub
};

} // namespace ll::event
