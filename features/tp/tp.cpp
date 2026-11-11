// Tests: tests/game/tp_teleport.yaml tests/game/keys_default.yaml
// User: Teleporter -- save a position and view, and teleport back to it (same map).
#include "binds.h"
#include "tp.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "num.h"
#include "state.h"

namespace fdcore::tp {
namespace {

using addr::Id;

struct Saved {
    bool valid;
    char map[16];
    float pos[3];
    float pitch, yaw;
};
Saved g_saved{};
bool g_pending = false;

uint32_t Player() {
    uintptr_t slot;
    uint32_t player = 0;
    if (addr::Resolve(Id::player_object, &slot)) addr::SafeRead(slot, &player, 4);
    return player;
}

bool InLevel(const char* who) {
    const state::State& s = state::Current();
    if (s.gameState != 1 || !s.playerValid || !s.map[0] || !Player()) {
        console::Print("%s: not in a level", who);
        return false;
    }
    return true;
}

void Describe(char* out, const float* p) {
    char x[24], y[24], z[24];
    num::Fixed(x, p[0], 1), num::Fixed(y, p[1], 1), num::Fixed(z, p[2], 1);
    wsprintfA(out, "%s %s %s", x, y, z);
}

void CmdSave(int, const char* const*) {
    if (!InLevel("fd_tp_save")) return;
    const uint32_t pl = Player();
    Saved s{};
    if (!addr::SafeRead(pl + addr::Field(Id::player_f_pos), s.pos, 12) ||
        !addr::SafeRead(pl + addr::Field(Id::player_f_pitch), &s.pitch, 4) ||
        !addr::SafeRead(pl + addr::Field(Id::player_f_yaw), &s.yaw, 4)) {
        console::Print("fd_tp_save: could not read the player");
        return;
    }
    lstrcpynA(s.map, state::Current().map, sizeof(s.map));
    s.valid = true;
    g_saved = s;
    char where[80];
    Describe(where, s.pos);
    console::Print("fd_tp_save: saved %s on %s", where, s.map);
    log::Info("tp: saved %s on '%s'", where, s.map);
}

void CmdLoad(int, const char* const*) {
    if (!g_saved.valid) {
        console::Print("fd_tp_load: nothing saved (fd_tp_save first)");
        return;
    }
    if (!InLevel("fd_tp_load")) return;
    const char* map = state::Current().map;
    if (lstrcmpiA(map, g_saved.map) != 0) {  // map gate: the location belongs to its map
        console::Print("fd_tp_load: saved on %s, this is %s -- refused", g_saved.map, map);
        log::Info("tp: refused load: saved on '%s', current map '%s'", g_saved.map, map);
        return;
    }
    g_pending = true;  // applied at frame end (between Think passes)
}

// Frame end: after this frame's Think, before the next one.
void Apply() {
    if (!g_pending) return;
    g_pending = false;
    const uint32_t pl = Player();
    const state::State& s = state::Current();
    if (!pl || s.gameState != 1 || lstrcmpiA(s.map, g_saved.map) != 0) return;  // level changed meanwhile
    static const float zero[3] = {0, 0, 0};
    const bool ok = addr::SafeWrite(pl + addr::Field(Id::player_f_pos), g_saved.pos, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_pitch), &g_saved.pitch, 4) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_yaw), &g_saved.yaw, 4) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_vel), zero, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_disp), zero, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_impulse), zero, 12);
    char where[80];
    Describe(where, g_saved.pos);
    if (!ok) {
        log::Warn("tp: write failed");
        return;
    }
    console::Print("fd_tp_load: %s", where);
    log::Info("tp: loaded %s on '%s'", where, g_saved.map);
}

} // namespace

bool Init() {
    const Id needs[] = {Id::player_object, Id::player_f_pos, Id::player_f_pitch, Id::player_f_yaw,
                        Id::player_f_vel, Id::player_f_disp, Id::player_f_impulse};
    for (Id id : needs)
        if (!addr::Usable(id)) {
            log::Warn("tp: disabled (needs %s)", addr::Name(id));
            return false;
        }
    cmd::RegisterCommand("fd_tp_save", "save the player's position + view (practice; this map only)", CmdSave);
    cmd::RegisterCommand("fd_tp_load", "teleport to the fd_tp_save location (same map only)", CmdLoad);
    cmd::SetPracticeOnly("fd_tp_save");
    cmd::SetPracticeOnly("fd_tp_load");
    binds::BindDefault("k", "fd_tp_save");  // D37: K keep, L load
    binds::BindDefault("l", "fd_tp_load");
    hooks::OnFrameEnd(Apply);
    log::Info("tp: ready (fd_tp_save, fd_tp_load)");
    return true;
}

} // namespace fdcore::tp

FDCORE_MODULE(tp, "tp", nullptr, 0, nullptr, &fdcore::tp::Init, 60)
