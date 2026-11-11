// Tests: tests/game/notify.yaml tests/unit/test_cmd.cpp
#include "notify.h"

#include <windows.h>
#include <stdarg.h>

#include "log.h"
#include "pause.h"
#include "state.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::notify {
namespace {

constexpr unsigned kShowMs = 2000;
bool g_ready = false, g_hints = false;
char g_last[160];     // the pipe thread reads it
unsigned long g_count = 0, g_shown = 0;
CRITICAL_SECTION g_lock;

} // namespace

bool Init() {
    InitializeCriticalSection(&g_lock);
    g_hints = textdraw::InitGameHints();
    if (!g_hints) log::Warn("notify: the game's hint text is unavailable; confirmations go to the log only");
#ifdef FDCORE_DEBUG
    // Test hook: `notify`: the last text, how many were asked for and how many reached the screen.
    testpipe::Register("notify", [](const char*, char* resp, int) {
        EnterCriticalSection(&g_lock);
        wsprintfA(resp, "count=%lu shown=%lu last=%s", g_count, g_shown, g_last);
        LeaveCriticalSection(&g_lock);
    });
#endif
    g_ready = true;
    log::Info("notify: ready (%s)", g_hints ? "game hint text" : "log only");
    return true;
}

void Show(const char* fmt, ...) {
    char text[1024];  // wvsprintf's own limit
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(text, fmt, ap);
    va_end(ap);
    text[159] = 0;  // one hint line
    log::Info("notify> %s", text);
    if (!g_ready) return;
    // Not over a pause: the hint's timer is the game clock, which is stopped.
    // In a level only: the hint belongs to the player object, and at a menu that pointer is the
    // last level's (a practice-only refusal from the settings page crashed in the hint; D67).
    const state::State& s = state::Current();
    const bool shown = g_hints && s.gameState == 1 && s.playerValid && !pause::Active() &&
                       textdraw::GameHint(text, kShowMs, false);
    EnterCriticalSection(&g_lock);
    lstrcpynA(g_last, text, sizeof(g_last));
    ++g_count, g_shown += shown;
    LeaveCriticalSection(&g_lock);
}

} // namespace fdcore::notify
