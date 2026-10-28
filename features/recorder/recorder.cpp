// Tests: tests/game/recorder.yaml
// User: Recorder -- automatically save player and victim trajectories as fdny-trajectory-v1 JSONL files in Recordings beside FDNYCore.asi.
// Port of FDNY Recorder 1.1: retain Player-update sampling, wall-clock time,
// measured bounds, sampled virtual keys and run-local victim lifetimes.
// entities owns the game callbacks; this feature owns only recording/files.
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "addr.h"
#include "binds.h"
#include "cmd.h"
#include "console.h"
#include "entities.h"
#include "gen/version_gen.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "state.h"
#include "testpipe.h"

namespace fdcore::recorder {
namespace {
using addr::Id;
constexpr int kPathCap = 1024, kVictimCap = 128;
HANDLE g_file = INVALID_HANDLE_VALUE;
char g_gameDir[kPathCap], g_path[kPathCap + 180], g_map[80];
char g_buffer[65536];
DWORD g_buffered, g_lastFlush, g_runTick, g_sequence, g_samples;
void* g_player;
bool g_failed, g_changed, g_victimAvailable, g_trackVictims, g_watchedPlayer;
struct Victim { void* pointer; unsigned id; DWORD lastTick; int state; bool alive; char name[96]; };
Victim g_victims[kVictimCap];
unsigned g_nVictims;

void Changed(cmd::Cvar&) { g_changed = true; }
cmd::Cvar g_enable = {"fd_record", "0", "1 = automatically record each stage attempt to JSONL; 0 = off", false, true, Changed, {0}};
cmd::Cvar g_victimEnable = {"fd_record_victims", "1", "1 = include victim tracks in new recordings; 0 = player only", false, true, Changed, {0}};
cmd::Cvar g_directory = {"fd_record_directory", "Recordings", "output directory, absolute or relative to FDNYCore.asi; changes start a new recording", false, true, Changed, {0}};

bool On(const cmd::Cvar& v) { return v.value[0] != '0'; }
bool Finite(float f) { uint32_t bits; memcpy(&bits, &f, 4); return (bits & 0x7f800000u) != 0x7f800000u; }

// Bounded JSON serialization without a CRT. Nine significant decimal digits
// preserve binary32 values, including very small/large values; never emit NaN.
struct Json {
    char text[2048];
    unsigned size;
    bool ok;
    void Begin() { size = 0; ok = true; Raw("{"); }
    void Raw(const char* s) {
        unsigned n = static_cast<unsigned>(lstrlenA(s));
        if (size + n >= sizeof(text)) { ok = false; return; }
        memcpy(text + size, s, n); size += n; text[size] = 0;
    }
    void String(const char* s) {
        Raw("\"");
        for (; *s; ++s) {
            unsigned char c = static_cast<unsigned char>(*s);
            char b[8] = {};
            if (c == '"' || c == '\\') { b[0] = '\\'; b[1] = static_cast<char>(c); }
            else if (c < 32 || c >= 127) wsprintfA(b, "\\u%04x", static_cast<unsigned>(c));
            else b[0] = static_cast<char>(c);
            Raw(b);
        }
        Raw("\"");
    }
    void Key(const char* key) { if (size > 1) Raw(","); String(key); Raw(":"); }
    void U32(uint32_t n) { char b[16]; wsprintfA(b, "%lu", static_cast<unsigned long>(n)); Raw(b); }
    void Int(int n) { char b[16]; wsprintfA(b, "%d", n); Raw(b); }
    void Number(float f) {
        if (!Finite(f)) { ok = false; return; }
        if (f == 0) {
            uint32_t bits; memcpy(&bits, &f, 4);
            Raw(bits >> 31 ? "-0.0" : "0"); return;
        }
        double d = f; bool negative = d < 0;
        if (negative) d = -d;
        int exponent = 0;
        while (d >= 10) { d /= 10; ++exponent; }
        while (d < 1) { d *= 10; --exponent; }
        uint32_t digits = static_cast<uint32_t>(d * 100000000.0 + 0.5);
        if (digits >= 1000000000u) { digits /= 10; ++exponent; }
        char b[48];
        wsprintfA(b, "%s%lu.%08lue%d", negative ? "-" : "", static_cast<unsigned long>(digits / 100000000u),
                  static_cast<unsigned long>(digits % 100000000u), exponent);
        Raw(b);
    }
    void Vector(const float* v) { Raw("["); Number(v[0]); Raw(","); Number(v[1]); Raw(","); Number(v[2]); Raw("]"); }
    void Text(const char* k, const char* v) { Key(k); String(v); }
    void UInt(const char* k, uint32_t v) { Key(k); U32(v); }
    void Vec(const char* k, const float* v) { Key(k); Vector(v); }
};
Json g_json;

void Fail(const char* reason) {
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE; g_buffered = 0; g_failed = true;
    log::Warn("recorder: %s; this attempt disabled", reason);
}
bool Flush() {
    if (g_file == INVALID_HANDLE_VALUE) return false;
    DWORD offset = 0;
    while (offset < g_buffered) {
        DWORD written = 0;
        if (!WriteFile(g_file, g_buffer + offset, g_buffered - offset, &written, nullptr) || !written) {
            Fail("output write failed"); return false;
        }
        offset += written;
    }
    g_buffered = 0; g_lastFlush = GetTickCount();
    return true;
}
bool Append() {
    g_json.Raw("}\n");
    if (!g_json.ok) { Fail("invalid or oversized record"); return false; }
    if (g_file == INVALID_HANDLE_VALUE) return false;
    if (g_buffered + g_json.size > sizeof(g_buffer) && !Flush()) return false;
    memcpy(g_buffer + g_buffered, g_json.text, g_json.size); g_buffered += g_json.size;
    return GetTickCount() - g_lastFlush < 1000 || Flush();
}
void End(const char* reason) {
    if (g_file != INVALID_HANDLE_VALUE) {
        g_json.Begin(); g_json.Text("type", "end"); g_json.Text("reason", reason);
        g_json.UInt("samples", g_samples); g_json.UInt("elapsed_ms", GetTickCount() - g_runTick);
        Append(); Flush();
        if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    }
    g_file = INVALID_HANDLE_VALUE; g_player = nullptr; g_buffered = 0;
}
void Exit() { End("process_exit"); }

void Start(void* player) {
    End("next_player_initialization");
    g_watchedPlayer = state::WatchOnly();  // a level loaded to be watched: nothing to record
    memset(g_victims, 0, sizeof(g_victims)); g_nVictims = 0;
    g_failed = false; g_changed = false;
    if (!On(g_enable) || g_watchedPlayer) return;
    if (!addr::ReadStr(Id::game_map_name, g_map, sizeof(g_map)) || !g_map[0]) lstrcpyA(g_map, "unknown_stage");
    // Use the same safe, lower-case map identifiers as the standalone recorder.
    char* name = g_map;
    for (char* p = g_map; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    unsigned i = 0;
    for (; name[i] && name[i] != '.'; ++i) {
        char c = name[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        g_map[i] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
    }
    g_map[i] = 0;
    static char directory[kPathCap];
    // The same resolution as the replay's (console::GamePath, D51).
    if (!g_directory.value[0] || !console::GamePath(g_directory.value, directory, sizeof(directory))) { Fail("invalid output directory"); return; }
    if (!CreateDirectoryA(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { Fail("cannot create output directory"); return; }
    SYSTEMTIME utc; GetSystemTime(&utc);
    wsprintfA(g_path, "%s\\%s_%04u-%02u-%02uT%02u-%02u-%02u.%03uZ_pid%lu_run%04lu.jsonl",
        directory, g_map, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long>(++g_sequence));
    g_file = CreateFileA(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) { Fail("cannot create a new recording file"); return; }
    g_player = player; g_samples = 0; g_runTick = g_lastFlush = GetTickCount();
    g_trackVictims = g_victimAvailable && On(g_victimEnable);
    char timestamp[40]; wsprintfA(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth,
        utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    g_json.Begin(); g_json.Text("type", "start"); g_json.Text("schema", "fdny-trajectory-v1");
    g_json.Text("recorder_version", "1.1-core"); g_json.Text("core_version", FDCORE_VERSION);
    g_json.Text("stage", g_map); g_json.Text("started_utc", timestamp); g_json.UInt("run", g_sequence);
    g_json.Key("victim_tracking"); g_json.Raw(g_trackVictims ? "true" : "false");
    g_json.Text("coordinate_system", "Genesis3D original units; Y up");
    g_json.Text("sampling", "after player root update callback");
    g_json.Text("time_basis", "wall clock milliseconds");
    g_json.Text("input_basis", "foreground Windows virtual keys, sampled; not resolved game actions");
    if (Append() && Flush()) log::Info("recorder: started %s", g_path);
}

template<typename T> bool Read(void* p, Id field, T* out) {
    return addr::SafeRead(reinterpret_cast<uintptr_t>(p) + addr::Field(field), out, sizeof(T));
}
bool VectorOK(const float* v) { return Finite(v[0]) && Finite(v[1]) && Finite(v[2]); }
bool Bounds(const float* lo, const float* hi) {
    return VectorOK(lo) && VectorOK(hi) && lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2];
}
unsigned Keys() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return 0;
    const int keys[] = {'W','A','S','D',VK_UP,VK_LEFT,VK_DOWN,VK_RIGHT,'C',VK_CONTROL,VK_SPACE,VK_SHIFT,VK_LBUTTON,VK_RBUTTON,'1','2','3'};
    unsigned mask = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) if (GetAsyncKeyState(keys[i]) & 0x8000) mask |= 1u << i;
    return mask;
}
void VictimEnd(void* p) {
    for (unsigned i = 0; i < g_nVictims; ++i) if (g_victims[i].pointer == p && g_victims[i].alive) {
        g_victims[i].alive = false;
        char id[16]; wsprintfA(id, "v%03u", g_victims[i].id);
        g_json.Begin(); g_json.Text("type", "victim_end"); g_json.Text("id", id);
        g_json.UInt("t_ms", GetTickCount() - g_runTick); Append(); return;
    }
}
void ObserveVictim(void* p, bool force) {
    if (!g_trackVictims || g_file == INVALID_HANDLE_VALUE || g_failed) return;
    int state = 0; if (!Read(p, Id::victim_state, &state)) return;
    DWORD now = GetTickCount(); Victim* entry = nullptr;
    for (unsigned i = 0; i < g_nVictims; ++i) if (g_victims[i].pointer == p && g_victims[i].alive) { entry = &g_victims[i]; break; }
    if (entry && !force && entry->state == state && now - entry->lastTick < 50) return;
    float pos[3], forward[3], lo[3], hi[3];
    if (!Read(p, Id::victim_pos, &pos) || !Read(p, Id::entity_f_forward, &forward) ||
        !Read(p, Id::player_f_mins, &lo) || !Read(p, Id::player_f_maxs, &hi) ||
        !VectorOK(pos) || !VectorOK(forward) || !Bounds(lo, hi)) return;
    if (!entry) {
        if (g_nVictims == kVictimCap) return;
        entry = &g_victims[g_nVictims++]; entry->pointer = p; entry->id = g_nVictims; entry->alive = true;
        uintptr_t def = 0, name = 0;
        Read(p, Id::victim_def, &def);
        if (def) addr::SafeRead(def + addr::Field(Id::victim_def_name), &name, sizeof(name));
        unsigned i = 0;
        for (; name && i < sizeof(entry->name) - 1; ++i) {
            char c = 0; if (!addr::SafeRead(name + i, &c, 1) || !c) break;
            entry->name[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-') ? c : '_';
        }
        entry->name[i] = 0;
        if (!entry->name[0]) wsprintfA(entry->name, "Victim %u", entry->id);
    }
    entry->lastTick = now; entry->state = state;
    char id[16]; wsprintfA(id, "v%03u", entry->id);
    g_json.Begin(); g_json.Text("type", "victim_sample"); g_json.Text("id", id); g_json.Text("name", entry->name);
    g_json.UInt("t_ms", now - g_runTick); g_json.Vec("position", pos); g_json.Vec("body_forward", forward);
    g_json.Key("state_raw"); g_json.Int(state); g_json.Vec("bounds_min", lo); g_json.Vec("bounds_max", hi); Append();
}
void VictimSpawn(void* p) { ObserveVictim(p, true); }
void VictimUpdate(void* p) { ObserveVictim(p, false); }
void Sample(void* player) {
    if (g_watchedPlayer || state::WatchOnly()) return;
    if (g_changed) { End("configuration_changed"); g_failed = false; g_changed = false; }
    if (!On(g_enable) || g_failed) return;
    if (g_player != player) Start(player);
    if (g_file == INVALID_HANDLE_VALUE) return;
    float pos[3], vel[3], forward[3], pitch, dt, lo[3], hi[3];
    unsigned state, slot; unsigned char grounded;
    if (!Read(player, Id::player_f_pos, &pos) || !Read(player, Id::player_f_vel, &vel) ||
        !Read(player, Id::entity_f_forward, &forward) || !Read(player, Id::player_f_pitch, &pitch) ||
        !Read(player, Id::player_f_state, &state) || !Read(player, Id::player_f_tool_slot, &slot) ||
        !Read(player, Id::player_f_ground, &grounded) || !addr::ReadF32(Id::clock_dt, &dt) ||
        !VectorOK(pos) || !VectorOK(vel) || !VectorOK(forward) || !Finite(pitch) || !Finite(dt)) return;
    g_json.Begin(); g_json.Text("type", "sample"); g_json.UInt("n", g_samples);
    g_json.UInt("t_ms", GetTickCount() - g_runTick); g_json.Key("game_dt_s"); g_json.Number(dt);
    g_json.Vec("position", pos); g_json.Vec("velocity", vel); g_json.Vec("body_forward", forward);
    g_json.Key("pitch_deg"); g_json.Number(pitch); g_json.UInt("player_state_raw", state);
    g_json.UInt("ground_contact_raw", grounded); g_json.UInt("tool_slot_raw", slot); g_json.UInt("keys_mask", Keys());
    if (Read(player, Id::player_f_mins, &lo) && Read(player, Id::player_f_maxs, &hi) && Bounds(lo, hi)) {
        g_json.Vec("bounds_min", lo); g_json.Vec("bounds_max", hi);
    }
    if (!Append()) return;
    ++g_samples;
    if (g_trackVictims) for (unsigned i = 0; i < g_nVictims; ++i) if (g_victims[i].alive) {
        uintptr_t vptr = 0, expected = 0;
        if (!addr::SafeRead(reinterpret_cast<uintptr_t>(g_victims[i].pointer), &vptr, sizeof(vptr)) ||
            !addr::Resolve(Id::victim_vtable, &expected) || vptr != expected) VictimEnd(g_victims[i].pointer);
        else ObserveVictim(g_victims[i].pointer, false);
    }
}
void FrameEnd() {
    if (state::WatchOnly()) {
        if (g_file != INVALID_HANDLE_VALUE) End("watch_only");
        g_watchedPlayer = true;
        return;
    }
    if (g_changed) { End("configuration_changed"); g_failed = false; g_changed = false; }
    // Flush pending bytes even when the simulation is paused but rendering continues.
    if (g_file != INVALID_HANDLE_VALUE && g_buffered && GetTickCount() - g_lastFlush >= 1000) Flush();
}
void Early(const char* gameDir) { lstrcpynA(g_gameDir, gameDir, sizeof(g_gameDir)); }
const Id kNeeds[] = {Id::game_map_name, Id::clock_dt, Id::player_initialize, Id::player_root_update,
    Id::player_f_pos, Id::player_f_vel, Id::entity_f_forward, Id::player_f_pitch, Id::player_f_state,
    Id::player_f_ground, Id::player_f_tool_slot, Id::player_f_mins, Id::player_f_maxs};
bool Init() {
    static char standalone[kPathCap + 32];
    lstrcpyA(standalone, g_gameDir); lstrcatA(standalone, "FDNYRecorder.asi");
    if (GetModuleHandleA("FDNYRecorder.asi") || GetFileAttributesA(standalone) != INVALID_FILE_ATTRIBUTES) {
        log::Warn("recorder: standalone FDNYRecorder.asi present; move it out to use this feature"); return false;
    }
    cmd::RegisterCvar(&g_enable); cmd::RegisterCvar(&g_victimEnable); cmd::RegisterCvar(&g_directory);
    // These keys are observed, never intercepted or rebound.
    binds::DescribeKey("w a s d uparrow leftarrow downarrow rightarrow c ctrl space shift mouse1 mouse2 1 2 3",
        "sample raw virtual keys into JSONL (no input changes)", "while fd_record is 1 and the game has focus");
    const Id victimNeeds[] = {Id::victim_spawn, Id::victim_root_update, Id::victim_destroy, Id::victim_vtable,
        Id::victim_pos, Id::victim_state, Id::victim_def, Id::victim_def_name};
    bool valid = true;
    for (Id id : victimNeeds) if (!addr::Usable(id)) valid = false;
    g_victimAvailable = valid && entities::ObserveVictims(VictimSpawn, VictimUpdate, VictimEnd);
    if (!g_victimAvailable) log::Warn("recorder: victim observers unavailable; player recording remains enabled");
    if (!entities::ObservePlayer(Start, Sample)) { log::Warn("recorder: player observers unavailable"); return false; }
    hooks::OnFrameEnd(FrameEnd); hooks::OnProcessExit(Exit);
#ifdef FDCORE_DEBUG
    testpipe::Register("recorder", [](const char*, char* out, int) {
        wsprintfA(out, "active=%d samples=%lu runs=%lu victims=%u failed=%d", g_file != INVALID_HANDLE_VALUE,
            static_cast<unsigned long>(g_samples), static_cast<unsigned long>(g_sequence), g_nVictims, g_failed);
    });
#endif
    log::Info("recorder: ready (automatic JSONL; fd_record, fd_record_victims, fd_record_directory)");
    return true;
}
} // namespace
} // namespace fdcore::recorder

FDCORE_MODULE(recorder, "recorder", fdcore::recorder::kNeeds,
    sizeof(fdcore::recorder::kNeeds) / sizeof(fdcore::recorder::kNeeds[0]), &fdcore::recorder::Early,
    &fdcore::recorder::Init, 100)
