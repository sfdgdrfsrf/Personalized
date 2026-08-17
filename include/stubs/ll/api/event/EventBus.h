#pragma once

/**
 * Stub: ll/api/event/EventBus.h
 *
 * Minimal event bus for standalone compilation.
 * Real LeviLamina provides a full event dispatch system.
 */

#include <functional>
#include <memory>
#include <type_traits>
#include <vector>
#include <string>

namespace ll::event {

/// Base class for all events
class Event {
public:
    virtual ~Event() = default;
};

/// Opaque listener handle
using ListenerPtr = std::shared_ptr<void>;

/// Stub EventBus — stores lambdas but never actually fires them
class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus inst;
        return inst;
    }

    /// Register a listener for event type T
    template <typename T, typename F>
    ListenerPtr emplaceListener(F&& /*func*/) {
        // In standalone mode, we just return a dummy handle
        return std::make_shared<int>(0);
    }

    /// Remove a listener
    template <typename T>
    void removeListener(ListenerPtr& /*ptr*/) {
        // No-op
    }

private:
    EventBus() = default;
};

} // namespace ll::event
