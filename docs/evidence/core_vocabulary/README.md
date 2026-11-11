# Core vocabulary: typed variables, practice query, number helpers, free-camera request (D63)

The first of six PRs unifying the tooling (owner 2026-11-11). No visible change is intended
except that a wrong value for a variable is refused instead of silently clamped or replaced.

## What was measured before (source at 0.23.0, main `43df882`)

| Duplicate | Copies |
|---|---|
| `bool On(const cmd::Cvar&)` | 4 (triggers, recorder, juggle, replay) |
| float parsers | 3 identical (fov, juggle, checkpoints' `Number`) plus freecam's `CvarF` and the replay's `Speed` |
| "wsprintf has no %f" formatters | 5 (hud, tp, juggle, fov, state) |
| `Sqrtf` | 3 identical (gfx, replay, replay_data) |
| `FindCvar("fd_profile")` string compares | 3 (checkpoints, replay x2) |
| a feature setting another feature's variable | 1 (checkpoints: `"fd_pause"`, 3 calls) |

Practice-only names: a Debug build registers 29 (`FDNYCore_registry.txt` from `docs_registry`);
the table held 32 and dropped the next one silently. It now holds 96 and logs an ERROR.

## Results (2026-11-11, VM, Debug unless a test says Release)

- `.\build.ps1`: Debug 380.5 KB and Release 319.5 KB, arch_check clean (42 core, 29 feature
  files, 12 modules), `-Werror` clean.
- `.\tests\unit\run.ps1`: 351 C++ checks, 0 failed; 44 Python tests OK. New: typed variables
  (wrong kind, out of range, the refusal texts, `SetCvarUngated` can't bypass), `num`
  (`Fixed`, `Short`, `Parse`), R11 (a violation, own and core names, the allowed exception).
- `python -I tools/test_check.py`: tests and code in step.
- Game tests the change touches (`test_check.py affected`: 31 of 34): 30 passed in one run.
  `profile_run` failed one step there and passed alone on rerun (26 of 26 steps):
  `watermark contains 'fov 95'` read the watermark pipe right after the console line, before
  the frame end that rebuilds the watermark text. The value had been set: the restart later
  in the same failing run shows `RUN | fov 95` from the saved config. A race in the read, not
  in the change (D21); the test is unchanged.
- The first attempt at the run failed before any test ran: the game took 9 s to reach
  "FDNYCore ready" (2 s on 2026-10-29) and the harness's hang check fired at 7 s. Rerun
  without changes: `addr_verified` passed.

## Found on the way

- `num::Parse` first summed tenths in floats; `"0.075"` then wasn't 0.075f, and the unit
  test caught it. It builds the digits in doubles now.
- A refusal text with the word "from" (`a number from 1 to 5`) made `test_check` accept any
  text containing " from " as printable (a format's fields match anything), which its own
  unit test caught. The texts read `takes a number, 1 to 5`.
- `-Wmissing-field-initializers` is on: the new `type`, `min`, `max` fields have default
  member values, so an untyped variable's initializer is unchanged.

## Verdict

Done for PR 1. Not done here, on purpose: the reference doesn't show a variable's type yet
(PR 5 uses the type for the settings page), and no names change (PR 6).
