#pragma once

/**
 * Logger.h — Thin wrapper around LeviLamina's logger.
 *
 * Every module should include this instead of raw ilaGetLogger() calls
 * so we can toggle verbosity, prefix module names, and compile out
 * debug traces in release builds.
 *
 * FORMAT STRING NOTE:
 *   The source code uses fmtlib-style format strings (which is what
 *   LeviLamina's real logger expects). The standalone stub Logger
 *   converts these to printf-style at runtime. For this to work:
 *     - Strings:    use {}          → stub converts to %s
 *     - uint64_t:   use {:016X}     → stub converts to %016llX
 *     - size_t:     use {:/d}       → stub converts to %zu
 *     - double:     use {:.2f}      → stub converts to %.2f
 *     - pointers:   use {:p}        → stub converts to %p
 *     - int:        use {:d}        → stub converts to %d
 *
 *   With the real LeviLamina SDK, all fmtlib syntax works natively.
 */

#include "ll/api/Logger.h"
#include <string>

// ─────────────────────────────────────────────
//  Module-level logger factory
// ─────────────────────────────────────────────
namespace personalized {

/// Returns a module-scoped logger.
inline ll::Logger& ModLogger() {
    static ll::Logger logger("Personalized");
    return logger;
}

/// Create a sub-logger for a specific hook module
inline ll::Logger MakeModuleLogger(const std::string& name) {
    return ll::Logger("Personalized::" + name);
}

} // namespace personalized

// ─────────────────────────────────────────────
//  Convenience macros
// ─────────────────────────────────────────────

#define PZ_LOG_DEBUG(...)  ::personalized::ModLogger().debug(__VA_ARGS__)
#define PZ_LOG_INFO(...)   ::personalized::ModLogger().info(__VA_ARGS__)
#define PZ_LOG_WARN(...)   ::personalized::ModLogger().warn(__VA_ARGS__)
#define PZ_LOG_ERROR(...)  ::personalized::ModLogger().error(__VA_ARGS__)
#define PZ_LOG_FATAL(...)  ::personalized::ModLogger().fatal(__VA_ARGS__)

// Trace-level: only compiled in debug builds
#ifndef NDEBUG
#define PZ_LOG_TRACE(...) ::personalized::ModLogger().debug("[TRACE] " __VA_ARGS__)
#else
#define PZ_LOG_TRACE(...) ((void)0)
#endif
