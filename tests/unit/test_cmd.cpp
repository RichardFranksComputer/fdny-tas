// cmd + binds: how a console line is split and run, toggle, the profile gates,
// exec, and what FDNYCore.cfg holds (only changes: archived variables off their default,
// binds that differ from a feature's default key, `unbind` for a removed default, `unbindall`),
// `cmdlist`. The
// game tests keep what needs the game: keys pressed in a level, the console window, a
// restart (vars_binds_persist, keys_default, profile_run).
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../../core/binds.h"
#include "../../core/cmd.h"
#include "../../core/num.h"
#include "unit.h"

namespace cmd = fdcore::cmd;
namespace binds = fdcore::binds;

namespace {
char g_dir[MAX_PATH];
int g_calls = 0, g_argc = 0;
char g_argv[4][64];

void Probe(int argc, const char* const* argv) {
    ++g_calls, g_argc = argc;
    for (int i = 0; i < argc && i < 4; ++i) snprintf(g_argv[i], sizeof(g_argv[i]), "%s", argv[i]);
}
int Writer(char* out, int cap) { return snprintf(out, cap, "fd_t_written \"by a module\"\r\n"); }

cmd::Cvar g_a = {"fd_t_a", "0", "archived", false, true, nullptr, {0}};
cmd::Cvar g_b = {"fd_t_b", "1", "not archived", false, false, nullptr, {0}};
cmd::Cvar g_p = {"fd_t_p", "0", "gated", false, true, nullptr, {0}};
cmd::Cvar g_on = {"fd_t_on", "0", "on / off", false, false, nullptr, {0}, cmd::T_BOOL};
cmd::Cvar g_int = {"fd_t_int", "9", "7-16", false, false, nullptr, {0}, cmd::T_INT, 7, 16};
cmd::Cvar g_float = {"fd_t_float", "0.075", "0.001-10", false, false, nullptr, {0}, cmd::T_FLOAT, 0.001f, 10};

bool CvarGate(const cmd::Cvar& v, const char*) { return strcmp(v.name, "fd_t_p") != 0; }
bool CommandGate(const char* name) { return strcmp(name, "fd_t_cmd") != 0; }

const char* Path(const char* name) {
    static char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s%s", g_dir, name);
    return p;
}
const char* ReadText(const char* name) {
    static char text[8192];
    text[0] = 0;
    FILE* f = fopen(Path(name), "rb");
    if (!f) return text;
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    return text;
}
void WriteText(const char* name, const char* text) {
    FILE* f = fopen(Path(name), "wb");
    if (f) fputs(text, f), fclose(f);
}
bool CfgHas(const char* text) { return strstr(ReadText(cmd::kConfigName), text) != nullptr; }
} // namespace

void TestCmdAndBinds() {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof(tmp), tmp);
    snprintf(g_dir, sizeof(g_dir), "%sfdcore_unit_cmd_%lu\\", tmp, GetCurrentProcessId());
    CreateDirectoryA(g_dir, nullptr);
    DeleteFileA(Path(cmd::kConfigName));
    DeleteFileA(Path("autoexec.cfg"));
    unit::SetGameDir(g_dir);
    unit::ClearLog();

    cmd::RegisterBuiltins();
    binds::Init();
    cmd::RegisterCommand("fd_t_cmd", "probe", Probe);
    cmd::RegisterCvar(&g_a), cmd::RegisterCvar(&g_b), cmd::RegisterCvar(&g_p);
    cmd::RegisterConfigWriter(Writer);
    CHECK(binds::BindDefault("j", "toggle fd_t_a"));
    CHECK(!binds::BindDefault("j", "echo taken"));       // a key already bound
    CHECK(!binds::BindDefault("nosuchkey", "echo x"));

    // Splitting: whitespace, tabs, "quoted words" stay whole; names in any case.
    CHECK(cmd::Dispatch("  FD_T_CMD one\t\"two words\"  three"));
    CHECK(g_calls == 1 && g_argc == 4 && strcmp(g_argv[2], "two words") == 0 && strcmp(g_argv[3], "three") == 0);
    CHECK(!cmd::Dispatch("   "));
    // Not ours: back to the game; a mistyped fd_ name never is.
    CHECK(!cmd::Dispatch("loadmap bank"));
    CHECK(cmd::Dispatch("fd_nosuch 1") && unit::LogContains("console> unknown command: fd_nosuch"));

    // Variables: show, set, toggle, read-only.
    CHECK(cmd::Dispatch("fd_t_a") && unit::LogContains("console> fd_t_a is \"0\" (default \"0\") - archived"));
    CHECK(cmd::Dispatch("fd_t_a 1") && strcmp(g_a.value, "1") == 0);
    CHECK(cmd::Dispatch("toggle fd_t_a") && strcmp(g_a.value, "0") == 0);
    CHECK(cmd::Dispatch("toggle FD_T_A") && strcmp(g_a.value, "1") == 0);
    CHECK(cmd::Dispatch("fd_version 9") && unit::LogContains("console> fd_version is read-only"));
    CHECK(strcmp(cmd::FindCvar("fd_version")->value, "0.0.0-unit") == 0);

    // Typed variables: a value of the wrong kind or out of range is refused with what the
    // variable takes, and the old value stays; Bool / Int / Float read it.
    cmd::RegisterCvar(&g_on), cmd::RegisterCvar(&g_int), cmd::RegisterCvar(&g_float);
    CHECK(cmd::Dispatch("fd_t_on 2") && strcmp(g_on.value, "0") == 0 && unit::LogContains("console> fd_t_on takes 0 or 1"));
    CHECK(cmd::Dispatch("fd_t_on yes") && cmd::Dispatch("fd_t_on 01") && !cmd::Bool(g_on));
    CHECK(cmd::Dispatch("fd_t_on 1") && cmd::Bool(g_on) && cmd::Dispatch("toggle fd_t_on") && !cmd::Bool(g_on));
    CHECK(cmd::Dispatch("fd_t_int 17") && cmd::Int(g_int) == 9 &&
          unit::LogContains("console> fd_t_int takes a whole number, 7 to 16"));
    CHECK(cmd::Dispatch("fd_t_int 7.5") && cmd::Dispatch("fd_t_int big") && cmd::Int(g_int) == 9);
    CHECK(cmd::Dispatch("fd_t_int 16") && cmd::Int(g_int) == 16);
    CHECK(cmd::Dispatch("fd_t_float 11") && cmd::Float(g_float) == 0.075f &&
          unit::LogContains("console> fd_t_float takes a number, 0.001 to 10"));
    CHECK(cmd::Dispatch("fd_t_float -1") && cmd::Dispatch("fd_t_float 1.2.3") && cmd::Float(g_float) == 0.075f);
    CHECK(cmd::Dispatch("fd_t_float 2.5") && cmd::Float(g_float) == 2.5f);
    CHECK(!cmd::SetCvarUngated(&g_int, "99") && cmd::Int(g_int) == 16);  // a profile restore can't bypass it
    // `toggle` confirms on screen with the variable's label; one without a label stays quiet.
    cmd::SetLabel("fd_t_on", "Probe");
    CHECK(strcmp(cmd::Label("FD_T_ON"), "Probe") == 0 && !cmd::Label("fd_t_a"));
    CHECK(cmd::Dispatch("toggle fd_t_on") && unit::LogContains("notify> Probe on"));
    CHECK(cmd::Dispatch("toggle fd_t_on") && unit::LogContains("notify> Probe off"));
    CHECK(cmd::Dispatch("toggle fd_t_a") && cmd::Dispatch("toggle fd_t_a") && unit::LogCount("notify> ") == 2);
    cmd::SetOptions("fd_t_int", "Low|High");
    CHECK(strcmp(cmd::Options("FD_T_INT"), "Low|High") == 0 && !cmd::Options("fd_t_on"));
    CHECK(cmd::Dispatch("fd_t_b anything goes"));                         // untyped: text

    // Numbers as text (num.h): what every panel and console line prints.
    char n[24];
    CHECK(fdcore::num::Fixed(n, -123.44f, 1) == 6 && strcmp(n, "-123.4") == 0);
    CHECK(fdcore::num::Fixed(n, 0.05f, 1) && strcmp(n, "0.1") == 0);
    CHECK(fdcore::num::Fixed(n, -0.04f, 1) && strcmp(n, "0.0") == 0);
    CHECK(fdcore::num::Fixed(n, 2.002f, 6) && strncmp(n, "2.00", 4) == 0 && strlen(n) == 8);
    CHECK(fdcore::num::Fixed(n, 119.6f, 0) && strcmp(n, "120") == 0);
    CHECK(fdcore::num::Short(n, 0.075f) && strcmp(n, "0.075") == 0);
    CHECK(fdcore::num::Short(n, 400.0f) && strcmp(n, "400") == 0);
    float parsed = 0;
    CHECK(fdcore::num::Parse("-12.5", &parsed) && parsed == -12.5f && fdcore::num::Parse(".5", &parsed));
    CHECK(!fdcore::num::Parse("", &parsed) && !fdcore::num::Parse("1x", &parsed) && !fdcore::num::Parse("-", &parsed));

    // Practice-only declarations and the gates (the profile installs them).
    cmd::SetPracticeOnly("fd_t_p");
    cmd::SetPracticeOnly("fd_t_p");  // once only
    CHECK(cmd::IsPracticeOnly("FD_T_P") && !cmd::IsPracticeOnly("fd_t_a"));
    cmd::SetGates(CvarGate, CommandGate);
    CHECK(cmd::Dispatch("fd_t_p 1") && strcmp(g_p.value, "0") == 0);
    CHECK(cmd::Dispatch("fd_t_cmd x") && g_calls == 1);
    CHECK(cmd::Dispatch("toggle fd_t_p") && strcmp(g_p.value, "0") == 0);  // a bind's toggle goes through the gate too
    CHECK(cmd::SetCvarUngated(&g_p, "1") && strcmp(g_p.value, "1") == 0);  // a profile switch restoring a value
    cmd::SetGates(nullptr, nullptr);
    CHECK(cmd::Dispatch("fd_t_p 0") && cmd::Dispatch("fd_t_cmd") && g_calls == 2);

    // FDNYCore.cfg: archived variables off their default only, then binds, then module lines.
    CHECK(cmd::Dispatch("fd_t_b 5"));  // not archived
    CHECK(cmd::SaveConfig());
    CHECK(CfgHas("fd_t_a \"1\"") && !CfgHas("fd_t_b") && !CfgHas("fd_t_p") && !CfgHas("fd_log_commands"));
    CHECK(CfgHas("fd_t_written \"by a module\""));
    CHECK(!CfgHas("bind j"));  // a default key on its default line is not written
    CHECK(cmd::Dispatch("bind j \"echo changed\"") && CfgHas("bind j \"echo changed\""));
    CHECK(cmd::Dispatch("bind J toggle fd_t_a") && !CfgHas("bind j"));  // back to the default: gone again
    CHECK(cmd::Dispatch("unbind j") && CfgHas("unbind j"));            // a removed default stays removed
    CHECK(cmd::Dispatch("bind f5 echo a b") && CfgHas("bind f5 \"echo a b\""));
    CHECK(cmd::Dispatch("bind w \"echo w\"") && unit::LogContains("note: the game also uses w"));
    CHECK(cmd::Dispatch("bind nosuchkey x") && unit::LogContains("bind: unknown key \"nosuchkey\""));
    CHECK(cmd::Dispatch("unbind w") && cmd::Dispatch("unbind f5") && !CfgHas("bind f5") && !CfgHas("bind w"));
    const char *key, *line;
    CHECK(binds::DefaultCount() == 1 && binds::DefaultAt(0, &key, &line) && strcmp(line, "toggle fd_t_a") == 0);
    // unbindall: every bind goes, a default key's too (kept unbound in FDNYCore.cfg).
    CHECK(cmd::Dispatch("bind j \"toggle fd_t_a\"") && cmd::Dispatch("bind f6 echo x") && CfgHas("bind f6"));
    CHECK(cmd::Dispatch("unbindall") && CfgHas("unbind j") && !CfgHas("bind f6"));
    CHECK(cmd::Dispatch("cmdlist") && unit::LogContains("console> fd_t_cmd - probe"));

    // exec: our lines run here, the game's are queued for it one per frame, comments skipped.
    WriteText("t_exec.cfg", "// a comment\r\n# another\r\n  fd_t_b 7\r\n\r\nloadmap bank\nfd_t_cmd z\n");
    CHECK(cmd::Dispatch("exec t_exec"));  // .cfg added
    CHECK(strcmp(g_b.value, "7") == 0 && g_calls == 3);
    char queued[256];
    CHECK(cmd::PopGameLine(queued, sizeof(queued)) && strcmp(queued, "loadmap bank") == 0);
    CHECK(!cmd::PopGameLine(queued, sizeof(queued)));
    CHECK(!cmd::Dispatch("exec no_such_file"));  // the game's own exec verb

    // Startup: FDNYCore.cfg runs without rewriting itself, then autoexec.cfg.
    WriteText(cmd::kConfigName, "fd_t_a \"0\"\r\n// kept: the file is not rewritten while it loads\r\n");
    WriteText("autoexec.cfg", "fd_t_b 3\r\n");
    unit::ClearLog();
    cmd::ExecStartup();
    CHECK(strcmp(g_a.value, "0") == 0 && strcmp(g_b.value, "3") == 0);
    CHECK(CfgHas("// kept"));
    CHECK(unit::LogContains("config: startup (FDNYCore.cfg: loaded, autoexec.cfg: ran)"));
    CHECK(cmd::Dispatch("fd_t_a 1") && !CfgHas("// kept"));  // after startup, a change writes it

    DeleteFileA(Path("t_exec.cfg"));
    DeleteFileA(Path("autoexec.cfg"));
    DeleteFileA(Path(cmd::kConfigName));
    RemoveDirectoryA(g_dir);
}
