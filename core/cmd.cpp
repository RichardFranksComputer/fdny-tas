// Tests: tests/game/console_basic.yaml tests/game/autoexec.yaml tests/game/vars_binds_persist.yaml tests/game/docs_registry.yaml tests/unit/test_cmd.cpp
// User: Console -- FDNYCore's commands and variables, typed in the game's console (` key); changed settings are saved to FDNYCore.cfg, your own lines go in autoexec.cfg.
#include "cmd.h"

#include <windows.h>

#include "binds.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"

namespace fdcore::cmd {
namespace {

struct Command {
    const char* name;
    const char* help;
    CommandFn fn;
};

constexpr int kMaxCommands = 128;
constexpr int kMaxCvars = 256;
Command g_commands[kMaxCommands];
int g_nCommands = 0;
Cvar* g_cvars[kMaxCvars];
int g_nCvars = 0;

constexpr int kQueue = 64;
char g_queue[kQueue][256];
int g_qHead = 0, g_qTail = 0;
CRITICAL_SECTION g_qLock;
bool g_qInit = false;

bool IEq(const char* a, const char* b) { return lstrcmpiA(a, b) == 0; }

// Splits a line into whitespace-separated tokens; "quoted strings" stay whole.
int Tokenize(const char* line, char* buf, int cap, const char** argv) {
    int argc = 0, o = 0;
    const char* p = line;
    while (*p && argc < kMaxArgs) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        argv[argc++] = buf + o;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"' && o < cap - 1) buf[o++] = *p++;
            if (*p == '"') ++p;
        } else {
            while (*p && *p != ' ' && *p != '\t' && o < cap - 1) buf[o++] = *p++;
        }
        if (o < cap) buf[o++] = 0;
    }
    return argc;
}

Command* FindCommand(const char* name) {
    for (int i = 0; i < g_nCommands; ++i)
        if (IEq(g_commands[i].name, name)) return &g_commands[i];
    return nullptr;
}

// --- builtins --------------------------------------------------------------
// `toggle <var>`: 0 <-> 1, through the profile gate like any other set (a key bind's line).
void CmdToggle(int argc, const char* const* argv) {
    Cvar* v = argc > 1 ? FindCvar(argv[1]) : nullptr;
    if (!v) {
        console::Print("usage: toggle <variable> (0 <-> 1)");
        return;
    }
    if (SetCvar(v, v->value[0] == '0' ? "1" : "0")) console::Print("%s %s", v->name, v->value);
}

void CmdEcho(int argc, const char* const* argv) {
    char out[512];
    int o = 0;
    for (int i = 1; i < argc; ++i) {
        for (const char* s = argv[i]; *s && o < static_cast<int>(sizeof(out)) - 2; ++s) out[o++] = *s;
        if (i + 1 < argc && o < static_cast<int>(sizeof(out)) - 2) out[o++] = ' ';
    }
    out[o] = 0;
    console::Print("%s", out);
}

void CmdCvarlist(int, const char* const*) {
    for (int i = 0; i < g_nCvars; ++i) {
        const Cvar* v = g_cvars[i];
        console::Print("%s \"%s\"%s - %s", v->name, v->value, v->readOnly ? " (read-only)" : "", v->help);
    }
    console::Print("%d variable(s)", g_nCvars);
}

void CmdCmdlist(int, const char* const*) {
    for (int i = 0; i < g_nCommands; ++i) console::Print("%s - %s", g_commands[i].name, g_commands[i].help);
    console::Print("%d command(s)", g_nCommands);
}

// exec <file>: runs FDNYCore config files (<file> or <file>.cfg next to the
// game exe). If no such file exists the line is the game's own `exec` verb
// (Data\Scripts / Data\Maps), so it is handed back to the game untouched.
bool ExecFile(const char* name) {
    char path[MAX_PATH];
    wsprintfA(path, "%s%s", console::GameDir(), name);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        wsprintfA(path, "%s%s.cfg", console::GameDir(), name);
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (f == INVALID_HANDLE_VALUE) return false;
    static char buf[32 * 1024];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    console::Print("exec %s", path);
    char* p = buf;
    int lines = 0;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n' && *e != '\r') ++e;
        char save = *e;
        *e = 0;
        const char* s = p;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s && !(s[0] == '/' && s[1] == '/') && s[0] != '#') {
            if (!Dispatch(s)) QueueGameLine(s);
            ++lines;
        }
        *e = save;
        p = e;
        while (*p == '\n' || *p == '\r') ++p;
    }
    log::Info("exec: %s (%d line(s))", path, lines);
    return true;
}

bool g_loading = false;  // while FDNYCore.cfg itself is being executed: don't rewrite it

constexpr int kMaxWriters = 16;
ConfigWriter g_writers[kMaxWriters];
int g_nWriters = 0;

Cvar g_fdVersion = {"fd_version", "", "this build's FDNYCore version", true, false, nullptr, {0}};
Cvar g_fdLogCommands = {"fd_log_commands", "1", "1 = log every FDNYCore console command to FDNYCore.log; 0 = don't",
                        false, true, nullptr, {0}};

void CmdWriteConfig(int, const char* const*) {
    if (SaveConfig()) console::Print("wrote %s%s", console::GameDir(), kConfigName);
}

// Registered so cmdlist and the reference show it; Dispatch handles `exec` before
// the command table, so this only runs if that ever changes.
void CmdExec(int, const char* const*) { console::Print("usage: exec <file>  (FDNYCore .cfg next to the game exe)"); }

void OnHooksFrame(Cvar& v) {
    bool on = v.value[0] != '0';
    if (!hooks::SetEnabled("frame", on)) console::Print("fd_hooks_frame: frame hooks unavailable");
}
Cvar g_fdHooksFrame = {"fd_hooks_frame", "1", "1 = per-frame hooks on; 0 = off, which stops every per-frame feature (HUDs, keys, pause): "
                       "for debugging",
                       false, false, OnHooksFrame, {0}};

} // namespace

void RegisterConfigWriter(ConfigWriter fn) {
    if (g_nWriters < kMaxWriters) g_writers[g_nWriters++] = fn;
}

void RegisterCommand(const char* name, const char* help, CommandFn fn) {
    if (g_nCommands < kMaxCommands) g_commands[g_nCommands++] = {name, help, fn};
}

void RegisterCvar(Cvar* v) {
    if (g_nCvars >= kMaxCvars) return;
    lstrcpynA(v->value, v->defaultValue, sizeof(v->value));
    g_cvars[g_nCvars++] = v;
}

Cvar* FindCvar(const char* name) {
    for (int i = 0; i < g_nCvars; ++i)
        if (IEq(g_cvars[i]->name, name)) return g_cvars[i];
    return nullptr;
}

CvarGate g_cvarGate = nullptr;
CommandGate g_commandGate = nullptr;

void SetGates(CvarGate cvarGate, CommandGate commandGate) { g_cvarGate = cvarGate, g_commandGate = commandGate; }
namespace {
constexpr int kMaxPractice = 32;
const char* g_practice[kMaxPractice];
int g_nPractice = 0;
} // namespace
void SetPracticeOnly(const char* name) {
    if (g_nPractice < kMaxPractice && !IsPracticeOnly(name)) g_practice[g_nPractice++] = name;
}
bool IsPracticeOnly(const char* name) {
    for (int i = 0; i < g_nPractice; ++i)
        if (IEq(g_practice[i], name)) return true;
    return false;
}
#ifdef FDCORE_DEBUG
namespace {
// fd_dbg_registry: what this build registered, one tab-separated line per item, for
// tests/game/docs_registry.yaml to compare with tools/docgen.py's reading of the source.
void CmdRegistry(int, const char* const*) {
    static char buf[48 * 1024];
    int o = 0;
    const int cap = static_cast<int>(sizeof(buf)) - 600;
    for (int i = 0; i < g_nCvars && o < cap; ++i) {
        const Cvar* v = g_cvars[i];
        o += wsprintfA(buf + o, "cvar\t%s\t%s\t%d\t%d\t%d\t%s\r\n", v->name, v->defaultValue, v->readOnly ? 1 : 0,
                       v->archive ? 1 : 0, IsPracticeOnly(v->name) ? 1 : 0, v->help);
    }
    for (int i = 0; i < g_nCommands && o < cap; ++i)
        o += wsprintfA(buf + o, "cmd\t%s\t%d\t%s\r\n", g_commands[i].name, IsPracticeOnly(g_commands[i].name) ? 1 : 0,
                       g_commands[i].help);
    const char *a, *b, *c;
    for (int i = 0; binds::DefaultAt(i, &a, &b) && o < cap; ++i) o += wsprintfA(buf + o, "bind\t%s\t%s\r\n", a, b);
    for (int i = 0; binds::DescribedAt(i, &a, &b, &c) && o < cap; ++i)
        o += wsprintfA(buf + o, "key\t%s\t%s\t%s\r\n", a, b, c);
    // A module that turned itself off (e.g. window, on a signature mismatch) registered nothing.
    for (int i = 0; i < modules::Count() && o < cap; ++i)
        o += wsprintfA(buf + o, "module\t%s\t%d\r\n", modules::At(i)->name, modules::At(i)->enabled ? 1 : 0);
    char path[MAX_PATH];
    wsprintfA(path, "%sFDNYCore_registry.txt", console::GameDir());
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        console::Print("fd_dbg_registry: cannot write %s", path);
        return;
    }
    DWORD wrote = 0;
    WriteFile(f, buf, static_cast<DWORD>(o), &wrote, nullptr);
    CloseHandle(f);
    console::Print("fd_dbg_registry: wrote %s (%d bytes)", path, o);
}
} // namespace
#endif

int CvarCount() { return g_nCvars; }
Cvar* CvarAt(int i) { return i >= 0 && i < g_nCvars ? g_cvars[i] : nullptr; }

bool SetCvar(Cvar* v, const char* value) {
    if (v->readOnly) return false;
    if (g_cvarGate && !g_cvarGate(*v, value)) return false;
    return SetCvarUngated(v, value);
}

bool SetCvarUngated(Cvar* v, const char* value) {
    if (v->readOnly) return false;
    lstrcpynA(v->value, value, sizeof(v->value));
    if (v->onChange) v->onChange(*v);
    if (v->archive) SaveConfig();
    return true;
}

// FDNYCore.cfg: archived variables that differ from their defaults, then
// binds. Rewritten whole on every change (the game can be killed at any
// time, so there's no reliable exit to save on).
bool SaveConfig() {
    if (g_loading) return false;
    static char buf[32 * 1024];
    int o = wsprintfA(buf, "// FDNYCore settings -- written by FDNYCore, overwritten on change.\r\n"
                           "// Put your own commands in autoexec.cfg instead.\r\n");
    for (int i = 0; i < g_nCvars; ++i) {
        const Cvar* v = g_cvars[i];
        if (!v->archive || lstrcmpA(v->value, v->defaultValue) == 0) continue;
        if (o + 300 > static_cast<int>(sizeof(buf))) break;
        o += wsprintfA(buf + o, "%s \"%s\"\r\n", v->name, v->value);
    }
    o += binds::AppendConfig(buf + o, static_cast<int>(sizeof(buf)) - o);
    for (int i = 0; i < g_nWriters; ++i) o += g_writers[i](buf + o, static_cast<int>(sizeof(buf)) - o);
    char path[MAX_PATH];
    wsprintfA(path, "%s%s", console::GameDir(), kConfigName);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        log::Error("config: cannot write %s (%lu)", path, GetLastError());
        return false;
    }
    DWORD wrote = 0;
    WriteFile(f, buf, static_cast<DWORD>(o), &wrote, nullptr);
    CloseHandle(f);
    return true;
}

void ExecStartup() {
    g_loading = true;
    bool haveCfg = ExecFile(kConfigName);
    g_loading = false;
    bool haveAuto = ExecFile("autoexec.cfg");
    log::Info("config: startup (%s: %s, autoexec.cfg: %s)", kConfigName, haveCfg ? "loaded" : "none",
              haveAuto ? "ran" : "none");
}

bool Dispatch(const char* line) {
    char buf[512];
    const char* argv[kMaxArgs];
    int argc = Tokenize(line, buf, sizeof(buf), argv);
    if (argc == 0) return false;
    if (IEq(argv[0], "exec")) {
        if (argc < 2) {
            console::Print("usage: exec <file>  (FDNYCore .cfg next to the game exe)");
            return true;
        }
        return ExecFile(argv[1]);  // false -> the game's own exec verb
    }
    const bool logIt = g_fdLogCommands.value[0] != '0';
    if (Command* c = FindCommand(argv[0])) {
        if (logIt) log::Info("cmd: %s", line);
        if (g_commandGate && !g_commandGate(c->name)) return true;
        c->fn(argc, argv);
        return true;
    }
    if (Cvar* v = FindCvar(argv[0])) {
        if (logIt) log::Info("cmd: %s", line);
        if (argc == 1) {
            console::Print("%s is \"%s\" (default \"%s\") - %s", v->name, v->value, v->defaultValue, v->help);
        } else if (!SetCvar(v, argv[1]) && v->readOnly) {  // a gate refusal prints its own reason
            console::Print("%s is read-only", v->name);
        }
        return true;
    }
    if (argv[0][0] == 'f' && argv[0][1] == 'd' && argv[0][2] == '_') {
        // Our namespace: never let a mistyped fd_ command reach the game.
        console::Print("unknown command: %s", argv[0]);
        return true;
    }
    return false;
}

void RegisterBuiltins() {
    if (!g_qInit) {
        InitializeCriticalSection(&g_qLock);
        g_qInit = true;
    }
    RegisterCommand("exec", "exec <file>: run an FDNYCore .cfg next to the game exe (FDNYCore.cfg and autoexec.cfg run "
                            "at startup); a file not found there goes to the game's own exec",
                    CmdExec);
    RegisterCommand("echo", "echo <text>: print text to the console", CmdEcho);
    RegisterCommand("toggle", "toggle <variable>: switch a 0/1 variable (for key binds)", CmdToggle);
    RegisterCommand("cvarlist", "list every FDNYCore variable with its value and help", CmdCvarlist);
    RegisterCommand("cmdlist", "list every FDNYCore command with its help", CmdCmdlist);
    RegisterCvar(&g_fdVersion);
    lstrcpynA(g_fdVersion.value, console::CoreVersion(), sizeof(g_fdVersion.value));
    RegisterCvar(&g_fdHooksFrame);
    RegisterCvar(&g_fdLogCommands);
    RegisterCommand("fd_writeconfig", "write FDNYCore.cfg now (it is also written on every change)", CmdWriteConfig);
#ifdef FDCORE_DEBUG
    RegisterCommand("fd_dbg_registry", "(Debug test hook) write FDNYCore_registry.txt: every variable, command, "
                                       "default bind and described key",
                    CmdRegistry);
#endif
}

void QueueGameLine(const char* line) {
    EnterCriticalSection(&g_qLock);
    int next = (g_qTail + 1) % kQueue;
    if (next != g_qHead) {
        lstrcpynA(g_queue[g_qTail], line, sizeof(g_queue[0]));
        g_qTail = next;
    } else {
        log::Warn("cmd: game-line queue full; dropped: %s", line);
    }
    LeaveCriticalSection(&g_qLock);
}

bool PopGameLine(char* out, int cap) {
    EnterCriticalSection(&g_qLock);
    bool have = g_qHead != g_qTail;
    if (have) {
        lstrcpynA(out, g_queue[g_qHead], cap);
        g_qHead = (g_qHead + 1) % kQueue;
    }
    LeaveCriticalSection(&g_qLock);
    return have;
}

} // namespace fdcore::cmd
