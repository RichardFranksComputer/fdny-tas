// Tests: tests/game/glitch_records.yaml tests/game/text_panels.yaml tests/game/keys_default.yaml
// User: Warp HUD -- shows what the player touches and whether it can warp them (the stale-impact warp), with a history of boxes.
// hud_glitch record layer (backlog item 11a): code caves on the collision hook sites that
// write one 128-byte record per event to a ring the analysis reads each frame (originally a
// port of the Python collision overlay's caves).
//
// Cave layout: 0x00 TICK, 0x04 HEAD, 0x08 INCALL, 0x0C FRESH, 0x40 scratch
// record (128 bytes), 0x100 code, 0x1000 ring of 16384 x 128-byte records.
#include "glitch.h"

#include <windows.h>
#include <tlhelp32.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "glitch_analysis.h"
#include "glitch_bsp.h"
#include "textdraw.h"
#include "binds.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "testpipe.h"

namespace fdcore::glitch {
namespace {

using addr::Id;

constexpr uint32_t kTick = 0x00, kHead = 0x04, kIncall = 0x08, kFresh = 0x0C, kRec = 0x40;
constexpr uint32_t kCode = 0x100, kRing = 0x1000, kRecSize = 128, kRingN = 16384;
constexpr uint32_t kAlloc = kRing + kRecSize * kRingN;
// record dword indices (REC_FMT "<11I3f3ff3fI3f7I")
constexpr int R_KIND = 0, R_TICK = 1, R_RET0 = 2, R_RESULT = 9, R_FRESH = 10, R_IMP = 11, R_NRM = 14,
              R_DIST = 17, R_PRE = 18, R_THIS = 21, R_GPL = 22;
constexpr int kStackSlots[6][2] = {{3, 0x30}, {4, 0x34}, {5, 0x44}, {6, 0x54}, {7, 0x90}, {8, 0xC4}};
constexpr uint32_t KIND_TRACE = 1, KIND_COMMIT = 2, KIND_ENGINE_RAY = 3;

struct Site {
    Id id;
    int len;            // displaced bytes
    int resumeOff;      // jump back to site+resumeOff (0 = cave re-emits a ret tail)
    uintptr_t addr = 0;
    uint8_t orig[16] = {};
    uintptr_t entry = 0;
};
Site g_sites[] = {
    {Id::glitch_entry, 7, 7},  {Id::glitch_exit_box, 5, 0}, {Id::glitch_exit_ray, 5, 0},
    {Id::glitch_exit_miss, 5, 0}, {Id::glitch_boxw, 5, 0},   {Id::glitch_rayw, 10, 10},
    {Id::glitch_commit, 7, 7}, {Id::glitch_tick, 5, 5},
};
enum { S_ENTRY, S_XBOX, S_XRAY, S_XMISS, S_BOXW, S_RAYW, S_COMMIT, S_TICK, S_COUNT };

uint8_t* g_cave = nullptr;
bool g_installed = false;
uint32_t g_lastHead = 0;
volatile long g_counts[4];  // per record kind, for tests
uint32_t g_dropped = 0;
// analysis cost per frame (game thread), for the pipe / tests
unsigned long g_pollUsMax = 0, g_pollFrames = 0;
double g_pollUsSum = 0;  // no 64-bit integer division helpers in this no-CRT build
double g_drawUsSum = 0;  // box drawing cost (geEngine_Printf calls)
unsigned long g_drawFrames = 0;

// --- tiny emitter (mirrors the Python Asm class) --------------------------------
struct Asm {
    uint8_t* p;
    uintptr_t pc() const { return reinterpret_cast<uintptr_t>(p); }
    void raw(const uint8_t* b, int n) {
        for (int i = 0; i < n; ++i) *p++ = b[i];
    }
    void b1(uint8_t v) { *p++ = v; }
    void d32(uint32_t v) {
        for (int i = 0; i < 4; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
    }
    void push_eax() { b1(0x50); }
    void pop_eax() { b1(0x58); }
    void ret() { b1(0xC3); }
    void mov_abs_eax(uint32_t a) { b1(0xA3), d32(a); }
    void mov_eax_abs(uint32_t a) { b1(0xA1), d32(a); }
    void mov_abs_esi(uint32_t a) { b1(0x89), b1(0x35), d32(a); }
    void mov_abs_imm(uint32_t a, uint32_t v) { b1(0xC7), b1(0x05), d32(a), d32(v); }
    void inc_abs(uint32_t a) { b1(0xFF), b1(0x05), d32(a); }
    void call(uintptr_t t) { b1(0xE8), d32(static_cast<uint32_t>(t - (pc() + 4))); }
    void jmp(uintptr_t t) { b1(0xE9), d32(static_cast<uint32_t>(t - (pc() + 4))); }
    void mov_eax_esp(uint32_t off) {
        if (off < 0x80) b1(0x8B), b1(0x44), b1(0x24), b1(static_cast<uint8_t>(off));
        else b1(0x8B), b1(0x84), b1(0x24), d32(off);
    }
    void mov_eax_esi(uint32_t off) { b1(0x8B), b1(0x46), b1(static_cast<uint8_t>(off)); }
    void copy_esp(uint32_t off, uint32_t dst) { mov_eax_esp(off), mov_abs_eax(dst); }
    void copy_esi(uint32_t off, uint32_t dst) { mov_eax_esi(off), mov_abs_eax(dst); }
    void copy_abs(uint32_t src, uint32_t dst) { mov_eax_abs(src), mov_abs_eax(dst); }
};

uint32_t A(uint32_t off) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cave) + off); }
uint32_t Rec(int i) { return A(kRec + 4 * i); }

void Emit() {
    const uint32_t TICK = A(kTick), HEAD = A(kHead), INCALL = A(kIncall), FRESH = A(kFresh);
    const uint32_t RING = A(kRing), REC = A(kRec);
    uintptr_t impact = 0, plane = 0;
    addr::Resolve(Id::genesis_impact, &impact);
    addr::Resolve(Id::genesis_plane, &plane);
    Asm a{g_cave + kCode};

    // LOG: copy REC into ring[head & mask], head++
    const uintptr_t LOG = a.pc();
    static const uint8_t kPush[] = {0x56, 0x57, 0x51};
    a.raw(kPush, 3);
    a.b1(0x8B), a.b1(0x3D), a.d32(HEAD);          // mov edi,[HEAD]
    a.b1(0x81), a.b1(0xE7), a.d32(kRingN - 1);    // and edi,mask
    a.b1(0xC1), a.b1(0xE7), a.b1(0x07);           // shl edi,7
    a.b1(0x81), a.b1(0xC7), a.d32(RING);          // add edi,RING
    a.b1(0xBE), a.d32(REC);                       // mov esi,REC
    a.b1(0xB9), a.d32(kRecSize / 4);              // mov ecx,32
    a.b1(0xFC), a.b1(0xF3), a.b1(0xA5);           // cld; rep movsd
    a.inc_abs(HEAD);
    static const uint8_t kPop[] = {0x59, 0x5F, 0x5E};
    a.raw(kPop, 3);
    a.ret();

    // ENTRY (offsets +4 for the pushed eax)
    Site& en = g_sites[S_ENTRY];
    en.entry = a.pc();
    a.push_eax();
    a.copy_esp(4, Rec(R_RET0));
    for (const auto& s : kStackSlots) a.copy_esp(4 + s[1], Rec(s[0]));
    a.copy_abs(TICK, Rec(R_TICK));
    for (int i = 0; i < 3; ++i) a.copy_abs(static_cast<uint32_t>(impact) + 4 * i, Rec(R_PRE + i));
    a.mov_abs_imm(FRESH, 0);
    a.mov_abs_imm(INCALL, 1);
    a.pop_eax();
    a.raw(en.orig, en.len);
    a.jmp(en.addr + en.resumeOff);

    // EXIT tails: ESI = Col*, EAX = result
    static const int kExits[] = {S_XBOX, S_XRAY, S_XMISS};
    for (int k : kExits) {
        Site& s = g_sites[k];
        s.entry = a.pc();
        a.mov_abs_eax(Rec(R_RESULT));
        a.push_eax();
        a.mov_abs_imm(Rec(R_KIND), KIND_TRACE);
        for (int i = 0; i < 3; ++i) {
            a.copy_esi(0x0C + 4 * i, Rec(R_IMP + i));  // Col->Impact
            a.copy_esi(0x1C + 4 * i, Rec(R_NRM + i));  // Col->Plane.Normal
        }
        a.copy_esi(0x28, Rec(R_DIST));
        for (int i = 0; i < 3; ++i) a.copy_abs(static_cast<uint32_t>(plane) + 4 * i, Rec(R_GPL + i));
        a.copy_abs(FRESH, Rec(R_FRESH));
        a.mov_abs_imm(INCALL, 0);
        a.call(LOG);
        a.pop_eax();
        a.raw(s.orig, s.len);  // pop esi; add esp,24h; ret
    }

    // fresh-write counters
    Site& bw = g_sites[S_BOXW];
    bw.entry = a.pc();
    a.inc_abs(FRESH);
    a.raw(bw.orig, bw.len);  // pop ebx; add esp,2ch; ret
    Site& rw = g_sites[S_RAYW];
    rw.entry = a.pc();
    a.inc_abs(FRESH);
    a.b1(0x83), a.b1(0x3D), a.d32(INCALL), a.b1(0x00);  // cmp dword [INCALL],0
    a.b1(0x75), a.b1(0x00);                             // jnz skip (patched below)
    uint8_t* jnzOperand = a.p - 1;
    a.push_eax();
    a.mov_abs_imm(Rec(R_KIND), KIND_ENGINE_RAY);
    a.mov_abs_imm(Rec(R_RET0), 0);
    a.copy_abs(TICK, Rec(R_TICK));
    for (int i = 0; i < 3; ++i) {
        a.copy_abs(static_cast<uint32_t>(impact) + 4 * i, Rec(R_IMP + i));
        a.copy_abs(static_cast<uint32_t>(plane) + 4 * i, Rec(R_GPL + i));
    }
    a.call(LOG);
    a.pop_eax();
    *jnzOperand = static_cast<uint8_t>(a.p - (jnzOperand + 1));
    a.raw(rw.orig, rw.len);  // mov [latch],1 (live, relocated bytes)
    a.jmp(rw.addr + rw.resumeOff);

    // COMMIT: ESI = entity, [esp+10h..18h] = impact, [esp+34h..3ch] = normal
    Site& cm = g_sites[S_COMMIT];
    cm.entry = a.pc();
    a.push_eax();
    a.mov_abs_imm(Rec(R_KIND), KIND_COMMIT);
    a.copy_abs(TICK, Rec(R_TICK));
    a.mov_abs_esi(Rec(R_THIS));
    for (int i = 0; i < 3; ++i) {
        a.copy_esp(4 + 0x10 + 4 * i, Rec(R_IMP + i));
        a.copy_esp(4 + 0x34 + 4 * i, Rec(R_NRM + i));
        a.copy_esi(addr::Field(Id::player_f_pos) + 4 * i, Rec(R_PRE + i));  // old position
    }
    a.call(LOG);
    a.pop_eax();
    a.raw(cm.orig, cm.len);
    a.jmp(cm.addr + cm.resumeOff);

    // TICK
    Site& tk = g_sites[S_TICK];
    tk.entry = a.pc();
    a.inc_abs(TICK);
    a.raw(tk.orig, tk.len);
    a.jmp(tk.addr + tk.resumeOff);
}

// Suspend every other thread of the process; fail if one is executing inside
// a range we're about to rewrite (retry later).
bool Freeze(HANDLE* held, int* n, int cap) {
    *n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    bool ok = true;
    for (BOOL more = Thread32First(snap, &te); more && *n < cap; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == static_cast<DWORD>(-1)) {
            CloseHandle(t);
            continue;
        }
        held[(*n)++] = t;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c))
            for (const Site& s : g_sites)
                if (c.Eip >= s.addr && c.Eip < s.addr + s.len) ok = false;
    }
    CloseHandle(snap);
    return ok;
}

void Unfreeze(HANDLE* held, int n) {
    for (int i = 0; i < n; ++i) {
        ResumeThread(held[i]);
        CloseHandle(held[i]);
    }
}

void WriteBytes(uintptr_t at, const uint8_t* b, int n) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old);
    for (int i = 0; i < n; ++i) reinterpret_cast<volatile uint8_t*>(at)[i] = b[i];
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
}

bool PatchAll(bool install) {
    static HANDLE held[512];
    for (int attempt = 0; attempt < 50; ++attempt) {
        int n = 0;
        if (Freeze(held, &n, 512)) {
            for (const Site& s : g_sites) {
                uint8_t patch[16];
                if (install) {
                    patch[0] = 0xE9;
                    uint32_t rel = static_cast<uint32_t>(s.entry - (s.addr + 5));
                    for (int i = 0; i < 4; ++i) patch[1 + i] = static_cast<uint8_t>(rel >> (8 * i));
                    for (int i = 5; i < s.len; ++i) patch[i] = 0x90;
                    WriteBytes(s.addr, patch, s.len);
                } else {
                    WriteBytes(s.addr, s.orig, s.len);
                }
            }
            Unfreeze(held, n);
            return true;
        }
        Unfreeze(held, n);
        Sleep(2);
    }
    return false;
}

int g_nShown = 0;        // box lines currently drawn
int g_panel = -1;        // the boxes' in-frame text panel (textdraw)

// <game>\Data\Maps, as the old tool's _default_maps_dir (exe folder).
const char* MapsDir() {
    static char dir[MAX_PATH];
    if (!dir[0]) {
        GetModuleFileNameA(nullptr, dir, MAX_PATH);
        for (int i = lstrlenA(dir) - 1; i >= 0 && dir[i] != '\\'; --i) dir[i] = 0;
        if (int n = lstrlenA(dir); n && dir[n - 1] == '\\') dir[n - 1] = 0;
        lstrcatA(dir, "\\Data\\Maps");
    }
    return dir;
}

ULONGLONG g_lastMapCheck = 0;
bool g_mapCheckDue = true;

bool Install() {
    if (g_installed) return true;
    for (Site& s : g_sites) {
        if (!addr::Usable(s.id)) {
            console::Print("fd_hud_warp: unavailable (%s not verified)", addr::Name(s.id));
            return false;
        }
        s.addr = addr::ExportAddress(s.id);
        if (!addr::SafeRead(s.addr, s.orig, s.len)) return false;  // live bytes (relocated operands included)
    }
    if (!g_cave) {
        g_cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, kAlloc, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_cave) return false;
    }
    for (uint32_t i = 0; i < kRing; ++i) g_cave[i] = 0;
    Emit();
    g_lastHead = 0;
    if (!PatchAll(true)) {
        log::Error("glitch: could not patch (a thread stayed inside a hook site)");
        return false;
    }
    g_installed = true;
    analysis::BeginSession(MapsDir());
    g_mapCheckDue = true;
    g_nShown = 0;
    log::Info("glitch: hooks installed: cave 0x%08lX, Genesis base 0x%08lX",
              reinterpret_cast<unsigned long>(g_cave), static_cast<unsigned long>(addr::ModuleBase(1)));
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    if (PatchAll(false)) {
        g_installed = false;  // the cave stays allocated: nothing may jump into freed memory
        log::Info("glitch: hooks removed, original bytes restored; analysis %lu us/frame avg, %lu max, %lu frames",
                  static_cast<unsigned long>(g_pollFrames ? g_pollUsSum / g_pollFrames : 0.0),
                  static_cast<unsigned long>(g_pollUsMax), static_cast<unsigned long>(g_pollFrames));
        analysis::EndSession();
        textdraw::Show(g_panel, false);
        g_nShown = 0;
    }
}

// MapFinder's _read_planes: the first kSigBytes of Genesis' live plane array.
const uint8_t* ReadPlanes() {
    static uint8_t buf[bsp::kSigBytes];
    uintptr_t a;
    uint32_t ptr = 0;
    if (!addr::Resolve(Id::genesis_planes_ptr, &a) || !addr::SafeRead(a, &ptr, 4) || !ptr) return nullptr;
    return addr::SafeRead(ptr, buf, sizeof(buf)) ? buf : nullptr;
}

// GameSource._read_live: the live player / slot state at this poll.
void ReadSnapshot(analysis::Snapshot& s) {
    s = analysis::Snapshot{};
    s.tick = *reinterpret_cast<volatile uint32_t*>(g_cave + kTick);
    uintptr_t slot;
    uint32_t player = 0;
    if (addr::Resolve(Id::player_object, &slot)) addr::SafeRead(slot, &player, 4);
    s.playerPtr = player;
    if (player) {
        uint8_t ground = 0, spec = 0;
        addr::SafeRead(player + addr::Field(Id::player_f_state), &s.state, 4);
        addr::SafeRead(player + addr::Field(Id::player_f_ground), &ground, 1);
        addr::SafeRead(player + addr::Field(Id::player_f_spectator), &spec, 1);
        s.ground = ground != 0, s.spectator = spec != 0;
        addr::SafeRead(player + addr::Field(Id::player_f_pos), s.pos, 12);
        addr::SafeRead(player + addr::Field(Id::player_f_mins), s.mins, 12);
        addr::SafeRead(player + addr::Field(Id::player_f_maxs), s.maxs, 12);
        addr::SafeRead(player + addr::Field(Id::player_f_disp), s.disp, 12);
    }
    uintptr_t a;
    if (addr::Resolve(Id::genesis_impact, &a)) addr::SafeRead(a, s.slotImpact, 12);
    if (addr::Resolve(Id::genesis_plane, &a)) addr::SafeRead(a, s.slotPlane, 16);
    s.genesisBase = static_cast<uint32_t>(addr::ModuleBase(1));
}

// --- the boxes (Overlay: build / render / hotkeys) ----------------------------------
bool g_hidden = false, g_frozen = false;  // B / Z (were F9 / F10, D37)
char g_build[analysis::kBoxLines][analysis::kBoxLineLen];
uint8_t g_buildA[analysis::kBoxLines][analysis::kBoxLineLen];  // colour tags
char g_shown[analysis::kBoxLines][analysis::kBoxLineLen];  // what is drawn (kept while frozen)
uint32_t g_shownTick = 0;
CRITICAL_SECTION g_boxLock;  // the pipe thread reads what the game thread draws

void ClearBoxHistory() { analysis::ClearHistory(); }  // X (was F12)

void CmdHide(int, const char* const*) { g_hidden = !g_hidden; }
void CmdFreeze(int, const char* const*) { g_frozen = !g_frozen; }
void CmdClear(int, const char* const*) {
    if (g_installed) ClearBoxHistory();
}

// Overlay._hotkeys (the original's F9 show/hide, F10 freeze, F12 clear; letters since
// D37: B, Z, X) -- edge-triggered, only while the game has the keyboard (binds' rule);
// the game still gets them. Kept here, not as binds: the clear must stay between this
// frame's hotkeys and build(), as in the original.
void Hotkeys() {
    static const int kVk[3] = {'B', 'Z', 'X'};
    static bool was[3];
    const bool live = binds::KeysLive();
    for (int i = 0; i < 3; ++i) {
        const bool down = (GetAsyncKeyState(kVk[i]) & 0x8000) != 0;
        if (down && !was[i] && live) {
            if (i == 0) CmdHide(0, nullptr);
            else if (i == 1) CmdFreeze(0, nullptr);
            else ClearBoxHistory();
        }
        was[i] = down;
    }
}

void FrameEnd() {
    if (!g_installed) return;
    static analysis::Snapshot snap;
    ReadSnapshot(snap);  // like Python: live read first, then the new records
    // MapFinder.update's MAP_CHECK_SEC throttle (only when maps were indexed)
    const ULONGLONG now = GetTickCount64();
    snap.mapCheck = bsp::HaveSigs() && (g_mapCheckDue || now - g_lastMapCheck >= 1000);
    if (snap.mapCheck) {
        g_mapCheckDue = false;
        g_lastMapCheck = now;
        snap.planes = ReadPlanes();
    }
    const uint32_t head = *reinterpret_cast<volatile uint32_t*>(g_cave + kHead);
    uint32_t n = head - g_lastHead;
    if (n > kRingN) {
        g_dropped += n - kRingN;
        log::Warn("glitch: ring overflow: %lu records lost", static_cast<unsigned long>(n - kRingN));
        g_lastHead = head - kRingN;
        n = kRingN;
    }
    static const uint32_t* recs[kRingN];
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t* rec =
            reinterpret_cast<const uint32_t*>(g_cave + kRing + ((g_lastHead + i) & (kRingN - 1)) * kRecSize);
        recs[i] = rec;
        uint32_t kind = rec[R_KIND];
        if (kind < 4) InterlockedIncrement(&g_counts[kind]);
    }
    g_lastHead = head;
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    analysis::Poll(snap, recs, static_cast<int>(n));
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    const LONGLONG dt = t1.QuadPart - t0.QuadPart;
    const unsigned long us = static_cast<unsigned long>(MulDiv(dt > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(dt), 1000000, static_cast<int>(fq.QuadPart)));
    g_pollUsSum += us, ++g_pollFrames;
    if (us > g_pollUsMax) g_pollUsMax = us;
    if (us > 10000)
        log::Info("glitch: slow analysis frame: %lu us, %lu records, map check %d", us, static_cast<unsigned long>(n),
                  snap.mapCheck ? 1 : 0);
    // Overlay.tick: poll, then hotkeys, then render
    Hotkeys();
    QueryPerformanceCounter(&t0);  // box cost on the game thread: build + hand-over (rendering is on the UI thread)
    if (!g_frozen && !g_hidden) {
        const int nb = analysis::BuildBox(g_build, g_buildA);
        EnterCriticalSection(&g_boxLock);
        for (int i = 0; i < nb; ++i) lstrcpynA(g_shown[i], g_build[i], analysis::kBoxLineLen);
        g_nShown = nb;
        g_shownTick = snap.tick;
        LeaveCriticalSection(&g_boxLock);
        static uint8_t drawA[analysis::kBoxLines][analysis::kBoxLineLen];  // + bold, for textdraw
        for (int i = 0; i < nb; ++i)
            for (int c = 0; c < analysis::kBoxLineLen; ++c) drawA[i][c] = palette::Attr(g_buildA[i][c]);
        textdraw::Set(g_panel, g_build, drawA, nb);
    }
    textdraw::Show(g_panel, !g_hidden);  // frozen: the panel keeps its last text
    QueryPerformanceCounter(&t1);
    g_drawUsSum += static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
    ++g_drawFrames;
}
void OnToggle(cmd::Cvar& v) {
    if (v.value[0] != '0') {
        // Turning the HUD on always shows it (owner 2026-10-22: a hide from B outlived G off/on,
        // so G alone looked dead).
        g_hidden = g_frozen = false;
        if (!Install()) console::Print("fd_hud_warp: hooks could not be installed (see FDNYCore.log)");
    } else {
        Uninstall();
    }
}
cmd::Cvar g_fdHudGlitch = {"fd_hud_warp", "0",
                           "1 = warp HUD: what you touch and whether it can warp you (installs hooks); 0 = off (removes them)",
                           false, true,
                           OnToggle, {0}};

} // namespace

bool Init() {
    InitializeCriticalSection(&g_boxLock);
    // Top-right, 12 px margin (the original's corner); re-rendered at most every 3 frames,
    // about the original's 50 ms refresh (REFRESH_MS). The text itself is built every frame.
    g_panel = textdraw::Create("glitch", textdraw::TOP_RIGHT, 12, 12, 3, palette::T_TEXT);
    textdraw::SetPalette(g_panel, palette::kFg, palette::T_COUNT, palette::kBg);
    textdraw::Show(g_panel, false);
    // Player-facing name: the warp HUD (owner 2026-10-22: "collision" / "glitch" / "warp" for one
    // thing was confusing). The code and the glitch| log lines keep "glitch".
    cmd::RegisterCommand("fd_warp_hide", "show / hide the warp HUD's boxes (key B)", CmdHide);
    cmd::RegisterCommand("fd_warp_freeze", "freeze / unfreeze the warp HUD's boxes; logging continues (key Z)", CmdFreeze);
    cmd::RegisterCommand("fd_warp_clear", "clear the warp HUD's history and last warp (key X)", CmdClear);
    cmd::RegisterCvar(&g_fdHudGlitch);
    cmd::SetPracticeOnly("fd_hud_warp");
    cmd::SetPracticeOnly("fd_warp_hide");
    cmd::SetPracticeOnly("fd_warp_freeze");
    cmd::SetPracticeOnly("fd_warp_clear");
    cmd::RegisterAlias("fd_hud_glitch", "fd_hud_warp");  // the names before 0.19
    cmd::RegisterAlias("fd_glitch_hide", "fd_warp_hide");
    cmd::RegisterAlias("fd_glitch_freeze", "fd_warp_freeze");
    cmd::RegisterAlias("fd_glitch_clear", "fd_warp_clear");
    binds::BindDefault("g", "toggle fd_hud_warp");  // D37
    // Hotkeys() reads these itself (the clear's place in the frame, above).
    binds::DescribeKey("b", "show / hide the warp HUD's boxes (fd_warp_hide)", "while fd_hud_warp is 1");
    binds::DescribeKey("z", "freeze / unfreeze the warp HUD's boxes (fd_warp_freeze)", "while fd_hud_warp is 1");
    binds::DescribeKey("x", "clear the warp HUD's history and last warp (fd_warp_clear)", "while fd_hud_warp is 1");
    hooks::OnFrameEnd(FrameEnd);
#ifdef FDCORE_DEBUG
    testpipe::Register("glitch", [](const char*, char* resp, int cap) { Stats(resp, cap); });
    testpipe::Register("glitch_box", [](const char*, char* resp, int cap) { BoxState(resp, cap); });
#endif
    log::Info("glitch: ready (fd_hud_warp: the warp HUD)");
    return true;
}

bool Installed() { return g_installed; }

int BoxState(char* out, int cap) {
    EnterCriticalSection(&g_boxLock);
    int o = wsprintfA(out, "hidden=%d frozen=%d shown_tick=%lu lines=%d", g_hidden ? 1 : 0, g_frozen ? 1 : 0,
                      static_cast<unsigned long>(g_shownTick), g_nShown);
    for (int i = 0; i < g_nShown && o + analysis::kBoxLineLen + 2 < cap; ++i) o += wsprintfA(out + o, "|%s", g_shown[i]);
    LeaveCriticalSection(&g_boxLock);
    return o;
}

int Stats(char* out, int cap) {
    if (cap < 128) return 0;
    uint32_t tick = g_cave ? *reinterpret_cast<volatile uint32_t*>(g_cave + kTick) : 0;
    return wsprintfA(out, "installed=%d tick=%lu trace=%ld commit=%ld engine_ray=%ld dropped=%lu poll_us_avg=%lu poll_us_max=%lu draw_us_avg=%lu",
                     g_installed ? 1 : 0, static_cast<unsigned long>(tick), g_counts[KIND_TRACE],
                     g_counts[KIND_COMMIT], g_counts[KIND_ENGINE_RAY], static_cast<unsigned long>(g_dropped),
                     static_cast<unsigned long>(g_pollFrames ? g_pollUsSum / g_pollFrames : 0.0),
                     static_cast<unsigned long>(g_pollUsMax),
                     static_cast<unsigned long>(g_drawFrames ? g_drawUsSum / g_drawFrames : 0.0));
}

} // namespace fdcore::glitch

FDCORE_MODULE(glitch, "glitch", nullptr, 0, nullptr, &fdcore::glitch::Init, 50)
