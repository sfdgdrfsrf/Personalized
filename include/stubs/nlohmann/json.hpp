#pragma once

/**
 * Stub: nlohmann/json.hpp — Minimal JSON stub for standalone compilation.
 *
 * Just enough for Config.cpp to compile. Does NOT do real JSON
 * serialization. For the real SDK build, use the actual nlohmann/json.
 */

#include <string>
#include <fstream>
#include <stdexcept>
#include <cstdint>

namespace nlohmann {

class json {
public:
    json() = default;

    // Implicit constructors for scalar types
    json(bool) {}
    json(int) {}
    json(uint64_t) {}
    json(double) {}
    json(const char*) {}
    json(const std::string&) {}

    // Stub: accept initializer_list of pairs for object construction
    // We use a single catch-all constructor
    template <typename T>
    json(std::initializer_list<T>) {}

    // Parse from stream (stub — always returns empty)
    static json parse(std::ifstream&) { return json(); }

    // value() with default — always returns default in stub mode
    template <typename T>
    T value(const std::string&, T defaultVal) const {
        return defaultVal;
    }

    // dump (stub)
    std::string dump(int = -1) const { return "{}"; }

    // exception
    class exception : public std::exception {
    public:
        const char* what() const noexcept override { return "json stub exception"; }
    };
};

} // namespace nlohmann
