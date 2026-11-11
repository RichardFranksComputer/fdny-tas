# On-screen confirmations and one console voice (D64)

PR 2 of 6 unifying the tooling (owner 2026-11-11). Stacked on PR 1 (D63).

## What the player sees

A key that used to answer only in the console now also shows one line in the game's own
hint text for 2 s (`position_saved.png`: K in Training, 640x480):

| Key / action | On screen |
|---|---|
| H, G, J, N, Y (`toggle` of a labelled variable) | `Player HUD on` / `off`, `Warp HUD ...`, `Juggle arrow ...`, `Checkpoint times ...`, `All triggers ...` |
| K / L | `Position saved` / `Teleported` (`No saved position`, `Saved position is on another map`) |
| T | `Trigger boxes on` / `on, through walls` / `off` |
| V | `Replay playing` / `paused` (`No recordings of this map`) |
| B / Z / X while the warp HUD is on | `Warp HUD hidden` / `shown`, `frozen` / `live`, `history cleared` |
| a practice-only key under `fd_profile run` | `Practice only: the profile is run` |

Nothing is drawn while the game is paused (the hint's timer is the game clock) or outside a
level; the text is still logged as `notify> ...`.

## Results (2026-11-11, VM)

- `.\build.ps1`: Debug 383.5 KB, Release 321.5 KB; arch_check clean (44 core files), `-Werror` clean.
- `.\tests\unit\run.ps1`: 355 C++ checks, 0 failed; 47 Python tests OK (new: `toggle` with
  and without a label; R12: a command-name prefix, a bare sentence, the allowed forms).
- `tests/game/notify.yaml` (new): 13 assertions pass. The pipe shows `count` and `shown`:
  8 of 8 shown in play, and over a pause `count` +1 with `shown` +0.
- Game tests the branch touches (32 of 35): 27 passed in the first run. The 5 that failed
  (`checkpoints`, `spectator_camera`, `keys_default`, `tp_teleport`, `triggers`) asserted a
  console line by its old wording; each assertion now quotes the new line, and all 5 pass.
  `checkpoints`' two `game_hints` failures in that run followed from its 5 s wait for the old
  line: the checkpoint's 4 s priority window had ended by the time the test pressed on. With
  the wait matching, `suppressed` and `passed` are as before.

## Found on the way

- `test_check` did not flag those five assertions before the run: its match is loose enough
  (a format's fields match anything) that another source string could "print" each old line.
  It did flag `trigger boxes: on, x-ray`. A reworded message can still slip past it; the
  game tests caught these.
- The hint font draws `(` and `)` as square brackets, so on-screen texts avoid them.
- The screenshot shows a few stray pixels above the hint text. The checkpoint time drawn by
  the same widget on main is too faint to compare (`timestamp_native_priority.png` in the
  checkpoints run); not investigated.

## Verdict

Done for PR 2. Variables without a default key have labels only where a key or the profile
could toggle them; the rest get theirs with the settings page (PR 5).
