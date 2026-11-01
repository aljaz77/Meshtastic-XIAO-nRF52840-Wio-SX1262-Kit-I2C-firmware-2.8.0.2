#pragma once
// Minimal host stand-ins for the Arduino / Meshtastic APIs used by the BQ25798 overlay.
// Only for tests/test_bq25798.cpp; the real firmware uses the real headers.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
inline void delay(uint32_t ms) { fakeMillis += ms; }

// Captured log lines so tests can assert on them
extern std::vector<std::string> logLines;
inline void fakeLog(const char *level, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logLines.push_back(std::string(level) + " " + buf);
}
#define LOG_DEBUG(...) fakeLog("DEBUG", __VA_ARGS__)
#define LOG_INFO(...) fakeLog("INFO", __VA_ARGS__)
#define LOG_WARN(...) fakeLog("WARN", __VA_ARGS__)
#define LOG_ERROR(...) fakeLog("ERROR", __VA_ARGS__)
