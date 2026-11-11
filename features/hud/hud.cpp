// Tests: tests/game/hud_player.yaml tests/game/text_panels.yaml tests/game/text_alpha.yaml tests/game/keys_default.yaml
// User: Player HUD -- position, velocity, XY speed and stance on screen.
#include "binds.h"
#include "hud.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "num.h"
#include "state.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::hud {
namespace {

using addr::Id;
int g_panel = -1;  // in-frame text panel (textdraw)

volatile long g_on = 0;  // fd_hud
void OnToggle(cmd::Cvar& v) {
    InterlockedExchange(&g_on, cmd::Bool(v));
    textdraw::Show(g_panel, g_on != 0);
}
cmd::Cvar g_fdHudPlayer = {"fd_hud", "0",
                           "1 = show player info in-game: position, velocity, XY speed, stance (standing / crouched / bugged); 0 = off",
                           false, true, OnToggle, {0}, cmd::T_BOOL};

constexpr int kLines = 6, kLineLen = 96;
char g_lines[kLines][kLineLen];
int g_nLines = 0;
uint32_t g_drawnFrame = 0;
state::State g_drawnFrom;  // the snapshot the last HUD was drawn from
CRITICAL_SECTION g_lock;  // the pipe thread reads what the game thread drew

// Owner 2026-10-23: only "standing", "crouched" or "bugged" (crouchbug). Crouched = state 4;
// crouchbug = any other state with the crouched collision box (the warp HUD's rule,
// glitch_analysis TrackCrouchbug: state != 4 and box top < 55; 85 standing, 25 crouched).
constexpr float kCrouchBoxTop = 55.0f;
const char* Stance(const state::State& s) {
    if (s.moveState == 4) return "crouched";
    return s.boxTop < kCrouchBoxTop ? "bugged" : "standing";
}

// Text right-justified to `width` columns (wsprintf has no %*s).
int Pad(char* out, const char* text, int width) {
    const int n = lstrlenA(text);
    int o = 0;
    for (; o < width - n; ++o) out[o] = ' ';
    lstrcpyA(out + o, text);
    return o + n;
}
int Col(char* out, float v, int width) {
    char num[24];
    num::Fixed(num, v, 1);
    return Pad(out, num, width);
}

void Draw() {
    if (!g_on) return;
    void* engine = hooks::CurrentEngine();
    const state::State& s = state::Current();
    if (!engine) return;

    // Owner 2026-10-23: x / y / z in right-justified columns; no screen line.
    constexpr int kW = 9;  // "-12345.6" + a space
    char lines[kLines][kLineLen];
    int n = 0, o;
    o = wsprintfA(lines[n], "     ");
    o += Pad(lines[n] + o, "x", kW), o += Pad(lines[n] + o, "y", kW), o += Pad(lines[n] + o, "z", kW);
    ++n;
    o = wsprintfA(lines[n], "pos  ");
    for (int i = 0; i < 3; ++i) o += Col(lines[n] + o, s.pos[i], kW);
    lines[n++][o] = 0;
    o = wsprintfA(lines[n], "vel  ");
    for (int i = 0; i < 3; ++i) o += Col(lines[n] + o, s.vel[i], kW);
    lines[n++][o] = 0;
    o = wsprintfA(lines[n], "speed");
    o += Col(lines[n] + o, s.speedXY, kW);
    lines[n++][o] = 0;
    o = wsprintfA(lines[n], "state");
    Pad(lines[n++] + o, Stance(s), kW);

    // Below the game's mission timer (top-left), one in-frame text panel.
    const char* rows[kLines];
    for (int i = 0; i < n; ++i) rows[i] = lines[i];
    textdraw::SetLines(g_panel, rows, n);

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < n; ++i) lstrcpynA(g_lines[i], lines[i], kLineLen);
    g_nLines = n;
    g_drawnFrame = s.frame;
    g_drawnFrom = s;
    LeaveCriticalSection(&g_lock);
}

} // namespace

bool Init() {
    // The game's own font, under its mission timer (D66).
    g_panel = textdraw::Create("hud_player", textdraw::TOP_LEFT, textdraw::kEdge, textdraw::kUnderTimer, 1, textdraw::C_TEXT,
                               textdraw::STYLE_NATIVE);
    textdraw::Show(g_panel, false);
    InitializeCriticalSection(&g_lock);
    cmd::RegisterCvar(&g_fdHudPlayer);
    cmd::SetPracticeOnly("fd_hud");
    binds::BindDefault("h", "toggle fd_hud");  // D37
    cmd::SetLabel("fd_hud", "Player HUD");
    hooks::OnFrameEnd(Draw);
#ifdef FDCORE_DEBUG
    testpipe::Register("hud", [](const char*, char* resp, int cap) { LastDrawn(resp, cap); });
#endif
    log::Info("hud: ready (fd_hud)");
    return true;
}

int LastDrawn(char* out, int cap) {
    EnterCriticalSection(&g_lock);
    int o = wsprintfA(out, "frame=%lu", static_cast<unsigned long>(g_drawnFrame));
    for (int i = 0; i < g_nLines && o + kLineLen + 2 < cap; ++i) o += wsprintfA(out + o, "|%s", g_lines[i]);
    // The same-frame source values, 3 decimals: tests check drawn == source.
    const state::State& s = g_drawnFrom;
    const float src[] = {s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2], s.speedXY};
    o += wsprintfA(out + o, "|src");
    for (float v : src) {
        long m = static_cast<long>(v * 1000.0f + (v >= 0 ? 0.5f : -0.5f));
        long a = m < 0 ? -m : m;
        o += wsprintfA(out + o, " %s%ld.%03ld", m < 0 ? "-" : "", a / 1000, a % 1000);
    }
    o += wsprintfA(out + o, " %ld %ld %ld %ld", static_cast<long>(s.moveState), static_cast<long>(s.gameState),
                   static_cast<long>(s.menuScreen), static_cast<long>(s.boxTop));  // + box top (stance)
    LeaveCriticalSection(&g_lock);
    return o;
}

} // namespace fdcore::hud

FDCORE_MODULE(hud, "hud", nullptr, 0, nullptr, &fdcore::hud::Init, 40)
