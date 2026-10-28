#include "pause.h"

#include <windows.h>

#include "addr.h"
#include "hooks.h"
#include "log.h"
#include "state.h"
#include "testpipe.h"

namespace fdcore::pause {
namespace {

using addr::Id;
using TickFn = void(__fastcall*)(void* session, void* edx);    // thiscall, no stack args
using ClockFn = void(__fastcall*)(void* subtimer, void* edx);  // thiscall, no stack args
using OnKeyFn = void(__fastcall*)(void* session, void* edx, int key, int down);  // thiscall, RET 8

TickFn g_origTick = nullptr;
OnKeyFn g_origOnKey = nullptr;
bool g_ok = false, g_active = false;
uintptr_t g_subtimer = 0;  // the subtimer this pause paused (0 = none)
uint32_t g_frames = 0;
LARGE_INTEGER g_freq, g_lastFrame;
LONG g_frameTicks = 0;  // 1/60 s in QPC ticks (32-bit division: no 64-bit helpers, D1)

const Id kNeeds[] = {Id::sim_tick_subsystems, Id::clock_pause,     Id::clock_resume,
                     Id::clock_game_timer,    Id::clock_real_timer, Id::clock_selector,
                     Id::controls_block_input, Id::console_state, Id::game_state, Id::session_on_key};  // + field clock.f_paused

bool SetImpl(bool on, const char* why);

void __fastcall TickDetour(void* session, void* edx) {
    if (g_active) {
        // A game command that leaves the level (loadmap typed while paused) moves the session
        // to loading, where RenderFrame and so the frame events stop: resume here, or the load
        // waits for a simulation step that never comes (pause_service run fc3).
        int32_t gs = 1;
        if (!addr::ReadI32(Id::game_state, &gs) || gs == 1) return;  // the whole simulation step (p2-p5)
        SetImpl(false, "level change");
    }
    g_origTick(session, edx);
}

// Window-message keys (Esc opens the in-game menu) bypass the input-block byte, which only
// gates DirectInput actions: key-downs to the game session are dropped while paused, key-ups
// pass so the game's own key latches still clear (freecam run fc5: Esc opened the menu).
void __fastcall OnKeyDetour(void* session, void* edx, int key, int down) {
    if (g_active && (down & 0xFF)) return;
    g_origOnKey(session, edx, key, down);
}

// The subtimer GameClock::Tick runs on, chosen as MainWindow::RunMessageLoop does on alt-tab.
uintptr_t ActiveSubtimer() {
    uint8_t sel = 0;
    uintptr_t t = 0;
    if (addr::Resolve(Id::clock_selector, &t)) addr::SafeRead(t, &sel, 1);
    return addr::Resolve(sel ? Id::clock_real_timer : Id::clock_game_timer, &t) ? t : 0;
}

void CallClock(Id fn, uintptr_t subtimer) {
    reinterpret_cast<ClockFn>(addr::ExportAddress(fn))(reinterpret_cast<void*>(subtimer), nullptr);
}

bool SubtimerPaused(uintptr_t subtimer) {
    uint8_t p = 0;
    return addr::SafeRead(subtimer + addr::Field(Id::clock_f_paused), &p, 1) && p;
}

bool ConsoleOpen() {
    int32_t s = 0;
    return addr::ReadI32(Id::console_state, &s) && s != 0;
}

#ifdef FDCORE_DEBUG
volatile long g_request = -1;  // Debug pipe `pause on|off`, applied on the game thread
#endif

void FrameBegin() {
#ifdef FDCORE_DEBUG
    if (const long r = InterlockedExchange(&g_request, -1); r >= 0) Set(r == 1, "test pipe");
#endif
    if (!g_active) return;
    const state::State& s = state::Current();
    if (s.gameState != 1 || !s.playerValid) {
        Set(false, "left the level");
        return;
    }
    ++g_frames;
    // Alt-tab back in runs the game's own GameClock::Resume on our subtimer: pause it again.
    if (g_subtimer && !SubtimerPaused(g_subtimer)) CallClock(Id::clock_pause, g_subtimer);
    // Closing the console clears the block; keep game actions off while paused.
    addr::WriteU8(Id::controls_block_input, 1);
    // ~60 fps: the game's Sleep cap is in the paused ComputeFrameDelta.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG frame = g_frameTicks;
    while (now.QuadPart - g_lastFrame.QuadPart < frame) {
        Sleep(1);
        QueryPerformanceCounter(&now);
    }
    g_lastFrame = now;
}

void OnLevel(const char*) {
    if (g_active) Set(false, "level load");
}

} // namespace

bool Init() {
    QueryPerformanceFrequency(&g_freq);
    g_frameTicks = g_freq.HighPart ? 0x7FFFFFFF / 60 : static_cast<LONG>(g_freq.LowPart / 60u);
    for (Id id : kNeeds)
        if (!addr::Usable(id)) {
            log::Warn("pause: disabled (needs %s)", addr::Name(id));
            return false;
        }
    if (!hooks::Create("pause", Id::sim_tick_subsystems, reinterpret_cast<void*>(&TickDetour),
                       reinterpret_cast<void**>(&g_origTick)) ||
        !hooks::Create("pause", Id::session_on_key, reinterpret_cast<void*>(&OnKeyDetour),
                       reinterpret_cast<void**>(&g_origOnKey)))
        return false;
    hooks::OnFrameBegin(FrameBegin);
    hooks::OnLevelLoad(OnLevel);
#ifdef FDCORE_DEBUG
    // Test hook (R6): `pause on|off` requests a pause for the next frame; any form replies
    // with the state.
    testpipe::Register("pause", [](const char* args, char* resp, int cap) {
        (void)cap;
        while (*args == ' ') ++args;
        if (args[0] == 'o' && args[1] == 'n') InterlockedExchange(&g_request, 1);
        else if (args[0] == 'o' && args[1] == 'f') InterlockedExchange(&g_request, 0);
        uint8_t block = 0;
        uintptr_t a;
        if (addr::Resolve(Id::controls_block_input, &a)) addr::SafeRead(a, &block, 1);
        wsprintfA(resp, "active=%d frames=%lu clockpaused=%d block=%d hook=%d", g_active,
                  static_cast<unsigned long>(g_frames), g_subtimer ? SubtimerPaused(g_subtimer) : 0, block,
                  hooks::IsEnabled("pause"));
    });
#endif
    g_ok = true;
    log::Info("pause: ready");
    return true;
}

bool Available() { return g_ok; }
bool Prepare() { return g_ok && (g_active || hooks::SetEnabled("pause", true)); }
void CancelPrepare() { if (g_ok && !g_active) hooks::SetEnabled("pause", false); }

bool Set(bool on, const char* why) { return SetImpl(on, why); }

namespace {
bool SetImpl(bool on, const char* why) {
    if (!g_ok || on == g_active) return g_ok;
    if (on) {
        const state::State& s = state::Current();
        if (s.gameState != 1 || !s.playerValid) return false;
        // The hook stays off outside a pause: zero cost, and the group toggle is atomic.
        if (!hooks::SetEnabled("pause", true)) return false;
        g_subtimer = ActiveSubtimer();
        if (g_subtimer && !SubtimerPaused(g_subtimer)) CallClock(Id::clock_pause, g_subtimer);
        else g_subtimer = 0;  // already paused by the game (focus lost): not ours to resume
        addr::WriteU8(Id::controls_block_input, 1);
        g_frames = 0;
        QueryPerformanceCounter(&g_lastFrame);
        g_active = true;
    } else {
        g_active = false;
        hooks::SetEnabled("pause", false);
        if (g_subtimer && SubtimerPaused(g_subtimer)) CallClock(Id::clock_resume, g_subtimer);
        g_subtimer = 0;
        if (!ConsoleOpen()) addr::WriteU8(Id::controls_block_input, 0);  // the console keeps its own block
    }
    log::Info("pause: %s (%s)", on ? "on" : "off", why ? why : "-");
    return true;
}
} // namespace

bool Active() { return g_active; }

uint32_t PausedFrames() { return g_frames; }

} // namespace fdcore::pause
