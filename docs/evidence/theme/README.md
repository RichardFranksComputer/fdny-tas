# One look: theme, native text, panel placement, the juggle overlay (D66)

PR 4 of 6 unifying the tooling (owner 2026-11-11). Stacked on PR 3 (D65).

## What it looks like (Training, 640x480, 2026-11-11)

- `hud_warp_juggle.png`: the player HUD (top left) and the juggle readout (centre: arrow,
  "343u", the frozen spot's coordinates) in the game's font; the warp HUD (top right) in
  Consolas with the theme's colours. The large text in the middle is the game's own hint.
- `editor.png`: the checkpoint editor's panel under the player HUD (they overlapped before):
  orange title, white body, grey key hints, on black.
- `picker.png`: the replay picker in the same form.

## The native label spike (read-only Ghidra, then the game)

The facts are in `docs/game-research.md` ("native text labels and the menu's widgets") and in
the seven `label.*` entries of `core/addresses.json`. In short: a `RichTextLabel` is 0x104
self-contained bytes; constructed, given a font, given text and a rect, and rendered by four
game functions, with no registration. The spike also corrected two notes this project had:
the rect is {left, right, top, bottom}, and hAlign 1 is right, 2 is centre.

Which font, tried in the game in this order:

| Font | Result |
|---|---|
| 0, the HUD status line's (8x12) | lines wrapped inside a rect of `cols x 8` (fixed by running the rect to the layout's edge); then: every glyph's top third missing. The game's own status line at the top of the screen draws the same way on this renderer |
| 3, the console's (8x14) | whole glyphs, but thin grey on the level's bright surfaces, and the minus signs of the coordinates were not visible |
| 1, the hint's (16x24) at 0.6 | reads as the game's; 6, 8 and 0 hard to tell apart |
| 1 at 0.75 | chosen: 9 x 18 layout pixels a glyph, the digits hold |

## Results (VM)

- `.\build.ps1`: Debug 401.5 KB, Release 336.5 KB; arch_check (45 core files) and `-Werror`
  clean. `signatures: all OK` now counts the three new code entries.
- `.\tests\unit\run.ps1`: 408 C++ checks, 0 failed; 49 Python tests OK.
- Game tests the branch touches (36 of 36): **36 passed** in the final run. Before that:
  - first full run, 32 passed. `freecam`, `notify` and `text_panels` crashed
    (`exception c0000005 at D3DDrv.dll+79e0` under `Genesis.dll+2321d`) on the first native
    panel drawn after another level had loaded; `triggers` asserted gfx's old ready line.
  - after the fix (below) those four pass. `text_panels` failed once more with
    `window hung > 10s` during the harness's session reset, with second-long gaps between log
    lines; it passed in the next two runs unchanged (the VM stalls like this on some runs:
    PR 1's first attempt, PR 3's `bindlist`).
- `text_alpha` now compares inside the warp HUD's panel: text opacity 255 vs 96 differs by
  2.85 (limit 2), 255 vs 255 again by 0.16 (limit 0.5). It was 4.56 and 0.00 inside the
  player HUD's panel, which is native text now and has no opacity setting.

## Found on the way

1. **The crash.** A label caches its font's `geBitmap*` and the bitmap belongs to the level.
   The first version noticed a new level by the world's address, which D30 had already
   found does not change between levels. gfx now counts levels (the level-load event, and
   every passage through a menu or a loading screen); a label drops its cached bitmap when
   the count has moved, and draws only while the game state is "in a level".
2. **140 KB of zeros.** `NativeText`'s members first had the defaults -1 and 1.0f, which
   moved textdraw's panel table from .bss into .data: Debug went from 393 to 536.5 KB.
   Every member starts as zero; 398.5 KB after.
3. **The rect.** The label wraps at its rect's width by its own measure, not `cols x cell`.

## Limits

- Native text is drawn in a level only. The HUD fonts are not loaded in menus, and a missing
  bitmap raises the game's own fatal error.
- The game's font shows a few stray pixels above some glyphs (in the game's own hints too).
- The small HUD font may draw correctly on other renderers; `kNativeFont` and
  `kNativeScale` in `core/textdraw.cpp` are the two lines to change.
