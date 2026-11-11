// Tests: tests/game/window_alttab.yaml
// User: Window -- keeps the mouse cursor in the game window only while the game is focused (the alt-tab fix).
// The game calls SetCursorPos(200, 200) every frame from
// GameControls::PollFrame (0x00439347: push 200; push 200; call [SetCursorPos]),
// even when it isn't the foreground window, so after alt-tab the mouse stays
// trapped at (200, 200). alttabfix.asi replaces those 16 bytes with a jump to
// a routine that recentres the cursor on the game window -- only while the
// game is foreground and 'O' isn't held. This module does the same: 11 NOPs +
// `call CursorWarp` (stdcall, no args: stack-balanced like the original call,
// and the caller doesn't use EAX/ECX/EDX after SetCursorPos).
#include "window.h"

#include <windows.h>

#include "addr.h"
#include "binds.h"
#include "cmd.h"
#include "log.h"
#include "modules.h"

namespace fdcore::window {
namespace {

using addr::Id;

volatile long g_mode = 1;  // fd_window_cursor; read every frame on the game thread
HWND g_hwnd = nullptr;
bool g_patched = false;

void __stdcall CursorWarp() {
    if (g_mode == 0) {  // the game's own behaviour
        SetCursorPos(200, 200);
        return;
    }
    if (!g_hwnd) {
        // Like alttabfix: the game's top-level window by title -- but only if
        // it belongs to this process (a second game or a same-titled window
        // elsewhere must not be picked up).
        HWND h = FindWindowA(nullptr, "FDNY Firefighter");
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        if (!h || pid != GetCurrentProcessId()) return;
        g_hwnd = h;
        log::Info("window: game window %08lx", reinterpret_cast<unsigned long>(h));
    }
    // alttabfix parity: any nonzero GetAsyncKeyState('O') (held, or pressed
    // since the last poll) leaves the cursor alone this frame.
    if (GetForegroundWindow() != g_hwnd || GetAsyncKeyState('O')) return;  // arch-allow: R9 read inside the game's own cursor call, any press since the last one counts
    RECT rc;
    if (!GetWindowRect(g_hwnd, &rc)) return;
    SetCursorPos((rc.left + rc.right) >> 1, (rc.top + rc.bottom) >> 1);
}

void OnMode(cmd::Cvar& v) { InterlockedExchange(&g_mode, cmd::Bool(v)); }
cmd::Cvar g_fdWindowCursor = {
    "fd_window_cursor", "1",
    "1 = keep the cursor in the game window only while it is focused (hold O to free it); "
    "0 = game default (warp to 200,200 every frame, even when alt-tabbed)",
    false, true, OnMode, {0}, cmd::T_BOOL};

} // namespace

bool ApplyEarlyPatch() {
    uintptr_t site = addr::EarlyCodeAddress(Id::patch_cursor_warp);
    if (!site) {
        log::Warn("window: cursor-warp site doesn't match (signature mismatch, see above); "
                  "window module inactive");
        return false;
    }
    uint8_t code[16];
    for (int i = 0; i < 11; ++i) code[i] = 0x90;
    code[11] = 0xE8;  // call rel32
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&CursorWarp) - (site + 16));
    for (int i = 0; i < 4; ++i) code[12 + i] = static_cast<uint8_t>(rel >> (8 * i));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
        log::Error("window: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    for (int i = 0; i < 16; ++i) reinterpret_cast<volatile uint8_t*>(site)[i] = code[i];
    VirtualProtect(reinterpret_cast<void*>(site), sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(code));
    g_patched = true;
    log::Info("window: cursor warp at %08lx now calls FDNYCore (alttabfix behaviour)",
              static_cast<unsigned long>(site));
    return true;
}

bool Init() {
    if (!g_patched) return false;  // arch-allow: R10 ApplyEarlyPatch logged why
    cmd::RegisterCvar(&g_fdWindowCursor);
    binds::DescribeKey("o", "free the mouse cursor from the game window (held)", "while fd_window_cursor is 1");  // CursorWarp
    return true;
}

namespace {
void Early(const char*) { ApplyEarlyPatch(); }
} // namespace

} // namespace fdcore::window

FDCORE_MODULE(window, "window", nullptr, 0, &fdcore::window::Early, &fdcore::window::Init, 20)
