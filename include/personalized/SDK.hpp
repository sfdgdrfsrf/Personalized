#pragma once

/// MC SDK wrapper — offset-based field access for MC Bedrock objects.
/// Follows the BedrockTools pattern: raw pointer + known offsets.

#include "personalized/Offsets.hpp"
#include <cstdint>
#include <string>
#include <cstring>

namespace personalized::sdk {

// ── Field accessor (same as BedrockTools) ──

template <class T>
T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(object) + offset);
}

template <class T>
const T& field(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<std::uintptr_t>(object) + offset);
}

// ── Virtual call helper ──

template <class Return, class... Args>
Return virtualCall(void* instance, std::size_t index, Args&&... args) {
    auto table = *reinterpret_cast<void***>(instance);
    auto function = reinterpret_cast<Return(*)(void*, Args...)>(table[index]);
    return function(instance, std::forward<Args>(args)...);
}

// ── Basic types ──

struct Vec3 {
    float x{}, y{}, z{};
};

struct Vec2 {
    float x{}, y{};
};

struct AABB {
    Vec3 min{}, max{};
};

// ── MC class wrappers ──

class Dimension {
public:
    void* nativePtr;
    explicit Dimension(void* ptr) : nativePtr(ptr) {}
    explicit operator bool() const { return nativePtr != nullptr; }
};

class Level {
public:
    void* nativePtr;
    explicit Level(void* ptr) : nativePtr(ptr) {}
    explicit operator bool() const { return nativePtr != nullptr; }
};

class Actor {
public:
    void* nativePtr;
    explicit Actor(void* ptr) : nativePtr(ptr) {}
    explicit operator bool() const { return nativePtr != nullptr; }

    Vec3 position() const {
        auto* component = field<void*>(nativePtr, offsets::Actor::mStateVectorComponent);
        return component ? field<Vec3>(component, 0) : Vec3{};
    }

    Vec2 rotation() const {
        auto* component = field<void*>(nativePtr, offsets::Actor::mActorRotationComponent);
        return component ? field<Vec2>(component, 0) : Vec2{};
    }

    Level* level() const {
        auto* ptr = field<void*>(nativePtr, offsets::Actor::mLevel);
        static Level lvl(nullptr);
        lvl = Level(ptr);
        return &lvl;
    }

    Dimension* dimension() const {
        auto* ptr = field<void*>(nativePtr, offsets::Actor::mDimension);
        static Dimension dim(nullptr);
        dim = Dimension(ptr);
        return &dim;
    }

    std::uint32_t categories() const {
        return field<std::uint32_t>(nativePtr, offsets::Actor::mCategories);
    }
};

class Player : public Actor {
public:
    explicit Player(void* ptr) : Actor(ptr) {}

    std::string& name() {
        return field<std::string>(nativePtr, offsets::Player::mName);
    }

    const std::string& name() const {
        return field<std::string>(nativePtr, offsets::Player::mName);
    }
};

} // namespace personalized::sdk
