#pragma once

/**
 * Stub: ll/api/mod/NativeMod.h
 *
 * LeviLamina mod loading infrastructure. Stub for standalone builds.
 */

namespace ll::mod {

class NativeMod {
public:
    virtual ~NativeMod() = default;
    virtual void onLoad() {}
    virtual void onEnable() {}
    virtual void onDisable() {}
};

} // namespace ll::mod
