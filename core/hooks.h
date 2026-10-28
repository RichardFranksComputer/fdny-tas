// Hook manager: owns every hook (MinHook inline hooks on exe code and on
// Genesis.dll exports resolved by name), toggles them at runtime, and
// dispatches core events to subscribers. Only the core installs hooks.
#pragma once

#include <stdint.h>

#include "addr.h"

namespace fdcore::hooks {

// Event subscribers run on the game thread.
using FrameFn = void (*)();
using LevelFn = void (*)(const char* map);

bool Init();  // MH_Initialize + install the core's own hooks (frame events)
void Shutdown();

// Creates (disabled) a hook on a verified address-table code entry, in a
// named group. Groups toggle atomically. "frame" = geEngine_BeginFrame +
// geEngine_EndFrame (created and enabled by Init).
bool Create(const char* group, addr::Id id, void* detour, void** original);
bool SetEnabled(const char* group, bool on);
bool IsEnabled(const char* group);

void OnFrameBegin(FrameFn fn); // before geEngine_BeginFrame runs (outside the engine's frame)
void OnFrameEnd(FrameFn fn);   // after the world is drawn, before the flip
// After geEngine_EndFrame has closed/flipped the frame. Scene/menu transitions
// belong here; callbacks must not draw.
void OnFrameComplete(FrameFn fn);
void OnFrameEndLate(FrameFn fn);  // after every OnFrameEnd subscriber: gfx's layers only (R8); draw via gfx::OnLayer
void OnLevelLoad(LevelFn fn);  // map entered (game state reaches 1 with a map name)

// Raised by the core itself (levelwatch) when it detects a level entry.
void RaiseLevelLoad(const char* map);

// Normal process termination, after game threads stop. Subscribers may flush
// open files; never wait for threads or load modules under the loader lock.
void OnProcessExit(FrameFn fn);
void RaiseProcessExit();

struct Counters {
    volatile long beginFrame;
    volatile long endFrame;
    volatile long frameEndEvents;
    volatile long levelLoads;
    volatile long toggles;
};
const Counters& Stats();

// geEngine* passed to the current frame's Begin/EndFrame (for in-frame drawing
// from frame_end subscribers); null before the first frame.
void* CurrentEngine();

} // namespace fdcore::hooks
