// The core services the code under test calls, for the host program. log keeps every
// line in memory (unit::LogContains); console::Print goes there too, as "console> ...".
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../core/console.h"
#include "../../core/hooks.h"
#include "../../core/log.h"
#include "../../core/state.h"
#include "unit.h"

namespace {
char g_lines[1 << 20];
int g_len = 0;

// "<tag><text>" as one line; memory keeps what fits.
void Add(const char* level, const char* tag, const char* text) {
    char line[2048];
    int n = snprintf(line, sizeof(line), "[%s] %s%s\n", level, tag, text);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;
    if (g_len + n < static_cast<int>(sizeof(g_lines))) memcpy(g_lines + g_len, line, n), g_len += n, g_lines[g_len] = 0;
}

void Format(const char* level, const char* fmt, va_list ap) {
    char body[1536];
    vsnprintf(body, sizeof(body), fmt, ap);
    Add(level, "", body);
}
} // namespace

namespace unit {
bool LogContains(const char* text) { return strstr(g_lines, text) != nullptr; }
int LogCount(const char* text) {
    int n = 0;
    for (const char* p = strstr(g_lines, text); p; p = strstr(p + 1, text)) ++n;
    return n;
}
void ClearLog() { g_len = 0, g_lines[0] = 0; }
} // namespace unit

namespace fdcore::log {
void Info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Format("INFO", fmt, ap);
    va_end(ap);
}
void Warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Format("WARN", fmt, ap);
    va_end(ap);
}
void Error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Format("ERROR", fmt, ap);
    va_end(ap);
}
void Raw(const char* tag, const char* text) { Add("INFO", tag, text); }
} // namespace fdcore::log

namespace {
char g_gameDir[260] = ".\\";
fdcore::state::State g_state = {};
} // namespace

namespace unit {
void SetGameDir(const char* dir) { snprintf(g_gameDir, sizeof(g_gameDir), "%s", dir); }
} // namespace unit

namespace fdcore::console {
void Print(const char* fmt, ...) {
    char body[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    Add("INFO", "console> ", body);
}
const char* GameDir() { return g_gameDir; }
const char* CoreVersion() { return "0.0.0-unit"; }
} // namespace fdcore::console

namespace fdcore::hooks {
bool SetEnabled(const char*, bool) { return true; }
void OnFrameBegin(FrameFn) {}
} // namespace fdcore::hooks

namespace fdcore::state {
const State& Current() { return g_state; }
} // namespace fdcore::state
