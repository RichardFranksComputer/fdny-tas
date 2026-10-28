// Tests: tests/game/fire_juggle.yaml tests/game/keys_default.yaml tests/unit/test_juggle.cpp
// User: Fire juggling -- an arrow showing where a carried survivor lands if you pass out now.
// juggle feature (owner request 2026-10-22, docs/evidence/fire_juggle_overlay): the
// fire-juggling helper. When the player passes out (fire or O2) while carrying a
// survivor, Verb_Reset copies the "last safe spot" player.f_safe (+0x3400) into the
// player, and DropAtPlayer puts the survivor there, -30 on X, on the floor below
// (research.db KB fire-juggling; docs/evidence/fire_juggling).
//
// fd_juggle 1 shows, 30 px below the screen centre with no background:
//   "-"          the spot is following the player;
//   an arrow     the spot is frozen: points at the landing point T = (S.x - 30, S.y, S.z),
//                relative to the view (up = straight ahead), 8 directions, with the
//                horizontal distance below ("120u", plus " ^" / " v" when T is over 48 u
//                above / below);
//   "o"          frozen and within 16 u.
// Following vs frozen is observed, not predicted (owner, D32): when Think writes the spot
// it copies the position the player had at the end of the previous frame, so
//   following = this frame's spot == last frame's position.
// No conditions (burn, O2, airborne, ...) are evaluated: players read those off the
// overlay themselves. Standing still, a frozen spot under your feet looks like following;
// the survivor lands at your feet either way. A write to anywhere else (e.g. the respawn
// mirroring the checkpoint) is logged as "written elsewhere".
// Read in OnFrameEnd, after Think and that frame's fire damage (round2_frame_order.txt).
// View yaw (live, docs/evidence/fire_juggle_overlay): forward = (sin yaw, cos yaw)
// in world (x, z), strafe right = (-cos yaw, sin yaw).
#include <windows.h>

#include "addr.h"
#include "binds.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "juggle_math.h"
#include "log.h"
#include "modules.h"
#include "state.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::juggle {
namespace {

using addr::Id;

constexpr float kDropDx = -30.0f;     // Verb_Reset: the survivor ends 30 u to -X of the spot
constexpr float kFloorStep = 48.0f;   // SnapToFloor lifts 48 u: beyond that it's another floor

// Directions 0-7 and kDirNear (juggle_math.h), then "-" (not locked).
constexpr int kDirNone = -1;
#ifdef FDCORE_DEBUG
constexpr int32_t kPassedOut = 6;  // player.f_state (test hook fd_dbg_tofire)
const char* const kDirName[] = {"up", "up-right", "right", "down-right", "down", "down-left", "left", "up-left"};
#endif
// Two lines of three characters each, ASCII only.
const char* const kGlyph[8][2] = {
    {" ^ ", " | "},    // up
    {"  ^", " / "},    // up-right
    {"-->", "   "},    // right
    {" \\ ", "  v"},   // down-right
    {" | ", " v "},    // down
    {" / ", "v  "},    // down-left
    {"<--", "   "},    // left
    {"^  ", " \\ "},   // up-left
};

struct View {  // what the panel and the pipe show (game thread writes, pipe thread reads)
    bool on, valid, locked;  // locked = frozen: the spot isn't last frame's position
    float safe[3], target[3], pos[3];
    float dist, dy, burn, o2;  // burn .. ground: logged and piped only, never evaluated
    uint32_t flinch;
    int32_t state;
    uint8_t ground;
    int dir;
    bool frozen;         // locked and the spot is still where it was when the lock began
    uint32_t locks;      // following -> frozen transitions
    uint32_t elsewhere;  // frames the spot was written to somewhere other than last frame's position
};
View g_view = {};
CRITICAL_SECTION g_lock;
int g_panel = -1;

// Previous frame.
float g_prevSafe[3] = {}, g_prevPos[3] = {};
bool g_prevValid = false;
uintptr_t g_prevPlayer = 0;
float g_lockSafe[3] = {};  // the spot when the current lock began

bool Same(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

bool On(const cmd::Cvar& v) { return v.value[0] != '0'; }
cmd::Cvar g_fdJuggle = {"fd_juggle", "0",
                        "1 = fire-juggling arrow: where a carried survivor lands if you pass out now; 0 = off", false, true,
                        nullptr, {0}};

float Sqrt(float x) {
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
void SinCos(float deg, float* s, float* c) {
    const float rad = deg * 0.017453292f;
    __asm__("fsincos" : "=t"(*c), "=u"(*s) : "0"(rad));
}

template <typename T>
bool At(uintptr_t base, Id field, T* out) {
    return addr::SafeRead(base + addr::Field(field), out, sizeof(T));
}

uintptr_t Player() {
    uintptr_t slot;
    uint32_t player = 0;
    if (addr::Resolve(Id::player_object, &slot)) addr::SafeRead(slot, &player, 4);
    return player;
}

long Round(float v) { return static_cast<long>(v + (v >= 0 ? 0.5f : -0.5f)); }

// "-123.4" (wsprintf has no %f), into out; returns out.
const char* F1(char* out, float v) {
    const long m = Round(v * 10.0f), a = m < 0 ? -m : m;
    wsprintfA(out, "%s%ld.%ld", m < 0 ? "-" : "", a / 10, a % 10);
    return out;
}

void Draw(const View& v) {
    if (g_panel < 0) return;
    if (!v.on || !v.valid) {
        textdraw::Show(g_panel, false);
        return;
    }
    char dist[24] = "", spot[48] = "";
    const char* lines[4];
    int n = 0;
    if (v.locked) {  // the frozen spot's coordinates above the arrow (owner 2026-10-23)
        char a[16], b[16], c[16];
        wsprintfA(spot, "%s %s %s", F1(a, v.safe[0]), F1(b, v.safe[1]), F1(c, v.safe[2]));
        lines[n++] = spot;
    }
    if (!v.locked) {
        lines[n++] = " - ";
    } else if (v.dir == kDirNear) {
        lines[n++] = " o ";
    } else {
        lines[n++] = kGlyph[v.dir][0];
        lines[n++] = kGlyph[v.dir][1];
    }
    if (v.locked) {
        wsprintfA(dist, "%ldu%s", Round(v.dist), v.dy > kFloorStep ? " ^" : v.dy < -kFloorStep ? " v" : "");
        lines[n++] = dist;
    }
    // Centre every line over the widest (monospace grid, panel centred); the glyph lines are
    // all 3 columns, so the arrow keeps its shape.
    static char pad[4][64];
    int width = 3;
    for (int i = 0; i < n; ++i)
        if (lstrlenA(lines[i]) > width) width = lstrlenA(lines[i]);
    for (int i = 0; i < n; ++i) {
        const int len = lstrlenA(lines[i]);
        const int left = (width - len) / 2;
        int o = 0;
        for (; o < left; ++o) pad[i][o] = ' ';
        lstrcpynA(pad[i] + o, lines[i], 64 - o);
        lines[i] = pad[i];
    }
    textdraw::SetLines(g_panel, lines, n);
    textdraw::Show(g_panel, true);
}

#ifdef FDCORE_DEBUG
void ApplySetPos();
void ApplyO2();
void StepToFire();
#endif

void FrameEnd() {
#ifdef FDCORE_DEBUG
    ApplySetPos();
    ApplyO2();
    StepToFire();
#endif
    const state::State& s = state::Current();
    View v = {};
    v.on = On(g_fdJuggle);
    v.dir = kDirNone;
    const uintptr_t pl = s.gameState == 1 && s.playerValid ? Player() : 0;
    bool elsewhere = false;
    if (pl && At(pl, Id::player_f_safe, &v.safe) && At(pl, Id::player_f_pos, &v.pos) &&
        At(pl, Id::player_f_burn, &v.burn) && At(pl, Id::player_f_o2, &v.o2) &&
        At(pl, Id::player_f_flinch, &v.flinch) && At(pl, Id::player_f_state, &v.state) &&
        At(pl, Id::player_f_ground, &v.ground)) {
        v.valid = true;
        // Observed: Think's write copies last frame's position. A new player object (level
        // load) starts as following.
        const bool known = g_prevValid && pl == g_prevPlayer;
        v.locked = known && !Same(v.safe, g_prevPos);
        elsewhere = v.locked && !Same(v.safe, g_prevSafe);
        v.target[0] = v.safe[0] + kDropDx, v.target[1] = v.safe[1], v.target[2] = v.safe[2];
        const float dx = v.target[0] - v.pos[0], dz = v.target[2] - v.pos[2];
        v.dist = Sqrt(dx * dx + dz * dz);
        v.dy = v.target[1] - v.pos[1];
        if (v.locked) {
            float sn, cs;
            SinCos(s.yaw, &sn, &cs);
            v.dir = Direction(dx, dz, v.dist, sn, cs);
        }
    }
    // A write that isn't Think's copy of last frame's position: log it with the fields, so
    // an unknown writer shows up (the respawn's checkpoint mirror is one known case).
    if (elsewhere && v.on)
        log::Info("juggle: spot written elsewhere (%ld %ld %ld -> %ld %ld %ld; burn %ld/1000, o2 %ld/100, flinch %lx, "
                  "state %ld, ground %d)",
                  Round(g_prevSafe[0]), Round(g_prevSafe[1]), Round(g_prevSafe[2]), Round(v.safe[0]),
                  Round(v.safe[1]), Round(v.safe[2]), Round(v.burn * 1000), Round(v.o2 * 100),
                  static_cast<unsigned long>(v.flinch), static_cast<long>(v.state), v.ground);
    g_prevValid = v.valid, g_prevPlayer = pl;
    __builtin_memcpy(g_prevSafe, v.safe, sizeof(g_prevSafe));
    __builtin_memcpy(g_prevPos, v.pos, sizeof(g_prevPos));

    EnterCriticalSection(&g_lock);
    if (v.locked && (!g_view.locked || elsewhere)) __builtin_memcpy(g_lockSafe, v.safe, sizeof(g_lockSafe));
    v.frozen = v.locked && Same(v.safe, g_lockSafe);
    v.locks = g_view.locks + (v.locked && !g_view.locked ? 1 : 0);
    v.elsewhere = g_view.elsewhere + (elsewhere ? 1 : 0);
    // The fields at each transition, for players working out what froze or freed the spot.
    if (v.locked != g_view.locked && v.on && v.valid)
        log::Info("juggle: %s (burn %ld/1000, o2 %ld/100, flinch %lx, state %ld, ground %d)",
                  v.locked ? "frozen" : "following", Round(v.burn * 1000), Round(v.o2 * 100),
                  static_cast<unsigned long>(v.flinch), static_cast<long>(v.state), v.ground);
    g_view = v;
    LeaveCriticalSection(&g_lock);
    Draw(v);
}

void OnToggle(cmd::Cvar& c) { log::Info("juggle: fd_juggle %s", c.value); }

#ifdef FDCORE_DEBUG
// Test hook (Debug builds only): `fd_dbg_setpos x y z [yaw [o2]]` moves the player at the
// next frame end, like fd_tp_load (position, zeroed motion; D18), so tests can stand in a
// fire; o2 (0..1) sets the player's O2 in the same frame (O2 <= 0.5 is a lock; it refills
// at about 0.1/s on clean ground).
struct SetPos {
    bool pending, yawToo, o2Too;
    float pos[3], yaw, o2;
};
SetPos g_setPos = {};

bool ParseFloat(const char* s, float* out) {
    float sign = 1.0f, v = 0.0f, scale = 0.1f;
    if (*s == '-') sign = -1.0f, ++s;
    else if (*s == '+') ++s;
    bool any = false;
    while (*s >= '0' && *s <= '9') v = v * 10.0f + static_cast<float>(*s++ - '0'), any = true;
    if (*s == '.') {
        ++s;
        while (*s >= '0' && *s <= '9') v += static_cast<float>(*s++ - '0') * scale, scale *= 0.1f, any = true;
    }
    if (!any || *s) return false;
    *out = sign * v;
    return true;
}

void CmdSetPos(int argc, const char* const* argv) {
    SetPos p = {};
    if (argc < 4 || !ParseFloat(argv[1], &p.pos[0]) || !ParseFloat(argv[2], &p.pos[1]) ||
        !ParseFloat(argv[3], &p.pos[2]) || (argc > 4 && !ParseFloat(argv[4], &p.yaw)) ||
        (argc > 5 && !ParseFloat(argv[5], &p.o2))) {
        console::Print("usage: fd_dbg_setpos x y z [yaw [o2]]");
        return;
    }
    p.yawToo = argc > 4, p.o2Too = argc > 5, p.pending = true;
    g_setPos = p;
}

// Test hook: `fd_dbg_o2 <0..1>` sets the player's O2. O2 <= 0.5 is one of the game's own
// safe-spot locks, so a test can hold a lock on clean ground without fire. Applied at
// frame end before the spot is read (like fd_dbg_setpos), so the frame a test changes it
// is well defined (suite run s0180: a mid-frame console write raced Think).
struct SetO2 {
    bool pending;
    float o2;
};
SetO2 g_setO2 = {};

void CmdO2(int argc, const char* const* argv) {
    float o2;
    if (argc < 2 || !ParseFloat(argv[1], &o2)) {
        console::Print("usage: fd_dbg_o2 <0..1> (in a level)");
        return;
    }
    g_setO2.o2 = o2, g_setO2.pending = true;
}

void ApplyO2() {
    if (!g_setO2.pending) return;
    g_setO2.pending = false;
    const uintptr_t pl = Player();
    const bool ok = pl && addr::SafeWrite(pl + addr::Field(Id::player_f_o2), &g_setO2.o2, 4);
    log::Info("juggle: o2 %ld/100: %s", Round(g_setO2.o2 * 100), ok ? "ok" : "failed");
}

// Test hook: `fd_dbg_tofire [burn]` stands the player in a fire that really burns: it tries
// the level's FireRescueEnt nodes (fire.center) one after another until the burn meter
// rises, stays until burn >= the target (default 0.5), then returns to where it started.
// A target >= 1 stays until the player passes out (state 6). Fire nodes move along their
// paths, so fixed coordinates don't reliably burn (it3 lesson).
struct ToFire {
    int phase;  // 0 idle, 1 trying a node, 2 burning
    float target, home[3], startBurn;
    int node, waited, stall;
    float lastBurn;
};
ToFire g_toFire = {};
constexpr int kTryFrames = 30, kStallFrames = 90, kMaxNodes = 256;

bool Teleport(uintptr_t pl, const float* p) {
    static const float zero[3] = {0, 0, 0};
    return addr::SafeWrite(pl + addr::Field(Id::player_f_pos), p, 12) &&
           addr::SafeWrite(pl + addr::Field(Id::player_f_vel), zero, 12) &&
           addr::SafeWrite(pl + addr::Field(Id::player_f_disp), zero, 12) &&
           addr::SafeWrite(pl + addr::Field(Id::player_f_impulse), zero, 12);
}

// The n-th fire node with a non-zero centre, or false when there are no more.
bool FireNode(int n, float* centre) {
    uintptr_t head = 0;
    uint32_t node = 0;
    if (!addr::Resolve(Id::entities_head, &head) || !addr::SafeRead(head, &node, 4)) return false;
    for (int guard = 0; node && guard < 8192; ++guard) {
        uint32_t ent = 0, next = 0;
        int32_t kind = 0;
        if (!At(node, Id::node_elem, &ent) || !At(node, Id::node_next, &next)) return false;
        node = next;
        if (!ent || !At(ent, Id::entity_kind, &kind) || kind != 6) continue;
        float c[3];
        if (!At(ent, Id::fire_center, &c) || (c[0] == 0 && c[1] == 0 && c[2] == 0)) continue;
        if (n-- == 0) {
            __builtin_memcpy(centre, c, sizeof(c));
            return true;
        }
    }
    return false;
}

void CmdToFire(int argc, const char* const* argv) {
    ToFire t = {};
    t.target = 0.5f;
    const uintptr_t pl = Player();
    if ((argc > 1 && !ParseFloat(argv[1], &t.target)) || !pl || !At(pl, Id::player_f_pos, &t.home)) {
        console::Print("usage: fd_dbg_tofire [burn target, >= 1 = until passed out] (in a level)");
        return;
    }
    t.phase = 1, t.node = -1, t.waited = kTryFrames;  // the first step moves to node 0
    g_toFire = t;
    log::Info("juggle: tofire: searching (target %ld/1000)", Round(t.target * 1000));
}

void StepToFire() {
    ToFire& t = g_toFire;
    if (!t.phase) return;
    const uintptr_t pl = Player();
    float burn = 0;
    int32_t st = 0;
    if (!pl || !At(pl, Id::player_f_burn, &burn) || !At(pl, Id::player_f_state, &st)) {
        t.phase = 0;
        log::Warn("juggle: tofire: no player");
        return;
    }
    if (t.phase == 1) {
        if (t.waited < kTryFrames) {
            if (burn > t.startBurn + 0.02f) {
                t.phase = 2, t.stall = 0, t.lastBurn = burn;
                log::Info("juggle: tofire: node %d burns (burn %ld/1000)", t.node, Round(burn * 1000));
            } else {
                ++t.waited;
            }
            return;
        }
        float c[3];
        if (++t.node >= kMaxNodes || !FireNode(t.node, c)) {
            t.phase = 0;
            Teleport(pl, t.home);
            log::Warn("juggle: tofire: no fire node burns (tried %d)", t.node);
            return;
        }
        c[1] += 30.0f;  // above the centre, as in the spike
        Teleport(pl, c);
        t.waited = 0, t.startBurn = burn;
        return;
    }
    // phase 2: burning
    if (t.target >= 1.0f ? st == kPassedOut : burn >= t.target) {
        t.phase = 0;
        if (t.target < 1.0f) Teleport(pl, t.home);
        log::Info("juggle: tofire: done (burn %ld/1000, state %ld)%s", Round(burn * 1000), static_cast<long>(st),
                  t.target < 1.0f ? ", back where it started" : "");
        return;
    }
    if (burn > t.lastBurn) t.lastBurn = burn, t.stall = 0;
    else if (++t.stall > kStallFrames) t.phase = 1, t.waited = kTryFrames;  // node moved on: next one
}

void ApplySetPos() {
    if (!g_setPos.pending) return;
    g_setPos.pending = false;
    const uintptr_t pl = Player();
    static const float zero[3] = {0, 0, 0};
    const bool ok = pl && addr::SafeWrite(pl + addr::Field(Id::player_f_pos), g_setPos.pos, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_vel), zero, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_disp), zero, 12) &&
                    addr::SafeWrite(pl + addr::Field(Id::player_f_impulse), zero, 12) &&
                    (!g_setPos.yawToo || addr::SafeWrite(pl + addr::Field(Id::player_f_yaw), &g_setPos.yaw, 4)) &&
                    (!g_setPos.o2Too || addr::SafeWrite(pl + addr::Field(Id::player_f_o2), &g_setPos.o2, 4));
    log::Info("juggle: setpos %ld %ld %ld%s%s: %s", Round(g_setPos.pos[0]), Round(g_setPos.pos[1]),
              Round(g_setPos.pos[2]), g_setPos.yawToo ? " (+yaw)" : "", g_setPos.o2Too ? " (+o2)" : "",
              ok ? "ok" : "failed");
}
#endif

const Id kNeeds[] = {Id::player_object};

bool Init() {
    InitializeCriticalSection(&g_lock);
    g_fdJuggle.onChange = OnToggle;
    cmd::RegisterCvar(&g_fdJuggle);
    cmd::SetPracticeOnly("fd_juggle");
    binds::BindDefault("j", "toggle fd_juggle");  // D37
    g_panel = textdraw::Create("juggle", textdraw::CENTER, 0, 30, 1, textdraw::C_WHITE | textdraw::A_BOLD,
                               textdraw::STYLE_TEXT_ONLY);
    if (g_panel >= 0) textdraw::Show(g_panel, false), textdraw::HideWhilePaused(g_panel, true);  // D35
    hooks::OnFrameEnd(FrameEnd);
#ifdef FDCORE_DEBUG
    cmd::RegisterCommand("fd_dbg_setpos", "(Debug test hook) move the player: x y z [yaw [o2]]", CmdSetPos);
    cmd::SetPracticeOnly("fd_dbg_setpos");
    cmd::RegisterCommand("fd_dbg_o2", "(Debug test hook) set the player's O2, 0..1", CmdO2);
    cmd::SetPracticeOnly("fd_dbg_o2");
    cmd::RegisterCommand("fd_dbg_tofire", "(Debug test hook) stand in a burning fire: [burn target, >= 1 = pass out]",
                         CmdToFire);
    cmd::SetPracticeOnly("fd_dbg_tofire");
    testpipe::Register("juggle", [](const char*, char* resp, int cap) {
        (void)cap;
        EnterCriticalSection(&g_lock);
        const View v = g_view;
        LeaveCriticalSection(&g_lock);
        // Tenths of a unit (wsprintf has no %f).
        wsprintfA(resp,
                  "enabled=%d valid=%d locked=%d dir=%d glyph=%s dist=%ld dy=%ld safe=%ld,%ld,%ld target=%ld,%ld,%ld "
                  "pos=%ld,%ld,%ld burn=%ld o2=%ld flinch=%lx state=%ld ground=%d frozen=%d locks=%lu elsewhere=%lu",
                  v.on, v.valid, v.locked, v.dir,
                  !v.locked ? "-" : v.dir == kDirNear ? "o" : v.dir >= 0 ? kDirName[v.dir] : "?", Round(v.dist * 10),
                  Round(v.dy * 10), Round(v.safe[0] * 10), Round(v.safe[1] * 10), Round(v.safe[2] * 10),
                  Round(v.target[0] * 10), Round(v.target[1] * 10), Round(v.target[2] * 10), Round(v.pos[0] * 10),
                  Round(v.pos[1] * 10), Round(v.pos[2] * 10), Round(v.burn * 1000), Round(v.o2 * 1000),
                  static_cast<unsigned long>(v.flinch), static_cast<long>(v.state), v.ground, v.frozen,
                  static_cast<unsigned long>(v.locks), static_cast<unsigned long>(v.elsewhere));
    });
#endif
    log::Info("juggle: ready (fd_juggle%s)", g_panel < 0 ? "; no text panel free: display off" : "");
    return true;
}

} // namespace
} // namespace fdcore::juggle

// id, log name, required Ids, count, DllMain-phase step (or nullptr), init, start order
FDCORE_MODULE(juggle, "juggle", fdcore::juggle::kNeeds, 1, nullptr, &fdcore::juggle::Init, 75)
