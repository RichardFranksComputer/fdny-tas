// Tests: tests/game/fov_sets_camera.yaml
// User: Field of view -- the camera FOV: the classic FOV fix by default, or a fixed angle, Hor+ on wide screens.
// fov feature (backlog item 6): the camera FOV, as a filter on the core's camera
// service (which owns the geCamera_SetAttributes hook). Genesis stores Fov raw at
// [camera+0] and projects with scale = Width / Fov, so horizontal FOV = 2*atan(Fov/2).
//
// Parity with the original FOVFix .asi (measured live, 2026-10-21): it
// replaces every Fov with the constant 1.43 * (W/H)/(4/3) * FOVFactor
// (W/H from its ini; 1.43 ~ the game's stock in-level 1.428). With the
// owner's ini (640x480, 1.4) that is 2.002 = 90.0 deg horizontal.
#include <windows.h>

#include "addr.h"
#include "camera.h"
#include "cmd.h"
#include "console.h"
#include "log.h"
#include "modules.h"
#include "num.h"

namespace fdcore::fov {
namespace {

using addr::Id;

float g_lastStored = -1.0f;

// --- no-CRT float helpers ----------------------------------------------------
float Tan(float x) {
    float r;
    __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x));
    return r;
}

void FormatFloat(float v, char* out) { num::Fixed(out, v, 6); }  // "123.456789"

// --- variables -----------------------------------------------------------------
void Reapply(cmd::Cvar&) { camera::Reapply(); }
cmd::Cvar g_fdFov = {"fd_fov", "0",
                     "horizontal FOV in degrees at 4:3 (up to 170), Hor+ for wider screens; 0 = classic FOV fix "
                     "(1.43 * aspect/(4/3) * fd_fov_factor); -1 = game default",
                     false, true, Reapply, {0}, cmd::T_FLOAT, -1, 170};
cmd::Cvar g_fdFovFactor = {"fd_fov_factor", "1.4", "FOV multiplier used when fd_fov is 0 (classic FOV fix), 0.1-5: 1.4 = 90 degrees at 4:3",
                           false, true, Reapply, {0}, cmd::T_FLOAT, 0.1f, 5};

// Fov to store for a camera with this rect, given the game's own Fov (camera filter).
float Wanted(const int* rect, float gameFov) {
    float fov = cmd::Float(g_fdFov);
    const float factor = cmd::Float(g_fdFovFactor);
    if (fov < 0.0f) return gameFov;
    float w = static_cast<float>(rect[1] - rect[0] + 1), h = static_cast<float>(rect[3] - rect[2] + 1);
    float horPlus = h > 0.0f ? (w / h) / (4.0f / 3.0f) : 1.0f;
    if (fov == 0.0f) return 1.43f * horPlus * factor;
    if (fov > 170.0f) fov = 170.0f;
    return 2.0f * Tan(fov * 3.14159265f / 360.0f) * horPlus;
}

void AfterSet(void* camera, float gameFov) {
    float stored = *reinterpret_cast<const float*>(camera);
    if (stored == g_lastStored) return;
    char a[32], b[32];
    FormatFloat(gameFov, a);
    FormatFloat(stored, b);
    log::Info("fov: camera %08lx game_fov=%s stored=%s", reinterpret_cast<unsigned long>(camera), a, b);
    g_lastStored = stored;
#ifdef FDCORE_DEBUG
    // Debug aid: report where the render object holds this camera, so the
    // address table can point at it (looked up once).
    static bool probed = false;
    uintptr_t field;
    uint32_t render = 0;
    if (!probed && addr::Resolve(Id::everGame_render, &field) && addr::SafeRead(field, &render, 4) && render) {
        probed = true;
        for (uint32_t off = 0; off < 0x200; off += 4) {
            uint32_t v = 0;
            if (!addr::SafeRead(render + off, &v, 4)) break;
            if (v == reinterpret_cast<uint32_t>(camera)) log::Info("fov: probe [render+%lx] == camera", off);
            for (uint32_t off2 = 0; v && off2 < 0x100; off2 += 4) {
                uint32_t w = 0;
                if (!addr::SafeRead(v + off2, &w, 4)) break;
                if (w == reinterpret_cast<uint32_t>(camera))
                    log::Info("fov: probe [[render+%lx]+%lx] == camera", off, off2);
            }
        }
    }
#endif
}

void CmdFovInfo(int, const char* const*) {
    char a[32], b[32];
    FormatFloat(camera::LastGameFov(), a);
    FormatFloat(g_lastStored, b);
    console::Print("fov: game %s, camera %s (fd_fov %s, fd_fov_factor %s)", a, b, g_fdFov.value, g_fdFovFactor.value);
}

const Id kNeeds[] = {Id::genesis_camera_set_attributes};

bool Init() {
    cmd::RegisterCvar(&g_fdFov);
    cmd::RegisterCvar(&g_fdFovFactor);
    cmd::RegisterCommand("fd_fovinfo", "show the game's FOV and the camera's current FOV", CmdFovInfo);
    camera::SetFovFilter(Wanted);
    camera::OnSet(AfterSet);
    log::Info("fov: ready (filter on the camera service)");
    return true;
}

} // namespace
} // namespace fdcore::fov

FDCORE_MODULE(fov, "fov", fdcore::fov::kNeeds, 1, nullptr, &fdcore::fov::Init, 10)
