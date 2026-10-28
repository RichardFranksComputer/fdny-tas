# Contributing

How to build, test and change FDNYCore. The design rules are in `docs/architecture.md` and
the reasons behind them in `docs/decisions.md`. To add or substantially change a feature,
follow the steps in `.claude/skills/new-feature/SKILL.md` (it is also the agents' procedure).

## Layout

| Path | What |
|---|---|
| `core/` | Core services, the DLL entry point, the module registry and the address table (`core/addresses.json`) |
| `features/<name>/` | One folder per feature module |
| `thirdparty/minhook/` | MinHook, vendored |
| `tests/game/` | Game-driven tests (YAML, run by the harness on the VM), with `data/` and `ondemand/` |
| `tests/unit/` | Unit tests that need no game: C++ and Python |
| `tools/` | `arch_check.py`, `addrgen.py`, `docgen.py`; `harness/` (the game-test runner); `templates/` (a new feature and test) |
| `ghidra/` | The Ghidra labels (names, comments, types; no game bytes), the script that exports them, and how to restore them |
| `docs/` | `architecture.md`, `decisions.md`, `reference.md` (generated), `game-research.md`, `decomp-vm.md`, `evidence/<topic>/` |
| `build/` | Build output and test runs (untracked) |

The repository holds only what the tooling needs: no game binaries, no Ghidra `.rep`, no raw
logs or run output, no files nothing uses.

## Build

```
.\build.ps1                      # Debug and Release
.\build.ps1 -Config Release
.\build.ps1 -Without glitch,tp   # leave feature folders out; -Without * builds the core alone
```

It needs Zig 0.16.0 and Python 3 (standard library only), from `environment\local\tools` on
the VM or from PATH. It runs `arch_check` first, regenerates the address-table header,
compiles with `-Werror`, and writes `build\<Config>\FDNYCore.asi` with that build's player
reference, `build\<Config>\reference.md`. A build also rewrites `docs/reference.md`.

Enable the tracked hooks once per clone: `git config core.hooksPath .githooks`. The
pre-commit hook runs `arch_check` and regenerates `docs/reference.md`.

## Test

- **Unit tests first where they can reach (D47, D60):** logic that doesn't need the running
  game (parsing, math, table logic, the console's commands and config, the tools) is tested in
  `tests/unit`. Pull it into a header or a game-free file so it can be (`juggle_math.h`,
  `recorder_json.h`); a game test then proves only what needs the game, once. The warp HUD's
  analysis has no correctness test since D52 (open, D60).
  After a `core/addresses.json` change, rerun `python -I tools/make_override_fixtures.py`
  (`test_override_fixtures` fails until you do).
  Run `.\tests\unit\run.ps1`: seconds, and CI runs it on every pull request.
- **Game tests** (`tests/game/*.yaml`) prove the behaviour in the game; don't use them to
  test arithmetic. They run on the VM:
  `python -I tools/harness/run_test.py tests/game/<name>.yaml` (several paths at once, or
  `--suite` for all of them, `--ondemand` for `tests/game/ondemand/`). Output goes to
  `build/runs/`.
  - Run the tests a change touches. The full suite is for a release.
  - The game runs at the display's refresh rate (about 57 fps on the VM console, 34 on a
    32 Hz remote session), so a test never asserts an absolute frame rate: compare against a
    baseline in the same run (D46, `assert_sample_ratio`).
  - Game tests share sessions (D48): a test that only needs Training and runtime settings
    joins a running game (reset to defaults, its cfg exec'd, Training reloaded). Load a map
    with `loadmap: <map>`, not a console line and a fixed wait. A test that needs a fresh
    process says `own_session: true`.
  - Tests run the Debug build (it has the test pipe) unless they say `core: Release`.
    `core_release` proves the shipped build starts and plays. Before a release,
    `tests/game/ondemand/baseline_route` (`expect: baseline_equivalent`, ~8 min) proves the
    game behaves the same with and without it.
  - A test that compares the core's view of the world with the harness's own reads pauses
    the simulation while it compares (Debug pipe `pause on`), so nothing moves between the
    two reads (`state_snapshot`, which used to race on a spreading fire or a roaming victim).
  - **Tests need a display.** If every test fails "Failed to initialize main window", the VM
    session is disconnected (`query user` shows `Disc`; this happens when the owner closes
    the Hyper-V / remote viewer). Fix: `tscon 1 /dest:console`, which keeps the desktop live
    with no viewer attached. Focus on that unattended console goes through the harness's
    title-bar-click fallback (`tools/harness/fdh/inputs.py`).
- **Never weaken or delete a test to make it pass** (D21). A flaky or environmental failure
  is rerun and reported as such, with evidence (for example the same failure on the previous
  version).
- **Spikes and probes never modify working code** (D22): standalone probes and probe tests
  (`tests/game/probes/`, outside the suite). Delete them once the result is in
  `docs/evidence/`.

## Done means

1. `.\build.ps1` builds both configs (arch_check + `-Werror` clean).
2. **Tests:** a new or changed behaviour gets a test (named in the module's `// Tests:`
   line), and the affected tests pass.
3. **Evidence** in `docs/evidence/<topic>/`: a README with the numbers (what ran, results,
   verdict). Add logs or screenshots only when the README can't carry the point.
4. **Docs:** anything player-visible is described in the code: help texts, the `// User:`
   line, `binds::DescribeKey` (the reference is generated from them, D41; check
   `docs/reference.md` reads well). A non-obvious choice gets a `docs/decisions.md` entry
   (what, why, rejected). The commit message says what changed and how it was tested; there
   is no separate progress log.
5. **Where findings go:** choices in `docs/decisions.md`; findings, measurements,
   assumptions and workarounds in the topic's `docs/evidence/<topic>/README.md` (the early
   ones are in `docs/evidence/foundation/`, the warp HUD's game research in
   `docs/evidence/warp_hud/`); game knowledge beyond the core in `docs/game-research.md`. A
   changed test tolerance is explained in the commit message.

## Branches, commits, releases

- One branch per feature or fix, cut from `main` and merged through a pull request. Never
  commit straight to `main`; the owner merges.
- One topic per commit.
- **Versions:** the PR that releases bumps `VERSION` (minor for features/architecture, patch
  for fixes) and writes its `## <VERSION>` section in `CHANGES.md` (hand-written; the
  release workflow publishes it). That is a feature PR when it ships on its own, or one
  release PR for several merged features; a PR that doesn't release leaves both alone.
- Merging a `VERSION` change to `main` runs `.github/workflows/release.yml`: it builds
  Release from source on a GitHub Windows runner and publishes `v<VERSION>`:
  `FDNYCore-<VERSION>.asi`, a zip with it and its `reference.md`, and the SHA-256. An
  existing tag is never overwritten. The GitHub release is the only download (D45): no
  `.asi` is committed.
- CI on pull requests and pushes to `main`: `unit` (`tests/unit`) and `sheet-check`
  (`docs/reference.md` matches the source).
- Never commit a file over 100 MB, the game's binaries, or the Ghidra project's `.rep` (it
  contains the game's bytes).
