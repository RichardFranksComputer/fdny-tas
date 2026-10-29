// Tests: tests/game/pause_service.yaml tests/game/spectator_camera.yaml tests/game/fov_sets_camera.yaml
#include "camera.h"

#include <windows.h>

#include "addr.h"
#include "hooks.h"
#include "log.h"
#include "testpipe.h"

namespace fdcore::camera {
namespace {

using addr::Id;
using SetAttributesFn = int(__fastcall*)(void* camera, const int* rect, float fov);
SetAttributesFn g_orig = nullptr;

void* g_lastCamera = nullptr;
int g_lastRect[4];
float g_lastGameFov = 0.0f;
FovFilter g_filter = nullptr;
constexpr int kMaxSubs = 4;
SetFn g_subs[kMaxSubs];
int g_nSubs = 0;
BeforeWorldFn g_beforeWorld[kMaxSubs];
int g_nBeforeWorld;

// World transform override (Genesis exports; fastcall ECX = camera).
using SetXFormFn = int(__fastcall*)(void* camera, const XForm* x);
using GetXFormFn = const XForm*(__fastcall*)(void* camera);
SetXFormFn g_setXf = nullptr;
GetXFormFn g_getXf = nullptr;
XForm g_override = {}, g_lastXf = {};
bool g_overrideOn = false, g_haveLastXf = false;
#ifdef FDCORE_DEBUG
unsigned long g_nativeSets, g_overriddenSets;
#endif

// Native scene setup consumes the camera before RenderWorld (visibility,
// attached objects and loading renders). Override the producer too, rather
// than briefly publishing the player's first-person transform to that setup.
int __fastcall SetWorldDetour(void* camera, const XForm* x) {
    for (int i = 0; i < g_nBeforeWorld; ++i) g_beforeWorld[i]();
#ifdef FDCORE_DEBUG
    ++g_nativeSets;
    if (g_overrideOn) ++g_overriddenSets;
#endif
    return g_setXf(camera, g_overrideOn ? &g_override : x);
}
#ifdef FDCORE_DEBUG
volatile long g_testLift = 0x7fffffff;  // Debug pipe `camera_override <dy>|off` (none pending)
#endif

int __fastcall SetAttributesDetour(void* camera, const int* rect, float fov) {
    const float use = g_filter ? g_filter(rect, fov) : fov;
    const int r = g_orig(camera, rect, use);
    g_lastCamera = camera;
    for (int i = 0; i < 4; ++i) g_lastRect[i] = rect[i];
    g_lastGameFov = fov;
    for (int i = 0; i < g_nSubs; ++i) g_subs[i](camera, fov);
    return r;
}

} // namespace

bool Init() {
    if (!addr::Usable(Id::genesis_camera_set_attributes)) {
        log::Warn("camera: disabled (needs genesis.camera_set_attributes); no FOV control, view size from the window");
        return false;
    }
    if (!hooks::Create("camera", Id::genesis_camera_set_attributes, reinterpret_cast<void*>(&SetAttributesDetour),
                       reinterpret_cast<void**>(&g_orig)) ||
        !hooks::SetEnabled("camera", true))
        return false;
    if (addr::Usable(Id::genesis_camera_set_world_xform) && addr::Usable(Id::genesis_camera_get_world_xform)) {
        g_getXf = reinterpret_cast<GetXFormFn>(addr::ExportAddress(Id::genesis_camera_get_world_xform));
        if (!hooks::Create("camera_pose", Id::genesis_camera_set_world_xform,
                           reinterpret_cast<void*>(&SetWorldDetour), reinterpret_cast<void**>(&g_setXf)) ||
            !hooks::SetEnabled("camera_pose", true)) {
            g_setXf = nullptr;
            g_getXf = nullptr;
        }
    }
#ifdef FDCORE_DEBUG
    testpipe::Register("camera_override", [](const char* args, char* resp, int cap) {
        (void)cap;
        while (*args == ' ') ++args;
        if (args[0] == 'o' && args[1] == 'f') InterlockedExchange(&g_testLift, 0x7ffffffe);
        else if (args[0]) {
            long v = 0, sign = 1;
            if (*args == '-') sign = -1, ++args;
            while (*args >= '0' && *args <= '9') v = v * 10 + (*args++ - '0');
            InterlockedExchange(&g_testLift, sign * v);
        }
        wsprintfA(resp, "available=%d active=%d have_last=%d native_sets=%lu overridden_sets=%lu last=%ld,%ld,%ld", WorldOverrideAvailable(),
                  g_overrideOn, g_haveLastXf, g_nativeSets, g_overriddenSets, static_cast<long>(g_lastXf.t[0]), static_cast<long>(g_lastXf.t[1]),
                  static_cast<long>(g_lastXf.t[2]));
    });
#endif
    log::Info("camera: ready (geCamera_SetAttributes hooked; world override %s)", g_setXf ? "available" : "unavailable");
    return true;
}

bool ViewSize(int* w, int* h) {
    if (!g_lastCamera || g_lastRect[1] <= 0 || g_lastRect[3] <= 0) return false;
    *w = g_lastRect[1] + 1, *h = g_lastRect[3] + 1;
    return true;
}

void SetFovFilter(FovFilter f) { g_filter = f; }

void OnSet(SetFn fn) {
    if (g_nSubs < kMaxSubs) g_subs[g_nSubs++] = fn;
}

bool Reapply() {
    uintptr_t fovField;
    if (!g_orig || !g_lastCamera || !addr::Resolve(Id::camera_fov, &fovField)) return false;
    void* camera = reinterpret_cast<void*>(fovField);  // Fov is the first field of geCamera
    float probe;
    if (!addr::SafeRead(fovField, &probe, 4)) return false;
    SetAttributesDetour(camera, g_lastRect, g_lastGameFov);
    return true;
}

float LastGameFov() { return g_lastGameFov; }

bool WorldOverrideAvailable() { return g_setXf && g_getXf; }

void SetWorldOverride(const XForm* x) {
    if (x) g_override = *x;
    g_overrideOn = x && g_setXf;
}

bool WorldOverrideActive() { return g_overrideOn; }

bool LastWorldXForm(XForm* out) {
    if (!g_haveLastXf) return false;
    *out = g_lastXf;
    return true;
}

bool ViewXForm(XForm* out) {
    if (g_overrideOn) { *out = g_override; return true; }
    return LastWorldXForm(out);
}

void OnBeforeWorldRender(BeforeWorldFn fn) {
    if (g_nBeforeWorld < kMaxSubs) g_beforeWorld[g_nBeforeWorld++] = fn;
}

void BeforeWorldRender(void* camera) {
    if (!camera || !g_getXf) return;
    for (int i = 0; i < g_nBeforeWorld; ++i) g_beforeWorld[i]();
#ifdef FDCORE_DEBUG
    // Test hook: the game's last transform lifted by dy, or off (the free camera's path).
    if (const long dy = InterlockedExchange(&g_testLift, 0x7fffffff); dy != 0x7fffffff) {
        XForm x = g_lastXf;
        x.t[1] += static_cast<float>(dy);
        SetWorldOverride(dy == 0x7ffffffe ? nullptr : &x);
    }
#endif
    if (g_overrideOn) {
        g_setXf(camera, &g_override);
        return;
    }
    if (const XForm* x = g_getXf(camera)) g_lastXf = *x, g_haveLastXf = true;
}

} // namespace fdcore::camera
