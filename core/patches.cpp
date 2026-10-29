// Tests: tests/game/core_release.yaml tests/game/console_basic.yaml
// User: Startup -- optional startup changes.
#include "patches.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "log.h"

namespace fdcore::patches {
namespace {

bool g_consoleGate = false;
bool g_skipIntro = false;

bool WriteCode(uintptr_t at, const uint8_t* bytes, uint32_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    for (uint32_t i = 0; i < n; ++i) reinterpret_cast<volatile uint8_t*>(at)[i] = bytes[i];
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

// fd_skip_intro has to act before WinMain reaches the movies, long before the
// console runs FDNYCore.cfg, so peek at the config files directly: the last
// `fd_skip_intro <v>` line in FDNYCore.cfg then autoexec.cfg wins.
int PeekSkipIntro(const char* path, int current) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return current;
    static char buf[32 * 1024];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    const char* key = "fd_skip_intro";
    for (char* p = buf; *p;) {
        while (*p == ' ' || *p == '\t') ++p;
        int i = 0;
        // Case-insensitive for letters only ('_' | 0x20 would become 0x7F).
        while (key[i] && ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] | 0x20 : p[i]) == key[i]) ++i;
        if (!key[i] && (p[i] == ' ' || p[i] == '\t')) {
            char* v = p + i;
            while (*v == ' ' || *v == '\t' || *v == '"') ++v;
            current = *v != '0' && *v != '\r' && *v != '\n' && *v != 0;
        }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
    return current;
}

cmd::Cvar g_fdSkipIntro = {"fd_skip_intro", "0",
                           "1 = skip the Activision/Mekada intro movies at startup; 0 = play them (takes effect next launch)",
                           false, true, nullptr, {0}};

} // namespace

void ApplyIntroSkip(const char* gameDir) {
    char path[MAX_PATH];
    int skip = 0;
    wsprintfA(path, "%s%s", gameDir, "FDNYCore.cfg");
    skip = PeekSkipIntro(path, skip);
    wsprintfA(path, "%s%s", gameDir, "autoexec.cfg");
    skip = PeekSkipIntro(path, skip);
#ifdef FDCORE_DEBUG
    // Test harness: the movies intermittently stall on the VM (DirectShow +
    // virtual audio), so Debug builds also take this from the environment.
    char env[8];
    if (GetEnvironmentVariableA("FDNYCORE_TEST_SKIP_INTRO", env, sizeof(env)) && env[0] == '1') skip = 1;
#endif
    if (!skip) return;
    uintptr_t at = addr::EarlyCodeAddress(addr::Id::patch_intro_movies);
    if (!at) {
        log::Warn("patches: intro-movie branch signature not found; movies play");
        return;
    }
    // jne rel32 (0F 85 FA 00 00 00) -> nop; jmp rel32 to the same target.
    static const uint8_t kJmp[6] = {0x90, 0xE9, 0xFA, 0x00, 0x00, 0x00};
    g_skipIntro = WriteCode(at, kJmp, sizeof(kJmp));
    if (g_skipIntro) log::Info("patches: intro movies skipped (fd_skip_intro)");
}

void Init() { cmd::RegisterCvar(&g_fdSkipIntro); }

void ApplyEarly() {
    // The game creates its developer console only when ImADeveloper is set
    // (JZ at roc_CreateEngineSubsystems+0xA0 skips it). Players need the
    // console, but forcing ImADeveloper would switch on ~16 debug paths, so
    // only this one branch is removed: the game then creates the console
    // itself, at the normal moment, exactly as in developer mode.
    uintptr_t gate = addr::EarlyCodeAddress(addr::Id::patch_console_gate);
    if (!gate) {
        log::Warn("patches: console gate signature not found; console only with ImADeveloper");
        return;
    }
    static const uint8_t kNops[2] = {0x90, 0x90};
    g_consoleGate = WriteCode(gate, kNops, sizeof(kNops));
    if (g_consoleGate) log::Info("patches: console creation ungated at %08lx", static_cast<unsigned long>(gate));
    else log::Error("patches: console gate VirtualProtect failed (%lu)", GetLastError());
}

bool ConsoleGatePatched() { return g_consoleGate; }

} // namespace fdcore::patches
