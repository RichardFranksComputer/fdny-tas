---
name: new-feature
description: Add or substantially change an FDNYCore feature (a mod feature, overlay, HUD, console command, game patch, speedrun/practice tool) the established way -- feature module in features/, core services only, address table, textdraw/gfx, tests, evidence, docs, version, commit, owner report. Use for any new FDNYCore capability, not for one-line fixes.
---

# Adding an FDNYCore feature

Follow these steps in order. The rules are in `docs/architecture.md` (arch_check enforces
R1-R5 and R8-R10, the compiler R6-R7), how to build, test and release in `CONTRIBUTING.md`
(both loaded through `CLAUDE.md`), and the reasons in `docs/decisions.md`. Don't re-derive
them, and don't work around them; if a rule blocks a good design, stop and ask the owner
(a rule changes only with a `docs/decisions.md` entry).

## 1. Orient (before writing code)

- Read `docs/decisions.md` and the recent `git log`.
- **Check for something existing first** (owner rule). Look in `features/`, the services in
  `core/` and the console commands (`docs/reference.md`). Extend what's there instead of
  adding a parallel path.
- Decide the shape:
  - **Feature or service?** A feature uses services. If it needs a hook another piece
    already owns, or something two features would share, it's a service change first
    (`core/`, with subscriptions or filters, like `camera`).
  - **Practice-only?** It is if it shows information the game doesn't, or moves the
    player. Then `cmd::SetPracticeOnly`.
  - **Drawing?** Pick the layer first (`docs/architecture.md`, "Drawing"; D40).
    - `LAYER_WORLD` for 3D things walls should hide.
    - `LAYER_OVERLAY` for x-ray views.
    - Text panels are `LAYER_HUD` automatically: a `textdraw` panel, `STYLE_TRANSLUCENT` by
      default, `STYLE_OPAQUE` if it must show in menus.
    - Use `gfx` blocks only for non-text, and never `hooks::OnFrameEndLate` (R8).
- Owner decisions that need asking (gameplay semantics, what's speedrun-legal): ask
  once, up front, with a recommendation.

## 2. Game research and addresses

- Ghidra lookups go through subagents (read-only); the primary session never calls
  GhidrAssist MCP. Prior knowledge: `docs/game-research.md`, `ghidra/`, memory, and on the
  VM the local-only `research/research.db`.
- Every address, offset or field gets a `core/addresses.json` entry with `sig` (first 16
  bytes, for code), `ghidra` (the evidence) and `note` (who uses it). Code uses
  `addr::Id::<entry>`; `build.ps1` regenerates the header.
- Never patch a site whose signature doesn't match (`addr::Usable`); the core doesn't look
  for other mods (D55).

## 3. Code

- Copy `tools/templates/feature.cpp` to `features/<name>/<name>.cpp` (more files for the
  feature go in the same folder):
  - fill the `// Tests:` line (paths from the repository root), the
    `// User: <Title> -- <one sentence>` line (the reference's entry, R9) and the header
    paragraph;
  - register variables, commands, practice-only names and default keys with string literals
    (the reference reads them, D41); a key the feature reads itself (`GetAsyncKeyState`, not
    a bind) gets a `binds::DescribeKey(key, action, when)`;
  - one `FDCORE_MODULE`, listing the required Ids;
  - pick a start `order` after the existing modules (the `FDCORE_MODULE` lines in
    `features/`);
  - startup patches before WinMain go in the `early` step, guarded with
    `addr::EarlyCodeAddress`.
- Globals constant-initialisable only (`= {}`, `constexpr`; R7). No STL, CRT or
  `<initializer_list>`.
- Log a `<name>: ready (...)` line, and a reason on every refusal or disable (R10 checks
  `Init()`). Core services never name the feature, not even in a string (R1).
- `.\build.ps1` must pass: arch_check and `-Werror`.

## 4. Test

- Logic that doesn't need the running game (parsing, math, decisions on data) gets a unit
  test in `tests/unit` (D47): add the source to `run.ps1`'s list, a `Test...` function to
  `main.cpp`, or a `test_*.py`. Run `.\tests\unit\run.ps1`.
- Copy `tools/templates/test.yaml` to `tests/game/<name>.yaml` (in the suite). Prove the
  behaviour with assertions (log lines, memory, pipe), not just "no crash". Add
  screenshots for anything visible.
- `python -I tools/test_check.py` (D61): every new variable and command is used by a test,
  or gets an `UNTESTED` line with the reason; what the tests name exists in the code.
- Run it and the other tests the change touches (`python -I tools/test_check.py affected`);
  the full suite only before a release (`CONTRIBUTING.md`, "Test").
- Never weaken a test (D21). For a failure you believe is environmental, rerun it, show it
  fails identically on the previous version or passes on rerun, and report it as such.

## 5. Finish

- Evidence: `docs/evidence/<name>/README.md` with the numbers (`CONTRIBUTING.md`, "Done
  means").
- The player docs are the generated reference (`docs/reference.md`, rewritten by every build
  and the pre-commit hook): check your feature's lines read well, and fix the help texts
  and `// User:` line in the code, never the reference. A new service goes in
  `docs/architecture.md`'s "Core services" table.
- `docs/decisions.md`: an entry for every non-obvious choice (what, why, rejected).
- Version only if this PR releases (`CONTRIBUTING.md`, "Branches, commits, releases"; ask
  the owner when unsure).
- Commit: one topic; the message says what changed and how it was tested (format in
  `CLAUDE.md`).

## 6. Report to the owner

In the format `CLAUDE.md` gives. Offer the obvious next step in one line; don't start it
unasked.
