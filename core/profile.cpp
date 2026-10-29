// Tests: tests/game/profile_run.yaml
// User: Profiles -- practice (everything allowed) or run (practice-only settings refused, watermark always on).
#include "profile.h"

#include <windows.h>
#include <tlhelp32.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::profile {
namespace {

using addr::Id;

// Practice-only: anything that shows information the game doesn't, or moves the
// player. Each feature declares its own (cmd::SetPracticeOnly); the profile only
// enforces. Agent's default classification (item 12), for the owner to confirm;
// speedrun-category mods (static spawns, RescueBot) are allowed in `run` and
// listed on the watermark instead.

bool IEq(const char* a, const char* b) { return lstrcmpiA(a, b) == 0; }

bool g_run = false;

// The practice values of practice-only variables while `run` is active.
struct Saved {
    char name[48];
    char value[128];
};
Saved g_saved[16];
int g_nSaved = 0;

void Remember(const char* name, const char* value) {
    for (int i = 0; i < g_nSaved; ++i)
        if (IEq(g_saved[i].name, name)) {
            lstrcpynA(g_saved[i].value, value, sizeof(g_saved[i].value));
            return;
        }
    if (g_nSaved < 16) {
        lstrcpynA(g_saved[g_nSaved].name, name, sizeof(g_saved[g_nSaved].name));
        lstrcpynA(g_saved[g_nSaved].value, value, sizeof(g_saved[g_nSaved].value));
        ++g_nSaved;
    }
}

bool CvarGate(const cmd::Cvar& v, const char* value) {
    if (!g_run || !cmd::IsPracticeOnly(v.name) || lstrcmpA(value, v.defaultValue) == 0) return true;
    console::Print("%s is practice-only (fd_profile is run)", v.name);
    log::Info("profile: refused %s \"%s\" in run", v.name, value);
    return false;
}

bool CommandGate(const char* name) {
    if (!g_run || !cmd::IsPracticeOnly(name)) return true;
    console::Print("%s is practice-only (fd_profile is run)", name);
    log::Info("profile: refused %s in run", name);
    return false;
}

void ExecProfileFile(const char* name) {
    char path[MAX_PATH], line[64];
    wsprintfA(path, "%sprofile_%s.cfg", console::GameDir(), name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
    wsprintfA(line, "exec profile_%s.cfg", name);
    cmd::Dispatch(line);
}

void OnProfile(cmd::Cvar& v) {
    const bool run = IEq(v.value, "run");
    if (!run && !IEq(v.value, "practice")) {
        console::Print("fd_profile: run or practice");
        cmd::SetCvarUngated(&v, g_run ? "run" : "practice");
        return;
    }
    if (run == g_run) return;
    g_run = run;
    for (int k = 0; k < cmd::CvarCount(); ++k) {
        cmd::Cvar* c = cmd::CvarAt(k);
        if (!c || !cmd::IsPracticeOnly(c->name)) continue;
        if (run) {
            if (lstrcmpA(c->value, c->defaultValue) == 0) continue;
            Remember(c->name, c->value);
            cmd::SetCvarUngated(c, c->defaultValue);
        } else {
            for (int i = 0; i < g_nSaved; ++i)
                if (IEq(g_saved[i].name, c->name)) cmd::SetCvarUngated(c, g_saved[i].value);
        }
    }
    if (!run) g_nSaved = 0;
    log::Info("profile: %s", run ? "run" : "practice");
    console::Print("fd_profile %s%s", run ? "run" : "practice",
                   run ? ": practice-only settings off until fd_profile practice" : "");
    ExecProfileFile(run ? "run" : "practice");
    cmd::SaveConfig();  // fd_profile isn't archived itself (see WriteConfig); no-op while loading
}

// Not archived itself: written last by WriteConfig, after the remembered set,
// so loading FDNYCore.cfg restores both in the right order.
cmd::Cvar g_fdProfile = {"fd_profile", "practice",
                         "practice = everything allowed; run = practice-only settings switched off and refused, watermark "
                         "always on (back to practice restores your values)",
                         false, false, OnProfile, {0}};
cmd::Cvar g_fdWatermark = {"fd_watermark", "1", "1 = show the watermark (bottom-left: version, profile, non-default "
                           "settings, other plugins); 0 = hide it (practice only: always on in run)",
                           false, true, nullptr, {0}};

// fd_profile_saved <var> <value>: a practice value remembered while in run.
void CmdSaved(int argc, const char* const* argv) {
    if (argc < 3) return;
    if (cmd::IsPracticeOnly(argv[1])) Remember(argv[1], argv[2]);
}

int WriteConfig(char* out, int cap) {
    int o = 0;
    for (int i = 0; i < g_nSaved && o + 200 < cap; ++i)
        o += wsprintfA(out + o, "fd_profile_saved %s \"%s\"\r\n", g_saved[i].name, g_saved[i].value);
    if (g_run && o + 40 < cap) o += wsprintfA(out + o, "fd_profile \"run\"\r\n");
    return o;
}

// --- watermark ---------------------------------------------------------------------
int g_panel = -1;          // in-frame text panel (textdraw)
char g_mods[256];          // other .asi modules, found once
char g_text[512];          // the drawn line
CRITICAL_SECTION g_lock;  // the pipe thread reads g_text

void FindMods() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    // Skip ourselves by handle, not by name: releases are FDNYCore-<version>.asi.
    // An inert second copy (core.cpp) is listed, which shows it's installed.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&FindMods), &self);
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int o = 0;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        const int len = lstrlenA(me.szModule);
        if (len < 4 || lstrcmpiA(me.szModule + len - 4, ".asi") != 0 || me.hModule == self)
            continue;
        if (o + len + 2 >= static_cast<int>(sizeof(g_mods))) break;
        if (o) g_mods[o++] = ' ';
        lstrcpynA(g_mods + o, me.szModule, len - 4 + 1);  // without ".asi"
        o += len - 4;
    }
    CloseHandle(snap);
}

void Build(char* out, int cap) {
    int o = wsprintfA(out, "FDNYCore %s %s", console::CoreVersion(), g_run ? "RUN" : "practice");
    for (int i = 0; i < cmd::CvarCount(); ++i) {
        const cmd::Cvar* v = cmd::CvarAt(i);
        if (!v || v->readOnly || v == &g_fdProfile || v == &g_fdWatermark) continue;
        if (lstrcmpA(v->value, v->defaultValue) == 0) continue;
        if (o + lstrlenA(v->name) + lstrlenA(v->value) + 4 >= cap) break;
        o += wsprintfA(out + o, " | %s %s", v->name + (v->name[0] == 'f' && v->name[2] == '_' ? 3 : 0), v->value);
    }
    if (g_mods[0] && o + lstrlenA(g_mods) + 8 < cap) wsprintfA(out + o, " | asi: %s", g_mods);
}

void Draw() {
    const bool on = g_run || g_fdWatermark.value[0] != '0';
    textdraw::Show(g_panel, on);
    if (!on) return;
    static char text[512];
    Build(text, sizeof(text));
    // Bottom-left, above the game's tool icons; re-rendered only when it changes.
    // Too wide for the view (100 cells = 600 px at 640): the asi list goes on a
    // second line so nothing is cut off.
    static char first[512];
    const char* rows[2] = {text, nullptr};
    int n = 1;
    const char* asi = nullptr;
    for (const char* s = text; *s; ++s)
        if (s[0] == ' ' && s[1] == '|' && s[2] == ' ' && s[3] == 'a' && s[4] == 's' && s[5] == 'i') asi = s;
    if (lstrlenA(text) > 100 && asi) {
        lstrcpynA(first, text, static_cast<int>(asi - text) + 1);
        rows[0] = first, rows[1] = asi + 3, n = 2;  // "asi: ..."
    }
    textdraw::SetLines(g_panel, rows, n);
    EnterCriticalSection(&g_lock);
    lstrcpynA(g_text, text, sizeof(g_text));
    LeaveCriticalSection(&g_lock);
}

} // namespace

bool Init() {
    InitializeCriticalSection(&g_lock);
    cmd::RegisterCvar(&g_fdProfile);
    cmd::RegisterCvar(&g_fdWatermark);
    cmd::SetPracticeOnly("fd_watermark");  // turning it off is refused in run
    cmd::RegisterCommand("fd_profile_saved", "internal: a practice setting remembered while fd_profile is run",
                         CmdSaved);
    cmd::RegisterConfigWriter(WriteConfig);
    cmd::SetGates(CvarGate, CommandGate);
    FindMods();
    // Opaque (owner: the watermark stays on the decal path; it must show in menus too).
    // Pinned to the bottom-left corner (owner 2026-10-23; was 8 px in, 60 px up, clear of the
    // game's bottom bar, which it now overlaps at its left end).
    g_panel = textdraw::Create("watermark", textdraw::BOTTOM_LEFT, 0, 0, 1, textdraw::C_WHITE, textdraw::STYLE_OPAQUE);
    hooks::OnFrameEnd(Draw);
#ifdef FDCORE_DEBUG
    testpipe::Register("watermark", [](const char*, char* resp, int cap) { LastWatermark(resp, cap); });
#endif
    log::Info("profile: ready (fd_profile, fd_watermark); other asi: %s", g_mods[0] ? g_mods : "none");
    return true;
}

int LastWatermark(char* out, int cap) {
    EnterCriticalSection(&g_lock);
    lstrcpynA(out, g_text, cap);
    LeaveCriticalSection(&g_lock);
    return lstrlenA(out);
}

} // namespace fdcore::profile
