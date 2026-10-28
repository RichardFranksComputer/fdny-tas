// Debug-only harness channel: \\.\pipe\FDNYCore, one request line -> one
// response line. Compiled out of Release (the whole file is #ifdef'd).
#ifdef FDCORE_DEBUG

#include "testpipe.h"

#include <windows.h>

#include "hooks.h"
#include "log.h"
#include "state.h"

namespace fdcore::testpipe {
namespace {

bool StartsWith(const char* s, const char* p) {
    while (*p)
        if (*s++ != *p++) return false;
    return true;
}

constexpr int kResp = 4096;

struct Reg {
    const char* name;
    Handler fn;
};
constexpr int kMaxReg = 32;
Reg g_regs[kMaxReg];
int g_nRegs = 0;

// Registered handler whose name is the request's first token.
bool Dispatch(const char* req, char* resp) {
    for (int i = 0; i < g_nRegs; ++i) {
        const char* n = g_regs[i].name;
        const char* s = req;
        while (*n && *s == *n) ++s, ++n;
        if (*n || (*s && *s != ' ')) continue;
        g_regs[i].fn(*s ? s + 1 : s, resp, kResp);
        return true;
    }
    return false;
}

void Handle(const char* req, char* resp) {
    const hooks::Counters& c = hooks::Stats();
    if (StartsWith(req, "ping")) {
        lstrcpyA(resp, "pong");
    } else if (StartsWith(req, "stats")) {
        wsprintfA(resp, "begin=%ld end=%ld events=%ld levels=%ld toggles=%ld frame=%s", c.beginFrame,
                  c.endFrame, c.frameEndEvents, c.levelLoads, c.toggles,
                  hooks::IsEnabled("frame") ? "on" : "off");
    } else if (Dispatch(req, resp)) {
    } else if (StartsWith(req, "snapshot")) {
        state::Format(resp, kResp);
    } else if (StartsWith(req, "hook ")) {
        // hook <group> on|off
        char group[32];
        int i = 0;
        const char* s = req + 5;
        while (*s && *s != ' ' && i < 31) group[i++] = *s++;
        group[i] = 0;
        bool on = StartsWith(s, " on");
        bool off = StartsWith(s, " off");
        lstrcpyA(resp, (on || off) && hooks::SetEnabled(group, on) ? "ok" : "err");
    } else {
        wsprintfA(resp, "err unknown command");
    }
}

DWORD WINAPI Server(LPVOID) {
    for (;;) {
        HANDLE p = CreateNamedPipeA("\\\\.\\pipe\\FDNYCore", PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, kResp,
                                    512, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) {
            log::Error("testpipe: CreateNamedPipe failed (%lu)", GetLastError());
            return 0;
        }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            static char req[512], resp[kResp];
            DWORD got = 0;
            while (ReadFile(p, req, sizeof(req) - 1, &got, nullptr) && got) {
                while (got && (req[got - 1] == '\n' || req[got - 1] == '\r')) --got;
                req[got] = 0;
                Handle(req, resp);
                DWORD wrote = 0;
                WriteFile(p, resp, lstrlenA(resp), &wrote, nullptr);
            }
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

} // namespace

// A full table is an error, not a silent drop: every test fails its log_errors check.
void Register(const char* name, Handler fn) {
    if (g_nRegs < kMaxReg) g_regs[g_nRegs++] = {name, fn};
    else log::Error("testpipe: no room for channel '%s' (%d max)", name, kMaxReg);
}

void Start() {
    HANDLE t = CreateThread(nullptr, 0, Server, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    log::Info("testpipe: listening on \\\\.\\pipe\\FDNYCore (Debug build)");
}

} // namespace fdcore::testpipe

#endif // FDCORE_DEBUG
