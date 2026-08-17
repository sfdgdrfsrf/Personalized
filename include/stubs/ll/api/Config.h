#pragma once
#include <string>

namespace ll::config {

template <typename T>
bool loadConfig(T&, const std::string&) { return false; }

template <typename T>
bool saveConfig(const T&, const std::string&) { return false; }

} // namespace ll::config
