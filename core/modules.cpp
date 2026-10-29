// Tests: tests/game/core_loads.yaml tests/game/addr_unknown_exe.yaml
#include "modules.h"

#include <windows.h>

#include "hooks.h"
#include "log.h"

// The section markers: every FDCORE_MODULE pointer lies between them.
extern "C" __attribute__((used, section(".fdmod$a"))) fdcore::modules::Module* const fdmod_first = nullptr;
extern "C" __attribute__((used, section(".fdmod$z"))) fdcore::modules::Module* const fdmod_last = nullptr;

namespace fdcore::modules {
namespace {

// --- levelwatch (core service, registered like any module) -----------------------
// On every frame end, logs changes of map name / game state and raises the
// core's level_load event when the game enters a map (state reaches 1).
using addr::Id;

const Id kLevelwatchNeeds[] = {Id::game_state, Id::game_map_name, Id::genesis_end_frame};

char g_lastMap[16] = "\x01";
int32_t g_lastState = -12345;

void LevelwatchFrame() {
    char map[16];
    int32_t state = -1;
    if (!addr::ReadStr(Id::game_map_name, map, sizeof(map))) map[0] = 0;
    addr::ReadI32(Id::game_state, &state);
    bool mapChanged = lstrcmpA(map, g_lastMap) != 0;
    if (state == g_lastState && !mapChanged) return;
    log::Info("levelwatch: map '%s' state %d", map, static_cast<int>(state));
    if (state == 1 && map[0] && (g_lastState != 1 || mapChanged)) hooks::RaiseLevelLoad(map);
    lstrcpynA(g_lastMap, map, sizeof(g_lastMap));
    g_lastState = state;
}

bool LevelwatchStart() {
    hooks::OnFrameEnd(LevelwatchFrame);
    return true;
}

// --- the registry ------------------------------------------------------------------
constexpr int kMax = 32;
Module* g_mods[kMax];
int g_n = -1;  // -1 = not collected yet

void Collect() {
    if (g_n >= 0) return;
    g_n = 0;
    // Walk the merged section between the markers (the linker may pad: skip nulls).
    auto* p = reinterpret_cast<Module* const*>(reinterpret_cast<uintptr_t>(&fdmod_first) + sizeof(void*));
    auto* end = reinterpret_cast<Module* const*>(reinterpret_cast<uintptr_t>(&fdmod_last));
    for (; p < end; ++p)
        if (*p && g_n < kMax) g_mods[g_n++] = *p;
    // Stable insertion sort by order.
    for (int i = 1; i < g_n; ++i)
        for (int j = i; j > 0 && g_mods[j - 1]->order > g_mods[j]->order; --j) {
            Module* t = g_mods[j];
            g_mods[j] = g_mods[j - 1];
            g_mods[j - 1] = t;
        }
}

} // namespace

void RunEarly(const char* gameDir) {
    Collect();
    for (int i = 0; i < g_n; ++i)
        if (g_mods[i]->early) g_mods[i]->early(gameDir);
}

void StartAll(bool servicesOk) {
    Collect();
    char names[256];
    int o = 0;
    names[0] = 0;
    for (int i = 0; i < g_n && o < 200; ++i) o += wsprintfA(names + o, "%s%s", i ? " " : "", g_mods[i]->name);
    log::Info("modules: %d registered (%s)", g_n, names);
    for (int i = 0; i < g_n; ++i) {
        Module& m = *g_mods[i];
        m.enabled = false;
        if (!addr::VersionKnown()) {
            log::Info("module %s: disabled (unknown version)", m.name);
            continue;
        }
        if (!servicesOk) {
            log::Info("module %s: disabled (core services unavailable)", m.name);
            continue;
        }
        const char* missing = nullptr;
        for (int k = 0; k < m.nrequires && !missing; ++k)
            if (!addr::Usable(m.requires[k])) missing = addr::Name(m.requires[k]);
        if (missing) {
            log::Info("module %s: disabled (needs %s)", m.name, missing);
            continue;
        }
        m.enabled = !m.init || m.init();
        log::Info("module %s: %s", m.name, m.enabled ? "enabled" : "disabled (init failed; see above)");
    }
}

int Count() {
    Collect();
    return g_n;
}

const Module* At(int i) { return i >= 0 && i < Count() ? g_mods[i] : nullptr; }

} // namespace fdcore::modules

FDCORE_MODULE(levelwatch, "levelwatch", fdcore::modules::kLevelwatchNeeds, 3, nullptr,
              &fdcore::modules::LevelwatchStart, 0)
