// Tests: tests/freecam.yaml tests/keys_default.yaml
// User: Pause and free camera -- freeze the game and fly a camera around; the view returns to the player on resume.
// freecam feature (owner request 2026-10-22, core/evidence/freecam): `fd_pause` freezes
// the game (core `pause` service: the simulation step is skipped and the game clock
// paused) and gives a free-flying camera until it is turned off; then the view snaps
// back to the player. Nothing in the game moves while paused, so nothing is restored.
//
// The camera is FDNYCore's own position + yaw/pitch, handed to the `camera` service as a
// world-transform override (applied at geEngine_RenderWorld, D34). Start pose: the game's
// last camera transform. Genesis camera transforms: columns right / up / back, so the
// view direction is -column 2 (geCamera_Project: forward depth = -z); yaw follows the
// measured player convention (forward = (sin yaw, cos yaw) in world x, z; up = +y). The
// rebuild error of the start pose against the game's own matrix is in the pipe.
//
// Each paused frame (frame begin, real time from QueryPerformanceCounter, not the paused
// game clock): W/S along the view (pitch included), A/D strafe, E/Q straight up/down;
// keys add, no acceleration; speed fd_freecam_speed (400 u/s, the run speed), Shift x4,
// Ctrl x0.25. Mouse: the game's own per-frame sums (controls.mouse_x/y; DirectInput,
// still polled while paused) x fd_freecam_sens degrees (0.075; owner: 0.15 was too fast); pitch kept within +-89. Keys only
// act while binds::KeysLive (game focused, console closed). No collision.
#include <windows.h>

#include "addr.h"
#include "binds.h"
#include "camera.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "pause.h"
#include "state.h"
#include "testpipe.h"

namespace fdcore::freecam {
namespace {

using addr::Id;

constexpr float kDeg = 0.017453292f;
constexpr float kPitchMax = 89.0f;
constexpr float kMaxStep = 0.1f;  // s; a hitch never throws the camera far

struct Cam {
    float pos[3];
    float yaw, pitch;  // degrees; yaw like player.f_yaw, pitch > 0 = up
};
Cam g_cam = {};
bool g_flying = false;
float g_rebuildErr = -1.0f;  // max |game - rebuilt| over the 9 rotation cells at the start
float g_moved = 0.0f;        // camera distance flown this pause
uint32_t g_keyMask = 0;      // keys seen last frame (pipe)
// Test measurements since the last `freecam reset` (Debug pipe): time with a move key held,
// distance flown then, the displacement, mouse turn; the first frame delta after a resume.
float g_keyTime = 0.0f, g_segDist = 0.0f, g_segDisp[3] = {}, g_segFwd[3] = {}, g_turn = 0.0f;
float g_start[3] = {};
float g_resumeDt = -1.0f;
bool g_wantResumeDt = false;
LARGE_INTEGER g_freq, g_last;
bool g_syncing = false;      // fd_pause set by the module itself

float Sqrt(float x) {
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
void SinCos(float deg, float* s, float* c) {
    const float rad = deg * kDeg;
    __asm__("fsincos" : "=t"(*c), "=u"(*s) : "0"(rad));
}
float Atan2Deg(float y, float x) {  // x87 fpatan: atan(y / x) with the quadrant
    float r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r / kDeg;
}
float Abs(float v) { return v < 0 ? -v : v; }

// Rotation + translation of a camera at `c` (see the header for the axes).
void Build(const Cam& c, camera::XForm* x) {
    float sy, cy, sp, cp;
    SinCos(c.yaw, &sy, &cy);
    SinCos(c.pitch, &sp, &cp);
    const float f[3] = {sy * cp, sp, cy * cp};  // forward
    const float r[3] = {-cy, 0.0f, sy};         // strafe right (measured, yaw_probe.log)
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};  // r x f
    for (int i = 0; i < 3; ++i) x->m[i * 3 + 0] = r[i], x->m[i * 3 + 1] = u[i], x->m[i * 3 + 2] = -f[i];
    for (int i = 0; i < 3; ++i) x->t[i] = c.pos[i];
}

bool StartFlying() {
    camera::XForm game;
    if (!camera::LastWorldXForm(&game)) return false;
    const float f[3] = {-game.m[2], -game.m[5], -game.m[8]};  // -column 2
    g_cam.yaw = Atan2Deg(f[0], f[2]);
    g_cam.pitch = Atan2Deg(f[1], Sqrt(f[0] * f[0] + f[2] * f[2]));
    for (int i = 0; i < 3; ++i) g_cam.pos[i] = game.t[i], g_start[i] = game.t[i];
    camera::XForm rebuilt;
    Build(g_cam, &rebuilt);
    g_rebuildErr = 0.0f;
    for (int i = 0; i < 9; ++i)
        if (const float d = Abs(rebuilt.m[i] - game.m[i]); d > g_rebuildErr) g_rebuildErr = d;
    g_moved = 0.0f;
    QueryPerformanceCounter(&g_last);
    camera::SetWorldOverride(&rebuilt);
    g_flying = true;
    log::Info("freecam: flying from %ld %ld %ld, yaw %ld pitch %ld (rebuild error %ld/1000)",
              static_cast<long>(g_cam.pos[0]), static_cast<long>(g_cam.pos[1]), static_cast<long>(g_cam.pos[2]),
              static_cast<long>(g_cam.yaw), static_cast<long>(g_cam.pitch), static_cast<long>(g_rebuildErr * 1000));
    return true;
}

void StopFlying() {
    if (!g_flying) return;
    g_wantResumeDt = true, g_resumeDt = -1.0f;
    camera::SetWorldOverride(nullptr);
    g_flying = false;
    log::Info("freecam: camera back to the player (flew %ld u)", static_cast<long>(g_moved));
}

float CvarF(const cmd::Cvar& v, float def) {
    float sign = 1.0f, r = 0.0f, scale = 0.1f;
    const char* s = v.value;
    if (*s == '-') sign = -1.0f, ++s;
    bool any = false;
    while (*s >= '0' && *s <= '9') r = r * 10.0f + static_cast<float>(*s++ - '0'), any = true;
    if (*s == '.')
        for (++s; *s >= '0' && *s <= '9'; scale *= 0.1f) r += static_cast<float>(*s++ - '0') * scale, any = true;
    return any ? sign * r : def;
}

void OnPause(cmd::Cvar& v);
cmd::Cvar g_fdPause = {"fd_pause", "0", "1 = freeze the game and fly a free camera (0 = resume, view back to the player)",
                       false, false, OnPause, {0}};
cmd::Cvar g_fdSpeed = {"fd_freecam_speed", "400", "free camera speed in units/s (400 = the run speed); Shift x4, Ctrl x0.25", false, true,
                       nullptr, {0}};
cmd::Cvar g_fdSens = {"fd_freecam_sens", "0.075", "free camera mouse look in degrees per mouse count; higher = faster", false, true,
                      nullptr, {0}};

void SetPauseVar(const char* v) {
    g_syncing = true;
    cmd::SetCvarUngated(&g_fdPause, v);
    g_syncing = false;
}

void OnPause(cmd::Cvar& v) {
    if (g_syncing) return;
    const bool on = v.value[0] != '0';
    if (on) {
        if (!pause::Set(true, "fd_pause")) {
            console::Print("fd_pause: only inside a level");
            SetPauseVar("0");
            return;
        }
        if (!StartFlying()) console::Print("fd_pause: paused; no camera pose yet, free camera off");
    } else {
        StopFlying();
        pause::Set(false, "fd_pause 0");
    }
}

void CmdToggle(int, const char* const*) { cmd::SetCvar(&g_fdPause, g_fdPause.value[0] == '0' ? "1" : "0"); }

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void FrameBegin() {
    // The service ends a pause on its own (level change, menu): follow it.
    if (!pause::Active()) {
        if (g_fdPause.value[0] != '0') StopFlying(), SetPauseVar("0");
        if (g_wantResumeDt && !g_flying) {
            // The first running frame after a resume: its delta (clock.dt, set by this frame's Tick).
            float dtGame = -1.0f;
            if (addr::ReadF32(Id::clock_dt, &dtGame)) g_resumeDt = dtGame, g_wantResumeDt = false;
        }
        return;
    }
    if (!g_flying) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(static_cast<double>(now.QuadPart - g_last.QuadPart) / static_cast<double>(g_freq.QuadPart));
    g_last = now;
    if (dt > kMaxStep) dt = kMaxStep;
    if (dt < 0) dt = 0;

    // Mouse: this frame's sums from the game's own DirectInput poll.
    int32_t mx = 0, my = 0;
    addr::ReadI32(Id::controls_mouse_x, &mx);
    addr::ReadI32(Id::controls_mouse_y, &my);
    const float sens = CvarF(g_fdSens, 0.075f);
    if (binds::KeysLive() && !binds::PointerCaptured()) {
        // Yaw grows to the left (forward (sin, cos), right (-cos, sin)): mouse right = yaw down
        // (owner 2026-10-22: left and right were swapped). g_turn counts degrees to the right.
        g_turn += static_cast<float>(mx) * sens;
        g_cam.yaw -= static_cast<float>(mx) * sens;
        g_cam.pitch -= static_cast<float>(my) * sens;
    }
    if (g_cam.pitch > kPitchMax) g_cam.pitch = kPitchMax;
    if (g_cam.pitch < -kPitchMax) g_cam.pitch = -kPitchMax;
    while (g_cam.yaw >= 360.0f) g_cam.yaw -= 360.0f;
    while (g_cam.yaw < 0.0f) g_cam.yaw += 360.0f;

    // Keys: W/S along the view, A/D strafe, E/Q world up/down; added, then scaled.
    float d[3] = {0, 0, 0};
    uint32_t mask = 0;
    if (binds::KeysLive()) {
        float sy, cy, sp, cp;
        SinCos(g_cam.yaw, &sy, &cy);
        SinCos(g_cam.pitch, &sp, &cp);
        const float f[3] = {sy * cp, sp, cy * cp}, r[3] = {-cy, 0.0f, sy};
        const struct {
            int vk;
            const float* dir;
            float sign;
            uint32_t bit;
        } keys[] = {{'W', f, 1, 1}, {'S', f, -1, 2}, {'D', r, 1, 4}, {'A', r, -1, 8}};
        for (const auto& k : keys)
            if (Down(k.vk) && !binds::KeyCaptured(k.vk))
                for (int i = 0; i < 3; ++i) d[i] += k.dir[i] * k.sign, mask |= k.bit;
        if (Down('E') && !binds::KeyCaptured('E')) d[1] += 1.0f, mask |= 16;
        if (Down('Q') && !binds::KeyCaptured('Q')) d[1] -= 1.0f, mask |= 32;
        float speed = CvarF(g_fdSpeed, 400.0f);
        if (Down(VK_SHIFT)) speed *= 4.0f, mask |= 64;
        if (Down(VK_CONTROL)) speed *= 0.25f, mask |= 128;
        for (int i = 0; i < 3; ++i) d[i] *= speed * dt;
        if (mask & 63) {
            g_keyTime += dt;
            for (int i = 0; i < 3; ++i) g_segDisp[i] += d[i], g_segFwd[i] = f[i];
            g_segDist += Sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        }
    }
    g_keyMask = mask;
    g_moved += Sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) g_cam.pos[i] += d[i];
    camera::XForm x;
    Build(g_cam, &x);
    camera::SetWorldOverride(&x);
}

#ifdef FDCORE_DEBUG
long Tenths(float v) { return static_cast<long>(v * 10.0f + (v >= 0 ? 0.5f : -0.5f)); }

// Victims: positions (tenths) and states folded into one number.
long VictimSum(const state::State& s) {
    long v = 0;
    for (int i = 0; i < s.victimCount && i < state::kMaxVictims; ++i)
        v += (Tenths(s.victims[i].pos[0]) + 3 * Tenths(s.victims[i].pos[1]) + 7 * Tenths(s.victims[i].pos[2])) * (i + 1) +
             s.victims[i].state * 1000003L;
    return v;
}

// State that must not change while paused, for the test: a sum over the fire entities
// (centre, intensity, mode) and the player's burn / O2.
void FireSums(float* centre, float* intensity, long* modes, int* count) {
    *centre = *intensity = 0, *modes = 0, *count = 0;
    uintptr_t head = 0;
    uint32_t node = 0;
    if (!addr::Resolve(Id::entities_head, &head) || !addr::SafeRead(head, &node, 4)) return;
    for (int guard = 0; node && guard < 8192; ++guard) {
        uint32_t ent = 0, next = 0;
        int32_t kind = 0, mode = 0;
        float c[3] = {}, in = 0;
        if (!addr::SafeRead(node + addr::Field(Id::node_elem), &ent, 4) ||
            !addr::SafeRead(node + addr::Field(Id::node_next), &next, 4))
            return;
        node = next;
        if (!ent || !addr::SafeRead(ent + addr::Field(Id::entity_kind), &kind, 4) || kind != 6) continue;
        addr::SafeRead(ent + addr::Field(Id::fire_center), c, 12);
        addr::SafeRead(ent + addr::Field(Id::fire_intensity), &in, 4);
        addr::SafeRead(ent + addr::Field(Id::fire_mode), &mode, 4);
        *centre += c[0] + c[1] + c[2], *intensity += in, *modes += mode, ++*count;
    }
}
#endif

const Id kNeeds[] = {Id::controls_mouse_x, Id::controls_mouse_y};

bool Init() {
    if (!pause::Available() || !camera::WorldOverrideAvailable()) {
        log::Warn("freecam: disabled (pause service %s, camera override %s)", pause::Available() ? "ok" : "unavailable",
                  camera::WorldOverrideAvailable() ? "ok" : "unavailable");
        return false;
    }
    QueryPerformanceFrequency(&g_freq);
    cmd::RegisterCvar(&g_fdPause);
    cmd::RegisterCvar(&g_fdSpeed);
    cmd::RegisterCvar(&g_fdSens);
    cmd::RegisterCommand("fd_pause_toggle", "pause / resume with the free camera (fd_pause; default key P)", CmdToggle);
    cmd::SetPracticeOnly("fd_pause");
    cmd::SetPracticeOnly("fd_pause_toggle");
    binds::BindDefault("p", "fd_pause_toggle");
    // Read each paused frame in the camera step (not binds: held, not pressed).
    binds::DescribeKey("w s", "free camera: forward / back along the view", "while fd_pause is 1");
    binds::DescribeKey("a d", "free camera: strafe left / right", "while fd_pause is 1");
    binds::DescribeKey("e q", "free camera: straight up / down", "while fd_pause is 1");
    binds::DescribeKey("shift", "free camera: 4x speed (held)", "while fd_pause is 1");
    binds::DescribeKey("ctrl", "free camera: 0.25x speed (held)", "while fd_pause is 1");
    binds::DescribeKey("mouse", "free camera: look (fd_freecam_sens)", "while fd_pause is 1");
    hooks::OnFrameBegin(FrameBegin);
#ifdef FDCORE_DEBUG
    testpipe::Register("freecam", [](const char* args, char* resp, int cap) {
        (void)cap;
        while (*args == ' ') ++args;
        if (args[0] == 'r') {  // reset: start a new measurement
            g_keyTime = g_segDist = g_turn = 0.0f;
            for (int i = 0; i < 3; ++i) g_segDisp[i] = g_segFwd[i] = 0.0f;
        }
        // rate = distance / key time (u/s); along = displacement . last forward / |displacement|
        const float rate = g_keyTime > 0 ? g_segDist / g_keyTime : 0.0f;
        const float dl = Sqrt(g_segDisp[0] * g_segDisp[0] + g_segDisp[1] * g_segDisp[1] + g_segDisp[2] * g_segDisp[2]);
        const float along = dl > 0 ? (g_segDisp[0] * g_segFwd[0] + g_segDisp[1] * g_segFwd[1] + g_segDisp[2] * g_segFwd[2]) / dl : 0.0f;
        camera::XForm gx;
        float gcd = -1.0f;
        if (camera::LastWorldXForm(&gx)) {
            const float e[3] = {gx.t[0] - g_start[0], gx.t[1] - g_start[1], gx.t[2] - g_start[2]};
            gcd = Sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        }
        const state::State& s = state::Current();
        uintptr_t pl = 0, slot = 0;
        if (addr::Resolve(Id::player_object, &slot)) addr::SafeRead(slot, &pl, 4);
        float burn = 0, o2 = 0, fc = 0, fi = 0;
        long fm = 0;
        int fn = 0;
        if (pl) {
            addr::SafeRead(pl + addr::Field(Id::player_f_burn), &burn, 4);
            addr::SafeRead(pl + addr::Field(Id::player_f_o2), &o2, 4);
        }
        FireSums(&fc, &fi, &fm, &fn);
        int32_t elapsed = 0;
        addr::ReadI32(Id::clock_elapsed_ms, &elapsed);
        wsprintfA(resp,
                  "paused=%d flying=%d frames=%lu cx=%ld cy=%ld cz=%ld yaw=%ld pitch=%ld moved=%ld keys=%lu "
                  "rebuild=%ld keytime=%ld rate=%ld along=%ld turn=%ld gamecam_dist=%ld resume_dt=%ld "
                  "px=%ld py=%ld pz=%ld vx=%ld vy=%ld vz=%ld burn=%ld o2=%ld time=%ld clock=%ld fires=%d "
                  "fire_c=%ld fire_i=%ld fire_m=%ld victims=%ld",
                  pause::Active(), g_flying, static_cast<unsigned long>(pause::PausedFrames()), Tenths(g_cam.pos[0]),
                  Tenths(g_cam.pos[1]), Tenths(g_cam.pos[2]), Tenths(g_cam.yaw), Tenths(g_cam.pitch), Tenths(g_moved),
                  static_cast<unsigned long>(g_keyMask), static_cast<long>(g_rebuildErr * 1000),
                  static_cast<long>(g_keyTime * 1000), Tenths(rate), static_cast<long>(along * 1000), Tenths(g_turn),
                  Tenths(gcd), static_cast<long>(g_resumeDt * 1000), Tenths(s.pos[0]), Tenths(s.pos[1]),
                  Tenths(s.pos[2]), Tenths(s.vel[0]), Tenths(s.vel[1]), Tenths(s.vel[2]), static_cast<long>(burn * 1000),
                  static_cast<long>(o2 * 1000), Tenths(s.timeLeft), static_cast<long>(elapsed), fn, Tenths(fc),
                  Tenths(fi * 100), fm, VictimSum(s));
    });
#endif
    log::Info("freecam: ready (fd_pause; key P)");
    return true;
}

} // namespace
} // namespace fdcore::freecam

// id, log name, required Ids, count, DllMain-phase step (or nullptr), init, start order
FDCORE_MODULE(freecam, "freecam", fdcore::freecam::kNeeds, 2, nullptr, &fdcore::freecam::Init, 80)
