#pragma once
#include <string>
#include <filesystem>

namespace ll::mod {

class NativeMod {
public:
    static NativeMod* current() { static NativeMod inst; return &inst; }

    std::filesystem::path getConfigDir() const { return "."; }
    std::filesystem::path getDataDir() const { return "."; }

    class LoggerStub {
    public:
        template <typename... Args>
        void info(Args&&...) {}
        template <typename... Args>
        void warn(Args&&...) {}
        template <typename... Args>
        void debug(Args&&...) {}
        template <typename... Args>
        void error(Args&&...) {}
    };

    LoggerStub& getLogger() { static LoggerStub s; return s; }
};

} // namespace ll::mod
