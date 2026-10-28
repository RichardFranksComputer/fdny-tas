#include "hooks.h"

#include <windows.h>

#include "../thirdparty/minhook/include/MinHook.h"
#include "log.h"

namespace fdcore::hooks {
namespace {

using addr::Id;

// geBoolean geEngine_BeginFrame(geEngine*, geCamera*, geBoolean) / EndFrame(geEngine*):
// cdecl (undecorated exports; the exe cleans the stack after both calls).
using BeginFrameFn = int(__cdecl*)(void* engine, void* camera, int clear);
using EndFrameFn = int(__cdecl*)(void* engine);

BeginFrameFn g_origBegin = nullptr;
EndFrameFn g_origEnd = nullptr;
bool g_mhInit = false;

struct HookRec {
    const char* group;
    void* target;
};
constexpr int kMaxHooks = 64;
HookRec g_hooks[kMaxHooks];
int g_nHooks = 0;

constexpr int kMaxGroups = 16;
const char* g_groupNames[kMaxGroups];
bool g_groupOn[kMaxGroups];
int g_nGroups = 0;

Counters g_stats;
void* g_engine = nullptr;  // geEngine* of the frame in progress (from Begin/EndFrame)

constexpr int kMaxSubs = 32;
FrameFn g_frameSubs[kMaxSubs];
int g_nFrameSubs = 0;
FrameFn g_lateSubs[kMaxSubs];
int g_nLateSubs = 0;
FrameFn g_completeSubs[kMaxSubs];
int g_nCompleteSubs = 0;
FrameFn g_beginSubs[kMaxSubs];
int g_nBeginSubs = 0;
LevelFn g_levelSubs[kMaxSubs];
int g_nLevelSubs = 0;
FrameFn g_exitSubs[kMaxSubs];
int g_nExitSubs = 0;

int __cdecl BeginFrameDetour(void* engine, void* camera, int clear) {
    InterlockedIncrement(&g_stats.beginFrame);
    g_engine = engine;
    for (int i = 0; i < g_nBeginSubs; ++i) g_beginSubs[i]();
    return g_origBegin(engine, camera, clear);
}

int __cdecl EndFrameDetour(void* engine) {
    InterlockedIncrement(&g_stats.endFrame);
    g_engine = engine;
    for (int i = 0; i < g_nFrameSubs; ++i) g_frameSubs[i]();
    for (int i = 0; i < g_nLateSubs; ++i) g_lateSubs[i]();
    InterlockedIncrement(&g_stats.frameEndEvents);
    const int result = g_origEnd(engine);
    for (int i = 0; i < g_nCompleteSubs; ++i) g_completeSubs[i]();
    return result;
}

bool StrEq(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

int GroupIndex(const char* group, bool create) {
    for (int i = 0; i < g_nGroups; ++i)
        if (StrEq(g_groupNames[i], group)) return i;
    if (!create || g_nGroups >= kMaxGroups) return -1;
    g_groupNames[g_nGroups] = group;
    g_groupOn[g_nGroups] = false;
    return g_nGroups++;
}

} // namespace

bool Create(const char* group, Id id, void* detour, void** original) {
    if (!g_mhInit) return false;
    uintptr_t t = addr::ExportAddress(id);
    if (!t) {
        log::Warn("hooks: %s unavailable (address table); not hooked", addr::Name(id));
        return false;
    }
    if (g_nHooks >= kMaxHooks || GroupIndex(group, true) < 0) return false;
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(t), detour, original);
    if (s != MH_OK) {
        log::Error("hooks: MH_CreateHook(%s) failed: %s", addr::Name(id), MH_StatusToString(s));
        return false;
    }
    g_hooks[g_nHooks++] = {group, reinterpret_cast<void*>(t)};
    log::Info("hooks: created %s at %08lx (group %s)", addr::Name(id), static_cast<unsigned long>(t), group);
    return true;
}

bool Init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        log::Error("hooks: MH_Initialize failed: %s", MH_StatusToString(s));
        return false;
    }
    g_mhInit = true;
    bool ok = Create("frame", Id::genesis_begin_frame, reinterpret_cast<void*>(&BeginFrameDetour),
                     reinterpret_cast<void**>(&g_origBegin)) &&
              Create("frame", Id::genesis_end_frame, reinterpret_cast<void*>(&EndFrameDetour),
                     reinterpret_cast<void**>(&g_origEnd));
    if (ok) ok = SetEnabled("frame", true);
    return ok;
}

void Shutdown() {
    if (!g_mhInit) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_mhInit = false;
}

bool SetEnabled(const char* group, bool on) {
    int g = GroupIndex(group, false);
    if (g < 0) return false;
    if (on == g_groupOn[g]) return true;
    // Queue every hook of the group, apply atomically: MinHook suspends the
    // game's threads while it patches, so no frame sees half a group.
    MH_STATUS bad = MH_OK;
    for (int i = 0; i < g_nHooks; ++i) {
        if (!StrEq(g_hooks[i].group, group)) continue;
        MH_STATUS s = on ? MH_QueueEnableHook(g_hooks[i].target) : MH_QueueDisableHook(g_hooks[i].target);
        if (s != MH_OK) bad = s;
    }
    MH_STATUS a = MH_ApplyQueued();
    if (bad != MH_OK || a != MH_OK) {
        log::Error("hooks: %s group %s failed: %s / %s", on ? "enable" : "disable", group,
                   MH_StatusToString(bad), MH_StatusToString(a));
        return false;
    }
    g_groupOn[g] = on;
    InterlockedIncrement(&g_stats.toggles);
    return true;
}

bool IsEnabled(const char* group) {
    int g = GroupIndex(group, false);
    return g >= 0 && g_groupOn[g];
}

void OnFrameBegin(FrameFn fn) {
    if (g_nBeginSubs < kMaxSubs) g_beginSubs[g_nBeginSubs++] = fn;
}

void OnFrameEnd(FrameFn fn) {
    if (g_nFrameSubs < kMaxSubs) g_frameSubs[g_nFrameSubs++] = fn;
}

void OnFrameComplete(FrameFn fn) {
    if (g_nCompleteSubs < kMaxSubs) g_completeSubs[g_nCompleteSubs++] = fn;
}

void OnFrameEndLate(FrameFn fn) {
    if (g_nLateSubs < kMaxSubs) g_lateSubs[g_nLateSubs++] = fn;
}

void OnLevelLoad(LevelFn fn) {
    if (g_nLevelSubs < kMaxSubs) g_levelSubs[g_nLevelSubs++] = fn;
}

void RaiseLevelLoad(const char* map) {
    InterlockedIncrement(&g_stats.levelLoads);
    log::Info("event: level_load '%s'", map);
    for (int i = 0; i < g_nLevelSubs; ++i) g_levelSubs[i](map);
}

const Counters& Stats() { return g_stats; }

void OnProcessExit(FrameFn fn) {
    if (g_nExitSubs < kMaxSubs) g_exitSubs[g_nExitSubs++] = fn;
}

void RaiseProcessExit() {
    for (int i = 0; i < g_nExitSubs; ++i) g_exitSubs[i]();
}

void* CurrentEngine() { return g_engine; }

} // namespace fdcore::hooks
