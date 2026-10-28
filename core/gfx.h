// gfx: reusable in-frame 2D drawing blocks (owner request after the alpha spike,
// docs/evidence/alpha_spike/README.md). Everything runs on the game thread at
// frame end (hooks::OnFrameEnd); textdraw is built on these.
//
//   Canvas        GDI text on a monospace grid -> 32-bit pixels (0x00RRGGBB).
//   Decal         opaque 565 engine bitmap, geEngine_DrawBitmap. Queued: drawn at
//                 EndFrame's flush, on top of everything. Works without a world (menus).
//   AlphaSurface  per-pixel alpha: <=256 px 4444 tiles attached to the rendered
//                 world (geWorld_AddBitmap), drawn with geEngine_RenderPoly. Needs a
//                 world this frame; only changed tiles are re-uploaded.
//   SolidRect     untextured opaque quad (D3DDrv ignores vertex alpha on these).
//
// Order within a frame: BeginDraw() once (flushes the cached 3D, or immediate polys
// would be drawn under it), then AlphaSurface / SolidRect draw immediately; Decals
// queue and land on top at EndFrame.
#pragma once

#include <stdint.h>

namespace fdcore::gfx {

bool Init();   // resolves the Genesis API, hooks geEngine_RenderWorld; false = no in-frame drawing
bool Ready();
// Drop queued world/overlay submissions after a completed frame, before a
// scene/menu transition. The next world must never inherit the old scene's lists.
void ClearWorldSubmissions();

void* Engine();  // geEngine* of the frame in progress
void* World();   // geWorld* rendered this frame (null when none was)
// Flushes the cached 3D once per frame; call before the first immediate draw.
// False when there is no engine this frame.
bool BeginDraw();
// The render view size (the camera rect; the window as fallback).
void ViewSize(int* w, int* h);
// Last rendered camera: screen projection and an unnormalised world picking ray.
// False without a rendered world, or when projecting behind the near plane.
bool ProjectWorld(const float* point, float* x, float* y);
bool ScreenRay(float x, float y, float* origin, float* direction);

// --- Canvas ------------------------------------------------------------------------
// One shared font (Consolas, NONANTIALIASED, `fd_text_size` px). Text pixels are
// exactly the colour given, so surfaces can key alpha on the background colour.
class Canvas {
public:
    // Sizes the canvas for cols x rows cells plus `pad` px all round, filled with bg.
    bool Begin(int cols, int rows, int pad, uint32_t bg);
    // One run of text at a cell position; bold selects the bold font.
    void Text(int col, int row, const char* s, int len, uint32_t fg, uint32_t bg, bool bold);
    void Finish();  // GdiFlush before the pixels are read

    const uint32_t* Pixels() const { return px_; }
    int Stride() const { return cap_w_; }  // pixels per row
    int Width() const { return w_; }       // content size
    int Height() const { return h_; }

private:
    void* dib_ = nullptr;
    uint32_t* px_ = nullptr;
    int w_ = 0, h_ = 0, cap_w_ = 0, cap_h_ = 0, pad_ = 0;
};
// Cell size of the current font; bumps FontGeneration() when fd_text_size changes.
void CellSize(int* w, int* h);
unsigned FontGeneration();

// --- Decal -------------------------------------------------------------------------
class Decal {
public:
    bool Upload(const uint32_t* px, int stride, int w, int h);  // (re)creates as needed
    bool Draw(int x, int y);  // false while settling / without a bitmap
    void Destroy();

private:
    void* bmp_ = nullptr;
    void* engine_ = nullptr;
    int cap_w_ = 0, cap_h_ = 0, w_ = 0, h_ = 0, settle_ = 0;
};

// --- AlphaSurface --------------------------------------------------------------------
// Alpha by background colour: pixels equal to a key's rgb get its alpha, all others
// (the text) `otherAlpha`. Same per-pixel cost whatever the values: the tile is
// blended either way; strictly opaque drawing is the Decal.
struct AlphaKey {
    uint32_t rgb;
    uint8_t alpha;
};
class AlphaSurface {
public:
    static constexpr int kTile = 256;  // D3DDrv's 3D texture limit (512 crashes it)
    static constexpr int kMaxTiles = 9;  // up to 768 x 768

    bool Upload(const uint32_t* px, int stride, int w, int h, const AlphaKey* keys, int nKeys,
                uint8_t otherAlpha = 255);
    // False when it can't draw this frame (no world, or tiles still settling); the
    // caller may fall back to a Decal.
    bool Draw(int x, int y);
    void Destroy();

private:
    struct Tile {
        void* bmp;
        uint16_t* shadow;  // last uploaded 4444 texels (dirty check)
        void* world;       // the world it is attached to
        int settle;
    };
    Tile tiles_[kMaxTiles] = {};
    int cols_ = 0, rows_ = 0, w_ = 0, h_ = 0;
    bool Attach(Tile& t);
};

// --- SolidRect -------------------------------------------------------------------------
bool SolidRect(int x, int y, int w, int h, uint32_t rgb);

// --- Draw layers (D40) ---------------------------------------------------------------------
// Everything FDNYCore draws belongs to one layer. gfx runs them in this order every frame
// (the order is code here, not registration order):
//   LAYER_WORLD    right after geEngine_RenderWorld: hidden by walls and floors (depth-tested
//                  against the world only). The game's sprites, actors and HUD go over it.
//                  3D things that live in the level (trigger boxes, markers, paths).
//   LAYER_OVERLAY  after everything the game draws: over walls (x-ray views).
//   LAYER_HUD      last: screen panels (textdraw), over everything.
// Why WORLD can't be drawn later: the game's later draws write depth over whole screen
// rectangles, which cut holes in anything depth-tested after them (D39, docs/evidence/trigger_depth).
// Update state in hooks::OnFrameEnd; draw from a layer (features use world boxes or textdraw;
// a new kind of drawing is a gfx block first, R5).
enum Layer : uint8_t { LAYER_WORLD, LAYER_OVERLAY, LAYER_HUD };
using LayerFn = void (*)();
void OnLayer(Layer layer, LayerFn fn);  // runs fn in that layer every frame, in registration order
#ifdef FDCORE_DEBUG
const char* LastLayerOrder();  // the layers that ran last frame, in order (tests/game/draw_layers.yaml)
#endif

// --- World boxes -------------------------------------------------------------------------
// Translucent axis-aligned boxes in world space (trigger boxes, docs/evidence/
// trigger_boxes). Each face is transformed by the rendered camera, clipped to the near
// plane and the view, and drawn as a polygon textured with a solid white 4444 tile of
// the box's alpha, tinted by vertex colour. Faces are drawn far to near.
//   LAYER_WORLD:   projected depth, so walls hide them (drawn in the next frame's world render
//                  from the list submitted at this frame end; boxes are static, the camera current)
//   LAYER_OVERLAY: depth 1.0, over everything
// Submit from OnFrameEnd, every frame; several features may submit in one frame.
struct WorldBox {
    float min[3], max[3];
    uint32_t rgb;           // 0xRRGGBB
    uint8_t alpha;          // 0..255, kept to 4 bits (4444)
    uint8_t alphaInside;    // used instead while the camera is inside the box
};
bool WorldBoxesReady();     // the camera / poly API resolved
// The array must stay valid until it is drawn: the next frame for LAYER_WORLD (the caller's
// static storage). Not LAYER_HUD.
void SubmitWorldBoxes(const WorldBox* boxes, int n, Layer layer);
struct WorldBoxStats {
    int boxes, faces, polys;  // last frame, both layers: boxes drawn, faces in front of the camera, polys drawn
    int inside;               // boxes containing the camera
};
WorldBoxStats LastWorldBoxStats();

// --- World lines -------------------------------------------------------------------------
// Polylines in world space (recording traces, the replay's box edges and arrow), each one
// strip facing the camera with mitered corners (D50), coloured per point (blended along each
// segment), drawn with the world-box tiles and alpha. Same layers and lifetime as world boxes.
struct WorldLine {
    const float (*pts)[3];
    const uint32_t* rgb;   // one per point, 0xRRGGBB
    const uint8_t* thin;   // null, or one per point: nonzero = half width there
    int n;                 // points; n - 1 segments
    float widthPx;         // width in px; with widthWorld, the least
    float widthWorld;      // 0, or the width in world units (narrower with distance), held
    float maxPx;           //   between widthPx and maxPx
    uint8_t alpha;         // kept to 4 bits like the boxes
    uint8_t glowAlpha;     // 0, or a strip glowScale x as wide under the line, at this alpha
    float glowScale;
};
void SubmitWorldLines(const WorldLine* lines, int n, Layer layer);
int LastWorldLineSegments();  // segments drawn last frame, both layers
#ifdef FDCORE_DEBUG
// CPU time drawing world lines (both layers), in microseconds: the frame before and the mean
// per frame since the previous call (frames that drew lines; resets the mean).
void WorldLineCost(int* lastUs, int* meanUs);
#endif

} // namespace fdcore::gfx
