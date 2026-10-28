# Recordings browser and viewer

The briefing's Recordings button lists six JSONL files per page for the selected
stage, newest first. Recent and Favorites tabs retain a visible selection.
Details offer Show ghost, Compare, Rename and Favorite. Names and favorites are
stored in `<recording>.jsonl.fdny-meta`; writes use a flushed temporary file and
an atomic replacement. Invalid metadata is preserved. JSONL data is never edited.
An incomplete file remains labeled and can be played or compared through its
last usable sample; a partial final line is ignored by the existing reader.

The viewer requires the practice profile, pauses simulation and suppresses new
recordings, checkpoints, trigger overlays and ordinary free-camera controls.
It starts in third person. F releases/re-hooks the camera, Tab switches targets,
right-drag orbits/looks, wheel/PageUp/PageDown zoom, Space plays/pauses,
Left/Right seek five seconds, Home restarts, X toggles through-walls drawing and
Escape returns to the list. Held/repeated keys do not create duplicate actions;
short window-message taps are retained until frame polling consumes them.

## Native integration

`core/menu.cpp` owns the briefing draw hook, native screen/row construction and
window input routing. The page reuses the Records background, profile rows and
matching native font atlas/metrics; detached labels cannot steal menu focus.
The selected tab uses the row font animation's highlight frame. Menu operations
use address-table functions, with structure fields and sizes also in
`core/addresses.json`; their signatures are the first 16 bytes of the retail
functions.

### The native menu, verified (menu-address spike, 2026-10-28)

All 36 `menu.*` entries were checked read-only in Ghidra by two analysis agents and
adjudicated against FDNYCore's calls; every identity, calling convention, offset and
size holds. Each entry's `ghidra` field cites the instructions; `research/research.db`
has the symbols, three confirmed layout hypotheses with evidence, and the events.

| Object | Layout |
|---|---|
| MenuManager (GameSession+0x3C8) | +0x10 active screen; +0x14.. nav stack of screen indices, +0x3C depth (max 10); +0x158 SArrayPtr of 10 screens (count +0x15C). `GotoScreen` 0x0042D3E0 bounds-checks the count, so an appended 11th screen (the Recordings page) is reachable. Title is screen 2. |
| MenuScreen (vtbl 0x004E0840, 0x2C bytes) | vtbl [0] OnActivate(bool), [1] OnButtonActivate(id), [3] Draw 0x00435D80; +4/+8 backgrounds; +0x10/+0x14 widget array; +0x28 MenuManager. `Construct(manager, bgA, bgB)` 0x00436630; (11, 17) are the backgrounds of `MenuScreen_List` 0x00432BB0 (screen 4; whether it is the game's Records page is not verified). |
| MenuScreen_Title (vtbl 0x004DFD14) | +0x2C selected level **id** (TLevelInfo+0x18, not an index); `SelectLevel(id)` 0x0042F280 (Ghidra's label, FindFirstPlayableLevel, is wrong; the comment there says so); `Draw` 0x0042EEA0 (vtbl[3]; now a function in Ghidra), which FDNYCore hooks. Draw also recomputes the title buttons' enabled bytes each frame, so the hook always calls it. |
| MenuWidget / ListRow (0x378 bytes) | +4 command id, +8..+0x14 rect, +0x1C draw gate; labels at +0x4C/+0x150 (label +0x10 = visible byte, +0xEC = highlight animation, +0xF8/+0xFC = float glyph scale); row sprite +0x254 (+0x101 visible byte). `menu.f_row_style` (+0x160) and `menu.f_row_flag` (+0x355) are the second label's and the sprite's visible bytes: clearing them hides those parts. |
| RichTextLabel::SetText 0x0048C780 | `(String text, const RECT*, hAlign, vAlign)`, RET 0x10: the String is consumed, so the page passes a fresh one (`console::MakeGameString`), as the console does. |

`core/spectator.cpp` accepts target poses and emits actions. It knows no file
format or feature commands. The camera producer applies the prepared follow
transform before the first world render. Scene transitions wait until after
`geEngine_EndFrame` and clear queued geometry before releasing the old scene.

## Validation

- Architecture rules and generated player reference: pass.
- Debug and Release builds with Zig 0.16.0: pass. Core-only and replay-excluded
  Release builds also pass.
- C++ unit tests: 202 checks passed, covering incomplete tails, metadata validation
  and round trips, paging bounds, orbit geometry, zoom limits and modal key edges.
- Python unit tests: 27 passed.
- `tests/game/spectator_camera.yaml` covers paused playback, camera/target actions,
  recording suppression and exit using real keypresses and read-only state queries.
