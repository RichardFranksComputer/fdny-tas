# Test review: overlaps removed, logic moved to unit tests (D60)

Branch `refactor-testing`, 2026-10-28. What changed and why is in `docs/decisions.md` (D60);
this file has what ran and the numbers.

## Suite: 39 → 35 game tests

| Before | After |
|---|---|
| `hooks_frame` + `hooks_toggle` | `hooks_frame` |
| `replay` + `replay_pick` | `replay` (mbase loaded once) |
| `text_panels` + `glitch_box_cost` | `text_panels` (+ `draw_us_avg <= 1 ms`) |
| `skip_intro` | removed: `core_release` checks the same patch line with the same cfg |
| level change while paused in `freecam` and `pause_service` | `freecam` only |
| practice-only refusals in `tp_teleport`, `checkpoints` (x2), `fire_juggle`, `freecam` | removed (`profile_run`, `keys_default`, `docs_registry`, `test_cmd`) |
| `fire_juggle`: 8 view yaws | 2 (0 and 135); all 8 and the sector edges in `test_juggle` |

## Stale fixture found

`tests/game/data/addr_override_*.txt` were last generated at c4ec7ba (2026-10-27): 93 lines
against the table's 190 today. The override loader leaves out every entry a fixture doesn't
list (`core/addr.cpp`, "unknown name: ignore"), so `addr_bad_signature` ran with about half
the table unusable, not with one bad signature. It passed because it checked only the
levelwatch line. Regenerated; `tests/unit/test_override_fixtures.py` now fails on a stale
fixture.

## Unit tests

`.\tests\unit\run.ps1`: C++ 20 tests, 328 checks, 0 failed; Python 37 tests OK.

| New | Checks |
|---|---|
| `test_juggle.cpp` | the 8 directions of `fire_juggle`'s setup, near, sector edges |
| `test_recorder.cpp` | `Json::Number` bit-exact for 12 edge values and 2000 spread values; NaN refused; escaping; oversize refused; a file of recorder lines parsed by `replay_data` with every position bit-exact, and its tail read as complete |
| `test_cmd.cpp` | splitting and quotes, case, not-ours lines go back to the game, unknown `fd_` swallowed, old names, `toggle`, read-only, practice-only gates (also for `toggle`), FDNYCore.cfg contents (non-default archived only, module writers, default binds omitted, changed / reverted / removed defaults, `unbind`), `exec` (comments, game lines queued), startup doesn't rewrite FDNYCore.cfg while loading it |
| `test_compare_runs.py` | the release gate's 7 checks each pass on equivalent runs and fail on their own fault; Genesis relocation and baseline-noisy chains allowed |
| `test_override_fixtures.py` | fixtures equal a fresh `make_override_fixtures` run |

## Game tests (Debug unless noted, VM console, shared sessions)

Run 1 (`build/refactor_testing_run1.txt`): `addr_bad_signature`, `addr_unknown_exe`,
`hooks_frame`, `checkpoints`, `text_panels`, `glitch_records`, `replay`, `freecam`,
`tp_teleport`, `fire_juggle`, `speedrun_static_spawns`, `recorder`, `window_alttab`,
`core_release` (Release) PASS; `pause_service` and `spectator_camera` FAIL:

- `pause_service`: "window hung > 10s" while joining the shared session after `freecam`,
  before any of its own steps (the log ends in the session's settings reset). Rerun with
  `freecam` before it again (run 2): PASS. Recorded as environmental; it failed 3 of 15 runs
  before this branch.
- `spectator_camera`: the new watched-level check asserted `triggers.boxes == 0`, but that
  field is gfx's count of every world box submitted, the viewer's own included (5 while
  playing). The check now compares the count before and after `fd_triggers 1` with the
  viewer's timeline paused: +0 (run 3, PASS). While playing, the viewer's own count moves
  (+3 in run 2), so the comparison needs the pause.

New steps' numbers (run 1 unless noted):

| Test | Step | Measured |
|---|---|---|
| `text_panels` | warp box build | `draw_us_avg` 43 us (limit 1000) |
| `text_panels` | panels on bank after the level change | `textdraw glitch` / `hud_player` drawn=1 |
| `glitch_records` | Z: frozen, then walking 1 s | `shown_tick` +0; after Z again +135 |
| `glitch_records` | X | "history cleared" logged |
| `speedrun_static_spawns` | `fd_spawns training 1 x` | "pinned to spot 1", "(random spot kept, pin -1)"; cfg line written, removed by `default` |
| `recorder` | `fd_record_victims 0` / `1` | victims 0 / 2 |
| `window_alttab` | O held, cursor set to (100,100) | stays (100,100); released: back at the centre (330,261) |
| `pause_service` (run 2) | camera override on screen | paused vs lifted 23.81, paused vs override off 0.00 |
| `spectator_camera` (run 3) | `fd_pause 1` in the viewer | refused ("not while watching a recording"), not flying |
| `fire_juggle` | yaw 0 / 135 | dir 2 (right) / 5 (down-left), dist 600 |

Not run (unchanged by this branch beyond comments): `keys_default`, `docs_registry` and the
rest of the suite; `baseline_route` (release only).
