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

    /// No-arg: print format string with placeholders stripped
    void log(const char* level, const char* fmt) {
        printPrefix(level);
        std::string out;
        for (const char* p = fmt; *p; ++p) {
            if (*p == '{') {
                // Skip escaped {{
                if (*(p+1) == '{') { out += '{'; ++p; continue; }
                while (*p && *p != '}') ++p;
                continue;  // skip closing }
            }
            if (*p == '}' && *(p+1) == '}') { out += '}'; ++p; continue; }
            out += *p;
        }
        std::fprintf(stderr, "%s\n", out.c_str());
    }

    /// Variadic: convert fmtlib → printf and forward
    template <typename... Args>
    void log(const char* level, const char* fmt, Args&&... args) {
        printPrefix(level);
        std::string pfmt = convertFmt(fmt);
        std::fprintf(stderr, pfmt.c_str(), std::forward<Args>(args)...);
        std::fputc('\n', stderr);
    }

    static std::string convertFmt(const char* fmt) {
        std::string result;
        for (const char* p = fmt; *p; ) {
            // Escaped {{
            if (*p == '{' && *(p+1) == '{') { result += '{'; p += 2; continue; }
            // Escaped }}
            if (*p == '}' && *(p+1) == '}') { result += '}'; p += 2; continue; }

            if (*p == '{') {
                ++p; // skip {
                std::string spec;
                while (*p && *p != '}') { spec += *p; ++p; }
                if (*p == '}') ++p; // skip }

                // Map format spec to printf
                if (spec.empty()) {
                    result += "%s";         // {} → %s (string/generic)
                } else if (spec == "d") {
                    result += "%d";         // {:d} → %d
                } else if (spec == "zu") {
                    result += "%zu";        // {:zu} → %zu (size_t)
                } else if (spec == "p") {
                    result += "%p";         // {:p} → %p
                } else if (spec == "016X") {
                    result += "%016llX";    // {:016X} → %016llX
                } else if (spec == "016x") {
                    result += "%016llx";    // {:016x} → %016llx
                } else if (spec.size() >= 2 && spec[0] == '.') {
                    result += "%" + spec;   // {:.0f} → %.0f, {:.2f} → %.2f
                } else {
                    result += "%" + spec;   // fallback
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
