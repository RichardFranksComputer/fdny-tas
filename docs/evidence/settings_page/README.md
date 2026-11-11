# The settings page (D67)

PR 5 of 6 unifying the tooling (owner 2026-11-11). Stacked on PR 4 (D66).

## What it looks like (640x480, 2026-11-11)

- `options_entry.png`: the game's Options screen with the "FDNYCore" row on its bottom bar.
- `page1.png`: the first of three pages: two sliders (Panel text size, Panel opacity) and six
  option rows (Profile, Watermark, the alt-tab cursor fix, Fixed victim spawns, RescueBot,
  Player HUD), in the game's own widgets on the Options screen's background.

19 rows today, from the registry: every variable with a label and a type.

## How it was found to be possible

A read-only Ghidra lookup (`docs/game-research.md`, "native text labels and the menu's
widgets"): the Options screen is built from `Slider` and `OptionCycler`, each constructed by
one call `(owner, String* label, id, rect)` and reporting through the owning screen's
vtable ([1] for a cycler's click, [2] for a slider's drag). Twelve new entries in
`core/addresses.json`: the two constructors with signatures, and ten sizes, offsets and
vtable slots. The page worked on the first build.

## Results (VM)

- `.\build.ps1`: Debug 412 KB, Release 345 KB; arch_check and `-Werror` clean.
- `.\tests\unit\run.ps1`: 409 C++ checks, 0 failed; 49 Python tests OK.
- `tests/game/settings_page.yaml` (new, real clicks, 27 assertions): the entry row is there
  and the page is not built until opened; Watermark Off writes `fd_watermark "0"` to
  FDNYCore.cfg and On removes it; Profile to `run` logs `profile: run`; Player HUD under
  `run` is refused and reads Off, and On after going back to practice; a click at the right
  end of the text-size slider gives 16; next / prev turn the pages; Back returns to Options;
  and the page opens again from the briefing's Options.
- Game tests the branch touches (37 of 37 in `tests/game`): 36 passed in one run.
  `keys_default` failed one step there (the second P press, `pause.active = 1, want 0`) and
  passed alone on rerun: the log shows 2.8 s between `bind: p` and the camera starting, so
  the 0.1 s key press that followed fell inside a stalled frame. The VM stalls like this on
  some runs (PRs 1, 3 and 4 each met one).

## Found by the test

A practice-only row clicked under `run` crashed inside the game's hint function
(`exception c0000005 at FDNYFirefighter.exe+564e1`, which is `hint.show` + 0x21). The
refusal asks for an on-screen confirmation (D64), and at a menu the player object's pointer
is the last level's. `notify::Show` now draws only in a level with a valid player. This was
latent since PR 2: until this page, nothing could trigger a refusal from a menu.

## Limits

- A slider has 21 positions (the game's widget steps by 0.05 of its range). Whole numbers
  are rounded; Panel opacity (0-255) moves in steps of about 13.
- The option rows don't wrap: Off's left arrow and On's right arrow are hidden, as on the
  game's own rows.
- The status line (page number and the row last changed) is small and sits behind the
  watermark's first characters at 640x480.
- Whether the in-level Escape menu is the briefing screen was read in Ghidra, not clicked
  in a level by this test; the test reaches the page through the briefing.
