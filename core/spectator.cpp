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
Target g_targets[kMaxTargets];
int g_count, g_followed;
float g_pos[3], g_yaw, g_pitch = -15, g_distance = 240;
input::Key g_keys[256];
// A consumed modal press owns its matching release too. Native Escape must
// not see a held-key repeat after the viewer deliberately opens its menu.
bool g_suppressUntilRelease[256];
ActionHandler g_actions;
LARGE_INTEGER g_frequency, g_last;
bool g_messages;  // the window-message filter is registered (wheel and key messages)
volatile LONG g_wheel;
bool Down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}
bool CaptureKey(int) {
    return g_active;
}
bool CapturePointer() {
    return g_active;
}
void Emit(Action action) {
    if (g_actions)
        g_actions(action);
}
bool ReservedKey(WPARAM key) {
    return key == 'X' || key == 'F' || key == 'W' || key == 'A' || key == 'S' || key == 'D' || key == 'E' ||
           key == 'Q' || key == VK_ESCAPE || key == VK_SPACE || key == VK_TAB || key == VK_LEFT || key == VK_RIGHT ||
           key == VK_HOME || key == VK_PRIOR || key == VK_NEXT;
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
// The viewer's keys and wheel, from the window messages binds owns; true consumes it.
bool Message(unsigned message, uintptr_t w, intptr_t l) {
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    const bool altTab = w == VK_TAB && Down(VK_MENU);
    if ((keyDown || keyUp) && w < 256 && g_suppressUntilRelease[w] && !altTab) {
        if (keyUp) {
            g_suppressUntilRelease[w] = false;
            g_keys[w].Release();
        }
        return true;
    }
    if (message == WM_CHAR) {
        const uintptr_t upper = w >= 'a' && w <= 'z' ? w - 'a' + 'A' : w;
        if (upper < 256 && g_suppressUntilRelease[upper])
            return true;
    }
    if (message == WM_MOUSEWHEEL && g_active && binds::KeysLive()) {
        InterlockedExchangeAdd(&g_wheel, static_cast<short>(HIWORD(w)));
        return true;
    }
    if (g_active && binds::KeysLive()) {
        // Keep Alt+Tab and the console key available to Windows/the native console.
        if ((keyDown || keyUp) && ReservedKey(w) && !altTab) {
            if (keyDown)
                g_keys[w].Message((l & (1L << 30)) != 0);
            else
                g_keys[w].Release();
            if (w < 256)
                g_suppressUntilRelease[w] = keyDown;
            return true;
        }
        if (message == WM_CHAR) {
            const uintptr_t upper = w >= 'a' && w <= 'z' ? w - 'a' + 'A' : w;
            if (ReservedKey(upper))
                return true;
        }
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
    const bool live = binds::KeysLive();
    bool pressed[256] = {};
    for (int key = 1; key < 256; ++key)
        pressed[key] = g_keys[key].Poll(Down(key), live);
    if (pressed['X'])
        Emit(Action::ToggleXray);
    if (pressed['F'])
        SetFree(!g_free);
    if (pressed[VK_TAB] && g_count > 1) {
        for (int n = 1; n <= g_count; ++n) {
            int i = (g_followed + n) % g_count;
            if (g_targets[i].valid) {
                Follow(i);
                break;
            }
        }
    }
    if (pressed[VK_SPACE])
        Emit(Action::TogglePlay);
    if (pressed[VK_LEFT])
        Emit(Action::SeekBack);
    if (pressed[VK_RIGHT])
        Emit(Action::SeekForward);
    if (pressed[VK_HOME])
        Emit(Action::Restart);
    if (pressed[VK_ESCAPE]) {
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
    if (live) {
        if (Down(VK_PRIOR))
            Zoom(dt * 5);
        if (Down(VK_NEXT))
            Zoom(-dt * 5);
        if (Down(VK_RBUTTON)) {
            int32_t mx = 0, my = 0;
            addr::ReadI32(addr::Id::controls_mouse_x, &mx);
            addr::ReadI32(addr::Id::controls_mouse_y, &my);
            g_yaw = math::Wrap(g_yaw - mx * 0.15f);
            g_pitch = math::Clamp(g_pitch - my * 0.15f, -85, 85);
        }
        if (g_free) {
            float sy, cy, sp, cp;
            math::SinCos(g_yaw, &sy, &cy);
            math::SinCos(g_pitch, &sp, &cp);
            const float f[3] = {sy * cp, sp, cy * cp}, r[3] = {-cy, 0, sy};
            float move[3] = {};
            for (int i = 0; i < 3; ++i)
                move[i] = (Down('W') - Down('S')) * f[i] + (Down('D') - Down('A')) * r[i];
            move[1] += Down('E') - Down('Q');
            const float norm = math::Sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
            float speed = 400;
            if (Down(VK_SHIFT))
                speed *= 4;
            if (Down(VK_CONTROL))
                speed *= 0.25f;
            if (norm > 0)
                for (int i = 0; i < 3; ++i)
                    g_pos[i] += move[i] / norm * speed * dt;
        }
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
    if (!binds::AddKeyCapture(CaptureKey) || !binds::AddPointerCapture(CapturePointer)) {
        log::Warn("spectator: modal input capture unavailable");
        return false;
    }
    g_messages = binds::OnWindowMessage(Message);
    if (!g_messages)
        log::Warn("spectator: no window-message slot; wheel zoom unavailable (PageUp/PageDown still zoom)");
    hooks::OnFrameBegin(Frame);
    hooks::OnLevelLoad(Level);
    g_ready = true;
#ifdef FDCORE_DEBUG
    testpipe::Register("spectator", [](const char*, char* out, int) {
        wsprintfA(out,
                  "active=%d free=%d followed=%d targets=%d distance=%ld yaw=%ld pitch=%ld x=%ld y=%ld z=%ld wheel=%d "
                  "owns_pause=%d",
                  g_active, g_free, g_followed, g_count, static_cast<long>(g_distance), static_cast<long>(g_yaw),
                  static_cast<long>(g_pitch), static_cast<long>(g_pos[0]), static_cast<long>(g_pos[1]),
                  static_cast<long>(g_pos[2]), g_messages, g_ownsPause);
    });
#endif
    log::Info("spectator: ready (modal follow/free camera)");
    return true;
}
bool Prepare() {
    if (!g_ready && !Init())
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
    if (!g_ready && !Init())
        return false;
    if (state::Current().gameState != 1)
        return false;
    g_ownsPause = !pause::Active();
    if (!pause::Set(true, "spectator")) {
        g_ownsPause = false;
        return false;
    }
    g_active = true;
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
    for (int key = 0; key < 256; ++key)
        g_keys[key].Reset(Down(key));
    InterlockedExchange(&g_wheel, 0);
    QueryPerformanceCounter(&g_last);
    Pose();
    log::Info("spectator: following target %d (%d loaded)", g_followed, g_count);
    return true;
}
void Stop() {
    if (!g_active)
        return;
    // Frame polling can observe Escape before its queued WM_KEYDOWN is
    // dispatched. Retain every held viewer key until the native key-up, even
    // when no corresponding key-down has reached our window procedure yet.
    for (int key = 0; key < 256; ++key)
        if (ReservedKey(static_cast<WPARAM>(key)) && Down(key))
            g_suppressUntilRelease[key] = true;
    g_active = false;
    g_free = false;
    camera::SetWorldOverride(nullptr);
    if (g_ownsPause)
        pause::Set(false, "spectator ended");
    g_ownsPause = false;
    log::Info("spectator: stopped");
}
bool Active() {
    return g_active;
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
    out->followed = g_followed;
    out->targets = g_count;
    out->distance = g_distance;
    out->yaw = g_yaw;
    out->pitch = g_pitch;
    for (int i = 0; i < 3; ++i)
        out->pos[i] = g_pos[i];
}
} // namespace fdcore::spectator
