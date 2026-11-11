# Key modes: a feature's keys as binds actions (D65)

PR 3 of 6 unifying the tooling (owner 2026-11-11). Stacked on PR 2 (D64).

## Before and after

| | Before (0.23.0) | After |
|---|---|---|
| Press detectors | 5 (binds, the warp HUD, the replay picker, the checkpoint editor, the spectator) | 1 (binds, once per frame) |
| Keys the player can rebind | 10 default binds | the 10, plus 49 actions in 5 modes |
| Keys that can't be rebound | 22 described groups | 4 (the mouse look, the window module's O, the recorder's observed keys, the Recordings menu page's) |
| Direct `GetAsyncKeyState` calls in features | 5 files | 2 lines, each with its reason (R9) |
| On-screen key hints | typed by hand (the editor: "P: exit") | written from the keys bound now (`binds::HintLines`) |

Modes and actions, from `docs_registry` (the build's registry against docgen's reading of the
source): `mode: 5 in the build, 5 read from the source`, `action: 49 in the build, 49 read
from the source`. They are `camera` 8, `viewer` 11, `editor` 18, `warp` 3, `picker` 9.

## Results (2026-11-11, VM)

- `.\build.ps1`: Debug 393 KB, Release 330.5 KB; arch_check and `-Werror` clean.
- `.\tests\unit\run.ps1`: 408 C++ checks, 0 failed; 49 Python tests OK. New: `test_binds.cpp`
  (53 checks: press and hold, a key down when the mode starts, the claim on an earlier mode,
  Ctrl chords, Solo, repeat after 300 ms, rebinding and what the config holds, `bind <mode>
  default`, an exclusive mode and its key messages, `SuppressHeld`, `HintLines`' bound), and
  R9's two new cases.
- Game tests the branch touches (36 of 36 in `tests/game`): 35 passed in one run;
  `text_panels` failed one step there and passes with the fix below. The tests that press the
  mode keys: `checkpoints` (the editor: place, select and drag, move with repeat, Ctrl chords,
  undo / redo, save), `spectator_camera` (the viewer: X, Space, PgUp / PgDn, F, Tab, seek,
  Escape), `replay` (the picker's digits), `keys_default` (B), `keys_modes` (new: rebinding).

## Found by the tests

1. **A crash on releasing the viewer's camera** (`spectator_camera`, first run:
   `crashlog: exception c0000005 at FDNYCore.asi+39ce1` right after `spectator: camera
   released`). `HintLines` returned `maxLines + 1` when the actions didn't fit its lines; the
   viewer's panel then wrote a row past its array. It never returns more than it was given,
   and `test_binds.cpp` checks that with a mode that doesn't fit.
2. **The game's window hung on `bindlist`** (`docs_registry`: `window hung > 10s`). It printed
   every action of every mode, about 60 console lines, at up to a second each on the VM that
   day. `bindlist` names the modes; `bind <mode>` lists one.
3. **A rebound key outlived its test** (`text_panels`, after `keys_modes` in the same session:
   B no longer hid the warp HUD's boxes). The session reset knew ordinary binds only.
   `bind <mode> default` restores a mode's keys, the reset runs it for every mode, and
   `keys_modes` now ends on it. The four tests pass together in one session.
4. **R1 caught the camera mode's text** naming `fd_pause` in a core string; it says "a paused
   game".

## Not changed, on purpose

- The viewer's key handling is the spectator's, moved: the same `input::Key` and the same
  suppress-until-release rule, now in binds. `test_spectator.cpp`'s key cases run unchanged
  through an alias.
- The Recordings menu page still reads its own keys from the window messages (D58: menus
  aren't levels, and modes act in a level).
- Panel layout and colours: PR 4.
