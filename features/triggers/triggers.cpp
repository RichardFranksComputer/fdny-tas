// Tests: tests/game/triggers.yaml tests/game/keys_default.yaml
// User: Trigger boxes -- draws the map's rescue zones and exits (and optionally every trigger) as boxes.
// triggers feature (owner request 2026-10-22, docs/evidence/trigger_boxes): draws every
// RCS_BoundsTrigger of the loaded map as a 50% box -- blue = rescue/escape zone, red =
// closed final exit, green = open final exit, grey = any other trigger (only with
// fd_triggers_all 1). Everything comes from live memory each level, nothing from files:
// the level entity list (entities.head) holds the triggers (entities whose vtable is
// trigger.vtbl; kind can't tell them apart, the constructor writes 8 for all of them),
// each with its world-space box (trigger.box), the game's own escape flag
// (trigger.escape: a "touch -> tell player escape" binding), its script lines
// (trigger.userdata -> trigdata.lines) and the open flag the win check sets on exits
// (trigger.enabled). An exit is classified the way PlayerRescueEnt::UpdateZonesAndWin
// does it: a touch line containing "tell player finalexit" or "tell player
// (finalexitloadmap". Drawing: gfx world boxes, depth-tested unless fd_triggers_xray.
#include <windows.h>

#include "addr.h"
#include "binds.h"
#include "cmd.h"
#include "console.h"
#include "gfx.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "state.h"
#include "testpipe.h"

namespace fdcore::triggers {
namespace {

using addr::Id;

enum Kind { K_OTHER, K_RESCUE, K_EXIT };
const char* const kKindName[] = {"other", "rescue", "exit"};

constexpr uint32_t kBlue = 0x2050FF, kRed = 0xFF2828, kGreen = 0x28E040, kGrey = 0x9A9A9A;
constexpr uint8_t kAlpha = 128;       // 50% (8/15 in 4444)
constexpr uint8_t kAlphaInside = 64;  // a box around the camera tints the whole view: fainter
constexpr int kMax = 256;
constexpr int kRescanFrames = 60;     // trigger set / classes re-read about once a second

struct Trigger {
    uintptr_t ent;
    Kind kind;
    bool open;     // exits: trigger.enabled
    float box[6];  // min xyz, max xyz
    char name[32];
};
Trigger g_trig[kMax];
int g_n = 0;
char g_map[16] = "";
uint32_t g_lastScan = 0;
gfx::WorldBox g_boxes[kMax];  // submitted to gfx each frame (static: drawn in the late step)
int g_nBoxes = 0;
int g_counts[3] = {}, g_openExits = 0;
CRITICAL_SECTION g_lock;  // the pipe thread reads the counts

bool On(const cmd::Cvar& v) { return v.value[0] != '0'; }
cmd::Cvar g_fdTriggers = {"fd_triggers", "0",
                          "1 = draw trigger boxes: blue rescue zone, red closed exit, green open exit (key T); 0 = off", false,
                          true, nullptr, {0}};
cmd::Cvar g_fdTriggersAll = {"fd_triggers_all", "0", "1 = also draw every other trigger, grey (zones, hints, doors); 0 = only rescue zones and exits",
                             false, true, nullptr, {0}};
cmd::Cvar g_fdTriggersXray = {"fd_triggers_xray", "0", "1 = trigger boxes show through walls; 0 = walls hide them (T's second press sets it)", false, true, nullptr,
                              {0}};

template <typename T>
bool At(uintptr_t base, Id field, T* out) {
    return addr::SafeRead(base + addr::Field(field), out, sizeof(T));
}

// A NUL-terminated string in game memory, read in 16-byte steps (it may end near a page edge).
bool ReadCStr(uintptr_t p, char* out, int cap) {
    out[0] = 0;
    if (!p) return false;
    for (int o = 0; o + 1 < cap;) {
        char chunk[16];
        int n = cap - 1 - o < 16 ? cap - 1 - o : 16;
        if (!addr::SafeRead(p + o, chunk, n)) {
            n = 1;  // fall back to bytes at the edge
            if (!addr::SafeRead(p + o, chunk, 1)) return o > 0;
        }
        for (int i = 0; i < n; ++i) {
            out[o + i] = chunk[i];
            if (!chunk[i]) return true;
        }
        o += n;
        out[o] = 0;
    }
    return true;
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool Contains(const char* s, const char* needle) {
    for (; *s; ++s) {
        const char *a = s, *b = needle;
        while (*b && Lower(*a) == Lower(*b)) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}

// "touch: <command>" with an exit command (UpdateZonesAndWin's two has-binding tests).
bool IsExitLine(const char* line) {
    while (*line == ' ') ++line;
    const char* ev = "touch";
    for (; *ev; ++ev, ++line)
        if (Lower(*line) != *ev) return false;
    while (*line == ' ') ++line;
    if (*line != ':') return false;
    return Contains(line, "tell player finalexit") || Contains(line, "tell player (finalexitloadmap");
}

Kind Classify(uintptr_t ent, char* name, int nameCap) {
    uint32_t data = 0, p = 0;
    name[0] = 0;
    bool exit = false;
    if (At(ent, Id::trigger_userdata, &data) && data) {
        if (At(data, Id::trigdata_name, &p)) ReadCStr(p, name, nameCap);
        static char line[256];
        for (int i = 0; i < 10 && !exit; ++i)
            if (addr::SafeRead(data + addr::Field(Id::trigdata_lines) + 4u * i, &p, 4) && p &&
                ReadCStr(p, line, sizeof(line)))
                exit = IsExitLine(line);
    }
    if (exit) return K_EXIT;
    uint8_t escape = 0;
    return At(ent, Id::trigger_escape, &escape) && escape ? K_RESCUE : K_OTHER;
}

// Rebuilds the trigger list from the entity list (on a map change and every kRescanFrames).
void Scan(const state::State& s) {
    // On the same map, exits keep their last open flag, so FrameEnd logs the change even when
    // it lands on a rescan frame (suite 2026-10-26: the exit opened, "exit ... open" never came).
    static uintptr_t wasOpen[kMax];
    int nWas = 0;
    const bool sameMap = lstrcmpA(g_map, s.map) == 0;
    for (int i = 0; sameMap && i < g_n; ++i)
        if (g_trig[i].kind == K_EXIT && g_trig[i].open) wasOpen[nWas++] = g_trig[i].ent;
    uintptr_t head = 0, vtbl = 0;
    uint32_t node = 0;
    int n = 0;
    if (addr::Resolve(Id::entities_head, &head) && addr::SafeRead(head, &node, 4) &&
        addr::Resolve(Id::trigger_vtbl, &vtbl))
        for (int guard = 0; node && guard < 4096 && n < kMax; ++guard) {
            uint32_t ent = 0, next = 0, vt = 0;
            if (!At(node, Id::node_elem, &ent) || !At(node, Id::node_next, &next)) break;
            node = next;
            if (!ent || !addr::SafeRead(ent, &vt, 4) || vt != vtbl) continue;
            Trigger& t = g_trig[n];
            float box[6];
            if (!At(ent, Id::trigger_box, &box)) continue;
            bool sane = true;
            for (int i = 0; i < 6; ++i) sane = sane && box[i] > -1.0e6f && box[i] < 1.0e6f;  // also NaN
            if (!sane || box[0] > box[3] || box[1] > box[4] || box[2] > box[5]) continue;
            t.ent = ent;
            __builtin_memcpy(t.box, box, sizeof(box));
            t.kind = Classify(ent, t.name, sizeof(t.name));
            uint8_t open = 0;
            t.open = t.kind == K_EXIT && !sameMap && At(ent, Id::trigger_enabled, &open) && open;
            for (int k = 0; t.kind == K_EXIT && k < nWas; ++k) t.open = t.open || wasOpen[k] == ent;
            ++n;
        }
    int counts[3] = {}, open = 0;
    for (int i = 0; i < n; ++i) ++counts[g_trig[i].kind], open += g_trig[i].open;
    const bool changed = lstrcmpA(g_map, s.map) != 0 || n != g_n || counts[K_EXIT] != g_counts[K_EXIT] ||
                         counts[K_RESCUE] != g_counts[K_RESCUE] || open != g_openExits;
    EnterCriticalSection(&g_lock);
    g_n = n;
    __builtin_memcpy(g_counts, counts, sizeof(counts));
    g_openExits = open;
    lstrcpynA(g_map, s.map, sizeof(g_map));
    LeaveCriticalSection(&g_lock);
    if (changed)
        log::Info("triggers: map '%s': %d trigger(s): %d rescue, %d exit (%d open), %d other", s.map, n,
                  counts[K_RESCUE], counts[K_EXIT], open, counts[K_OTHER]);
}

// Frame end: keep the list current, refresh the exits' open flags, build the box list.
void FrameEnd() {
    const state::State& s = state::Current();
    if (s.gameState != 1) {  // menus / loading: the entity list is stale
        if (g_n) {
            EnterCriticalSection(&g_lock);
            g_n = 0, g_map[0] = 0;
            __builtin_memset(g_counts, 0, sizeof(g_counts));
            g_openExits = 0;
            LeaveCriticalSection(&g_lock);
        }
        g_nBoxes = 0;
        return;
    }
    // A watched level (state::WatchOnly, the recordings viewer) shows no trigger volumes;
    // nor is the newly loaded entity list walked during its first camera handoff.
    if (state::WatchOnly()) { g_nBoxes = 0; return; }
    if (lstrcmpA(g_map, s.map) != 0 || s.frame - g_lastScan >= kRescanFrames) {
        g_lastScan = s.frame;
        Scan(s);
    }
    int open = 0;
    for (int i = 0; i < g_n; ++i) {  // the win check opens exits mid-level: every frame
        Trigger& t = g_trig[i];
        if (t.kind != K_EXIT) continue;
        uint8_t v = 0;
        const bool now = At(t.ent, Id::trigger_enabled, &v) && v;
        if (now != t.open) log::Info("triggers: exit '%s' %s", t.name, now ? "open" : "closed");
        t.open = now;
        open += now;
    }
    g_openExits = open;
    g_nBoxes = 0;
    if (!On(g_fdTriggers) || !gfx::WorldBoxesReady()) {
        // nothing submitted: nothing drawn (submissions last one frame)
        return;
    }
    const bool all = On(g_fdTriggersAll);
    for (int i = 0; i < g_n; ++i) {
        const Trigger& t = g_trig[i];
        if (t.kind == K_OTHER && !all) continue;
        gfx::WorldBox& b = g_boxes[g_nBoxes++];
        __builtin_memcpy(b.min, t.box, sizeof(b.min));
        __builtin_memcpy(b.max, t.box + 3, sizeof(b.max));
        b.rgb = t.kind == K_RESCUE ? kBlue : t.kind == K_EXIT ? (t.open ? kGreen : kRed) : kGrey;
        b.alpha = kAlpha, b.alphaInside = kAlphaInside;
    }
    gfx::SubmitWorldBoxes(g_boxes, g_nBoxes, On(g_fdTriggersXray) ? gfx::LAYER_OVERLAY : gfx::LAYER_WORLD);
}

// Key T cycles (owner 2026-10-22): off -> boxes -> boxes + x-ray -> off.
void CmdToggle(int, const char* const*) {
    if (!On(g_fdTriggers)) {
        if (!cmd::SetCvar(&g_fdTriggers, "1")) return;  // refused (profile run): the gate printed why
        cmd::SetCvar(&g_fdTriggersXray, "0");
    } else if (!On(g_fdTriggersXray)) {
        cmd::SetCvar(&g_fdTriggersXray, "1");
    } else {
        cmd::SetCvar(&g_fdTriggers, "0");
        cmd::SetCvar(&g_fdTriggersXray, "0");
    }
    console::Print("trigger boxes: %s", !On(g_fdTriggers) ? "off" : On(g_fdTriggersXray) ? "on, x-ray" : "on");
}

// "-123" (wsprintf has no %f; whole units are enough for a listing)
long R(float v) { return static_cast<long>(v + (v >= 0 ? 0.5f : -0.5f)); }

void CmdList(int, const char* const*) {
    console::Print("triggers on '%s': %d", g_map, g_n);
    for (int i = 0; i < g_n; ++i) {
        const Trigger& t = g_trig[i];
        console::Print("%2d %-6s%s %-16s (%ld %ld %ld)-(%ld %ld %ld)", i, kKindName[t.kind],
                       t.kind == K_EXIT ? (t.open ? " open" : " shut") : "     ", t.name, R(t.box[0]), R(t.box[1]),
                       R(t.box[2]), R(t.box[3]), R(t.box[4]), R(t.box[5]));
    }
}

// Signed entries only: struct fields (node.*, trigger.box ...) have no signature to verify.
const Id kNeeds[] = {Id::entities_head, Id::trigger_vtbl};

bool Init() {
    InitializeCriticalSection(&g_lock);
    cmd::RegisterCvar(&g_fdTriggers);
    cmd::RegisterCvar(&g_fdTriggersAll);
    cmd::RegisterCvar(&g_fdTriggersXray);
    cmd::RegisterCommand("fd_triggers_toggle", "cycle the trigger boxes: off -> on -> on + x-ray -> off (key T)", CmdToggle);
    cmd::RegisterCommand("fd_triggers_list", "list the loaded map's triggers: class, open/shut, name, box", CmdList);
    cmd::SetPracticeOnly("fd_triggers");
    cmd::SetPracticeOnly("fd_triggers_toggle");
    cmd::SetPracticeOnly("fd_triggers_list");
    binds::BindDefault("t", "fd_triggers_toggle");
    binds::BindDefault("y", "toggle fd_triggers_all");  // D37; x-ray is T's second press
    hooks::OnFrameEnd(FrameEnd);
#ifdef FDCORE_DEBUG
    testpipe::Register("triggers", [](const char*, char* resp, int cap) {
        (void)cap;
        const gfx::WorldBoxStats st = gfx::LastWorldBoxStats();
        EnterCriticalSection(&g_lock);
        wsprintfA(resp, "map=%s n=%d rescue=%d exit=%d open=%d other=%d boxes=%d faces=%d polys=%d inside=%d", g_map,
                  g_n, g_counts[K_RESCUE], g_counts[K_EXIT], g_openExits, g_counts[K_OTHER], st.boxes, st.faces,
                  st.polys, st.inside);
        LeaveCriticalSection(&g_lock);
    });
#endif
    log::Info("triggers: ready (fd_triggers, key T%s)", gfx::WorldBoxesReady() ? "" : "; no world boxes: drawing off");
    return true;
}

} // namespace
} // namespace fdcore::triggers

// id, log name, required Ids, count, DllMain-phase step (or nullptr), init, start order
FDCORE_MODULE(triggers, "triggers", fdcore::triggers::kNeeds, 2, nullptr, &fdcore::triggers::Init, 70)
