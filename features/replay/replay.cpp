// Tests: tests/game/replay.yaml tests/game/spectator_camera.yaml tests/game/recordings_menu.yaml tests/unit/test_replay.cpp tests/unit/test_spectator.cpp
// User: Replay -- Recordings on the briefing browses, favorites, renames and compares runs with a follow/free camera; V plays this map's best and latest, fd_replay picks runs, fd_replay_ghost races each spawn.
// replay feature (owner request 2026-10-25): the fdny-maps website's recording playback
// (map_explorer/recording.js), in the game. The data is read the site's way (replay_data.h),
// so both draw the same trace. Colours (owner spec, not the site's): one recording fades red ->
// blue -> green over its run; several on one map get one colour each by duration, pure green
// for the fastest through blue to pure red for the slowest. Playback runs in real time
// (fd_replay_speed), and the line is drawn up to the playback time, like the site. Drawing: gfx
// world lines and boxes, hidden by walls; fd_replay_xray adds a dimmed copy over walls (D50).
// The line is narrower with distance, half as wide in the air, has a white tick every second,
// and is drawn through the samples it needs (replay_data's Simplify; all are used for playback).
// Choosing runs without file names (D51): V loads this map's best finished and latest run,
// fd_replay opens a picker, fd_replay_ghost starts a run with each spawn.
#include <windows.h>
#include <xmmintrin.h>

#include "binds.h"
#include "camera.h"
#include "cmd.h"
#include "console.h"
#include "entities.h"
#include "gfx.h"
#include "hooks.h"
#include "log.h"
#include "modules.h"
#include "pause.h"
#include "replay_data.h"
#include "replay_browser.h"
#include "state.h"
#include "spectator.h"
#include "testpipe.h"
#include "textdraw.h"

namespace fdcore::replay {
namespace {

namespace d = data;

constexpr int kMaxRec = 8;
// Trace: 8 units wide (the box is 60), at least 3 px far away and at most 8 px close up.
constexpr float kTraceWorld = 8, kTraceMinPx = 3, kTraceMaxPx = 8, kGlowScale = 3.5f;
constexpr float kEdgeWidth = 2, kArrowWidth = 4;
constexpr float kSimplifyEps = 0.5f;  // world units: under a pixel from 160 units away
// The line and ticks, above the recorded origin: halfway to the eye (72 above it; owner 2026-10-26).
constexpr float kLineLift = 36;
constexpr float kArrowLen = 100, kHeadLen = 20, kHeadWidth = 10;  // the site's ArrowHelper
constexpr uint32_t kArrowRgb = 0xFFFF40;  // yellow: reads on sky, walls and the box
constexpr uint32_t kTickRgb = 0xFFFFFF;
constexpr float kTickHalf = 5;        // tick cube half size, units
constexpr int kMaxTicks = 60;         // per recording: every second, or sparser for long runs
// Alpha levels: 4 bits, so 16 (gfx keeps a tile per level in use).
constexpr uint8_t kLineAlpha = 255, kGlowAlpha = 64, kDimAlpha = 64, kBoxAlpha = 64, kBoxAlphaInside = 32;

struct Rec {
    d::Recording r;
    char file[MAX_PATH];
    float (*pts)[3];       // sample positions, as read
    uint32_t* rgb;         // per sample (time fade) or all one colour (several on a map)
    int* runStart;         // first sample of each joined run (the site's canJoin breaks)
    int nRuns;
    int* kept;             // the samples the drawn line goes through, in order
    int nKept;
    uint8_t* air;          // per sample: drawn thin (d::AirFlags)
    float (*dpts)[3];      // this frame's line points: kept samples up to now, then the playback point
    uint32_t* drgb;
    uint8_t* dair;
    float tick[kMaxTicks][3];
    float tickStep;        // ms between ticks
    int nTicks;
    bool ranked;           // one colour (several recordings on its map)
};
Rec g_rec[kMaxRec];
int g_nRec = 0;
bool ViewerRunning();
void EndViewer(bool openMenu);
void ViewerAction(spectator::Action action);

float g_time = 0;          // playback time, ms
bool g_playing = false;
DWORD g_lastTick = 0;

// Per frame and layer (0 world, 1 x-ray overlay): what's submitted (static, the world layer
// draws it in the next frame).
constexpr int kMaxLines = 1024, kMaxBoxes = kMaxRec * (kMaxTicks + 1);
struct DrawList {
    gfx::WorldLine lines[kMaxLines];
    int nLines;
    gfx::WorldBox boxes[kMaxBoxes];
    int nBoxes;
};
DrawList g_draw[2];
float g_shapePts[kMaxRec][26][3];  // box edges (18 points) and arrow (5)
uint32_t g_shapeRgb[kMaxRec][26];
int g_visible = 0, g_ticks = 0;
char g_map[16] = "";

// sqrt without the CRT (SSE, like glitch_bsp's Sqrt).
float Sqrtf(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }

bool On(const cmd::Cvar& v) { return v.value[0] != '0'; }
cmd::Cvar g_fdReplaySpeed = {"fd_replay_speed", "1", "playback speed: 0.25 / 0.5 / 1 / 2 / 4 (any value > 0)", false, true, nullptr, {0}};
cmd::Cvar g_fdReplayXray = {"fd_replay_xray", "0", "1 = the replay shows through walls (the trace dimmed there); 0 = walls hide it", false, true, nullptr, {0}};

float Speed() {
    float v = 0, scale = 1;
    bool frac = false;
    for (const char* s = g_fdReplaySpeed.value; *s; ++s) {
        if (*s == '.') frac = true;
        else if (*s >= '0' && *s <= '9') v = v * 10 + (*s - '0'), scale *= frac ? 10 : 1;
    }
    v /= scale;
    return v > 0 && v <= 64 ? v : 1;
}

bool SameMap(const char* a, const char* b) { return lstrcmpiA(a, b) == 0; }

uint32_t TimeRgb(const Rec& x, float t) { return d::TimeColor(x.r.duration > 0 ? t / x.r.duration : 0); }

// Colours: a recording alone on its map fades over time; several on one map are ranked.
void Recolor() {
    for (int i = 0; i < g_nRec; ++i) {
        Rec& x = g_rec[i];
        int rank = 0, count = 0;
        for (int j = 0; j < g_nRec; ++j)
            if (SameMap(g_rec[j].r.stage, x.r.stage)) {
                ++count;
                rank += g_rec[j].r.duration < x.r.duration || (g_rec[j].r.duration == x.r.duration && j < i);
            }
        x.ranked = count > 1;
        for (int k = 0; k < x.r.n; ++k) x.rgb[k] = x.ranked ? d::RankColor(rank, count) : TimeRgb(x, x.r.s[k].t);
    }
}

void FreeRec(Rec& x) {
    d::Free(&x.r);
    if (x.pts) VirtualFree(x.pts, 0, MEM_RELEASE);
    x = Rec{};
}

void* Alloc(int bytes) { return VirtualAlloc(nullptr, bytes > 0 ? bytes : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

// A file in the recordings folder (where the recorder writes: fd_record_directory, resolved
// like the recorder does, console::GamePath; D51), or name itself when it's a full path.
void Path(const char* name, char* out) {
    out[0] = 0;
    if (name[0] && (name[1] == ':' || name[0] == '\\' || name[0] == '/')) {
        lstrcpynA(out, name, MAX_PATH);
        return;
    }
    char folder[MAX_PATH];
    if (!RecordingDirectory(folder, sizeof(folder))) return;
    if (lstrlenA(folder) + 1 + lstrlenA(name) >= MAX_PATH) return;
    wsprintfA(out, "%s\\%s", folder, name);
}

HANDLE OpenRead(const char* path) {
    return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

// Loads a recording (a name in the recordings folder, or a full path); messages to the console.
bool Load(const char* name) {
    if (g_nRec >= kMaxRec) return console::Print("replay: %d recordings loaded already (fd_replay_clear)", kMaxRec), false;
    char path[MAX_PATH];
    Path(name, path);
    HANDLE f = OpenRead(path);
    if (f == INVALID_HANDLE_VALUE) return console::Print("replay: cannot open %s", path), false;
    const DWORD size = GetFileSize(f, nullptr);
    char* text = size < 128u * 1024 * 1024 ? static_cast<char*>(Alloc(static_cast<int>(size) + 1)) : nullptr;
    DWORD got = 0;
    const bool read = text && size < 128u * 1024 * 1024 && ReadFile(f, text, size, &got, nullptr) && got == size;
    CloseHandle(f);
    Rec& x = g_rec[g_nRec];
    char err[128];
    const bool ok = read && d::Parse(text, static_cast<int>(got), &x.r, err, sizeof(err));
    if (text) VirtualFree(text, 0, MEM_RELEASE);
    if (!ok) {
        d::Free(&x.r);
        return console::Print("replay: %s: %s", path, read ? err : "cannot read"), false;
    }
    // One block: positions, colours, run starts, kept samples, the frame's line (up to one
    // extra point per run), Simplify's work space, then the byte arrays.
    const int n = x.r.n;
    x.pts = static_cast<float(*)[3]>(Alloc(n * (12 + 4 + 4 + 4 + 2 * (12 + 4 + 1) + 8 + 1 + 1)));
    if (!x.pts) {
        d::Free(&x.r);
        return console::Print("replay: out of memory"), false;
    }
    x.dpts = x.pts + n;
    x.rgb = reinterpret_cast<uint32_t*>(x.dpts + 2 * n);
    x.drgb = x.rgb + n;
    x.runStart = reinterpret_cast<int*>(x.drgb + 2 * n);
    x.kept = x.runStart + n;
    int* work = x.kept + n;
    x.air = reinterpret_cast<uint8_t*>(work + 2 * n);
    x.dair = x.air + n;
    uint8_t* keep = x.dair + 2 * n;  // also the forced points first
    x.nRuns = 0;
    for (int i = 0; i < n; ++i) {
        __builtin_memcpy(x.pts[i], x.r.s[i].pos, sizeof(x.pts[i]));
        x.pts[i][1] += kLineLift;
        if (i == 0 || !d::CanJoin(x.r.s[i - 1], x.r.s[i])) x.runStart[x.nRuns++] = i;
    }
    // Drawn through: Simplify's points plus where the line turns thin, and the sample at half
    // time (the time fade's blue, so the colour blends the same way).
    d::AirFlags(x.r.s, n, x.air);
    const int half = d::IndexAt(x.r, x.r.duration / 2);
    for (int i = 0; i < n; ++i) keep[i] = (i && x.air[i] != x.air[i - 1]) || i == half;
    x.nKept = 0;
    for (int k = 0; k < x.nRuns; ++k) {
        const int a = x.runStart[k], b = k + 1 < x.nRuns ? x.runStart[k + 1] - 1 : n - 1;
        d::Simplify(x.r.s, a, b, kSimplifyEps, keep, keep, work);
        for (int i = a; i <= b; ++i)
            if (keep[i]) x.kept[x.nKept++] = i;
    }
    // A tick every second (sparser for runs over kMaxTicks s), on the interpolated path.
    x.tickStep = 1000;
    while (x.r.duration / x.tickStep > kMaxTicks) x.tickStep += 1000;
    x.nTicks = 0;
    for (float t = x.tickStep; t <= x.r.duration && x.nTicks < kMaxTicks; t += x.tickStep) {
        __builtin_memcpy(x.tick[x.nTicks], d::FrameAt(x.r, t).pos, sizeof(x.tick[0]));
        x.tick[x.nTicks++][1] += kLineLift;
    }
    lstrcpynA(x.file, name, sizeof(x.file));
    ++g_nRec;
    Recolor();
    g_time = 0, g_playing = false;
    console::Print("replay: loaded %s: map %s, %d samples, %ld.%03ld s, %d line(s)", name, x.r.stage, n,
                   static_cast<long>(x.r.duration) / 1000, static_cast<long>(x.r.duration) % 1000, x.nRuns);
    log::Info("replay: loaded %s (map %s, %d samples, %ld ms, %d runs)", path, x.r.stage, n,
              static_cast<long>(x.r.duration), x.nRuns);
    log::Info("replay: line through %d of %d samples, %d ticks", x.nKept, n, x.nTicks);
    return true;
}

void CmdLoad(int argc, const char* const* argv) {
    if (ViewerRunning()) return console::Print("Exit the recordings viewer before changing loaded runs.");
    if (argc < 2) return console::Print("usage: fd_replay_load <file.jsonl> (in the recordings folder, or a full path)");
    Load(argv[1]);
}

// Cleared recordings are freed at the next frame end: the world layer draws the line list
// submitted at the previous frame end, which points into them, until then.
Rec g_cleared[2 * kMaxRec];
int g_nCleared = 0;

void Unload(int i) {
    if (g_nCleared < 2 * kMaxRec) g_cleared[g_nCleared++] = g_rec[i];
    for (int j = i; j + 1 < g_nRec; ++j) g_rec[j] = g_rec[j + 1];
    g_rec[--g_nRec] = Rec{};
    Recolor();
}

void ClearAll() {
    while (g_nRec) Unload(g_nRec - 1);
    g_time = 0, g_playing = false;
}

void CmdClear(int, const char* const*) {
    if (ViewerRunning()) return EndViewer(true);
    ClearAll();
    console::Print("fd_replay_clear: no recordings loaded");
}

int Loaded(const char* name) {
    for (int i = 0; i < g_nRec; ++i)
        if (lstrcmpiA(g_rec[i].file, name) == 0) return i;
    return -1;
}

// --- The recordings folder (D51) --------------------------------------------------------
constexpr int kMaxFiles = 1024, kTailBytes = 65536;
d::FileInfo g_files[kMaxFiles];
int g_nFiles = 0;
char g_tail[kTailBytes];

// Every .jsonl's name; for the files of `map` (if given) also the end of the file
// (duration, complete). Then which runs were finished.
void Scan(const char* map) {
    char pattern[MAX_PATH];
    Path("*.jsonl", pattern);
    g_nFiles = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = pattern[0] ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    for (BOOL more = h != INVALID_HANDLE_VALUE; more && g_nFiles < kMaxFiles; more = FindNextFileA(h, &fd)) {
        d::FileInfo& f = g_files[g_nFiles++];
        d::ParseName(fd.cFileName, &f);
        if (!map || !SameMap(f.map, map)) continue;
        char path[MAX_PATH];
        Path(f.name, path);
        HANDLE file = OpenRead(path);
        if (file == INVALID_HANDLE_VALUE) continue;
        const DWORD size = GetFileSize(file, nullptr), want = size < kTailBytes ? size : kTailBytes;
        DWORD got = 0;
        if (SetFilePointer(file, static_cast<LONG>(size - want), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
            ReadFile(file, g_tail, want, &got, nullptr))
            d::ReadTail(g_tail, static_cast<int>(got), &f);
        CloseHandle(file);
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    d::MarkFinished(g_files, g_nFiles);
}

void Seconds(float ms, char* out) { wsprintfA(out, "%ld.%01ld s", static_cast<long>(ms) / 1000, static_cast<long>(ms) % 1000 / 100); }

// Key V with nothing loaded for this map: the best finished run and the latest one.
bool AutoLoad() {
    Scan(g_map);
    const int best = d::Best(g_files, g_nFiles, g_map), latest = d::Latest(g_files, g_nFiles, g_map);
    log::Info("replay: V on %s: best %s, latest %s", g_map, best >= 0 ? g_files[best].name : "none",
              latest >= 0 ? g_files[latest].name : "none");
    if (best >= 0) Load(g_files[best].name);
    if (latest >= 0 && latest != best) Load(g_files[latest].name);
    return best >= 0 || latest >= 0;
}

float VisibleDuration() {
    float dur = 0;
    for (int i = 0; i < g_nRec; ++i)
        if (SameMap(g_rec[i].r.stage, g_map) && g_rec[i].r.duration > dur) dur = g_rec[i].r.duration;
    return dur;
}

// --- Menu-launched replay viewer -------------------------------------------------------
// The browser validates recordings on the game thread before native Go loads
// the selected stage.
bool g_viewPending, g_viewActive, g_viewReady;
bool g_viewExitRequested, g_viewExitToMenu, g_viewXray;
void ViewerTargets();
void PrimeViewerCamera();
DWORD g_viewRequested;
char g_viewStage[32], g_viewError[192];
int g_viewPanel = -1;
bool ViewerRunning() { return g_viewPending || g_viewActive; }

bool ViewError(const char* text) {
    lstrcpynA(g_viewError, text, sizeof(g_viewError));
    log::Warn("viewer: %s", text);
    console::Print("Recordings: %s", text);
    return false;
}

bool PrepareViewImpl(const char* stage, const char* first, const char* second) {
    g_viewError[0] = 0;
    g_viewXray = false;
    g_viewExitRequested = g_viewExitToMenu = false;
    if (!g_viewReady) return ViewError("Replay viewer is not available in this build.");
    const cmd::Cvar* profile = cmd::FindCvar("fd_profile");
    if (profile && lstrcmpiA(profile->value, "run") == 0)
        return ViewError("Switch fd_profile to practice before viewing recordings.");
    if (!stage || !stage[0] || lstrlenA(stage) >= static_cast<int>(sizeof(g_viewStage)))
        return ViewError("Invalid recording stage.");
    for (const char* p = stage; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return ViewError("Invalid recording stage.");
    if (!first || !first[0] || lstrlenA(first) >= MAX_PATH ||
        (second && lstrlenA(second) >= MAX_PATH)) return ViewError("Recording path is too long or empty.");
    if (second && second[0] && lstrcmpiA(first, second) == 0)
        return ViewError("Choose two different recordings to compare.");
    if (spectator::Active()) spectator::Stop();
    g_viewPending = g_viewActive = false;
    textdraw::Show(g_viewPanel, false);
    spectator::SetPending(false);
    state::SetWatchOnly(false, "viewer replaced");
    ClearAll();
    // Load validates a snapshot of complete sample lines; an end record is optional.
    if (!Load(first) || !SameMap(g_rec[0].r.stage, stage)) {
        ClearAll(); return ViewError("Cannot load the recording, or its stage does not match this menu.");
    }
    if (second && second[0] && (!Load(second) || g_nRec != 2 || !SameMap(g_rec[1].r.stage, stage))) {
        ClearAll(); return ViewError("Cannot load the comparison recording, or its stage differs.");
    }
    lstrcpynA(g_viewStage, stage, sizeof(g_viewStage));
    g_viewPending = true; g_viewRequested = GetTickCount();
    spectator::SetPending(true);
    state::SetWatchOnly(true, "recordings viewer");  // before the stage loads: nothing records the watched run
    ViewerTargets();
    if (!spectator::Prepare()) {
        g_viewPending = false; spectator::SetPending(false); state::SetWatchOnly(false, "viewer not prepared");
        ClearAll(); return ViewError("The replay camera could not be prepared.");
    }
    log::Info("viewer: prepared %d recording(s) on %s", g_nRec, stage);
    return true;
}

void CmdView(int argc, const char* const* argv) {
    if (argc < 3) return console::Print("usage: fd_replay_view <stage> <file.jsonl> [second.jsonl]");
    const auto& state = state::Current();
    if (state.gameState != 1 || !SameMap(state.map, argv[1]))
        return console::Print("fd_replay_view: use Recordings on the briefing, or load that stage first");
    PrepareView(argv[1], argv[2], argc > 3 ? argv[3] : nullptr);
}

void ViewerTargets() {
    spectator::Target targets[kMaxRec] = {};
    for (int i = 0; i < g_nRec; ++i) {
        if (!SameMap(g_rec[i].r.stage, g_viewStage)) continue;
        const d::Sample frame = d::FrameAt(g_rec[i].r, g_time);
        for (int axis = 0; axis < 3; ++axis) {
            targets[i].pos[axis] = frame.pos[axis];
            targets[i].forward[axis] = frame.fwd[axis];
        }
        targets[i].valid = true;
    }
    spectator::SetTargets(targets, g_nRec);
}

// Native map state can become live before the frame-end state snapshot.
// Install the prepared follow pose at the actual camera-use boundary, before
// the selected stage's first world render, including loading states 2 and 3.
void PrimeViewerCamera() {
    if (!g_viewPending || g_viewExitRequested) return;
    int32_t nativeState = -1;
    char map[32] = {};
    if (!addr::ReadI32(addr::Id::game_state, &nativeState) || (nativeState < 1 || nativeState > 3) ||
        !addr::ReadStr(addr::Id::game_map_name, map, sizeof(map)) || !SameMap(map, g_viewStage)) return;
    spectator::PrimeCamera();
}

void EndViewer(bool openMenu) {
    if (!g_viewActive && !g_viewPending) return;
    g_viewExitRequested = true;
    g_viewExitToMenu = g_viewExitToMenu || openMenu;
}

void CompleteViewerExit() {
    if (!g_viewExitRequested) return;
    const bool openMenu = g_viewExitToMenu;
    g_viewExitRequested = g_viewExitToMenu = false;
    g_viewPending = g_viewActive = false;
    spectator::SetPending(false);
    spectator::Stop();
    state::SetWatchOnly(false, "viewer ended");
    g_playing = false;
    gfx::ClearWorldSubmissions();
    ClearAll();
    textdraw::Show(g_viewPanel, false);
    if (openMenu) browser::Restore();  // leaves the level and shows the Recordings list (menu service)
    log::Info("viewer: ended; menu=%d", openMenu);
}

void ViewerAction(spectator::Action action) {
    if (!g_viewActive || g_viewExitRequested) return;
    switch (action) {
    case spectator::Action::Exit: EndViewer(true); return;
    case spectator::Action::TogglePlay:
        if (!g_playing && g_time >= VisibleDuration()) g_time = 0;
        g_playing = !g_playing; break;
    case spectator::Action::SeekBack: g_time -= 5000; break;
    case spectator::Action::SeekForward: g_time += 5000; break;
    case spectator::Action::Restart: g_time = 0; break;
    case spectator::Action::ToggleXray:
        g_viewXray = !g_viewXray;
        log::Info("viewer: through walls %s", g_viewXray ? "on" : "off");
        return;
    }
    if (g_time < 0) g_time = 0;
    if (g_time > VisibleDuration()) g_time = VisibleDuration();
    g_lastTick = GetTickCount();
    ViewerTargets();
}

void ViewerFrame(const state::State& s) {
    if (g_viewExitRequested) return;
    if (g_viewPending) {
        if (s.gameState == 1) {
            if (!SameMap(s.map, g_viewStage)) {
                ViewError("The loaded stage differs from the selected recording."); EndViewer(true); return;
            }
            if (!s.playerValid || !gfx::World()) return; // wait for the first rendered live world
            g_time = 0; ViewerTargets();
            if (!spectator::Start()) { ViewError("The spectator camera could not start."); EndViewer(true); return; }
            g_viewPending = false; g_viewActive = true; spectator::SetPending(false);
            g_playing = true; g_lastTick = GetTickCount();
            log::Info("viewer: started %d recording(s) on %s", g_nRec, g_viewStage);
        } else if (GetTickCount() - g_viewRequested > 120000) {
            ViewError("Stage loading did not complete; viewer request cancelled."); EndViewer(false);
        }
    }
    if (!g_viewActive) return;
    if (s.gameState != 1 || !spectator::Active() || !SameMap(s.map, g_viewStage)) {
        EndViewer(s.gameState == 0); return;
    }
    const cmd::Cvar* profile = cmd::FindCvar("fd_profile");
    if (profile && lstrcmpiA(profile->value, "run") == 0) { EndViewer(true); return; }
    ViewerTargets();
    static char text[6][textdraw::kLineLen];
    spectator::State camera; spectator::ReadState(&camera);
    wsprintfA(text[0], "%s  %s  %ld.%01ld / %ld.%01ld s", g_viewStage, g_playing ? "PLAYING" : "PAUSED",
        static_cast<long>(g_time)/1000, static_cast<long>(g_time)%1000/100,
        static_cast<long>(VisibleDuration())/1000, static_cast<long>(VisibleDuration())%1000/100);
    wsprintfA(text[1], "%s  Run %d/%d  | F: release / re-hook camera", camera.free ? "FREE CAMERA" : "FOLLOWING", camera.followed+1, g_nRec);
    lstrcpyA(text[2], "Right-drag: orbit/look | Wheel: zoom | Tab: switch run");
    lstrcpyA(text[3], "Space: play/pause | Left/Right: seek 5s | Home: restart");
    lstrcpyA(text[4], "Free: WASD + E/Q | Shift: fast | Ctrl: slow | Esc: Recordings");
    wsprintfA(text[5], "X: through walls %s (replay traces and boxes)", g_viewXray ? "ON" : "OFF");
    const char* lines[] = {text[0], text[1], text[2], text[3], text[4], text[5]};
    textdraw::SetLines(g_viewPanel, lines, 6); textdraw::Show(g_viewPanel, true);
}

// --- The picker (fd_replay; D51) --------------------------------------------------------
// This map's newest runs (and the best, if older), numbered 1-9; a number shows / hides that
// run. The game is paused while it's open (game input blocked; the keys are read here).
constexpr int kRows = 9;
int g_panel = -1;
bool g_pickerOpen = false, g_pickerPaused = false;
int g_row[kRows], g_nRows = 0;
bool g_digitDown[kRows];

void FillPicker() {
    static char lines[kRows + 3][textdraw::kLineLen];
    const char* ptrs[kRows + 3];
    int n = 0;
    wsprintfA(lines[n++], "Replays on %s: 1-9 show / hide, V plays, fd_replay closes", g_map);
    const int best = d::Best(g_files, g_nFiles, g_map), latest = d::Latest(g_files, g_nFiles, g_map);
    for (int r = 0; r < g_nRows; ++r) {
        const d::FileInfo& f = g_files[g_row[r]];
        SYSTEMTIME utc = {}, local = {};  // the name's UTC start, shown in local time
        const char* s = f.stamp;
        auto num = [](const char* p, int k) { WORD v = 0; for (int i = 0; i < k; ++i) v = static_cast<WORD>(v * 10 + (p[i] - '0')); return v; };
        utc.wYear = num(s, 4), utc.wMonth = num(s + 5, 2), utc.wDay = num(s + 8, 2);
        utc.wHour = num(s + 11, 2), utc.wMinute = num(s + 14, 2);
        if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
        char secs[16];
        Seconds(f.duration, secs);
        wsprintfA(lines[n++], "%d %s %02u-%02u %02u:%02u  %8s  %s%s%s", r + 1, Loaded(f.name) >= 0 ? "[x]" : "[ ]", local.wMonth,
                  local.wDay, local.wHour, local.wMinute, secs, f.finished ? "finished" : "", g_row[r] == best ? "  best" : "",
                  g_row[r] == latest ? "  latest" : "");
    }
    if (!g_nRows) lstrcpyA(lines[n++], "(no complete recordings of this map yet: fd_record 1 records your runs)");
    for (int i = 0; i < n; ++i) ptrs[i] = lines[i];
    textdraw::SetLines(g_panel, ptrs, n);
}

void OpenPicker() {
    Scan(g_map);
    // Newest complete runs first; the best keeps the last row if it's older.
    g_nRows = 0;
    bool used[kMaxFiles] = {};
    while (g_nRows < kRows) {
        int pick = -1;
        for (int i = 0; i < g_nFiles; ++i)
            if (!used[i] && g_files[i].complete && SameMap(g_files[i].map, g_map) &&
                (pick < 0 || lstrcmpA(g_files[i].stamp, g_files[pick].stamp) > 0))
                pick = i;
        if (pick < 0) break;
        used[pick] = true, g_row[g_nRows++] = pick;
    }
    const int best = d::Best(g_files, g_nFiles, g_map);
    if (best >= 0 && !used[best]) g_row[g_nRows == kRows ? kRows - 1 : g_nRows++] = best;
    for (bool& d : g_digitDown) d = true;  // a key already held doesn't count
    g_pickerPaused = !pause::Active() && pause::Set(true, "replay picker");
    g_pickerOpen = true;
    FillPicker();
    textdraw::Show(g_panel, true);
    log::Info("replay: picker on %s: %d run(s)", g_map, g_nRows);
}

void ClosePicker() {
    if (!g_pickerOpen) return;
    g_pickerOpen = false;
    textdraw::Show(g_panel, false);
    if (g_pickerPaused && pause::Active()) pause::Set(false, "replay picker");
    g_pickerPaused = false;
}

void PickerKeys() {
    if (!g_pickerOpen) return;
    if (!pause::Active() && g_pickerPaused) return ClosePicker();  // the level was left
    for (int r = 0; r < kRows; ++r) {
        const bool down = (GetAsyncKeyState('1' + r) & 0x8000) != 0;
        const bool press = down && !g_digitDown[r] && binds::KeysLive();
        g_digitDown[r] = down;
        if (!press || r >= g_nRows) continue;
        const d::FileInfo& f = g_files[g_row[r]];
        const int i = Loaded(f.name);
        if (i >= 0) Unload(i);
        else Load(f.name);
        FillPicker();
    }
}

// Key V: play / pause; from the end it starts again. With nothing loaded for this map, it
// loads the best finished run and the latest one first; with the picker open, closes it.
void CmdToggle(int, const char* const*) {
    if (ViewerRunning()) return ViewerAction(spectator::Action::TogglePlay);
    ClosePicker();
    if (g_map[0] && VisibleDuration() <= 0 && !AutoLoad())
        return console::Print("replay: no recordings of %s in the recordings folder (fd_record 1 records your runs)", g_map);
    const float dur = VisibleDuration();
    if (dur <= 0) return console::Print("replay: no loaded recording is for this map ('%s')", g_map);
    if (!g_playing && g_time >= dur) g_time = 0;
    g_playing = !g_playing;
    g_lastTick = GetTickCount();
    console::Print("replay: %s at %ld.%01ld s", g_playing ? "playing" : "paused", static_cast<long>(g_time) / 1000,
                   static_cast<long>(g_time) % 1000 / 100);
}

void CmdList(int, const char* const*) {
    char pattern[MAX_PATH];
    Path("*.jsonl", pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int n = 0;
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) console::Print("  %s", fd.cFileName), ++n;
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    console::Print("%d recording(s) in %s; loaded: %d", n, pattern, g_nRec);
}

// fd_replay: the picker; fd_replay <text>: load the first recording (by name) containing text.
void CmdReplay(int argc, const char* const* argv) {
    if (ViewerRunning()) return console::Print("Exit the recordings viewer before changing loaded runs.");
    if (argc < 2) {
        if (g_pickerOpen) return ClosePicker();
        if (!g_map[0]) return console::Print("fd_replay: load a level first");
        return OpenPicker();
    }
    Scan(nullptr);
    const int i = d::Match(g_files, g_nFiles, argv[1]);
    if (i < 0) return console::Print("fd_replay: no recording's name contains '%s' (fd_replay_list)", argv[1]);
    if (Loaded(g_files[i].name) < 0) Load(g_files[i].name);
}

// --- Ghost (fd_replay_ghost; D51) -------------------------------------------------------
// Each spawn (the recorder starts a run at the same moment), the chosen run of this map
// starts with you, in time with the recording's clock.
cmd::Cvar g_fdReplayGhost = {"fd_replay_ghost", "0",
                             "1 = each time you spawn, your best finished run on the map starts with you (your latest if none "
                             "is finished); 2 = your latest run; 0 = off", false, true, nullptr, {0}};
DWORD g_spawnTick = 0;
bool g_ghostPending = false;

void PlayerInit(void*) { g_spawnTick = GetTickCount(), g_ghostPending = true; }

// From the frame end after the spawn: the recorder has closed the previous run by then.
void Ghost() {
    g_ghostPending = false;
    const char mode = g_fdReplayGhost.value[0];
    if (g_viewPending || g_viewActive || (mode != '1' && mode != '2')) return;
    ClosePicker();
    ClearAll();
    Scan(g_map);
    int i = mode == '1' ? d::Best(g_files, g_nFiles, g_map) : -1;
    const char* which = "best";
    if (i < 0) i = d::Latest(g_files, g_nFiles, g_map), which = "latest";
    if (i < 0) return log::Info("replay: ghost: no complete recording of %s", g_map);
    if (!Load(g_files[i].name)) return;
    g_lastTick = GetTickCount();
    g_time = static_cast<float>(g_lastTick - g_spawnTick);  // the recording's clock starts at the spawn
    g_playing = true;
    log::Info("replay: ghost %s (%s, %ld ms) from %ld ms", g_files[i].name, which, static_cast<long>(g_files[i].duration),
              static_cast<long>(g_time));
}

void AddLine(DrawList& L, const gfx::WorldLine& line) {
    if (line.n >= 2 && L.nLines < kMaxLines) L.lines[L.nLines++] = line;
}
void AddLine(DrawList& L, const float (*pts)[3], const uint32_t* rgb, int n, float width) {
    AddLine(L, {pts, rgb, nullptr, n, width, 0, 0, kLineAlpha, 0, 0});
}
void AddBox(DrawList& L, const gfx::WorldBox& box) {
    if (L.nBoxes < kMaxBoxes) L.boxes[L.nBoxes++] = box;
}

// The trace: glowing in the world layer, and dimmed over walls with x-ray.
void AddTrace(const float (*pts)[3], const uint32_t* rgb, const uint8_t* air, int n, bool xray) {
    AddLine(g_draw[0], {pts, rgb, air, n, kTraceMinPx, kTraceWorld, kTraceMaxPx, kLineAlpha, kGlowAlpha, kGlowScale});
    if (xray) AddLine(g_draw[1], {pts, rgb, air, n, kTraceMinPx, kTraceWorld, kTraceMaxPx, kDimAlpha, 0, 0});
}

void AddTicks(const Rec& x, bool xray) {
    for (int k = 0; k < x.nTicks && (k + 1) * x.tickStep <= g_time; ++k) {
        const float* p = x.tick[k];
        gfx::WorldBox b = {{p[0] - kTickHalf, p[1] - kTickHalf, p[2] - kTickHalf},
                           {p[0] + kTickHalf, p[1] + kTickHalf, p[2] + kTickHalf}, kTickRgb, kLineAlpha, kLineAlpha};
        AddBox(g_draw[0], b);
        b.alpha = b.alphaInside = kDimAlpha;
        if (xray) AddBox(g_draw[1], b);
        ++g_ticks;
    }
}

// The box at frame f as gfx box + 12 edges, and the facing arrow; points into g_shapePts[slot].
void AddShape(DrawList& L, int slot, const d::Sample& f, uint32_t rgb) {
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) lo[i] = f.pos[i] + f.mins[i], hi[i] = f.pos[i] + f.maxs[i];
    AddBox(L, {{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}, rgb, kBoxAlpha, kBoxAlphaInside});
    float (*p)[3] = g_shapePts[slot];
    uint32_t* c = g_shapeRgb[slot];
    auto corner = [&](int k, int bits) {
        p[k][0] = bits & 1 ? hi[0] : lo[0], p[k][1] = bits & 2 ? hi[1] : lo[1], p[k][2] = bits & 4 ? hi[2] : lo[2];
        c[k] = rgb;
    };
    static const int kLoop[5] = {0, 1, 5, 4, 0};  // around x/z, bottom (y lo) then top (y hi)
    for (int k = 0; k < 5; ++k) corner(k, kLoop[k]), corner(5 + k, kLoop[k] | 2);
    static const int kPillar[4] = {0, 1, 5, 4};
    for (int k = 0; k < 4; ++k) corner(10 + 2 * k, kPillar[k]), corner(11 + 2 * k, kPillar[k] | 2);
    AddLine(L, p, c, 5, kEdgeWidth);
    AddLine(L, p + 5, c + 5, 5, kEdgeWidth);
    for (int k = 0; k < 4; ++k) AddLine(L, p + 10 + 2 * k, c + 10 + 2 * k, 2, kEdgeWidth);
    // Arrow from the box centre: shaft, then the head as a V around the tip.
    float dir[3];
    d::ArrowDir(f, dir);
    float o[3], tip[3];
    for (int i = 0; i < 3; ++i) o[i] = (lo[i] + hi[i]) * 0.5f, tip[i] = o[i] + dir[i] * kArrowLen;
    float side[3] = {-dir[2], 0, dir[0]};  // horizontal, across the arrow
    const float sl = Sqrtf(side[0] * side[0] + side[2] * side[2]);
    if (sl < 1e-4f) side[0] = 1, side[2] = 0;
    else side[0] /= sl, side[2] /= sl;
    float (*a)[3] = p + 18;
    for (int i = 0; i < 3; ++i) {
        a[0][i] = o[i], a[1][i] = tip[i];
        const float back = tip[i] - dir[i] * kHeadLen;
        a[2][i] = back + side[i] * kHeadWidth * 0.5f, a[3][i] = tip[i], a[4][i] = back - side[i] * kHeadWidth * 0.5f;
    }
    for (int k = 18; k < 23; ++k) c[k] = kArrowRgb;
    AddLine(L, a, c + 18, 2, kArrowWidth);
    AddLine(L, a + 2, c + 20, 3, kArrowWidth);
}

void FrameEnd() {
    const state::State& s = state::Current();
    for (; g_nCleared; --g_nCleared) FreeRec(g_cleared[g_nCleared - 1]);
    g_draw[0].nLines = g_draw[0].nBoxes = g_draw[1].nLines = g_draw[1].nBoxes = 0, g_visible = 0, g_ticks = 0;
    ViewerFrame(s);
    if (s.gameState != 1) return ClosePicker();
    if (!SameMap(g_map, s.map)) lstrcpynA(g_map, s.map, sizeof(g_map)), g_time = 0, g_playing = g_viewActive;
    if (g_ghostPending) Ghost();
    PickerKeys();
    if (!g_nRec) return;
    const float dur = VisibleDuration();
    if (g_playing) {
        const DWORD now = GetTickCount();
        DWORD dt = now - g_lastTick;
        if (dt > 250) dt = 250;  // the site's cap per frame
        g_lastTick = now;
        g_time += static_cast<float>(dt) * Speed();
        if (g_time >= dur) g_time = dur, g_playing = false;
    }
    const bool xray = g_viewActive ? g_viewXray : On(g_fdReplayXray);
    DrawList& shapes = g_draw[xray ? 1 : 0];  // the box and arrow: full strength over walls with x-ray
    int slot = 0;
    for (int i = 0; i < g_nRec; ++i) {
        Rec& x = g_rec[i];
        if (!SameMap(x.r.stage, s.map)) continue;
        ++g_visible;
        const int last = d::IndexAt(x.r, g_time);
        if (last < 0) {  // before its first sample: the box waits at the start
            AddShape(shapes, slot++, x.r.s[0], x.rgb[0]);
            continue;
        }
        const d::Sample f = d::FrameAt(x.r, g_time);
        // Each run: its kept samples up to now, then the playback point (the box's position).
        int k = 0, o = 0;
        for (int r = 0; r < x.nRuns; ++r) {
            const int a = x.runStart[r], b = r + 1 < x.nRuns ? x.runStart[r + 1] - 1 : x.r.n - 1;
            if (a > last) break;
            const int from = o;
            for (; k < x.nKept && x.kept[k] <= b; ++k) {
                const int j = x.kept[k];
                if (j > last) continue;
                __builtin_memcpy(x.dpts[o], x.pts[j], sizeof(x.dpts[o]));
                x.drgb[o] = x.rgb[j], x.dair[o++] = x.air[j];
            }
            if (last < b) {
                __builtin_memcpy(x.dpts[o], f.pos, sizeof(x.dpts[o]));
                x.dpts[o][1] += kLineLift;
                x.drgb[o] = x.ranked ? x.rgb[last] : TimeRgb(x, g_time), x.dair[o++] = x.air[last];
            }
            AddTrace(x.dpts + from, x.drgb + from, x.dair + from, o - from, xray);
        }
        AddTicks(x, xray);
        AddShape(shapes, slot++, f, x.rgb[last]);
    }
    if (g_viewActive) ViewerFrame(s);
    for (int l = 0; l < 2; ++l) {
        const gfx::Layer layer = l ? gfx::LAYER_OVERLAY : gfx::LAYER_WORLD;
        gfx::SubmitWorldBoxes(g_draw[l].boxes, g_draw[l].nBoxes, layer);
        gfx::SubmitWorldLines(g_draw[l].lines, g_draw[l].nLines, layer);
    }
}

bool Init() {
    g_viewPanel = textdraw::Create("Replay viewer", textdraw::BOTTOM_LEFT, 8, 24, 3, textdraw::C_TEXT);
    spectator::SetActionHandler(ViewerAction);
    camera::OnBeforeWorldRender(PrimeViewerCamera);
    hooks::OnFrameComplete(CompleteViewerExit);
    g_viewReady = spectator::Init();
    if (!browser::Init()) log::Warn("replay: Recordings menu disabled; console playback remains available");
    binds::DescribeKey("esc pgup pgdn", "back / previous page / next page", "in the Recordings menu");
    binds::DescribeKey("x", "toggle replay traces and ghost boxes through walls (shown in viewer controls)", "in the recordings viewer");
    binds::DescribeKey("mouse2", "drag to orbit the followed run or look around with the released camera", "in the recordings viewer");
    binds::DescribeKey("f tab space leftarrow rightarrow home esc pgup pgdn", "viewer: follow/free, switch run, play/pause, seek, restart, exit, zoom", "in the recordings viewer");
    binds::DescribeKey("w a s d e q shift ctrl", "move the released viewer camera; Shift fast, Ctrl slow", "in the recordings viewer");
    cmd::RegisterCommand("fd_replay_view", "fd_replay_view <stage> <file.jsonl> [second.jsonl]: watch one or two runs in the currently loaded stage with a follow/free camera", CmdView);
    cmd::SetPracticeOnly("fd_replay_view");
    cmd::RegisterCvar(&g_fdReplaySpeed);
    cmd::RegisterCvar(&g_fdReplayXray);
    cmd::RegisterCommand("fd_replay_load", "fd_replay_load <file.jsonl>: load a recording (the recordings folder, or a full path); "
                         "several on one map are drawn together", CmdLoad);
    cmd::RegisterCommand("fd_replay_clear", "unload every recording", CmdClear);
    cmd::RegisterCommand("fd_replay_toggle", "play / pause this map's recordings (with none loaded, loads its best finished and latest run first); from the end, start again (key V)", CmdToggle);
    cmd::RegisterCommand("fd_replay_list", "list the .jsonl recordings in the recordings folder", CmdList);
    cmd::RegisterCommand("fd_replay", "fd_replay: pick this map's runs from a list (pauses); fd_replay <text>: load the first "
                         "recording whose name contains text", CmdReplay);
    cmd::RegisterCvar(&g_fdReplayGhost);
    cmd::SetPracticeOnly("fd_replay_speed");
    cmd::SetPracticeOnly("fd_replay_xray");
    cmd::SetPracticeOnly("fd_replay_load");
    cmd::SetPracticeOnly("fd_replay_toggle");
    cmd::SetPracticeOnly("fd_replay");
    cmd::SetPracticeOnly("fd_replay_ghost");
    binds::BindDefault("v", "fd_replay_toggle");  // D37
    binds::DescribeKey("1 2 3 4 5 6 7 8 9", "replay picker: show / hide that run", "while the fd_replay list is open");
    g_panel = textdraw::Create("replay_picker", textdraw::CENTER, 0, -120, 1, textdraw::C_TEXT);
    textdraw::Show(g_panel, false);
    if (!entities::ObservePlayer(PlayerInit, nullptr)) log::Warn("replay: no player observer; fd_replay_ghost off");
    hooks::OnFrameEnd(FrameEnd);
#ifdef FDCORE_DEBUG
    testpipe::Register("viewer", [](const char*, char* out, int) {
        wsprintfA(out, "pending=%d active=%d loaded=%d playing=%d time_ms=%ld duration_ms=%ld stage=%s",
            g_viewPending, g_viewActive, g_nRec, g_playing, static_cast<long>(g_time),
            static_cast<long>(VisibleDuration()), g_viewStage);
    });
    testpipe::Register("replay", [](const char*, char* resp, int) {
        int us, meanUs;
        gfx::WorldLineCost(&us, &meanUs);
        wsprintfA(resp, "loaded=%d visible=%d playing=%d time_ms=%ld lines=%d segs=%d ticks=%d us=%d mean_us=%d picker=%d rows=%d",
                  g_nRec, g_visible, g_playing ? 1 : 0, static_cast<long>(g_time), g_draw[0].nLines + g_draw[1].nLines,
                  gfx::LastWorldLineSegments(), g_ticks, us, meanUs, g_pickerOpen ? 1 : 0, g_nRows);
    });
#endif
    log::Info("replay: ready (fd_replay_load, key V%s)", gfx::WorldBoxesReady() ? "" : "; no world drawing: off");
    return true;
}

} // namespace
bool PrepareView(const char* stage, const char* first, const char* second) {
    return PrepareViewImpl(stage, first, second);
}
const char* ViewError() { return g_viewError; }
bool RecordingDirectory(char* out, int cap) {
    const auto* dir = cmd::FindCvar("fd_record_directory");
    return console::GamePath(dir && dir->value[0] ? dir->value : "Recordings", out, cap);
}
} // namespace fdcore::replay

// After the recorder (100): it creates and enables the player hooks; the replay only adds a
// listener. Enabling them here first, the recorder's victim hooks were then patched while the
// game ran through the player hooks, and startup hung (keys_default, 2026-10-26; D51).
FDCORE_MODULE(replay, "replay", nullptr, 0, nullptr, &fdcore::replay::Init, 110)
