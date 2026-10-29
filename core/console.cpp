// Tests: tests/game/console_basic.yaml tests/game/console_menu_safe.yaml
#include "console.h"

#include <windows.h>
#include <stdarg.h>

#include "addr.h"
#include "binds.h"
#include "cmd.h"
#include "hooks.h"
#include "log.h"
#include "patches.h"

namespace fdcore::console {
namespace {

using addr::Id;

using StrHandle = uint32_t;
using ExecLineFn = void(__thiscall*)(void* engine, StrHandle line);
using AddLineFn = void(__thiscall*)(void* console, StrHandle line);

ExecLineFn g_origExecLine = nullptr;
char g_gameDir[MAX_PATH];
char g_coreVersion[32];
bool g_ready = false;
bool g_checked = false;

void* GameConsole() {
    uintptr_t a;
    uint32_t p = 0;
    if (!addr::Resolve(Id::mainwindow_console, &a) || !addr::SafeRead(a, &p, 4)) return nullptr;
    return reinterpret_cast<void*>(p);
}

void __fastcall ExecLineDetour(void* engine, void* /*edx*/, StrHandle line) {
    const char* text = reinterpret_cast<const char*>(line + 0xC);
    if (cmd::Dispatch(text)) {
        ReleaseGameString(line);  // ours: the game never sees it
        return;
    }
    g_origExecLine(engine, line);
}

// The game creates the console at startup (patches::ApplyEarly removes the
// ImADeveloper gate). The core never creates it: calling the ctor from a
// frame crashed (it needs the root object g_everGame, which doesn't exist yet
// for the first frames rendered before roc_CreateEngineSubsystems). Instead,
// log once when the console appears, and warn if it's still missing when the
// first level loads.
void CheckConsole(bool final) {
    if (g_checked) return;
    uint32_t mw = 0, global = 0;
    uintptr_t a;
    if (addr::Resolve(Id::mainwindow_console, &a)) addr::SafeRead(a, &mw, 4);
    if (addr::Resolve(Id::console_instance, &a)) addr::SafeRead(a, &global, 4);
    if (!mw && !final) return;
    g_checked = true;
    if (!mw) log::Warn("console: the game has no console (gate patch %s)",
                       patches::ConsoleGatePatched() ? "applied but no console" : "not applied");
    else if (mw != global) log::Warn("console: MainWindow+0x70 %08lx != g_console %08lx",
                                     static_cast<unsigned long>(mw), static_cast<unsigned long>(global));
    else log::Info("console: game console present at %08lx", static_cast<unsigned long>(mw));
    // FDNYCore.cfg + autoexec.cfg, once, on the game thread, with the console up.
    cmd::ExecStartup();
}

using FindTextureFn = int(__thiscall*)(void* render, int id, int logMiss);

// Is the console font registered with the renderer? Same lookup Console::Render
// does (0x00427F47) before it raises "Console font texture not loaded".
bool FontLoaded() {
    uintptr_t field;
    uint32_t render = 0;
    int32_t id = 0;
    if (!addr::Resolve(Id::everGame_render, &field) || !addr::SafeRead(field, &render, 4) || !render) return false;
    if (!addr::ReadI32(Id::console_font_id, &id)) return false;
    return reinterpret_cast<FindTextureFn>(addr::ExportAddress(Id::gfx_find_texture))(
               reinterpret_cast<void*>(render), id, 0) != 0;
}

// Game bug: its console font is only registered while a level loads
// (ROCRender::LoadWorldGeometry -> Console::LoadFont), and Console::Render
// raises a FATAL ERROR if the console is open without it -- e.g. opened at the
// main menu (reproduced on the unmodified game in developer mode). Render runs
// after geEngine_BeginFrame in RenderFrame, so close it again here first, and
// release the game input the console's open path blocked.
void GuardFont() {
    int32_t state = 0;
    if (!addr::ReadI32(Id::console_state, &state) || state == 0) return;
    if (FontLoaded()) return;
    addr::WriteI32(Id::console_state, 0);
    addr::WriteU8(Id::controls_block_input, 0);
    Print("console: not available until a level has loaded (the game loads the console font with the level)");
}

void FrameBegin() {
    CheckConsole(false);
    GuardFont();
}
void LevelLoad(const char*) { CheckConsole(true); }

void FrameEnd() {
    // exec'd lines that belong to the game: one per frame (its script engine
    // keeps a single pending line).
    char line[256];
    if (g_origExecLine && cmd::PopGameLine(line, sizeof(line))) {
        uintptr_t engine = 0;
        addr::Resolve(Id::script_engine, &engine);
        log::Info("cmd: to game: %s", line);
        g_origExecLine(reinterpret_cast<void*>(engine), MakeGameString(line));
    }
}

const Id kNeeds[] = {Id::con_exec_line, Id::con_add_line, Id::str_from_cstr, Id::str_free_rep,
                     Id::str_empty_rep, Id::script_engine, Id::mainwindow_console, Id::gfx_find_texture,
                     Id::everGame_render, Id::console_font_id, Id::controls_block_input, Id::console_state,
                     Id::console_instance};

} // namespace

uint32_t MakeGameString(const char* text) {
    uint32_t string = 0;
    using FromCstr = void(__thiscall*)(uint32_t*, const char*);
    reinterpret_cast<FromCstr>(addr::ExportAddress(Id::str_from_cstr))(&string, text);
    return string;
}

void ReleaseGameString(uint32_t string) {
    const LONG refs = InterlockedDecrement(reinterpret_cast<volatile LONG*>(string));
    if (refs <= 0 && string != addr::ExportAddress(Id::str_empty_rep)) {
        using Free = void(__thiscall*)(uint32_t*);
        reinterpret_cast<Free>(addr::ExportAddress(Id::str_free_rep))(&string);
    }
}

bool Init(const char* gameDir, const char* coreVersion) {
    lstrcpynA(g_gameDir, gameDir, sizeof(g_gameDir));
    lstrcpynA(g_coreVersion, coreVersion, sizeof(g_coreVersion));
    for (Id id : kNeeds)
        if (!addr::Usable(id)) {
            log::Warn("console: disabled (needs %s)", addr::Name(id));
            return false;
        }
    cmd::RegisterBuiltins();
    binds::Init();
    if (!hooks::Create("console", Id::con_exec_line, reinterpret_cast<void*>(&ExecLineDetour),
                       reinterpret_cast<void**>(&g_origExecLine)) ||
        !hooks::SetEnabled("console", true))
        return false;
    hooks::OnFrameBegin(FrameBegin);
    hooks::OnLevelLoad(LevelLoad);
    hooks::OnFrameEnd(FrameEnd);
    g_ready = true;
    log::Info("console: ready (game console; FDNYCore commands intercepted at ScriptEngine::ExecLine)");
    return true;
}

void Print(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(text, fmt, ap);
    va_end(ap);
    log::Info("console> %s", text);
    void* con = g_ready ? GameConsole() : nullptr;
    if (con) reinterpret_cast<AddLineFn>(addr::ExportAddress(Id::con_add_line))(con, MakeGameString(text));
}

const char* GameDir() { return g_gameDir; }

bool GamePath(const char* path, char* out, int cap) {
    const bool absolute = path[0] && (path[1] == ':' || path[0] == '\\' || path[0] == '/');
    const char* base = absolute ? "" : g_gameDir;
    if (lstrlenA(base) + lstrlenA(path) + 1 > cap) return false;
    lstrcpyA(out, base);
    lstrcatA(out, path);
    return true;
}
const char* CoreVersion() { return g_coreVersion; }

} // namespace fdcore::console
