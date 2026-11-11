// Tests: tests/game/speedrun_static_spawns.yaml tests/game/speedrun_rescuebot.yaml
// User: Speedrun options -- fixed victim spawns and the RescueBot switch (shown on the watermark).
// speedrun module (backlog item 8): port of asi/FDNYDeterministic.
//
// Static victim spawns: VictimRescueEnt::SpawnAtSpot copies the RNG-chosen
// spot into the victim (+0x4C..+0x54), then floor-snaps it. The entities
// service's spot filter (installed in our early step, before any level exists)
// lets us substitute the configured spot in between; the game then floor-snaps
// it. The RNG stream is untouched; facing follows the RNG pick.
// Victim ordinals reset after a >2 s gap between spawns (every (re)load).
//
// RescueBot: TryStartRescue first byte 6A -> C3 (no claims) and the ctor's
// enabled imm8 01 -> 00 (new bots start disabled), plus the live level's
// enabled byte cleared. Single-byte writes, toggled at runtime.
#include "speedrun.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "entities.h"
#include "log.h"
#include "modules.h"

namespace fdcore::speedrun {
namespace {

using addr::Id;

bool g_hooked = false;

// --- variables -----------------------------------------------------------------
volatile long g_static = 0;  // fd_static_spawns
void OnStatic(cmd::Cvar& v) { InterlockedExchange(&g_static, cmd::Bool(v)); }
cmd::Cvar g_fdStaticSpawns = {"fd_static_spawns", "0",
                              "1 = victims spawn at fixed spots (fd_spawns; default spot 0 for every victim), "
                              "0 = the game's random pick",
                              false, true, OnStatic, {0}, cmd::T_BOOL};
void ApplyRescueBot(cmd::Cvar& v);
cmd::Cvar g_fdRescueBot = {"fd_rescuebot", "1",
                           "0 = the AI RescueBot never claims victims and its chatter is muted (all difficulties); "
                           "1 = as the game ships (applies fully from the next level load)",
                           false, true, ApplyRescueBot, {0}, cmd::T_BOOL};

// --- spawn pins: fd_spawns <map> <i> <i> ... (-1 = random); default all 0 --------
constexpr int kMaxMaps = 32, kMaxVictims = 24;
struct Pins {
    char map[24];
    int n;
    int spot[kMaxVictims];
};
Pins g_pins[kMaxMaps];
int g_nPins = 0;

Pins* FindPins(const char* map) {
    for (int i = 0; i < g_nPins; ++i)
        if (lstrcmpiA(g_pins[i].map, map) == 0) return &g_pins[i];
    return nullptr;
}

int ParseInt(const char* s, bool* ok) {
    int sign = 1, v = 0;
    *ok = false;
    if (*s == '-') sign = -1, ++s;
    if (*s == 'x' || *s == 'X') return *ok = !s[1], -1;  // "x" = random, like the old ini
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'), *ok = true;
    if (*s) *ok = false;
    return sign * v;
}

void PrintPins(const char* map) {
    Pins* p = FindPins(map);
    if (!p) {
        console::Print("fd_spawns %s: default (every victim at spot 0 while fd_static_spawns is 1)", map);
        return;
    }
    char line[256];
    int o = wsprintfA(line, "fd_spawns %s:", p->map);
    for (int i = 0; i < p->n && o < 240; ++i) o += wsprintfA(line + o, " %d", p->spot[i]);
    console::Print("%s", line);
}

void CmdSpawns(int argc, const char* const* argv) {
    if (argc < 2) {
        console::Print("usage: fd_spawns <map> [spot spot ...]   (one per victim in spawn order: 0 = editor origin, "
                       "1..N = alt spots, -1 = random; 'default' to reset)");
        for (int i = 0; i < g_nPins; ++i) PrintPins(g_pins[i].map);
        return;
    }
    if (argc == 2) {
        PrintPins(argv[1]);
        return;
    }
    Pins* p = FindPins(argv[1]);
    if (lstrcmpiA(argv[2], "default") == 0) {
        if (p) *p = g_pins[--g_nPins];
        cmd::SaveConfig();
        PrintPins(argv[1]);
        return;
    }
    if (!p) {
        if (g_nPins >= kMaxMaps) return console::Print("fd_spawns: too many maps");
        p = &g_pins[g_nPins++];
        lstrcpynA(p->map, argv[1], sizeof(p->map));
    }
    p->n = 0;
    for (int i = 2; i < argc && p->n < kMaxVictims; ++i) {
        // accept "0,1,-1" as well as "0 1 -1"
        char tok[64];
        lstrcpynA(tok, argv[i], sizeof(tok));
        char* s = tok;
        while (*s) {
            char* e = s;
            while (*e && *e != ',') ++e;
            char save = *e;
            *e = 0;
            if (*s) {
                bool ok;
                int v = ParseInt(s, &ok);
                if (!ok) return console::Print("fd_spawns: bad spot \"%s\"", s);
                if (p->n < kMaxVictims) p->spot[p->n++] = v;
            }
            if (!save) break;
            s = e + 1;
        }
    }
    cmd::SaveConfig();
    PrintPins(p->map);
}

int WritePins(char* out, int cap) {
    int o = 0;
    for (int i = 0; i < g_nPins && o + 200 < cap; ++i) {
        o += wsprintfA(out + o, "fd_spawns %s", g_pins[i].map);
        for (int k = 0; k < g_pins[i].n; ++k) o += wsprintfA(out + o, " %d", g_pins[i].spot[k]);
        o += wsprintfA(out + o, "\r\n");
    }
    return o;
}

// --- spawn hook ------------------------------------------------------------------
DWORD g_lastTick = 0;
int g_ordinal = 0;
char g_map[24];

void OnVictimSpot(void* entity) {
    char* victim = static_cast<char*>(entity);
    DWORD now = GetTickCount();
    if (now - g_lastTick > 2000) {  // a quiet gap = a new (re)load
        g_ordinal = 0;
        if (!addr::ReadStr(Id::game_map_name, g_map, sizeof(g_map))) g_map[0] = 0;
    }
    g_lastTick = now;
    const int ord = g_ordinal++;
    const char* name = "?";
    if (char* def = *reinterpret_cast<char**>(victim + addr::Field(Id::victim_def)))
        if (char* n = *reinterpret_cast<char**>(def + addr::Field(Id::def_name))) name = n;
    if (!g_static) {
        log::Info("event: victim_spawn %s #%d '%s' (random spot kept)", g_map, ord, name);
        return;
    }
    Pins* p = FindPins(g_map);
    int want = p ? (ord < p->n ? p->spot[ord] : -1) : 0;
    if (want < 0) {
        log::Info("event: victim_spawn %s #%d '%s' (random spot kept, pin -1)", g_map, ord, name);
        return;
    }
    int count = *reinterpret_cast<int*>(victim + addr::Field(Id::victim_spot_count));
    float** spots = *reinterpret_cast<float***>(victim + addr::Field(Id::victim_spots));
    if (!spots || want >= count || !spots[want]) {
        log::Warn("speedrun: %s victim #%d '%s': spot %d out of range (%d spots); random spot kept", g_map, ord,
                  name, want, count);
        return;
    }
    float* rec = spots[want];
    float* pos = reinterpret_cast<float*>(victim + addr::Field(Id::victim_pos));
    pos[0] = rec[0];
    pos[1] = rec[1];
    pos[2] = rec[2];
    log::Info("event: victim_spawn %s #%d '%s' pinned to spot %d", g_map, ord, name, want);
}

// --- RescueBot -----------------------------------------------------------------
bool WriteByte(uintptr_t at, uint8_t v) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<volatile uint8_t*>(at) = v;
    VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
    return true;
}

void ApplyRescueBot(cmd::Cvar& v) {
    const bool on = cmd::Bool(v);
    uintptr_t tryStart = addr::ExportAddress(Id::patch_rescuebot_trystart);
    uintptr_t ctorImm = addr::ExportAddress(Id::patch_rescuebot_ctor_enable);
    if (!tryStart || !ctorImm) {
        console::Print("fd_rescuebot: unavailable (patch sites not verified)");
        return;
    }
    bool ok = WriteByte(tryStart, on ? 0x6A : 0xC3) && WriteByte(ctorImm, on ? 0x01 : 0x00);
    if (!on) addr::WriteU8(Id::rescuebot_enabled, 0);  // the current level's bot, if one exists
    log::Info("speedrun: rescuebot %s (%s)", on ? "as shipped" : "disabled", ok ? "patched" : "write failed");
}

const Id kSpawnNeeds[] = {Id::patch_victim_spawn, Id::game_snap_to_floor, Id::game_map_name};

} // namespace

bool ApplyEarlyPatch() {
    g_hooked = entities::FilterVictimSpot(OnVictimSpot);
    if (!g_hooked) log::Warn("speedrun: no victim-spot filter (see the entities line); static spawns inactive");
    else log::Info("speedrun: static spawns use the entities victim-spot filter");
    return g_hooked;
}

bool Init() {
    bool spawnsOk = g_hooked;
    for (Id id : kSpawnNeeds) spawnsOk = spawnsOk && addr::Usable(id);
    if (spawnsOk) {
        cmd::RegisterCvar(&g_fdStaticSpawns);
        cmd::RegisterCommand("fd_spawns", "fd_spawns <map> [spot ...]: pin victim spawn spots, one per victim in spawn order "
                             "(0 = editor origin, 1..N = alt spots, -1 = random; 'default' resets; the map alone "
                             "shows them; used while fd_static_spawns is 1)",
                             CmdSpawns);
        cmd::RegisterConfigWriter(WritePins);
    }
    if (addr::Usable(Id::patch_rescuebot_trystart) && addr::Usable(Id::patch_rescuebot_ctor_enable) &&
        addr::Usable(Id::rescuebot_enabled))
        cmd::RegisterCvar(&g_fdRescueBot);
    else log::Warn("speedrun: RescueBot sites unavailable (signature mismatch, see above)");
    log::Info("speedrun: ready (static spawns %s, rescuebot %s)", spawnsOk ? "available" : "unavailable",
              addr::Usable(Id::patch_rescuebot_trystart) ? "available" : "unavailable");
    return true;
}

namespace {
void Early(const char*) { ApplyEarlyPatch(); }
} // namespace

} // namespace fdcore::speedrun

FDCORE_MODULE(speedrun, "speedrun", nullptr, 0, &fdcore::speedrun::Early, &fdcore::speedrun::Init, 30)
