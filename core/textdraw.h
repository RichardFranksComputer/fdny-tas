// In-frame text panels (after item 13, owner request "cut it over"): every
// on-screen text of the core (hud_glitch boxes, hud_player, watermark) is drawn
// through here, on the gfx blocks (gfx.h). 0.15: translucent panels (owner: smaller
// font, watermark stays opaque, reusable helpers per rendering type).
//
// Why: geEngine_Printf blits one glyph per character (~29 us per blit through
// D3DDrv/dgVoodoo, area free) and keeps at most 40 strings x 80 chars, so a
// full HUD cost most of the frame. A panel is rendered with GDI into ONE
// Genesis bitmap only when its text changes (at most every `minFrames`
// frames), and each frame costs one geEngine_FlushPipeline + one
// geEngine_DrawBitmap per visible panel. The flush matters: without it the
// engine's cached 3D polys are drawn after (over) the blit inside EndFrame.
// Measured: a 400x346 box at full frame rate (docs/evidence/foundation, "In-frame text
// investigation").
#pragma once

#include <stdint.h>

namespace fdcore::textdraw {

constexpr int kMaxLines = 40, kLineLen = 160;

// Per-character attribute: foreground colour index into the panel's palette
// (bits 0-4), bold (bit 5), background slot 0-3 (bits 6-7). The default palette
// has C_TEXT and C_WHITE; a feature with its own colours sets them with SetPalette
// (the collision overlay: features/glitch/glitch_palette.h).
constexpr uint8_t A_COLOR_MASK = 0x1F, A_BOLD = 0x20, A_BG_SHIFT = 6;
// The default palette (core/theme.h): every panel that doesn't bring its own has these.
constexpr uint8_t C_TEXT = 0, C_WHITE = 1, C_TITLE = 2, C_DIM = 3, C_GOOD = 4, C_BAD = 5;
// Where a corner panel sits when it is alone (view pixels): the edge margin, and the first
// free row under the game's mission timer for the top-left corner.
constexpr int kEdge = 8, kUnderTimer = 52;
constexpr int kMaxColors = 32;

// CENTER: dx from the horizontal centre (the panel is centred on it), dy from the
// vertical centre to the panel's top edge.
enum Anchor : uint8_t { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, CENTER };
// STYLE_TRANSLUCENT: gfx::AlphaSurface (background at fd_text_alpha) while the frame has a
// world, the opaque decal otherwise (menus). STYLE_OPAQUE: always the decal.
// STYLE_TEXT_ONLY: the AlphaSurface with the panel background fully transparent (text
// only); not drawn at all without a world (no opaque fallback box).
// STYLE_NATIVE: the game's own HUD font, no background, like the game's status line and
// hints (gfx::NativeText, D66). For short texts: a readout, a tool's title and hints. In a
// level only. Colours work (the palette), bold and background slots don't. If the game's
// label functions aren't verified it falls back to STYLE_TRANSLUCENT.
enum Style : uint8_t { STYLE_TRANSLUCENT, STYLE_OPAQUE, STYLE_TEXT_ONLY, STYLE_NATIVE };

// A panel: anchor + offset (pixels from that corner of the view, or from the centre for
// CENTER), how often its bitmap may be re-rendered, the attribute for lines set without
// attrs, and its style. Panels on one corner never overlap: each is placed clear of the
// ones created before it that are shown (downwards at the top, upwards at the bottom), so
// a feature's offset is where its panel sits when it is alone (D66).
int Create(const char* name, Anchor anchor, int dx, int dy, int minFrames, uint8_t defaultAttr,
           Style style = STYLE_TRANSLUCENT);
// The panel's colours: fg[nFg] (0x00RRGGBB, nFg <= kMaxColors) and the four
// background slots (slot 0 = the panel background, at fd_text_alpha; slots 1-3
// are highlight backgrounds, kept nearly opaque). bg may be null (default).
void SetPalette(int panel, const uint32_t* fg, int nFg, const uint32_t* bg4);
// New content. Re-rendered (at most every minFrames frames) only if it changed.
void Set(int panel, const char (*lines)[kLineLen], const uint8_t (*attrs)[kLineLen], int n);
void SetLines(int panel, const char* const* lines, int n);  // plain text, the panel's default tag
// A tool's panel, the same way everywhere: the title line (the tool's name in capitals,
// then what it is working on) in the accent colour, the body in the text colour, and the
// last nHints lines (keys, how to leave) dimmed.
void SetTitled(int panel, const char* title, const char* const* lines, int n, int nHints);
void Show(int panel, bool visible);
// A view-relative panel (e.g. an arrow relative to the player's view) is meaningless to a
// free camera: not drawn while the game is paused (pause service, D35).
void HideWhilePaused(int panel, bool on);

// The game's own bitmap-font hint renderer; no Canvas/AlphaSurface uploads.
// A priority hint suppresses other hints until it expires; newer priority hints replace it.
// Without priority (a confirmation, notify.h) the game's next hint replaces it, and a
// priority hint still in its window keeps the window. Offsets FDNYCore's hints upward;
// retains the original center if positioning is unavailable.
bool InitGameHints();
bool GameHint(const char* text, uint32_t milliseconds, bool priority = true);
void ClearGameHint(bool hide = true);

// Core service: registers the draw as a late frame-end subscriber, so it runs
// after every module's frame-end update of its panels.
bool Init();

} // namespace fdcore::textdraw
