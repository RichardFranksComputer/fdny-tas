# Renames: one grammar for names (D68)

PR 6 of 6 unifying the tooling (owner 2026-11-11: "Renames are fine for a clean break").
Stacked on PR 5 (D67). The table of old and new names is D68.

## How

- `fd_<noun>[_<what>]`: the switch is the bare noun; settings and commands add a word.
- 110 replacements in 32 files (source, tests, fixtures, tools, the living docs) by one
  script, word-bounded and ordered: `fd_spawns` became `fd_spawns_pin` before
  `fd_static_spawns` became `fd_spawns`.
- By hand: `fd_pause_toggle` removed (P binds `toggle fd_pause`), five help texts that named
  a key ("(key T)", "(N toggles)", "default key P"), and `profile_run`'s watermark strings
  (`hud_player 1` is `hud 1`; the "absent" checks match `| hud ` so `hud` can't match inside
  another name).
- arch_check R13 holds the grammar: lower case `fd_<noun>[_<what>]`, no `fd_*_toggle`
  command, two nouns per feature at most.

## Results (VM, 2026-11-11)

- `.\build.ps1`: Debug 411.5 KB, Release 345 KB; arch_check (now R1-R13) and `-Werror` clean.
  The reference: 33 variables, 30 commands (one fewer: `fd_pause_toggle`).
- `.\tests\unit\run.ps1`: 409 C++ checks, 0 failed; 52 Python tests OK (R13's three cases).
- `python -I tools/test_check.py`: tests and code in step.
- The whole suite (`--suite`, 37 tests), as for a release:
  - first attempt: stopped after 13 tests. The game took over 6 s to reach the address table
    and the harness's hang check fired; the next launch then couldn't replace the locked
    `.asi` (the same stall as PR 1's first attempt).
  - second: 36 passed; `draw_layers` failed only `no_crash: window hung > 10s`, every
    assertion passing, and passed alone on rerun.

## What a player meets

- An old name in `FDNYCore.cfg`, `autoexec.cfg` or a bind prints `unknown command: <name>`
  and that line does nothing.
- **`fd_static_spawns "1"` from an old config is ignored**: victims spawn at random until
  `fd_spawns 1` is set. The watermark shows `spawns 1` when it is on.
- **An old `fd_spawns <map> <spots>` line is refused** (`fd_spawns takes 0 or 1`): pins are
  entered again with `fd_spawns_pin`.
- Both belong in the release notes (`CHANGES.md`) of the release that ships this.
