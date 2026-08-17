#pragma once

/**
 * Stub: ll/api/memory/Hook.h
 *
 * Provides the LL_TYPE_INSTANCE_HOOK / LL_TYPE_STATIC_HOOK macros
 * as no-ops for standalone compilation. When the real SDK is present,
 * these expand to actual memory hook registrations.
 */

// In standalone mode, hook macros expand to empty struct declarations.
// The actual hook logic lives inside commented-out blocks in the .cpp
// files anyway, so this just needs to compile.

#define LL_TYPE_INSTANCE_HOOK(ClassName, BaseClass, Offset, RetType, ...) \
    struct ClassName { /* stub hook — no-op without real SDK */ }

#define LL_TYPE_STATIC_HOOK(ClassName, BaseClass, Offset, RetType, ...) \
    struct ClassName { /* stub hook — no-op without real SDK */ }

#define LL_TYPE_INSTANCE_HOOK2(ClassName, BaseClass, Offset, RetType, ...) \
    struct ClassName { /* stub hook — no-op without real SDK */ }
