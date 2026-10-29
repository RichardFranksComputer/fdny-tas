// Tests: tests/game/text_panels.yaml tests/game/text_alpha.yaml
// User: On-screen text -- size and opacity of every FDNYCore text panel.
#include "textdraw.h"

#include <windows.h>

#include "addr.h"
#include "cmd.h"
#include "console.h"
#include "gfx.h"
#include "hooks.h"
#include "log.h"
#include "pause.h"
#include "testpipe.h"

namespace fdcore::textdraw {
namespace {

constexpr int kPad = 4;  // around the text, like the original Text widget

// Default palette (C_TEXT, C_WHITE) and background slots (panel, then highlights).
const uint32_t kDefaultFg[] = {0xe8e8e8, 0xffffff};
const uint32_t kDefaultBg[4] = {0x101418, 0x5a1010, 0xb02020, 0xb02020};
constexpr uint8_t kHighlightAlpha = 230;  // background slots 1-3 stay nearly opaque

void OnAlpha(cmd::Cvar&);
cmd::Cvar g_fdTextAlpha = {"fd_text_alpha", "160",
                           "panel background opacity 0-255 (translucent panels; 255 = solid)", false, true, OnAlpha,
                           {0}};
cmd::Cvar g_fdTextFgAlpha = {"fd_text_fg_alpha", "255",
                             "text opacity 0-255 on translucent panels (255 = solid text; same cost either way)",
                             false, true, OnAlpha, {0}};

struct Panel {
    char name[24];
    Anchor anchor;
    Style style;
    int dx, dy, minFrames;
    uint8_t defAttr;
    uint32_t fg[kMaxColors];
    int nFg;
    uint32_t bg[4];
    bool visible;
    bool pauseHidden;   // HideWhilePaused: not drawn while pause::Active()
    char text[kMaxLines][kLineLen];
    uint8_t attr[kMaxLines][kLineLen];
    int n;
    bool dirty;
    unsigned long lastRender;
    gfx::Canvas canvas;
    gfx::AlphaSurface surf;
    gfx::Decal decal;
    bool rendered;      // the canvas holds the current text
    bool alphaReady;    // surf holds it
    bool decalStale;    // decal doesn't (translucent panels upload it only for the fallback)
    bool translucent;   // how it was drawn last frame (fd_textdraw, logged on change)
    int x, y;           // last draw position
    bool reported, failed;
    unsigned long drawnFrame;  // last frame it was drawn (Debug pipe `textdraw <name>`)
};
constexpr int kMaxPanels = 8;
Panel g_panels[kMaxPanels] = {};  // constant-initialised: no CRT runs constructors (rule R7)
int g_nPanels = 0;
unsigned long g_frame = 0;
unsigned g_fontGen = 0;

void DirtyAll() {
    for (int i = 0; i < g_nPanels; ++i) g_panels[i].dirty = true;
}
void OnAlpha(cmd::Cvar&) { DirtyAll(); }

int Byte(const cmd::Cvar& c) {
    int v = 0;
    for (const char* s = c.value; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
    return v > 255 ? 255 : v;
}
int Alpha() { return Byte(g_fdTextAlpha); }

// Runs of equal tag, opaque cells on the exact column grid.
bool Render(Panel& p) {
    int cols = 0;
    for (int i = 0; i < p.n; ++i) {
        const int len = lstrlenA(p.text[i]);
        if (len > cols) cols = len;
    }
    if (!p.canvas.Begin(cols, p.n, kPad, p.bg[0])) return false;
    for (int r = 0; r < p.n; ++r) {
        const int len = lstrlenA(p.text[r]);
        for (int c = 0; c < len;) {
            const uint8_t a = p.attr[r][c];
            int e = c + 1;
            while (e < len && p.attr[r][e] == a) ++e;
            const int color = (a & A_COLOR_MASK) < p.nFg ? (a & A_COLOR_MASK) : 0;
            p.canvas.Text(c, r, p.text[r] + c, e - c, p.fg[color], p.bg[a >> A_BG_SHIFT], (a & A_BOLD) != 0);
            c = e;
        }
    }
    p.canvas.Finish();
    return true;
}

void Fail(Panel& p, const char* what) {  // once per panel, not every frame
    if (p.failed) return;
    p.failed = true;
    log::Warn("textdraw: %s: %s (%dx%d)", p.name, what, p.canvas.Width(), p.canvas.Height());
}

#ifdef FDCORE_DEBUG
// Debug pipe `textdraw_cost`: the time this layer takes per frame (every panel: render
// on change, upload, blit), so tests check the cost itself, not the frame rate (D46).
double g_costUs = 0, g_costMaxUs = 0;
unsigned long g_costFrames = 0;
#endif

void DrawPanels();

void Draw() {
#ifdef FDCORE_DEBUG
    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    DrawPanels();
    QueryPerformanceCounter(&t1);
    const double us = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
    g_costUs += us;
    if (us > g_costMaxUs) g_costMaxUs = us;
    ++g_costFrames;
#else
    DrawPanels();
#endif
}

void DrawPanels() {
    ++g_frame;
    if (!gfx::Ready() || !gfx::Engine()) return;
    if (const unsigned gen = gfx::FontGeneration(); gen != g_fontGen) g_fontGen = gen, DirtyAll();
    int vw, vh;
    gfx::ViewSize(&vw, &vh);
    const uint8_t alpha = static_cast<uint8_t>(Alpha());
    const uint8_t high = alpha > kHighlightAlpha ? alpha : kHighlightAlpha;
    for (int i = 0; i < g_nPanels; ++i) {
        Panel& p = g_panels[i];
        const bool textOnly = p.style == STYLE_TEXT_ONLY;
        const gfx::AlphaKey keys[] = {
            {p.bg[0], textOnly ? static_cast<uint8_t>(0) : alpha}, {p.bg[1], high}, {p.bg[2], high}, {p.bg[3], high}};
        if (!p.visible || p.n <= 0 || (p.pauseHidden && pause::Active())) continue;
        if (p.dirty && (!p.rendered || g_frame - p.lastRender >= static_cast<unsigned long>(p.minFrames))) {
            if (!Render(p)) {
                Fail(p, "GDI could not render it");
                continue;
            }
            p.rendered = true, p.dirty = false, p.lastRender = g_frame;
            const uint32_t* px = p.canvas.Pixels();
            const int stride = p.canvas.Stride(), w = p.canvas.Width(), h = p.canvas.Height();
            p.alphaReady = p.style != STYLE_OPAQUE &&
                           p.surf.Upload(px, stride, w, h, keys, 4, static_cast<uint8_t>(Byte(g_fdTextFgAlpha)));
            p.decalStale = true;
            if (textOnly) {
                if (!p.alphaReady) Fail(p, "could not upload its text-only surface");
            } else if (!p.alphaReady && !p.decal.Upload(px, stride, w, h)) {
                Fail(p, "could not create / upload its bitmap");
            } else if (!p.alphaReady) {
                p.decalStale = false;
            }
        }
        if (!p.rendered) continue;
        const int w = p.canvas.Width(), h = p.canvas.Height();
        int x = p.dx, y = p.dy;
        if (p.anchor == TOP_RIGHT) x = vw - p.dx - w;
        if (p.anchor == BOTTOM_LEFT) y = vh - p.dy - h;
        if (p.anchor == CENTER) x = (vw - w) / 2 + p.dx, y = vh / 2 + p.dy;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        p.x = x, p.y = y;
        // Translucent when the frame has a world; otherwise the opaque decal (menus).
        // Text-only panels have no fallback: without a world they are not drawn.
        bool translucent = p.alphaReady && p.surf.Draw(x, y);
        if (!translucent && !textOnly) {
            if (p.decalStale && p.decal.Upload(p.canvas.Pixels(), p.canvas.Stride(), w, h)) p.decalStale = false;
            p.decal.Draw(x, y);
        }
        if (!p.reported || translucent != p.translucent) {
            p.reported = true;
            log::Info("textdraw: %s: %dx%d at %d,%d (view %dx%d), %s", p.name, w, h, x, y, vw, vh,
                      translucent ? "translucent" : "opaque");
        }
        p.translucent = translucent;
        p.drawnFrame = g_frame;
    }
}

void CmdInfo(int, const char* const*) {
    int vw, vh, cw, ch;
    gfx::ViewSize(&vw, &vh);
    gfx::CellSize(&cw, &ch);
    console::Print("textdraw: view %dx%d, cell %dx%d, fd_text_alpha %d, fd_text_fg_alpha %d", vw, vh, cw, ch, Alpha(),
                   Byte(g_fdTextFgAlpha));
    for (int i = 0; i < g_nPanels; ++i) {
        const Panel& p = g_panels[i];
        console::Print("  %s: %s, %d line(s), %dx%d at %d,%d, %s", p.name, p.visible ? "shown" : "hidden", p.n,
                       p.canvas.Width(), p.canvas.Height(), p.x, p.y,
                       !p.rendered ? "not drawn yet" : p.translucent ? "translucent" : "opaque");
    }
}

} // namespace

int Create(const char* name, Anchor anchor, int dx, int dy, int minFrames, uint8_t defaultAttr, Style style) {
    if (g_nPanels >= kMaxPanels) return -1;
    Panel& p = g_panels[g_nPanels];
    lstrcpynA(p.name, name, sizeof(p.name));
    p.anchor = anchor, p.style = style, p.dx = dx, p.dy = dy, p.minFrames = minFrames < 1 ? 1 : minFrames;
    p.defAttr = defaultAttr;
    p.visible = true;
    const int id = g_nPanels++;
    SetPalette(id, kDefaultFg, 2, nullptr);
    return id;
}

void SetPalette(int panel, const uint32_t* fg, int nFg, const uint32_t* bg4) {
    if (panel < 0 || panel >= g_nPanels) return;
    Panel& p = g_panels[panel];
    if (nFg > kMaxColors) nFg = kMaxColors;
    for (int i = 0; i < nFg; ++i) p.fg[i] = fg[i];
    p.nFg = nFg;
    for (int i = 0; i < 4; ++i) p.bg[i] = bg4 ? bg4[i] : kDefaultBg[i];
    p.dirty = true;
}

void Set(int panel, const char (*lines)[kLineLen], const uint8_t (*attrs)[kLineLen], int n) {
    if (panel < 0 || panel >= g_nPanels) return;
    Panel& p = g_panels[panel];
    if (n > kMaxLines) n = kMaxLines;
    bool same = n == p.n;
    for (int i = 0; same && i < n; ++i) {
        same = lstrcmpA(lines[i], p.text[i]) == 0;
        if (same && attrs) same = __builtin_memcmp(attrs[i], p.attr[i], lstrlenA(lines[i])) == 0;
    }
    if (same) return;
    for (int i = 0; i < n; ++i) {
        lstrcpynA(p.text[i], lines[i], kLineLen);
        if (attrs) __builtin_memcpy(p.attr[i], attrs[i], kLineLen);
        else __builtin_memset(p.attr[i], p.defAttr, kLineLen);
    }
    p.n = n;
    p.dirty = true;
}

void SetLines(int panel, const char* const* lines, int n) {
    static char buf[kMaxLines][kLineLen];
    if (n > kMaxLines) n = kMaxLines;
    for (int i = 0; i < n; ++i) lstrcpynA(buf[i], lines[i], kLineLen);
    Set(panel, buf, nullptr, n);
}

void HideWhilePaused(int panel, bool on) {
    if (panel >= 0 && panel < g_nPanels) g_panels[panel].pauseHidden = on;
}

void Show(int panel, bool visible) {
    if (panel >= 0 && panel < g_nPanels) g_panels[panel].visible = visible;
}

bool Init() {
    if (!gfx::Init()) return false;
    cmd::RegisterCvar(&g_fdTextAlpha);
    cmd::RegisterCvar(&g_fdTextFgAlpha);
    cmd::RegisterCommand("fd_textdraw", "list the on-screen text panels: shown, size, position, translucent/opaque",
                         CmdInfo);
    gfx::OnLayer(gfx::LAYER_HUD, Draw);  // last, over everything (D40)
#ifdef FDCORE_DEBUG
    // Test hook: `textdraw <panel name>`: was it drawn on the last frame (or the one before:
    // the pipe thread may read mid-frame)?
    testpipe::Register("textdraw", [](const char* args, char* resp, int cap) {
        (void)cap;
        while (*args == ' ') ++args;
        for (int i = 0; i < g_nPanels; ++i) {
            const Panel& p = g_panels[i];
            if (lstrcmpA(p.name, args) != 0) continue;
            wsprintfA(resp, "found=1 visible=%d pause_hidden=%d drawn=%d", p.visible, p.pauseHidden,
                      p.drawnFrame && g_frame - p.drawnFrame <= 1);
            return;
        }
        wsprintfA(resp, "found=0");
    });
    testpipe::Register("textdraw_cost", [](const char*, char* resp, int) {
        wsprintfA(resp, "frames=%lu us_total=%lu us_max=%lu", g_costFrames, static_cast<unsigned long>(g_costUs),
                  static_cast<unsigned long>(g_costMaxUs));
    });
#endif
    log::Info("textdraw: ready");
    return true;
}

// Native game hints: original bitmap font, checkpoint priority and position.
namespace {
using HintFn = uintptr_t(__thiscall*)(void*, float, uint32_t);
using WidgetFn = uintptr_t(__thiscall*)(void*, uint32_t, const int*, int, int);
HintFn g_hintOriginal;
WidgetFn g_widgetOriginal;
bool g_hintReady, g_ownHint, g_priority;
void* g_hintOwner;
uint32_t g_hintStart, g_hintDuration;
#ifdef FDCORE_DEBUG
unsigned long g_hintPassed, g_hintSuppressed;  // the game's own hints, for tests/game/checkpoints.yaml
#endif
bool Tick(uint32_t* tick) {
    uintptr_t slot = 0;
    return addr::Resolve(addr::Id::clock_elapsed_ms, &slot) && addr::SafeRead(slot, tick, 4);
}
void* HintOwner() {
    uintptr_t slot = 0; uint32_t player = 0;
    if (!addr::Resolve(addr::Id::player_object, &slot) || !addr::SafeRead(slot, &player, 4) || !player) return nullptr;
    return reinterpret_cast<void*>(player+addr::Field(addr::Id::player_hint));
}
uintptr_t __fastcall Hint(void* owner, void*, float seconds, uint32_t text) {
    const DWORD error = GetLastError(); uint32_t now = 0;
    const bool suppress = g_priority && owner == g_hintOwner && Tick(&now) && now-g_hintStart < g_hintDuration;
    SetLastError(error);
    if (suppress) {
#ifdef FDCORE_DEBUG
        ++g_hintSuppressed;
#endif
        // The original consumes this by-value String, even when no widget is drawn.
        console::ReleaseGameString(text); SetLastError(error); return 0;
    }
#ifdef FDCORE_DEBUG
    ++g_hintPassed;
#endif
    if (owner == g_hintOwner) g_priority = false;
    return g_hintOriginal(owner, seconds, text);
}

uintptr_t __fastcall Widget(void* widget, void*, uint32_t text, const int* rect, int xMode, int yMode) {
    if (g_ownHint && rect) {
        // Translate only checkpoint hints in the original 640x480 layout.
        // Keep rectangle size, native wrapping/alignment, and bitmap font.
        const int top[4] = {rect[0], rect[1], rect[2]-180, rect[3]-180};
        return g_widgetOriginal(widget, text, top, xMode, yMode);
    }
    return g_widgetOriginal(widget, text, rect, xMode, yMode);
}
}
bool InitGameHints() {
    if (g_hintReady) return true;
    if (!hooks::Create("game_hints", addr::Id::hint_show, reinterpret_cast<void*>(Hint), reinterpret_cast<void**>(&g_hintOriginal))) return false;
    // If optional positioning is unavailable, preserve the original centered hint.
    if (addr::Usable(addr::Id::hint_text_widget))
        hooks::Create("game_hints", addr::Id::hint_text_widget, reinterpret_cast<void*>(Widget), reinterpret_cast<void**>(&g_widgetOriginal));
    g_hintReady = hooks::SetEnabled("game_hints", true);
#ifdef FDCORE_DEBUG
    // Test hook: `game_hints`: the game's hints shown and held back by a GameHint priority window.
    if (g_hintReady)
        testpipe::Register("game_hints", [](const char*, char* resp, int) {
            wsprintfA(resp, "passed=%lu suppressed=%lu", g_hintPassed, g_hintSuppressed);
        });
#endif
    return g_hintReady;
}
bool GameHint(const char* text, uint32_t milliseconds) {
    void* owner = HintOwner(); uint32_t now = 0;
    if (!g_hintReady || !owner || !milliseconds || !Tick(&now)) return false;
    g_ownHint = true;
    g_hintOriginal(owner, static_cast<float>(milliseconds)*.001f, console::MakeGameString(text));
    g_ownHint = false;
    g_hintOwner = owner; g_hintStart = now; g_hintDuration = milliseconds; g_priority = true;
    return true;
}
void ClearGameHint(bool hide) {
    uint32_t now = 0;
    if (hide && g_priority && g_hintReady && HintOwner() == g_hintOwner && Tick(&now) && now-g_hintStart < g_hintDuration)
        g_hintOriginal(g_hintOwner, 0, console::MakeGameString(""));
    g_priority = false;
}

} // namespace fdcore::textdraw
