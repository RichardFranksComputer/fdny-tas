// Tests: tests/game/spectator_camera.yaml tests/game/freecam.yaml tests/unit/test_spectator.cpp
// User: Flying camera -- the keys of the free camera and of the recordings viewer.
#include "spectator.h"
#include <windows.h>
#include "addr.h"
#include "binds.h"
#include "hooks.h"
#include "log.h"
#include "pause.h"
#include "spectator_math.h"
#include "state.h"
#include "testpipe.h"
namespace fdcore::spectator {
namespace {
bool g_ready, g_active, g_free, g_pending, g_ownsPause;
bool g_primed;
bool g_modal;  // the viewer (Start); the free camera (StartFree) is not modal
constexpr Flight kViewerFlight = {400, 0.15f, false};
constexpr float kPitchMax = 89;
Flight g_flight = kViewerFlight;
float g_dt;
unsigned g_keysHeld;
Target g_targets[kMaxTargets];
int g_count, g_followed;
float g_pos[3], g_yaw, g_pitch = -15, g_distance = 240;
// The keys (binds modes "camera" and "viewer"; Init registers them).
int kForward = -1, kBack = -1, kRight = -1, kLeft = -1, kUp = -1, kDown = -1, kFast = -1, kSlow = -1;
int kXray = -1, kFree = -1, kNext = -1, kPlay = -1, kSeekBack = -1, kSeekForward = -1, kRestart = -1, kExit = -1;
int kZoomIn = -1, kZoomOut = -1, kLook = -1;
ActionHandler g_actions;
LARGE_INTEGER g_frequency, g_last;
bool g_messages;  // the window-message filter is registered (wheel and key messages)
volatile LONG g_wheel;
// Only the viewer takes the mouse from other tools (its keys: the exclusive "viewer" mode).
bool ViewerActive() {
    return g_active && g_modal;
}
bool FlightActive() {
    return g_active && g_free;
}
bool CapturePointer() {
    return g_active && g_modal;
}
void Emit(Action action) {
    if (g_actions)
        g_actions(action);
}
int FirstValid() {
    for (int i = 0; i < g_count; ++i)
        if (g_targets[i].valid)
            return i;
    return -1;
}
void Pose() {
    if (!g_free) {
        if (g_followed < 0 || g_followed >= g_count || !g_targets[g_followed].valid)
            g_followed = FirstValid();
        if (g_followed >= 0)
            math::Orbit(g_targets[g_followed].pos, g_yaw, g_pitch, g_distance, g_pos);
    }
    camera::XForm x;
    math::Build(g_pos, g_yaw, g_pitch, &x);
    camera::SetWorldOverride(&x);
}
// The viewer's wheel, from the window messages binds owns; true consumes it. Its keys are
// the exclusive "viewer" mode's: binds consumes those messages itself.
bool Message(unsigned message, uintptr_t w, intptr_t) {
    if (message == WM_MOUSEWHEEL && g_active && g_modal && binds::KeysLive()) {
        InterlockedExchangeAdd(&g_wheel, static_cast<short>(HIWORD(w)));
        return true;
    }
    return false;
}
void Frame() {
    if (!g_active)
        return;
    const state::State& s = state::Current();
    if (s.gameState != 1 || !pause::Active()) {
        Stop();
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(static_cast<double>(now.QuadPart - g_last.QuadPart) /
                                  static_cast<double>(g_frequency.QuadPart));
    g_last = now;
    dt = math::Clamp(dt, 0, 0.1f);
    g_dt = dt;
    const bool live = binds::KeysLive();
    if (g_modal) {
        if (binds::Pressed(kXray))
            Emit(Action::ToggleXray);
        if (binds::Pressed(kFree))
            SetFree(!g_free);
        if (binds::Pressed(kNext) && g_count > 1) {
            for (int n = 1; n <= g_count; ++n) {
                int i = (g_followed + n) % g_count;
                if (g_targets[i].valid) {
                    Follow(i);
                    break;
                }
            }
        }
        if (binds::Pressed(kPlay))
            Emit(Action::TogglePlay);
        if (binds::Pressed(kSeekBack))
            Emit(Action::SeekBack);
        if (binds::Pressed(kSeekForward))
            Emit(Action::SeekForward);
        if (binds::Pressed(kRestart))
            Emit(Action::Restart);
        if (binds::Pressed(kExit)) {
            Emit(Action::Exit);
            if (!g_actions && g_active)
                Stop();
            return;
        }
        if (!g_active)
            return;
        const LONG wheel = InterlockedExchange(&g_wheel, 0);
        if (live && wheel)
            Zoom(static_cast<float>(wheel) / WHEEL_DELTA);
        if (binds::Held(kZoomIn))
            Zoom(dt * 5);
        if (binds::Held(kZoomOut))
            Zoom(-dt * 5);
    }
    // Look: the viewer while right-dragging; the free camera always, unless another tool
    // holds the pointer (the checkpoint editor).
    if (live && (g_flight.lookAlways || binds::Held(kLook)) && (g_modal || !binds::PointerCaptured())) {
        int32_t mx = 0, my = 0;
        addr::ReadI32(addr::Id::controls_mouse_x, &mx);
        addr::ReadI32(addr::Id::controls_mouse_y, &my);
        // Yaw grows to the left (forward (sin, cos)): mouse right turns right.
        g_yaw = math::Wrap(g_yaw - mx * g_flight.sens);
        g_pitch = math::Clamp(g_pitch - my * g_flight.sens, -kPitchMax, kPitchMax);
    }
    g_keysHeld = 0;
    if (live && g_free) {
        // Held keys (not presses): the "camera" mode. binds leaves out a key a later tool's
        // action has taken (the checkpoint editor's Ctrl+D is not "strafe right").
        const struct {
            int action;
            unsigned bit;
        } flight[] = {{kForward, 1}, {kBack, 2}, {kRight, 4}, {kLeft, 8}, {kUp, 16}, {kDown, 32}, {kFast, 64}, {kSlow, 128}};
        for (const auto& k : flight)
            if (binds::Held(k.action))
                g_keysHeld |= k.bit;
        auto held = [](unsigned bit) { return (g_keysHeld & bit) ? 1.0f : 0.0f; };
        float sy, cy, sp, cp;
        math::SinCos(g_yaw, &sy, &cy);
        math::SinCos(g_pitch, &sp, &cp);
        const float f[3] = {sy * cp, sp, cy * cp}, r[3] = {-cy, 0, sy};
        float move[3] = {};
        for (int i = 0; i < 3; ++i)
            move[i] = (held(1) - held(2)) * f[i] + (held(4) - held(8)) * r[i];
        move[1] += held(16) - held(32);
        const float norm = math::Sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
        float speed = g_flight.speed;
        if (g_keysHeld & 64)
            speed *= 4;
        if (g_keysHeld & 128)
            speed *= 0.25f;
        if (norm > 0)
            for (int i = 0; i < 3; ++i)
                g_pos[i] += move[i] / norm * speed * dt;
    }
    Pose();
}
void Level(const char*) {
    if (g_active)
        Stop();
}
} // namespace
bool Init() {
    if (g_ready)
        return true;
    if (!pause::Available() || !camera::WorldOverrideAvailable() || !addr::Usable(addr::Id::controls_mouse_x) ||
        !addr::Usable(addr::Id::controls_mouse_y)) {
        log::Warn("spectator: pause, camera or mouse input unavailable");
        return false;
    }
    QueryPerformanceFrequency(&g_frequency);
    if (!binds::AddPointerCapture(CapturePointer)) {
        log::Warn("spectator: modal input capture unavailable");
        return false;
    }
    // Flight, in the free camera and the viewer's released camera.
    binds::Mode("camera", "while the free camera flies (a paused game, or the viewer's released camera)", FlightActive, binds::M_SHARED);
    kForward = binds::ActionHeld("camera", "forward", "w", "fly forward along the view");
    kBack = binds::ActionHeld("camera", "back", "s", "fly back");
    kLeft = binds::ActionHeld("camera", "left", "a", "strafe left");
    kRight = binds::ActionHeld("camera", "right", "d", "strafe right");
    kUp = binds::ActionHeld("camera", "up", "e", "straight up");
    kDown = binds::ActionHeld("camera", "down", "q", "straight down");
    kFast = binds::ActionHeld("camera", "fast", "shift", "4x speed");
    kSlow = binds::ActionHeld("camera", "slow", "ctrl", "0.25x speed");
    // The recordings viewer: it takes the keyboard (ordinary binds and the game get none).
    binds::Mode("viewer", "in the recordings viewer", ViewerActive, binds::M_EXCLUSIVE);
    kPlay = binds::Action("viewer", "play", "space", "play / pause");
    kSeekBack = binds::Action("viewer", "seek_back", "leftarrow", "back 5 seconds");
    kSeekForward = binds::Action("viewer", "seek_forward", "rightarrow", "forward 5 seconds");
    kRestart = binds::Action("viewer", "restart", "home", "back to the start");
    kNext = binds::Action("viewer", "next_run", "tab", "follow the next run");
    kFree = binds::Action("viewer", "free", "f", "release the camera / follow again");
    kXray = binds::Action("viewer", "xray", "x", "traces and ghost boxes through walls on / off");
    kLook = binds::ActionHeld("viewer", "look", "mouse2", "drag: orbit the followed run, or look around with the released camera");
    kZoomIn = binds::ActionHeld("viewer", "zoom_in", "pgup", "closer (also the mouse wheel)");
    kZoomOut = binds::ActionHeld("viewer", "zoom_out", "pgdn", "farther (also the mouse wheel)");
    kExit = binds::Action("viewer", "exit", "escape", "back to the Recordings list");
    g_messages = binds::OnWindowMessage(Message);
    if (!g_messages)
        log::Warn("spectator: no window-message slot; wheel zoom unavailable (PageUp/PageDown still zoom)");
    hooks::OnFrameBegin(Frame);
    hooks::OnLevelLoad(Level);
    g_ready = true;
#ifdef FDCORE_DEBUG
    testpipe::Register("spectator", [](const char*, char* out, int) {
        wsprintfA(out,
                  "active=%d modal=%d free=%d followed=%d targets=%d distance=%ld yaw=%ld pitch=%ld x=%ld y=%ld z=%ld wheel=%d "
                  "owns_pause=%d",
                  g_active, g_active && g_modal, g_free, g_followed, g_count, static_cast<long>(g_distance), static_cast<long>(g_yaw),
                  static_cast<long>(g_pitch), static_cast<long>(g_pos[0]), static_cast<long>(g_pos[1]),
                  static_cast<long>(g_pos[2]), g_messages, g_ownsPause);
    });
#endif
    log::Info("spectator: ready (viewer and free camera)");
    return true;
}
bool Prepare() {
    if (!g_ready)
        return false;
    if (!g_pending || FirstValid() < 0)
        return false;
    g_free = false;
    g_followed = FirstValid();
    g_distance = 240;
    g_pitch = -15;
    const Target& target = g_targets[g_followed];
    g_yaw = math::Yaw(target.forward[0], target.forward[2]);
    math::Orbit(target.pos, g_yaw, g_pitch, g_distance, g_pos);
    return pause::Prepare();
}
void PrimeCamera() {
    if (!g_pending || g_active || g_followed < 0 || g_followed >= g_count)
        return;
    camera::XForm x;
    math::Build(g_pos, g_yaw, g_pitch, &x);
    camera::SetWorldOverride(&x);
    if (!g_primed)
        log::Info("spectator: follow camera primed before first level render");
    g_primed = true;
}
bool Start() {
    if (!g_ready)
        return false;
    if (state::Current().gameState != 1)
        return false;
    g_ownsPause = !pause::Active();
    if (!pause::Set(true, "spectator")) {
        g_ownsPause = false;
        return false;
    }
    g_active = true;
    g_modal = true;
    g_flight = kViewerFlight;
    g_primed = false;
    g_free = false;
    g_followed = FirstValid();
    g_distance = 240;
    g_pitch = -15;
    if (g_followed >= 0) {
        const Target& t = g_targets[g_followed];
        g_yaw = math::Yaw(t.forward[0], t.forward[2]);
    } else {
        camera::XForm x;
        if (camera::ViewXForm(&x)) {
            for (int i = 0; i < 3; ++i)
                g_pos[i] = x.t[i];
            g_yaw = math::Yaw(-x.m[2], -x.m[8]);
        }
    }
    InterlockedExchange(&g_wheel, 0);
    QueryPerformanceCounter(&g_last);
    Pose();
    log::Info("spectator: following target %d (%d loaded)", g_followed, g_count);
    return true;
}
bool StartFree(const Flight& flight) {
    if (!g_ready)
        return false;
    camera::XForm game;
    if (g_active || !pause::Active() || !camera::LastWorldXForm(&game))
        return false;
    // The game's own last view: Genesis camera columns are right / up / back.
    const float forward[3] = {-game.m[2], -game.m[5], -game.m[8]};
    math::Angles(forward, &g_yaw, &g_pitch);
    for (int i = 0; i < 3; ++i)
        g_pos[i] = game.t[i];
    g_active = true;
    g_modal = false;
    g_free = true;
    g_ownsPause = false;  // the client paused, and resumes
    g_flight = flight;
    g_dt = 0;
    g_keysHeld = 0;
    QueryPerformanceCounter(&g_last);
    Pose();
    log::Info("spectator: free camera");
    return true;
}
namespace {
FreePauseFn g_freePauseHost;
}
void SetFreePauseHost(FreePauseFn host) { g_freePauseHost = host; }
bool RequestFreePause(bool on) { return g_freePauseHost && g_freePauseHost(on); }
void SetFlight(const Flight& flight) {
    if (!g_modal)
        g_flight = flight;
}
void Stop() {
    if (!g_active)
        return;
    // Frame polling can observe Escape before its queued WM_KEYDOWN is
    // dispatched. Retain every held viewer key until the native key-up, even
    // when no corresponding key-down has reached our window procedure yet.
    if (g_modal)
        binds::SuppressHeld();
    g_active = false;
    g_modal = false;
    g_free = false;
    camera::SetWorldOverride(nullptr);
    if (g_ownsPause)
        pause::Set(false, "spectator ended");
    g_ownsPause = false;
    log::Info("spectator: stopped");
}
bool Available() {
    return g_ready;
}
bool Active() {
    return g_active;
}
bool Modal() {
    return g_active && g_modal;
}
void SetPending(bool pending) {
    if (g_pending && !pending && !g_active) {
        if (g_primed)
            camera::SetWorldOverride(nullptr);
        g_primed = false;
        pause::CancelPrepare();
    }
    g_pending = pending;
}
bool Pending() {
    return g_pending;
}
void SetTargets(const Target* targets, int count) {
    if (!targets)
        count = 0;
    g_count = count < 0 ? 0 : count > kMaxTargets ? kMaxTargets : count;
    for (int i = 0; i < g_count; ++i)
        g_targets[i] = targets[i];
}
void SetActionHandler(ActionHandler handler) {
    g_actions = handler;
}
void SetFree(bool free) {
    if (!g_active)
        return;
    g_free = free;
    if (!free && g_followed < 0)
        g_followed = FirstValid();
    Pose();
    log::Info("spectator: camera %s", free ? "released" : "following");
}
void Follow(int index) {
    if (index < 0 || index >= g_count || !g_targets[index].valid)
        return;
    g_followed = index;
    g_free = false;
    Pose();
    log::Info("spectator: following target %d", index);
}
void Zoom(float notches) {
    const float previous = g_distance;
    g_distance = math::Zoom(g_distance, notches);
    if (g_active && g_free) {
        float sy, cy, sp, cp;
        math::SinCos(g_yaw, &sy, &cy);
        math::SinCos(g_pitch, &sp, &cp);
        const float step = previous - g_distance;
        g_pos[0] += sy * cp * step;
        g_pos[1] += sp * step;
        g_pos[2] += cy * cp * step;
    }
}
void ReadState(State* out) {
    out->active = g_active;
    out->free = g_free;
    out->modal = g_active && g_modal;
    out->dt = g_dt;
    out->keys = g_keysHeld;
    out->followed = g_followed;
    out->targets = g_count;
    out->distance = g_distance;
    out->yaw = g_yaw;
    out->pitch = g_pitch;
    for (int i = 0; i < 3; ++i)
        out->pos[i] = g_pos[i];
}
} // namespace fdcore::spectator
