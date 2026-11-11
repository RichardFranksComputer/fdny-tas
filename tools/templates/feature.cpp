// Tests: tests/game/<name>.yaml
// <name> feature (<owner request / backlog item, date>): <one paragraph: what it does
// for the player, and how it works in the game -- the hooked/patched sites by
// addresses.json name, never by address>.
//
// Copy to features/<name>/<name>.cpp (one file, or <name>_*.cpp for a group), replace
// every <...>, delete what you don't use. Rules: docs/architecture.md. Why: docs/decisions.md.
#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::<name> {
namespace {

using addr::Id;

// Every global constant-initialisable (rule R7): no constructors run in this DLL.
int g_panel = -1;

void OnToggle(cmd::Cvar& v) {
    textdraw::Show(g_panel, cmd::Bool(v));
}
// Typed (cmd.h): T_BOOL, or `{0}, cmd::T_INT, min, max` / `cmd::T_FLOAT, min, max`; read with
// cmd::Bool / Int / Float. The console refuses anything else, so never parse or clamp here.
cmd::Cvar g_fdEnable = {"fd_<name>", "0", "1 = <what it shows / does>; 0 = off", false, true, OnToggle, {0}, cmd::T_BOOL};

void CmdDoThing(int argc, const char* const* argv) {
    (void)argc, (void)argv;
    console::Print("<name>: did the thing");
}

// Frame end: update state / panel text here; textdraw draws later (OnFrameEndLate).
void FrameEnd() {
    if (!cmd::Bool(g_fdEnable)) return;
    const char* lines[] = {"<line 1>", "<line 2>"};
    textdraw::SetLines(g_panel, lines, 2);
}

// Address-table entries this module needs (also listed in FDCORE_MODULE below:
// the core disables the module, logging why, if any signature doesn't verify).
const Id kNeeds[] = {Id::game_state};

bool Init() {
    cmd::RegisterCvar(&g_fdEnable);
    cmd::SetLabel("fd_<name>", "<Short name>");  // `toggle` then confirms on screen: "<Short name> on"
    cmd::RegisterCommand("fd_<name>_do", "<help line>", CmdDoThing);
    cmd::SetPracticeOnly("fd_<name>");  // only if it shows information the game doesn't, or moves the player
    g_panel = textdraw::Create("<name>", textdraw::TOP_LEFT, 8, 160, 1, textdraw::C_TEXT);
    textdraw::Show(g_panel, false);
    hooks::OnFrameEnd(FrameEnd);
#ifdef FDCORE_DEBUG
    // Only if a test needs to read state the log / memory can't show.
    testpipe::Register("<name>", [](const char*, char* resp, int cap) { (void)cap; wsprintfA(resp, "ok"); });
#endif
    log::Info("<name>: ready (fd_<name>)");
    return true;
}

} // namespace
} // namespace fdcore::<name>

// id, log name, required Ids, count, DllMain-phase step (or nullptr), init, start order
FDCORE_MODULE(<name>, "<name>", fdcore::<name>::kNeeds, 1, nullptr, &fdcore::<name>::Init, 70)
