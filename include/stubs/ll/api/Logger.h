#pragma once

/**
 * Stub: ll/api/Logger.h — Minimal standalone Logger for compilation without SDK.
 *
 * Accepts fmtlib-style format strings and converts to printf-style at runtime.
 * When building with the real LeviLamina SDK, this file is overridden.
 *
 * Supported format specs (what Personalized uses):
 *   {}        → %s       (strings / generic)
 *   {:d}      → %d       (int)
 *   {:zu}     → %zu      (size_t)
 *   {:016X}   → %016llX  (uint64_t uppercase hex)
 *   {:016x}   → %016llx  (uint64_t lowercase hex)
 *   {:.0f}    → %.0f     (double, 0 decimal places)
 *   {:.2f}    → %.2f     (double, 2 decimal places)
 *   {:p}      → %p       (pointer)
 */

#include <string>
#include <cstdio>
#include <cstdint>
#include <utility>

namespace ll {

class Logger {
public:
    explicit Logger(std::string name) : m_name(std::move(name)) {}

    template <typename... Args>
    void debug(Args&&... args) { log("DEBUG", std::forward<Args>(args)...); }

    template <typename... Args>
    void info(Args&&... args)  { log("INFO",  std::forward<Args>(args)...); }

    template <typename... Args>
    void warn(Args&&... args)  { log("WARN",  std::forward<Args>(args)...); }

    template <typename... Args>
    void error(Args&&... args) { log("ERROR", std::forward<Args>(args)...); }

    template <typename... Args>
    void fatal(Args&&... args) { log("FATAL", std::forward<Args>(args)...); }

private:
    std::string m_name;

    void printPrefix(const char* level) {
        std::fprintf(stderr, "[%s][%s] ", level, m_name.c_str());
    }

    /// Convert std::string to const char* for printf; pass everything else through
    static const char* cstr(const std::string& s) { return s.c_str(); }
    static const char* cstr(const char* s)        { return s; }
    template <typename T>
    static T cstr(T v) { return v; }

    /// No-arg: print format string with placeholders stripped
    void log(const char* level, const char* fmt) {
        printPrefix(level);
        std::string out;
        for (const char* p = fmt; *p; ++p) {
            if (*p == '{') {
                if (*(p+1) == '{') { out += '{'; ++p; continue; }
                while (*p && *p != '}') ++p;
                continue;
            }
            if (*p == '}' && *(p+1) == '}') { out += '}'; ++p; continue; }
            out += *p;
        }
        std::fprintf(stderr, "%s\n", out.c_str());
    }

    /// Variadic: convert fmtlib → printf and forward, converting std::string→c_str
    template <typename... Args>
    void log(const char* level, const char* fmt, Args&&... args) {
        printPrefix(level);
        std::string pfmt = convertFmt(fmt);
        std::fprintf(stderr, pfmt.c_str(), cstr(std::forward<Args>(args))...);
        std::fputc('\n', stderr);
    }

    static std::string convertFmt(const char* fmt) {
        std::string result;
        for (const char* p = fmt; *p; ) {
            if (*p == '{' && *(p+1) == '{') { result += '{'; p += 2; continue; }
            if (*p == '}' && *(p+1) == '}') { result += '}'; p += 2; continue; }

            if (*p == '{') {
                ++p;
                std::string spec;
                while (*p && *p != '}') { spec += *p; ++p; }
                if (*p == '}') ++p;

                if (spec.empty()) {
                    result += "%s";
                } else if (spec == "d") {
                    result += "%d";
                } else if (spec == "zu") {
                    result += "%zu";
                } else if (spec == "p") {
                    result += "%p";
                } else if (spec == "016X") {
                    result += "%016llX";
                } else if (spec == "016x") {
                    result += "%016llx";
                } else if (spec.size() >= 2 && spec[0] == '.') {
                    result += "%" + spec;
                } else {
                    result += "%" + spec;
                }
            } else {
                result += *p;
                ++p;
            }
        }
        return result;
    }
};

} // namespace ll
