// User: On-screen text -- size and opacity of every FDNYCore text panel.
#include "gfx.h"

#include <windows.h>
#include <xmmintrin.h>

#include "addr.h"
#include "cmd.h"
#include "camera.h"
#include "hooks.h"
#include "log.h"
#include "testpipe.h"

namespace fdcore::gfx {
namespace {

using addr::Id;

// Genesis API (ABIs in addresses.json "ghidra"; alpha path: docs/evidence/alpha_spike).
struct GeRect {
    int32_t left, right, top, bottom;  // GE_Rect, inclusive
};
struct TLVertex {
    float x, y, z, u, v, r, g, b, a;  // GE_TLVertex; colours 0..255
};
using CreateFn = void*(__fastcall*)(int width, int height, int maxMip, int format);
using LockFn = int(__fastcall*)(void* bmp, void** target, int minMip, int maxMip, int format);
using BmpFn = int(__fastcall*)(void* bmp);
using GetBitsFn = void*(__fastcall*)(void* bmp);
using GetInfoFn = int(__fastcall*)(void* bmp, uint32_t* info, uint32_t* info2);
using DestroyFn = void(__fastcall*)(void** bmp);
using DrawFn = int(__fastcall*)(void* engine, void* bmp, const GeRect* src, uint32_t x, uint32_t y);
using EngineBmpFn = int(__cdecl*)(void* engine, void* bmp);
using FlushFn = int(__cdecl*)(void* engine);
using RenderPolyFn = int(__fastcall*)(void* engine, const TLVertex* pts, int n, void* bmp, uint32_t flags);
using WorldBmpFn = int(__cdecl*)(void* world, void* bmp);
using RenderWorldFn = int(__cdecl*)(void* engine, void* world, void* camera, void* a4);
struct Vec3 {
    float x, y, z;  // geVec3d
};
using CameraFn = void(__fastcall*)(void* camera, const Vec3* in, Vec3* out);

CreateFn g_create;
LockFn g_lock;
BmpFn g_unlock;
GetBitsFn g_bits;
GetInfoFn g_info;
DestroyFn g_destroy;
DrawFn g_draw;
EngineBmpFn g_add, g_remove;
FlushFn g_flush;
RenderPolyFn g_poly;
WorldBmpFn g_worldAdd, g_worldRemove;
RenderWorldFn g_origRenderWorld;
CameraFn g_camXform, g_camProject;
bool g_ok = false, g_alphaOk = false, g_boxOk = false;

constexpr int kFormat565 = 5;   // gePixelFormat 565 RGB: the driver's own format, cheapest upload
constexpr int kFormat4444 = 7;  // 4444 ARGB: the only alpha texture format D3DDrv keeps
constexpr int kSettle = 2;      // frames from an add to a usable handle (D3DDrv crashed earlier)
constexpr uint32_t kImmediate = 1;  // RenderPoly flags bit 0: straight to the driver

// --- frame state -------------------------------------------------------------------
void* g_world = nullptr;      // rendered this frame
void* g_camera = nullptr;     // the camera it was rendered with
void* g_lastWorld = nullptr;  // last one seen (detach on Destroy outside a frame)
bool g_flushed = false;

void RunWorldLayer();

int __cdecl RenderWorldDetour(void* engine, void* world, void* camera, void* a4) {
    if (world) {
        if (world != g_lastWorld) log::Info("gfx: world %08lx", reinterpret_cast<unsigned long>(world));
        g_world = g_lastWorld = world;
        g_camera = camera;
    }
    camera::BeforeWorldRender(camera);  // the free-camera override goes on here (D34)
    const int r = g_origRenderWorld(engine, world, camera, a4);
    if (world) RunWorldLayer();  // before what the game draws next (D39, D40)
    return r;
}

// --- layers (D40) ---------------------------------------------------------------------
constexpr int kMaxLayerSubs = 8;
LayerFn g_layerSubs[3][kMaxLayerSubs];
int g_nLayerSubs[3];
// World boxes submitted this frame end, per layer. WORLD lists are drawn inside the NEXT
// frame's world render (D39), so they move to g_worldPrev at the late step.
struct BoxList {
    const WorldBox* boxes;
    int n;
};
BoxList g_boxSubs[2][kMaxLayerSubs];
int g_nBoxSubs[2];
BoxList g_worldPrev[kMaxLayerSubs];
int g_nWorldPrev = 0;
struct LineList {
    const WorldLine* lines;
    int n;
};
LineList g_lineSubs[2][kMaxLayerSubs], g_linePrev[kMaxLayerSubs];
int g_nLineSubs[2], g_nLinePrev = 0;
int g_lineSegs = 0, g_lineSegsAcc = 0;
#ifdef FDCORE_DEBUG
double g_lineUsAcc = 0, g_lineUs = 0, g_lineUsSum = 0;  // this frame, last frame, since WorldLineCost
int g_lineUsFrames = 0;
#endif
WorldBoxStats g_boxStats = {}, g_boxAcc = {};
#ifdef FDCORE_DEBUG
char g_order[32], g_lastOrder[32];
// Bounded: FrameBegin resets g_order, but with the frame hooks off (fd_hooks_frame 0,
// tests/game/hooks_frame.yaml) only the RenderWorld detour runs, and an unbounded append grew
// " world" past 32 bytes into the box-list globals next to it (DrawBoxes read a list at 0x1).
void MarkLayer(const char* name) {
    const int used = lstrlenA(g_order), need = lstrlenA(name) + (used ? 1 : 0);
    if (used + need >= static_cast<int>(sizeof(g_order))) return;
    if (used) lstrcatA(g_order, " ");
    lstrcatA(g_order, name);
}
#endif

void FrameBegin() {
    g_world = nullptr;
    g_camera = nullptr;
    g_flushed = false;
    g_boxAcc = {};
#ifdef FDCORE_DEBUG
    g_order[0] = 0;
#endif
}

// --- font ----------------------------------------------------------------------------
HDC g_dc;
HFONT g_font, g_bold;
int g_cw = 6, g_ch = 13;
unsigned g_fontGen = 1;
int g_fontPx = 0;

void OnTextSize(cmd::Cvar&);
cmd::Cvar g_fdTextSize = {"fd_text_size", "9", "on-screen text height in pixels, 7-16", false, true,
                          OnTextSize, {0}};

int TextPx() {
    int v = 0;
    for (const char* s = g_fdTextSize.value; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
    return v < 7 ? 7 : v > 16 ? 16 : v;
}

bool Fonts() {
    if (!g_dc && !(g_dc = CreateCompatibleDC(nullptr))) return false;
    const int px = TextPx();
    if (g_font && px == g_fontPx) return true;
    if (g_font) DeleteObject(g_font), DeleteObject(g_bold);
    g_font = CreateFontA(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    g_bold = CreateFontA(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(g_dc, g_font);
    TEXTMETRICA tm;
    GetTextMetricsA(g_dc, &tm);
    g_cw = tm.tmAveCharWidth, g_ch = tm.tmHeight;
    g_fontPx = px;
    ++g_fontGen;
    log::Info("gfx: font Consolas %d px, cell %dx%d", px, g_cw, g_ch);
    return true;
}

void OnTextSize(cmd::Cvar&) { Fonts(); }

COLORREF Ref(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }
int RoundUp(int v, int m) { return (v + m - 1) / m * m; }

// --- view ------------------------------------------------------------------------------
// The render size: the camera's rect. The window's client area is only a fallback
// (before the first camera): it is the render size windowed through dgVoodoo, but
// larger when the image is scaled up, which put right / bottom anchored panels
// outside the frame (0.14.1).
int g_vw = 640, g_vh = 480;
bool g_vCamera = false;

bool HandleOf(void* bmp, uint32_t* h) {
    return addr::SafeRead(reinterpret_cast<uintptr_t>(bmp) + addr::Field(Id::bitmap_f_driver_handle), h, 4);
}

} // namespace

// --- frame ------------------------------------------------------------------------------
bool Ready() { return g_ok; }
void* Engine() { return hooks::CurrentEngine(); }
void* World() { return g_world; }

bool BeginDraw() {
    void* engine = Engine();
    if (!g_ok || !engine) return false;
    if (!g_flushed) {
        g_flush(engine);  // the cached 3D polys first, or they're drawn over immediate polys
        g_flushed = true;
    }
    return true;
}

void ViewSize(int* w, int* h) {
    int vw = 0, vh = 0;
    RECT rc;
    const bool fromCamera = camera::ViewSize(&vw, &vh);
    if (!fromCamera) {
        if (HWND wnd = GetActiveWindow(); wnd && GetClientRect(wnd, &rc) && rc.right > 0 && rc.bottom > 0)
            vw = rc.right, vh = rc.bottom;
        else
            vw = g_vw, vh = g_vh;
    }
    if (vw != g_vw || vh != g_vh || fromCamera != g_vCamera) {
        RECT cr = {};
        if (HWND wnd = GetActiveWindow()) GetClientRect(wnd, &cr);
        log::Info("gfx: view %dx%d (%s; window client %ldx%ld)", vw, vh, fromCamera ? "camera" : "window", cr.right,
                  cr.bottom);
        g_vw = vw, g_vh = vh, g_vCamera = fromCamera;
    }
    *w = vw, *h = vh;
}

// --- Canvas ------------------------------------------------------------------------------
void CellSize(int* w, int* h) {
    Fonts();
    *w = g_cw, *h = g_ch;
}
unsigned FontGeneration() {
    Fonts();
    return g_fontGen;
}

bool Canvas::Begin(int cols, int rows, int pad, uint32_t bg) {
    if (!Fonts()) return false;
    pad_ = pad;
    w_ = cols * g_cw + 2 * pad, h_ = rows * g_ch + 2 * pad;
    if (w_ > cap_w_ || h_ > cap_h_ || !dib_) {  // grow in steps so changing line lengths don't churn
        if (dib_) DeleteObject(static_cast<HBITMAP>(dib_));
        cap_w_ = RoundUp(w_, 64), cap_h_ = RoundUp(h_, 32);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = cap_w_, bi.bmiHeader.biHeight = -cap_h_;  // top-down
        bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px_), nullptr, 0);
        if (!dib_) {
            px_ = nullptr;
            return false;
        }
    }
    SelectObject(g_dc, static_cast<HBITMAP>(dib_));
    RECT all = {0, 0, w_, h_};
    SetBkColor(g_dc, Ref(bg));
    ExtTextOutA(g_dc, 0, 0, ETO_OPAQUE, &all, "", 0, nullptr);
    return true;
}

void Canvas::Text(int col, int row, const char* s, int len, uint32_t fg, uint32_t bg, bool bold) {
    static INT dx[256];
    if (len > 256) len = 256;
    for (int i = 0; i < len; ++i) dx[i] = g_cw;
    SelectObject(g_dc, bold ? g_bold : g_font);
    SetTextColor(g_dc, Ref(fg));
    SetBkColor(g_dc, Ref(bg));
    RECT rc = {pad_ + col * g_cw, pad_ + row * g_ch, pad_ + (col + len) * g_cw, pad_ + (row + 1) * g_ch};
    ExtTextOutA(g_dc, rc.left, rc.top, ETO_OPAQUE | ETO_CLIPPED, &rc, s, len, dx);
}

void Canvas::Finish() { GdiFlush(); }

// --- Decal -------------------------------------------------------------------------------
void Decal::Destroy() {
    if (!bmp_) return;
    if (engine_) g_remove(engine_, bmp_);
    g_destroy(&bmp_);
    bmp_ = nullptr, engine_ = nullptr;
}

bool Decal::Upload(const uint32_t* px, int stride, int w, int h) {
    void* engine = Engine();
    if (!g_ok || !engine || !px) return false;
    if (bmp_ && (engine_ != engine || w > cap_w_ || h > cap_h_)) Destroy();
    const bool fresh = !bmp_;
    if (fresh) {
        cap_w_ = RoundUp(w, 64), cap_h_ = RoundUp(h, 32);
        if (!(bmp_ = g_create(cap_w_, cap_h_, 1, kFormat565))) return false;
    }
    void* target = nullptr;
    bool ok = g_lock(bmp_, &target, 0, 0, kFormat565) && target;
    if (ok) {
        uint32_t info[9] = {};
        g_info(target, info, nullptr);
        const int ds = static_cast<int>(info[2]);
        auto* dst = static_cast<uint16_t*>(g_bits(target));
        ok = dst && ds >= w;
        for (int y = 0; ok && y < h; ++y) {
            const uint32_t* s = px + y * stride;
            uint16_t* d = dst + y * ds;
            for (int x = 0; x < w; ++x) {
                const uint32_t c = s[x];
                d[x] = static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
            }
        }
        g_unlock(target);
    }
    if (ok && fresh) {
        ok = g_add(engine, bmp_) != 0;
        engine_ = engine, settle_ = kSettle;
    }
    if (!ok) {
        Destroy();
        return false;
    }
    w_ = w, h_ = h;
    return true;
}

bool Decal::Draw(int x, int y) {
    if (!bmp_ || engine_ != Engine() || !BeginDraw()) return false;
    if (settle_ > 0) {
        --settle_;
        return false;
    }
    const GeRect src = {0, w_ - 1, 0, h_ - 1};
    return g_draw(engine_, bmp_, &src, static_cast<uint32_t>(x < 0 ? 0 : x), static_cast<uint32_t>(y < 0 ? 0 : y)) != 0;
}

// --- AlphaSurface ----------------------------------------------------------------------------
namespace {
uint16_t g_tileBuf[AlphaSurface::kTile * AlphaSurface::kTile];  // game thread only
}

bool AlphaSurface::Upload(const uint32_t* px, int stride, int w, int h, const AlphaKey* keys, int nKeys,
                          uint8_t otherAlpha) {
    if (!g_alphaOk || !px) return false;
    constexpr int kSide = 3;  // kMaxTiles = 3 x 3
    if (w > kSide * kTile) w = kSide * kTile;
    if (h > kSide * kTile) h = kSide * kTile;
    w_ = w, h_ = h;
    cols_ = (w + kTile - 1) / kTile, rows_ = (h + kTile - 1) / kTile;
    for (int ty = 0; ty < rows_; ++ty)
        for (int tx = 0; tx < cols_; ++tx) {
            Tile& t = tiles_[ty * kSide + tx];
            for (int y = 0; y < kTile; ++y) {
                const int sy = ty * kTile + y;
                uint16_t* d = g_tileBuf + y * kTile;
                for (int x = 0; x < kTile; ++x) {
                    const int sx = tx * kTile + x;
                    if (sx >= w || sy >= h) {
                        d[x] = 0;  // outside the content: fully transparent
                        continue;
                    }
                    const uint32_t c = px[sy * stride + sx] & 0xFFFFFF;
                    uint32_t a = otherAlpha;
                    for (int k = 0; k < nKeys; ++k)
                        if (keys[k].rgb == c) a = keys[k].alpha;
                    d[x] = static_cast<uint16_t>(((a >> 4) << 12) | (((c >> 20) & 0xF) << 8) | (((c >> 12) & 0xF) << 4) |
                                                 ((c >> 4) & 0xF));
                }
            }
            if (t.bmp && t.shadow && __builtin_memcmp(t.shadow, g_tileBuf, sizeof(g_tileBuf)) == 0) continue;
            if (!t.shadow &&
                !(t.shadow = static_cast<uint16_t*>(VirtualAlloc(nullptr, sizeof(g_tileBuf), MEM_COMMIT | MEM_RESERVE,
                                                                  PAGE_READWRITE))))
                return false;
            if (!t.bmp && !(t.bmp = g_create(kTile, kTile, 1, kFormat4444))) return false;
            void* target = nullptr;
            if (!g_lock(t.bmp, &target, 0, 0, kFormat4444) || !target) return false;
            uint32_t info[9] = {};
            g_info(target, info, nullptr);
            const int ds = static_cast<int>(info[2]);
            auto* dst = static_cast<uint16_t*>(g_bits(target));
            for (int y = 0; dst && ds >= kTile && y < kTile; ++y)
                __builtin_memcpy(dst + y * ds, g_tileBuf + y * kTile, kTile * sizeof(uint16_t));
            g_unlock(target);
            __builtin_memcpy(t.shadow, g_tileBuf, sizeof(g_tileBuf));
        }
    return true;
}

bool AlphaSurface::Attach(Tile& t) {
    void* world = World();
    if (!world || !t.bmp) return false;
    if (t.world != world) {
        if (!g_worldAdd(world, t.bmp)) return false;
        t.world = world, t.settle = kSettle;
    }
    return true;
}

bool AlphaSurface::Draw(int x, int y) {
    if (!g_alphaOk || !World() || cols_ == 0 || !BeginDraw()) return false;
    constexpr int kSide = 3;
    void* engine = Engine();
    for (int ty = 0; ty < rows_; ++ty)
        for (int tx = 0; tx < cols_; ++tx) {
            Tile& t = tiles_[ty * kSide + tx];
            if (!Attach(t)) return false;
            if (t.settle > 0) {
                --t.settle;
                continue;
            }
            uint32_t handle = 0;
            if (!HandleOf(t.bmp, &handle) || !handle) {
                // Settled without a handle: a level change put the new world at the old
                // world's address (seen bank after Training), so re-add next frame.
                t.world = nullptr;
                return false;  // let the caller fall back this frame
            }
            const int w = w_ - tx * kTile < kTile ? w_ - tx * kTile : kTile;
            const int h = h_ - ty * kTile < kTile ? h_ - ty * kTile : kTile;
            // Half a pixel back: texel centres on pixel centres, so the text stays crisp.
            const float x0 = static_cast<float>(x + tx * kTile) - 0.5f, y0 = static_cast<float>(y + ty * kTile) - 0.5f;
            const float x1 = x0 + w, y1 = y0 + h, u1 = static_cast<float>(w) / kTile, v1 = static_cast<float>(h) / kTile;
            const TLVertex v[4] = {{x0, y0, 1.0f, 0, 0, 255, 255, 255, 255},
                                   {x1, y0, 1.0f, u1, 0, 255, 255, 255, 255},
                                   {x1, y1, 1.0f, u1, v1, 255, 255, 255, 255},
                                   {x0, y1, 1.0f, 0, v1, 255, 255, 255, 255}};
            g_poly(engine, v, 4, t.bmp, kImmediate);
        }
    return true;
}

void AlphaSurface::Destroy() {
    for (Tile& t : tiles_) {
        // Only the world still alive can be told; an earlier one (level change) was
        // destroyed with its bitmap list.
        if (t.bmp && t.world && t.world == g_lastWorld) g_worldRemove(t.world, t.bmp);
        if (t.bmp) g_destroy(&t.bmp);
        if (t.shadow) VirtualFree(t.shadow, 0, MEM_RELEASE);
        t = {};
    }
    cols_ = rows_ = 0;
}

// --- SolidRect ---------------------------------------------------------------------------------
bool SolidRect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_alphaOk || !BeginDraw()) return false;
    const float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = x0 + w, y1 = y0 + h;
    const float r = static_cast<float>((rgb >> 16) & 0xFF), g = static_cast<float>((rgb >> 8) & 0xFF),
                b = static_cast<float>(rgb & 0xFF);
    const TLVertex v[4] = {{x0, y0, 1.0f, 0, 0, r, g, b, 255},
                           {x1, y0, 1.0f, 0, 0, r, g, b, 255},
                           {x1, y1, 1.0f, 0, 0, r, g, b, 255},
                           {x0, y1, 1.0f, 0, 0, r, g, b, 255}};
    return g_poly(Engine(), v, 4, nullptr, kImmediate) != 0;
}

// --- World boxes ---------------------------------------------------------------------------------
namespace {

// One solid white 4444 tile per alpha level in use; vertex colour tints it (spike:
// D3DDrv modulates textured polys, it ignores alpha on untextured ones).
constexpr int kBoxTex = 64;
struct BoxTile {
    void* bmp;
    void* world;
    int settle;
};
// By alpha nibble, 8 KB each, kept for the session. There were 4 slots, never freed: after the
// replay (3 levels) and one more, trigger boxes got no tile and drew nothing (2026-10-26).
BoxTile g_boxTiles[16] = {};

void* BoxTile_(int a4) {
    BoxTile* t = &g_boxTiles[a4 & 15];
    if (!t->bmp) {
        if (!(t->bmp = g_create(kBoxTex, kBoxTex, 1, kFormat4444))) return nullptr;
        void* target = nullptr;
        if (!g_lock(t->bmp, &target, 0, 0, kFormat4444) || !target) return nullptr;
        uint32_t info[9] = {};
        g_info(target, info, nullptr);
        auto* d = static_cast<uint16_t*>(g_bits(target));
        const auto texel = static_cast<uint16_t>((a4 << 12) | 0x0FFF);
        for (int y = 0; d && static_cast<int>(info[2]) >= kBoxTex && y < kBoxTex; ++y)
            for (int x = 0; x < kBoxTex; ++x) d[y * info[2] + x] = texel;
        g_unlock(target);
        t->world = nullptr;
    }
    if (t->world != g_world) {
        if (!g_worldAdd(g_world, t->bmp)) return nullptr;
        t->world = g_world, t->settle = kSettle;
    }
    if (t->settle > 0) {
        --t->settle;
        return nullptr;
    }
    uint32_t handle = 0;
    if (HandleOf(t->bmp, &handle) && handle) return t->bmp;
    log::Info("gfx: world box tile without a handle; re-adding to world %08lx",
              reinterpret_cast<unsigned long>(g_world));
    t->world = nullptr;  // settled without a handle: the world was replaced at the same address, re-add
    return nullptr;
}

constexpr float kNear = 2.0f;  // world units in front of the eye
constexpr int kMaxBoxes = 512;
struct CamV {
    float x, y, d;  // camera space; d = forward depth (= -z)
};
struct ScrV {
    float x, y, z;  // projected; z = geCamera_Project's depth
};
struct Face {
    CamV c[4];
    float depth;
    uint32_t rgb;
    int a4;
};
Face g_faces[kMaxBoxes * 6];

template <class V, class D> int Clip(const V* in, int n, V* out, D dist) {  // keep dist >= 0
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const V &a = in[i], &b = in[(i + 1) % n];
        const float da = dist(a), db = dist(b);
        if (da >= 0) out[m++] = a;
        if ((da >= 0) != (db >= 0)) {
            const float t = da / (da - db);
            V v = a;
            const float* pa = &a.x;
            const float* pb = &b.x;
            float* pv = &v.x;
            for (int k = 0; k < static_cast<int>(sizeof(V) / sizeof(float)); ++k) pv[k] = pa[k] + (pb[k] - pa[k]) * t;
            out[m++] = v;
        }
    }
    return m;
}

// Screen clipping interpolates in screen space, where 1/depth is linear and depth is not: clip on
// 1/z. Clipped on z, a strip from behind the eye to far away got nearly the far end's depth all
// along and sank under the floor (owner report 2026-10-26, mbase; docs/evidence/replay_depth).
template <class V> void InvertZ(V* v, int n) {
    for (int i = 0; i < n; ++i)
        if (v[i].z > 0) v[i].z = 1 / v[i].z;
}

float Dot(float ax, float ay, float az, float bx, float by, float bz) { return ax * bx + ay * by + az * bz; }

void DrawBoxes(const WorldBox* list, int n, bool xray);
void DrawLines(const WorldLine* list, int n, bool xray);

void RunSubs(Layer layer) {
    for (int i = 0; i < g_nLayerSubs[layer]; ++i) g_layerSubs[layer][i]();
}

// LAYER_WORLD, right after geEngine_RenderWorld: immediate polys with the projected z, tested
// against the world's depth only. Drawn any later (D27's queued polys at the late step), things
// the game draws after the world (screen-aligned sprites writing depth over large rectangles)
// cut rectangular holes in the boxes (owner report 2026-10-23, oil_rig_A; docs/evidence/trigger_depth).
void RunWorldLayer() {
#ifdef FDCORE_DEBUG
    MarkLayer("world");
#endif
    for (int i = 0; i < g_nWorldPrev; ++i) DrawBoxes(g_worldPrev[i].boxes, g_worldPrev[i].n, false);
    for (int i = 0; i < g_nLinePrev; ++i) DrawLines(g_linePrev[i].lines, g_linePrev[i].n, false);
    RunSubs(LAYER_WORLD);
    g_flushed = false;  // the frame's later immediate draws flush the game's queued polys again
}

// LAYER_OVERLAY then LAYER_HUD, after everything the game drew (one late-frame subscriber, so
// the order is this code's, D40). This frame end's WORLD boxes move on to the next world render.
void RunLateLayers() {
#ifdef FDCORE_DEBUG
    MarkLayer("overlay");
#endif
    for (int i = 0; i < g_nBoxSubs[LAYER_OVERLAY]; ++i)
        DrawBoxes(g_boxSubs[LAYER_OVERLAY][i].boxes, g_boxSubs[LAYER_OVERLAY][i].n, true);
    for (int i = 0; i < g_nLineSubs[LAYER_OVERLAY]; ++i)
        DrawLines(g_lineSubs[LAYER_OVERLAY][i].lines, g_lineSubs[LAYER_OVERLAY][i].n, true);
    RunSubs(LAYER_OVERLAY);
    g_nWorldPrev = g_nBoxSubs[LAYER_WORLD];
    for (int i = 0; i < g_nWorldPrev; ++i) g_worldPrev[i] = g_boxSubs[LAYER_WORLD][i];
    g_nBoxSubs[LAYER_WORLD] = g_nBoxSubs[LAYER_OVERLAY] = 0;
    g_nLinePrev = g_nLineSubs[LAYER_WORLD];
    for (int i = 0; i < g_nLinePrev; ++i) g_linePrev[i] = g_lineSubs[LAYER_WORLD][i];
    g_nLineSubs[LAYER_WORLD] = g_nLineSubs[LAYER_OVERLAY] = 0;
    g_boxStats = g_boxAcc;
    g_lineSegs = g_lineSegsAcc, g_lineSegsAcc = 0;
#ifdef FDCORE_DEBUG
    if (g_lineUsAcc > 0) g_lineUsSum += g_lineUsAcc, ++g_lineUsFrames;
    g_lineUs = g_lineUsAcc, g_lineUsAcc = 0;
    MarkLayer("hud");
#endif
    RunSubs(LAYER_HUD);
#ifdef FDCORE_DEBUG
    lstrcpynA(g_lastOrder, g_order, sizeof(g_lastOrder));
#endif
}

void DrawBoxes(const WorldBox* list, int n, bool xray) {
    if (!list || n <= 0) return;
    void* engine = Engine();
    if (!g_boxOk || !g_world || !g_camera || !BeginDraw()) return;
    WorldBoxStats st = {n, 0, 0, 0};
    int nf = 0;
    static const int kFace[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (int b = 0; b < n; ++b) {
        const WorldBox& box = list[b];
        CamV c[8];
        for (int i = 0; i < 8; ++i) {
            const Vec3 w = {(i & 1) ? box.max[0] : box.min[0], (i & 2) ? box.max[1] : box.min[1],
                            (i & 4) ? box.max[2] : box.min[2]};
            Vec3 o;
            g_camXform(g_camera, &w, &o);
            c[i] = {o.x, o.y, -o.z};
        }
        // Camera (the origin) inside: 0 <= (O - c0).e <= e.e along the three edges.
        bool inside = true;
        static const int kEdge[3] = {1, 2, 4};
        for (int e : kEdge) {
            const float ex = c[e].x - c[0].x, ey = c[e].y - c[0].y, ed = c[e].d - c[0].d;
            const float t = Dot(-c[0].x, -c[0].y, -c[0].d, ex, ey, ed);
            inside = inside && t >= 0 && t <= Dot(ex, ey, ed, ex, ey, ed);
        }
        st.inside += inside;
        const int a4 = (inside ? box.alphaInside : box.alpha) >> 4;
        if (a4 == 0) continue;
        for (const auto& f : kFace) {
            Face& face = g_faces[nf];
            float sum = 0, maxd = c[f[0]].d;
            for (int k = 0; k < 4; ++k) {
                face.c[k] = c[f[k]], sum += c[f[k]].d;
                if (c[f[k]].d > maxd) maxd = c[f[k]].d;
            }
            if (maxd < kNear) continue;  // wholly behind the eye
            face.depth = sum * 0.25f, face.rgb = box.rgb, face.a4 = a4;
            ++nf;
        }
    }
    st.faces = nf;
    // Far to near (shell sort: up to 3072 faces).
    for (int gap = nf / 2; gap > 0; gap /= 2)
        for (int i = gap; i < nf; ++i) {
            const Face t = g_faces[i];
            int j = i;
            for (; j >= gap && g_faces[j - gap].depth < t.depth; j -= gap) g_faces[j] = g_faces[j - gap];
            g_faces[j] = t;
        }
    int vw, vh;
    ViewSize(&vw, &vh);
    const float W = static_cast<float>(vw), H = static_cast<float>(vh);
    const uint32_t flags = kImmediate;  // both paths: straight to the driver, no depth writes
    void* tiles[16] = {};  // per alpha nibble, resolved once a frame (BoxTile_ counts settle frames)
    bool asked[16] = {};
    for (int i = 0; i < nf; ++i) {
        const Face& f = g_faces[i];
        if (!asked[f.a4]) asked[f.a4] = true, tiles[f.a4] = BoxTile_(f.a4);
        void* tile = tiles[f.a4];
        if (!tile) continue;
        CamV cv[8];
        const int m = Clip(f.c, 4, cv, [](const CamV& v) { return v.d - kNear; });
        if (m < 3) continue;
        ScrV a[16], b[16];
        for (int k = 0; k < m; ++k) {
            const Vec3 in = {cv[k].x, cv[k].y, -cv[k].d};
            Vec3 p;
            g_camProject(g_camera, &in, &p);
            a[k] = {p.x, p.y, p.z};
        }
        InvertZ(a, m);
        int k = Clip(a, m, b, [](const ScrV& v) { return v.x; });
        k = Clip(b, k, a, [W](const ScrV& v) { return W - v.x; });
        k = Clip(a, k, b, [](const ScrV& v) { return v.y; });
        k = Clip(b, k, a, [H](const ScrV& v) { return H - v.y; });
        if (k < 3) continue;
        InvertZ(a, k);
        const float r = static_cast<float>((f.rgb >> 16) & 0xFF), g = static_cast<float>((f.rgb >> 8) & 0xFF),
                    bl = static_cast<float>(f.rgb & 0xFF);
        TLVertex v[16];
        for (int q = 0; q < k; ++q) v[q] = {a[q].x, a[q].y, xray ? 1.0f : a[q].z, 0.5f, 0.5f, r, g, bl, 255};
        g_poly(engine, v, k, tile, flags);
        ++st.polys;
    }
    g_boxAcc.boxes += st.boxes, g_boxAcc.faces += st.faces, g_boxAcc.polys += st.polys, g_boxAcc.inside += st.inside;
}

// sqrt without the CRT (SSE, like glitch_bsp's Sqrt).
float Sqrtf(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }

// One screen-space vertex with its colour (Clip interpolates every float).
struct ScrC {
    float x, y, z, r, g, b;
};

// World lines are drawn as strips (D50): every point is transformed and projected once, and
// consecutive segments share mitered corners, so translucent strips neither overlap nor notch
// at the joins. A point closer than kMergePx on screen to the last one kept adds nothing and
// is skipped (far-away stretches of a dense recording are a pixel per sample).
constexpr int kLineChunk = 1024;     // points per pass; a longer line goes in chunks sharing an end point
constexpr float kMergePx = 1.5f;
constexpr float kMiterLimit = 2.0f;  // longest corner in half widths; sharper turns overlap a little

struct LinePt {
    CamV c;                // camera space
    ScrC s;                // projected (valid when c.d >= kNear)
    float half;            // half width, px
    float ox, oy;          // corner direction: unit perpendicular scaled by the miter
};
LinePt g_lp[kLineChunk];
float g_segDx[kLineChunk], g_segDy[kLineChunk];  // unit screen direction of segment j -> j + 1 (0, 0: none)

// Emits one strip quad, clipped to the view only when it reaches past it.
void EmitQuad(const ScrC (&q)[4], void* tile, float W, float H) {
    bool inside = true;
    for (const ScrC& v : q) inside = inside && v.x >= 0 && v.x <= W && v.y >= 0 && v.y <= H;
    TLVertex v[12];
    int k = 4;
    if (inside) {
        for (int i = 0; i < 4; ++i) v[i] = {q[i].x, q[i].y, q[i].z, 0.5f, 0.5f, q[i].r, q[i].g, q[i].b, 255};
    } else {
        ScrC c1[12], c2[12];
        for (int i = 0; i < 4; ++i) c2[i] = q[i];
        InvertZ(c2, 4);
        k = Clip(c2, 4, c1, [](const ScrC& p) { return p.x; });
        k = Clip(c1, k, c2, [W](const ScrC& p) { return W - p.x; });
        k = Clip(c2, k, c1, [](const ScrC& p) { return p.y; });
        k = Clip(c1, k, c2, [H](const ScrC& p) { return H - p.y; });
        if (k < 3) return;
        InvertZ(c2, k);
        for (int i = 0; i < k; ++i) v[i] = {c2[i].x, c2[i].y, c2[i].z, 0.5f, 0.5f, c2[i].r, c2[i].g, c2[i].b, 255};
    }
    g_poly(Engine(), v, k, tile, kImmediate);
    ++g_lineSegsAcc;
}

ScrC Project(const CamV& c, float r, float g, float b, bool xray) {
    const Vec3 in = {c.x, c.y, -c.d};
    Vec3 p;
    g_camProject(g_camera, &in, &p);
    return {p.x, p.y, xray ? 1.0f : p.z, r, g, b};
}

// Points [from, from + count) of a line: transform, project, merge, corners, then the line and
// its glow as strips. focal: px per world unit at depth 1.
void DrawChunk(const WorldLine& line, int from, int count, float focal, void* tile, void* glowTile, bool xray, float W, float H) {
    int m = 0;
    for (int i = from; i < from + count; ++i) {
        const Vec3 w = {line.pts[i][0], line.pts[i][1], line.pts[i][2]};
        Vec3 o;
        g_camXform(g_camera, &w, &o);
        LinePt& p = g_lp[m];
        p.c = {o.x, o.y, -o.z};
        const uint32_t rgb = line.rgb[i];
        p.s = {0, 0, 0, static_cast<float>((rgb >> 16) & 0xFF), static_cast<float>((rgb >> 8) & 0xFF), static_cast<float>(rgb & 0xFF)};
        if (p.c.d >= kNear) {
            p.s = Project(p.c, p.s.r, p.s.g, p.s.b, xray);
            const LinePt* q = m ? &g_lp[m - 1] : nullptr;
            const float dx = q ? p.s.x - q->s.x : 0, dy = q ? p.s.y - q->s.y : 0;
            if (q && q->c.d >= kNear && i != from + count - 1 && dx * dx + dy * dy < kMergePx * kMergePx) continue;
            float wpx = line.widthPx;
            if (line.widthWorld > 0) {
                wpx = line.widthWorld * focal / p.c.d;
                wpx = wpx < line.widthPx ? line.widthPx : wpx > line.maxPx ? line.maxPx : wpx;
            }
            p.half = wpx * (line.thin && line.thin[i] ? 0.25f : 0.5f);
        }
        ++m;
    }
    // Segment directions, then each point's corner: the mean of its two segments' normals,
    // lengthened so the strip keeps its width through the turn.
    for (int j = 0; j + 1 < m; ++j) {
        const LinePt &a = g_lp[j], &b = g_lp[j + 1];
        const float dx = b.s.x - a.s.x, dy = b.s.y - a.s.y, len = Sqrtf(dx * dx + dy * dy);
        const bool ok = a.c.d >= kNear && b.c.d >= kNear && len >= 0.01f;
        g_segDx[j] = ok ? dx / len : 0, g_segDy[j] = ok ? dy / len : 0;
    }
    for (int j = 0; j < m; ++j) {
        const bool in = j > 0 && (g_segDx[j - 1] != 0 || g_segDy[j - 1] != 0);
        const bool out = j + 1 < m && (g_segDx[j] != 0 || g_segDy[j] != 0);
        const float inx = in ? -g_segDy[j - 1] : 0, iny = in ? g_segDx[j - 1] : 0;
        const float outx = out ? -g_segDy[j] : 0, outy = out ? g_segDx[j] : 0;
        float x = inx + outx, y = iny + outy;
        const float len = Sqrtf(x * x + y * y);
        LinePt& p = g_lp[j];
        if (len < 1e-3f) {  // an end, or a full turn back
            p.ox = out ? outx : inx, p.oy = out ? outy : iny;
            continue;
        }
        x /= len, y /= len;
        const float cosHalf = in ? x * inx + y * iny : 1;  // cos of half the turn
        const float k = cosHalf > 1 / kMiterLimit ? 1 / cosHalf : kMiterLimit;
        p.ox = x * k, p.oy = y * k;
    }
    // The line, then the glow: drawn first, the glow's depth would hide the line in LAYER_WORLD.
    for (int pass = 0; pass < 2; ++pass) {
        const bool glow = pass == 1;
        if (glow && (!line.glowAlpha || !glowTile)) continue;
        const float scale = glow ? line.glowScale : 1.0f;
        for (int j = 0; j + 1 < m; ++j) {
            const LinePt &a = g_lp[j], &b = g_lp[j + 1];
            if (a.c.d < kNear && b.c.d < kNear) continue;
            ScrC q[4];
            if (a.c.d >= kNear && b.c.d >= kNear) {
                if (g_segDx[j] == 0 && g_segDy[j] == 0) continue;
                const float ha = a.half * scale, hb = b.half * scale;
                q[0] = a.s, q[1] = b.s, q[2] = b.s, q[3] = a.s;
                q[0].x += a.ox * ha, q[0].y += a.oy * ha, q[3].x -= a.ox * ha, q[3].y -= a.oy * ha;
                q[1].x += b.ox * hb, q[1].y += b.oy * hb, q[2].x -= b.ox * hb, q[2].y -= b.oy * hb;
            } else {
                // Crosses the near plane: cut there, plain square ends (the cut is at the eye).
                const LinePt& v = a.c.d >= kNear ? a : b;
                const LinePt& h = a.c.d >= kNear ? b : a;
                const float t = (kNear - v.c.d) / (h.c.d - v.c.d);
                const CamV cc = {v.c.x + (h.c.x - v.c.x) * t, v.c.y + (h.c.y - v.c.y) * t, kNear};
                const ScrC cut = Project(cc, v.s.r + (h.s.r - v.s.r) * t, v.s.g + (h.s.g - v.s.g) * t, v.s.b + (h.s.b - v.s.b) * t, xray);
                const float dx = cut.x - v.s.x, dy = cut.y - v.s.y, len = Sqrtf(dx * dx + dy * dy);
                if (len < 0.01f) continue;
                const float hv = v.half * scale, hc = (line.widthWorld > 0 ? line.maxPx : line.widthPx) * 0.5f * scale;
                const float nx = -dy / len, ny = dx / len;
                q[0] = v.s, q[1] = cut, q[2] = cut, q[3] = v.s;
                q[0].x += nx * hv, q[0].y += ny * hv, q[3].x -= nx * hv, q[3].y -= ny * hv;
                q[1].x += nx * hc, q[1].y += ny * hc, q[2].x -= nx * hc, q[2].y -= ny * hc;
            }
            EmitQuad(q, glow ? glowTile : tile, W, H);
        }
    }
}

void DrawLinesTimed(const WorldLine* list, int n, bool xray);

void DrawLines(const WorldLine* list, int n, bool xray) {
#ifdef FDCORE_DEBUG
    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    DrawLinesTimed(list, n, xray);
    QueryPerformanceCounter(&t1);
    g_lineUsAcc += static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(fq.QuadPart);
#else
    DrawLinesTimed(list, n, xray);
#endif
}

void DrawLinesTimed(const WorldLine* list, int n, bool xray) {
    if (!list || n <= 0 || !g_boxOk || !g_world || !g_camera || !BeginDraw()) return;
    int vw, vh;
    ViewSize(&vw, &vh);
    const float W = static_cast<float>(vw), H = static_cast<float>(vh);
    const Vec3 c0 = {0, 0, -100}, c1 = {100, 0, -100};  // px per unit at depth 1, from the camera's projection
    Vec3 p0, p1;
    g_camProject(g_camera, &c0, &p0);
    g_camProject(g_camera, &c1, &p1);
    const float focal = p1.x > p0.x ? p1.x - p0.x : p0.x - p1.x;
    void* tiles[16] = {};  // per alpha nibble, resolved once a frame (BoxTile_ counts settle frames)
    bool asked[16] = {};
    auto tileFor = [&](uint8_t alpha) -> void* {
        const int a4 = alpha >> 4;
        if (a4 && !asked[a4]) asked[a4] = true, tiles[a4] = BoxTile_(a4);
        return tiles[a4];
    };
    for (int l = 0; l < n; ++l) {
        const WorldLine& line = list[l];
        if (line.n < 2 || !line.pts || !line.rgb) continue;
        void* tile = tileFor(line.alpha);
        if (!tile) continue;
        void* glowTile = line.glowAlpha ? tileFor(line.glowAlpha) : nullptr;
        for (int from = 0; from < line.n - 1; from += kLineChunk - 1)
            DrawChunk(line, from, line.n - from < kLineChunk ? line.n - from : kLineChunk, focal, tile, glowTile, xray, W, H);
    }
}

} // namespace

bool ProjectWorld(const float* point, float* x, float* y) {
    if (!g_camera || !g_camXform || !g_camProject) return false;
    const Vec3 world = {point[0],point[1],point[2]}; Vec3 view, screen;
    g_camXform(g_camera, &world, &view);
    if (view.z > -1) return false;
    g_camProject(g_camera, &view, &screen); *x = screen.x; *y = screen.y; return true;
}
bool ScreenRay(float x, float y, float* origin, float* direction) {
    camera::XForm view;
    if (!g_camera || !g_camProject || !camera::ViewXForm(&view)) return false;
    const Vec3 zero = {0,0,-1}, unit = {1,1,-1}; Vec3 center, scale;
    g_camProject(g_camera, &zero, &center); g_camProject(g_camera, &unit, &scale);
    if (scale.x == center.x || scale.y == center.y) return false;
    const float right = (x-center.x)/(scale.x-center.x), up = (y-center.y)/(scale.y-center.y);
    for (int a = 0; a < 3; ++a) {
        origin[a] = view.t[a];
        direction[a] = view.m[a*3]*right + view.m[a*3+1]*up - view.m[a*3+2];
    }
    return true;
}

void ClearWorldSubmissions() {
    g_nWorldPrev = g_nLinePrev = 0;
    g_nBoxSubs[0] = g_nBoxSubs[1] = 0;
    g_nLineSubs[0] = g_nLineSubs[1] = 0;
}

void SubmitWorldLines(const WorldLine* lines, int n, Layer layer) {
    if (!lines || n <= 0 || layer == LAYER_HUD) return;
    if (g_nLineSubs[layer] < kMaxLayerSubs) g_lineSubs[layer][g_nLineSubs[layer]++] = {lines, n};
}

int LastWorldLineSegments() { return g_lineSegs; }

#ifdef FDCORE_DEBUG
void WorldLineCost(int* lastUs, int* meanUs) {
    *lastUs = static_cast<int>(g_lineUs + 0.5);
    *meanUs = g_lineUsFrames ? static_cast<int>(g_lineUsSum / g_lineUsFrames + 0.5) : 0;
    g_lineUsSum = 0, g_lineUsFrames = 0;
}
#endif

bool WorldBoxesReady() { return g_boxOk; }

void SubmitWorldBoxes(const WorldBox* boxes, int n, Layer layer) {
    if (!boxes || n <= 0 || layer == LAYER_HUD) return;
    if (n > kMaxBoxes) n = kMaxBoxes;
    if (g_nBoxSubs[layer] < kMaxLayerSubs) g_boxSubs[layer][g_nBoxSubs[layer]++] = {boxes, n};
}

void OnLayer(Layer layer, LayerFn fn) {
    if (g_nLayerSubs[layer] < kMaxLayerSubs) g_layerSubs[layer][g_nLayerSubs[layer]++] = fn;
}

#ifdef FDCORE_DEBUG
const char* LastLayerOrder() { return g_lastOrder; }

namespace {
// Test hook (tests/game/draw_layers.yaml): `fd_dbg_layer_box world|overlay|off x0 y0 z0 x1 y1 z1`
// submits one magenta box in that layer every frame end.
WorldBox g_testBox = {};
int g_testLayer = -1;
float ParseF(const char* s) {
    float sign = 1, v = 0, sc = 0.1f;
    if (*s == '-') sign = -1, ++s;
    while (*s >= '0' && *s <= '9') v = v * 10 + static_cast<float>(*s++ - '0');
    if (*s == '.')
        for (++s; *s >= '0' && *s <= '9'; sc *= 0.1f) v += static_cast<float>(*s++ - '0') * sc;
    return sign * v;
}
void CmdLayerBox(int argc, const char* const* argv) {
    if (argc >= 2 && argv[1][0] == 'o' && argv[1][1] == 'f') {
        g_testLayer = -1;
        log::Info("gfx: test box off");
        return;
    }
    if (argc < 8 || (argv[1][0] != 'w' && argv[1][0] != 'o')) {
        log::Info("gfx: usage: fd_dbg_layer_box world|overlay|off x0 y0 z0 x1 y1 z1");
        return;
    }
    for (int i = 0; i < 3; ++i) g_testBox.min[i] = ParseF(argv[2 + i]), g_testBox.max[i] = ParseF(argv[5 + i]);
    g_testBox.rgb = 0xFF00FF, g_testBox.alpha = 0xC0, g_testBox.alphaInside = 0xC0;
    g_testLayer = argv[1][0] == 'w' ? LAYER_WORLD : LAYER_OVERLAY;
    log::Info("gfx: test box in layer %s", g_testLayer == LAYER_WORLD ? "world" : "overlay");
}
void SubmitTestBox() {
    if (g_testLayer >= 0) SubmitWorldBoxes(&g_testBox, 1, static_cast<Layer>(g_testLayer));
}
}  // namespace
#endif

WorldBoxStats LastWorldBoxStats() { return g_boxStats; }

// --- init ----------------------------------------------------------------------------------------
bool Init() {
    const Id needs[] = {Id::genesis_bitmap_create, Id::genesis_bitmap_lock_write_format, Id::genesis_bitmap_unlock,
                        Id::genesis_bitmap_get_bits, Id::genesis_bitmap_get_info, Id::genesis_bitmap_destroy,
                        Id::genesis_draw_bitmap, Id::genesis_add_bitmap, Id::genesis_remove_bitmap,
                        Id::genesis_flush_pipeline};
    for (Id id : needs)
        if (!addr::Usable(id)) {
            log::Warn("gfx: disabled, no on-screen text (needs %s)", addr::Name(id));
            return false;
        }
    g_create = reinterpret_cast<CreateFn>(addr::ExportAddress(Id::genesis_bitmap_create));
    g_lock = reinterpret_cast<LockFn>(addr::ExportAddress(Id::genesis_bitmap_lock_write_format));
    g_unlock = reinterpret_cast<BmpFn>(addr::ExportAddress(Id::genesis_bitmap_unlock));
    g_bits = reinterpret_cast<GetBitsFn>(addr::ExportAddress(Id::genesis_bitmap_get_bits));
    g_info = reinterpret_cast<GetInfoFn>(addr::ExportAddress(Id::genesis_bitmap_get_info));
    g_destroy = reinterpret_cast<DestroyFn>(addr::ExportAddress(Id::genesis_bitmap_destroy));
    g_draw = reinterpret_cast<DrawFn>(addr::ExportAddress(Id::genesis_draw_bitmap));
    g_add = reinterpret_cast<EngineBmpFn>(addr::ExportAddress(Id::genesis_add_bitmap));
    g_remove = reinterpret_cast<EngineBmpFn>(addr::ExportAddress(Id::genesis_remove_bitmap));
    g_flush = reinterpret_cast<FlushFn>(addr::ExportAddress(Id::genesis_flush_pipeline));
    g_ok = true;
    cmd::RegisterCvar(&g_fdTextSize);
    hooks::OnFrameBegin(FrameBegin);

    // Alpha surfaces / solid rects: optional on top of the decal path.
    const Id alphaNeeds[] = {Id::genesis_render_poly, Id::genesis_render_world, Id::genesis_world_add_bitmap,
                             Id::genesis_world_remove_bitmap};
    const char* missing = nullptr;
    for (Id id : alphaNeeds)
        if (!missing && !addr::Usable(id)) missing = addr::Name(id);
    if (missing) {
        log::Warn("gfx: no alpha surfaces (needs %s); panels drawn opaque", missing);
    } else {
        g_poly = reinterpret_cast<RenderPolyFn>(addr::ExportAddress(Id::genesis_render_poly));
        g_worldAdd = reinterpret_cast<WorldBmpFn>(addr::ExportAddress(Id::genesis_world_add_bitmap));
        g_worldRemove = reinterpret_cast<WorldBmpFn>(addr::ExportAddress(Id::genesis_world_remove_bitmap));
        g_alphaOk = hooks::Create("gfx", Id::genesis_render_world, reinterpret_cast<void*>(&RenderWorldDetour),
                                  reinterpret_cast<void**>(&g_origRenderWorld)) &&
                    hooks::SetEnabled("gfx", true);
        if (!g_alphaOk) log::Warn("gfx: geEngine_RenderWorld not hooked; panels drawn opaque");
    }
    // World boxes: the alpha path plus the camera transform / projection.
    if (g_alphaOk) {
        if (!addr::Usable(Id::genesis_camera_transform) || !addr::Usable(Id::genesis_camera_project)) {
            log::Warn("gfx: no world boxes (needs genesis.camera_transform / camera_project)");
        } else {
            g_camXform = reinterpret_cast<CameraFn>(addr::ExportAddress(Id::genesis_camera_transform));
            g_camProject = reinterpret_cast<CameraFn>(addr::ExportAddress(Id::genesis_camera_project));
            g_boxOk = true;
        }
    }
    hooks::OnFrameEndLate(RunLateLayers);  // the only late-frame subscriber: overlay, then HUD (D40)
#ifdef FDCORE_DEBUG
    cmd::RegisterCommand("fd_dbg_layer_box", "(Debug test hook) a magenta box in a draw layer: world|overlay|off x0 y0 z0 x1 y1 z1",
                         CmdLayerBox);
    hooks::OnFrameEnd(SubmitTestBox);
    testpipe::Register("layers", [](const char*, char* resp, int cap) {
        (void)cap;
        const WorldBoxStats st = g_boxStats;
        wsprintfA(resp, "order=%s boxes=%d polys=%d", g_lastOrder[0] ? "" : "none", st.boxes, st.polys);
        if (g_lastOrder[0]) {  // "world overlay hud" -> order=world,overlay,hud
            char o[32];
            lstrcpynA(o, g_lastOrder, sizeof(o));
            for (char* p = o; *p; ++p)
                if (*p == ' ') *p = ',';
            wsprintfA(resp, "order=%s boxes=%d polys=%d", o, st.boxes, st.polys);
        }
    });
#endif
    log::Info("gfx: ready (decals%s%s)", g_alphaOk ? ", alpha surfaces, solid rects" : "",
              g_boxOk ? ", world boxes" : "");
    return true;
}

} // namespace fdcore::gfx
