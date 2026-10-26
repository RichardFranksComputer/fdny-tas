# core/ — FDNYCore working rules

FDNYCore.asi is one in-process plugin for FDNY Firefighter: American Hero:
**core services** (`src/`) plus **self-registering feature modules**
(`src/features/`). These rules are how every piece so far was built. The
machine-checkable ones are enforced by `tools/arch_check.py` (run by `build.ps1`) and
compiler flags; the reasons are in `DECISIONS.md`. Change a rule only with an
owner-approved entry there.

Read before working: the repository `README.md` (the key sheet: every feature, key, variable and command; generated
from the source, D41), the core services below, `DECISIONS.md` (why), the tail of
`PROGRESS.md` (what happened last). To add a feature, use the `new-feature` skill
(`.claude/skills/new-feature/`).

## Core services

The core (`src/`) is services only; every feature is a self-registering module in
`src/features/` (`modules.h`). The core never names a feature, so a feature can be left
out of the build (`.\build.ps1 -Without glitch`, or `-Without *` for the core alone).
The log lists `modules: N registered (...)` and one `module <name>: enabled | disabled
(reason)` line each.

| Core service | What a module gets |
|---|---|
| `addr` | address table: verified addresses by Id (never raw addresses) |
| `hooks` | `Create` (inline hook in a group), `OnFrameBegin` / `OnFrameEnd` / `OnLevelLoad` (`OnFrameEndLate` is gfx's only, R8) |
| `cmd`, `console`, `binds` | variables, commands, config persistence (`RegisterConfigWriter`), `SetPracticeOnly` (refused in `fd_profile run`), console output, `console::GamePath` (a folder setting, absolute or under the game folder: the recorder and the replay share it); `binds::BindDefault(key, line)` for a feature's default key (the player's binds win), `binds::DescribeKey` for keys a feature reads itself; modal tools reserve keys/mouse look with `SetKeyCapture` / `SetPointerCapture` |
| `camera` | view size; one FOV filter (`fov` uses it); a world-transform override (`SetWorldOverride`, `LastWorldXForm`: the free camera; `ViewXForm` includes the active override) |
| `pause` | `Set(on, why)` / `Active()`: skip the simulation step and pause the game clock; ends itself on leaving the level. `textdraw::HideWhilePaused` for view-relative panels |
| `state` | the frame's player snapshot |
| `entities` | lazy Player/Victim initialization, update and destruction observers; original calls, last-error and FP state preserved. `ObserveNativeContacts` + `AddNativeTrigger`: runtime trigger construction and contact notifications (world owns roots, service owns definitions until level entry). `FilterVictimSpot`: one filter that may move a victim's spawn spot before the floor snap (early step only; speedrun's static spawns) |
| `gfx`, `textdraw` | in-frame drawing ("Drawing" below; `src/gfx.h`, `src/textdraw.h`); `gfx::ProjectWorld` / `ScreenRay` for pointer tools; `textdraw::GameHint` reuses the original native font/widget with a timed priority window, no custom surface |
| `profile` | run/practice gates + watermark (reads `SetPracticeOnly`) |
| `testpipe` (Debug) | `Register("<name>", handler)` for harness requests |
| `log`, `crashlog`, `patches`, `levelwatch` | logging, crash reports, the core's startup patches, level events |

## Architecture (enforced)

| Rule | What | Enforced by |
|---|---|---|
| R1 | Core files (`src/*`) never include a feature header. The core never names a feature. | arch_check |
| R2 | A feature includes core headers and its own group's headers only (`glitch_*` = group `glitch`). Features talk to each other only through core services. | arch_check |
| R3 | Each feature group has exactly one `FDCORE_MODULE(...)`, in a file whose `// Tests:` line names existing tests. | arch_check |
| R4 | No game addresses or module-base+RVA in code: every address/offset is an `addresses.json` entry, used by `addr::Id`. Exceptions need `// arch-allow: R4 <why>` on the line. | arch_check |
| R5 | Only `gfx` calls the Genesis drawing/bitmap API. Features draw with `textdraw` panels (or `gfx` blocks). | arch_check |
| R6 | Harness channels are Debug-only (`#ifdef FDCORE_DEBUG` around `testpipe::Register`). | Release build fails otherwise |
| R7 | No dynamically initialised globals or exit-time destructors: there is no CRT, they never run. Globals must be constant-initialisable (`= {}`, `constexpr`). | `-Wglobal-constructors -Wexit-time-destructors -Werror` |
| R8 | The draw order is gfx's: only gfx subscribes to the late frame step (`hooks::OnFrameEndLate`); everything else draws from a gfx layer (D40). | arch_check |
| R9 | The key sheet is built from the source (D41). Each feature module has a `// User: <Title> -- <one sentence>` line. A feature that reads keys itself (not a bind) declares them with `binds::DescribeKey`. Registrations take string literals. | arch_check, `tools/docgen.py` (in `build.ps1` and the pre-commit hook, which also stages `KEY_SHEET.md`), `tests/docs_registry.yaml` |

Not machine-checked, still rules:
- **Service ownership.** A Genesis/exe function is hooked by exactly one owner. Shared hooks belong to a core service that offers subscriptions/filters (frame events → `hooks`, camera → `camera`, rendered world → `gfx`). A feature that needs a hooked function asks the owning service; if none exists, add the service to the core first.
- **Practice-only** variables/commands are declared by the feature (`cmd::SetPracticeOnly`). Anything that shows information the game doesn't, or moves the player, is practice-only. Speedrun-category options (static spawns, RescueBot) are not; they show on the watermark.
- **Drawing order:** state updates in `hooks::OnFrameEnd`; drawing happens in a gfx layer (the table below, R8).
- **Freestanding C++17:** no CRT, no STL, no exceptions/RTTI, no `<initializer_list>`; no 64-bit integer division (use doubles or `MulDiv`); large locals are `static` (no stack probes). Windows API via kernel32/user32/gdi32/bcrypt only.

## Drawing: pick the block and the layer

Every drawing belongs to a gfx layer, run in this fixed order each frame (D40, `gfx.h`):

| Layer | When | Seen | For |
|---|---|---|---|
| `LAYER_WORLD` | right after the level geometry | hidden by walls and floors; the game's sprites, actors and HUD go over it | 3D things in the level: trigger boxes, markers, paths |
| `LAYER_OVERLAY` | after everything the game draws | over walls | x-ray views |
| `LAYER_HUD` | last | over everything | screen panels (textdraw does this) |

- Anything depth-tested must be drawn in `LAYER_WORLD`. Later in the frame, the game's sprites and particles have written depth over whole screen rectangles, which cut holes in it (D39, `evidence/trigger_depth/`).
- `tests/draw_layers.yaml` checks it. Changing the order is a DECISIONS entry plus that test.

| Need | Use |
|---|---|
| A text panel | `textdraw::Create(...)` (HUD layer): `STYLE_TRANSLUCENT` (default; opaque fallback without a world) or `STYLE_OPAQUE` (always visible, e.g. the watermark). Own colours: `textdraw::SetPalette`. |
| Boxes in the level | `gfx::SubmitWorldBoxes(boxes, n, LAYER_WORLD or LAYER_OVERLAY)` from `OnFrameEnd`, every frame |
| Lines in the level | `gfx::SubmitWorldLines(lines, n, layer)`: polylines as mitered strips, colour per point, width in px or world units, optional glow and thin points (the replay; D50) |
| Your own drawing in a layer | `gfx::OnLayer(layer, fn)` (a new kind of drawing is a gfx block first, R5) |
| Opaque image / rect | `gfx::Decal`, `gfx::SolidRect` |
| Per-pixel alpha | `gfx::AlphaSurface` (256 px 4444 tiles, world-attached; colours are 4 bits per channel) |
| Screen size | `gfx::ViewSize` (the camera rect, never the window) |

## Game, VM and safety

- Never edit the exe or Genesis.dll on disk; in-memory patches only, and only through `addr` / `hooks`. Back up any game-folder file before replacing it.
- **Never rebuild the VM / run packer** (root `CLAUDE.md`).
- **Tests need a display.** If every test fails "Failed to initialize main window", the session is disconnected (`query user` shows `Disc`; this happens when the owner closes the Hyper-V / remote viewer). Fix: `tscon 1 /dest:console`, which keeps the desktop live with no viewer attached. Focus on that unattended console goes through the harness's title-bar-click fallback (`fdh/inputs.py`).
- Ghidra lookups go through subagents; the primary session never calls GhidrAssist MCP itself.
- Python and git are not on PATH: prepend `C:\fdny-decomp\environment\local\tools\python` and `...\tools\git\cmd`.
- Edit UTF-8 docs with the Edit/Write tools, not a PowerShell `Get-Content`/`Set-Content` round trip (mojibake). Some `.cpp` and `build.ps1` are CRLF.

## Done means

1. `.\build.ps1` builds both configs (arch_check + `-Werror` clean).
2. **Tests:** a new or changed behaviour gets a test in `tests/` (named in the module's `// Tests:` line). Never weaken or delete a test to make it pass; a flaky or environmental failure is rerun and reported as such, with evidence (e.g. the same failure on the previous version). Run the affected tests, and the full suite (`python -I harness/run_test.py --suite`) before a version bump.
   - **Unit tests first where they can reach (D47):** logic that doesn't need the running game (parsing, math, table logic, the warp HUD analysis, the tools) is tested in `core/unit` (`.\unit\run.ps1`, seconds, also in CI on every push). Game-driven tests in `tests/` prove the behaviour in the game; don't use them to test arithmetic.
   - The game runs at the display's refresh rate (about 57 fps on the VM console, 34 on a 32 Hz remote session), so a test never asserts an absolute frame rate: compare against a baseline in the same run (D46, `assert_sample_ratio`).
   - Game tests share sessions (D48): a test that only needs Training and runtime settings joins a running game (reset to defaults, its cfg exec'd, Training reloaded). Load a map with `loadmap: <map>`, not a console line and a fixed wait. A test that needs a fresh process says `own_session: true`.
   - Tests run the Debug build (it has the test pipe) unless they say `core: Release`. `core_release` proves the shipped build starts and plays. Before a release, `tests/ondemand/baseline_route` (`expect: baseline_equivalent`, ~8 min) proves the game behaves the same with and without it.
   - A test that compares the core's view of the world with the harness's own reads pauses the simulation while it compares (Debug pipe `pause on`), so nothing moves between the two reads (`state_snapshot`, which used to race on a spreading fire or a roaming victim).
3. **Evidence** in `evidence/<topic>/`: a README with the numbers (what ran, results, verdict). Add logs or screenshots only when the README can't carry the point; test runs keep theirs in `runs/` (untracked).
4. **Docs:** anything user-visible is described in the code: help texts, the `// User:` line, `binds::DescribeKey` (the key sheet is generated from them, D41; check the root `README.md` reads well); `DECISIONS.md` for any non-obvious choice; a `PROGRESS.md` entry (`- <date> it<N>: **<version>** ...`).
5. **Version:** bump `VERSION` (minor for features/architecture, patch for fixes).
   - Add the release notes to the root `CHANGES.md` (hand-written), under `## <VERSION>`: the release workflow puts that section in the release's notes.
   - Merging the `core\VERSION` change to `main` runs `.github/workflows/release.yml`. It builds Release from source on a GitHub Windows runner (same zig, `build.ps1`) and publishes the GitHub release `v<VERSION>`: `FDNYCore-<VERSION>.asi`, a zip with it and its key sheet (D41: version, SHA-256, keys, features, variables, commands), and the SHA-256. An existing tag is never overwritten.
   - The GitHub release is the only download (D45): no `.asi` is committed to the repo.
6. **Commit:** one topic per commit; message via `-F` from a UTF-8 file without BOM, ending with the Co-Authored-By / Claude-Session lines; don't stage `research/research.db-*` side files.
7. **Report to the owner** (end of every task):
   - the outcome and version/commit, first sentence;
   - what changed, in owner terms (commands, behaviour, look);
   - verification: tests with numbers; failures stated plainly with cause and evidence;
   - evidence path;
   - open items and anything needing an owner ruling.

## Owner decisions in force

- Old tools stay installed (FOV fix, alttabfix, FDNYDeterministic, Python overlays): no further development, don't delete. The core defers to them where they own a patch site.
- The watermark stays opaque (decal path).
- The practice-only list is the agent's default; the owner still has to confirm it.
- No extra log files for overlay output: it goes to `FDNYCore.log` (`glitch|` lines).
- Spikes and probes never modify working code: standalone probes in `probes/`, tests in `tests/probes/` (outside the suite).
