// camera (core service): owns the geCamera_SetAttributes hook (Genesis.dll,
// fastcall ECX = geCamera*, EDX = const geRect*, stack = Fov float; ret 4).
// Gives the render view size to everyone (gfx) and lets one feature (fov)
// replace the FOV the game asks for.
#pragma once

namespace fdcore::camera {

bool Init();

// The render view size: the rect of the game's last SetAttributes (inclusive
// GE_Rect, so right+1 x bottom+1). False before the first call.
bool ViewSize(int* w, int* h);

// Maps the game's Fov (and the camera rect) to the one stored. One filter; null = unchanged.
using FovFilter = float (*)(const int* rect, float gameFov);
void SetFovFilter(FovFilter f);
// After every SetAttributes: the camera and the game's own Fov (the stored one is at [camera+0]).
using SetFn = void (*)(void* camera, float gameFov);
void OnSet(SetFn fn);

// Re-runs the game's last SetAttributes on the live in-level camera (found
// through the address table), e.g. after the filter's settings changed.
bool Reapply();
float LastGameFov();

// World transform override (fd_pause free camera; D34, evidence/freecam). geXForm3d:
// rows AX..CZ, then the translation. While set, it is put on the camera of every
// geEngine_RenderWorld (gfx's detour calls BeforeWorldRender): set any earlier in the
// frame, the render rewrites the camera (probe runs p2-p4). Null = off; the game's own
// per-frame camera set takes over again.
struct XForm {
    float m[9];
    float t[3];
};
bool WorldOverrideAvailable();
void SetWorldOverride(const XForm* x);
bool WorldOverrideActive();
// The game's transform on the last world render without an override (the free camera's
// start pose). False before the first world render.
bool LastWorldXForm(XForm* out);
// Current view, including the free camera override; false before the first render.
bool ViewXForm(XForm* out);
void BeforeWorldRender(void* camera);  // gfx only

} // namespace fdcore::camera
