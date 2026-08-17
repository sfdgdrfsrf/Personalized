#pragma once

#include "ll/api/Logger.h"
#include <string>

namespace personalized {

inline ll::Logger& ModLogger() {
    static ll::Logger logger("Personalized");
    return logger;
}

} // namespace personalized

#define PZ_LOG_DEBUG(...)  ::personalized::ModLogger().debug(__VA_ARGS__)
#define PZ_LOG_INFO(...)   ::personalized::ModLogger().info(__VA_ARGS__)
#define PZ_LOG_WARN(...)   ::personalized::ModLogger().warn(__VA_ARGS__)
#define PZ_LOG_ERROR(...)  ::personalized::ModLogger().error(__VA_ARGS__)
#define PZ_LOG_FATAL(...)  ::personalized::ModLogger().fatal(__VA_ARGS__)

#ifndef NDEBUG
#define PZ_LOG_TRACE(...) ::personalized::ModLogger().debug("[TRACE] " __VA_ARGS__)
#else
#define PZ_LOG_TRACE(...) ((void)0)
#endif
