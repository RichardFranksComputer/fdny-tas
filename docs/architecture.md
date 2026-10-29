# FDNYCore architecture

FDNYCore.asi is one in-process plugin for FDNY Firefighter: American Hero: **core services**
(`core/`) plus **self-registering feature modules** (`features/<name>/`). These rules are how
every piece so far was built. The machine-checkable ones are enforced by `tools/arch_check.py`
(run by `build.ps1` and the pre-commit hook) and compiler flags; the reasons are in
`docs/decisions.md`. Change a rule only with an owner-approved entry there.

## Core services

The core (`core/`) is services only; every feature is a self-registering module in its own
folder under `features/` (`core/modules.h`). Dependencies point one way: features use core
services, the core uses MinHook (`thirdparty/`), the Windows API and the address table, and
never names a feature. So a feature can be left out of the build (`.\build.ps1 -Without
glitch`, or `-Without *` for the core alone). The log lists `modules: N registered (...)`
and one `module <name>: enabled | disabled (reason)` line each.

| Core service | What a module gets |
|---|---|
| `addr` | address table (`core/addresses.json`): verified addresses by Id (never raw addresses) |
| `hooks` | `Create` (inline hook in a group), `OnFrameBegin` / `OnFrameEnd` / `OnLevelLoad` (`OnFrameEndLate` is gfx's only, R8); `OnFrameComplete`: after `geEngine_EndFrame` has closed the frame, for scene and menu transitions (never draws) |
| `cmd`, `console`, `binds` | variables, commands, config persistence (`RegisterConfigWriter`), `SetPracticeOnly` (refused in `fd_profile run`), console output, `console::GamePath` (a folder setting, absolute or under the game folder: the recorder and the replay share it); `binds::BindDefault(key, line)` for a feature's default key (the player's binds win), `binds::DescribeKey` for keys a feature reads itself; modal tools reserve keys/mouse look with `SetKeyCapture` / `SetPointerCapture` (`AddKeyCapture` / `AddPointerCapture` for a second, independent tool). Binds fire only inside a level (D58). `binds::OnWindowMessage(filter)`: the game window's messages (keys, typing, the wheel) before the game sees them; binds subclasses the window once and is its only owner, so a menu or modal tool registers a filter instead of hooking the window |
| `camera` | view size; one FOV filter (`fov` uses it); a world-transform override (`SetWorldOverride`, `LastWorldXForm`: the free camera and the spectator; `ViewXForm` includes the active override), applied by its hook on `geCamera_SetWorldSpaceXForm`, so the game's own camera setup sees it too; `OnBeforeWorldRender(fn)`: prepare the override before each native camera set and world render (runs more than once a frame; never call a native camera setter) |
| `pause` | `Set(on, why)` / `Active()`: skip the simulation step and pause the game clock; ends itself on leaving the level. `Prepare` / `CancelPrepare`: install the pause hooks during a stage load without pausing yet (the spectator). `textdraw::HideWhilePaused` for view-relative panels |
| `menu` | one native briefing button/page (`RegisterPage`) with row, label and highlight operations; drawing, button and input callbacks (input arrives through `binds::OnWindowMessage` while the page is shown); native stage launch (`LaunchStage`) and the way back (`ReturnFromLevel`: leaves the level the game's way and shows the page over the briefing) |
| `spectator` | FDNYCore's one flying camera, over a paused game (D59; started by the core, `Available()`). The viewer (`Start`): modal, follows target poses its client supplies (`SetTargets`; `Prepare` + `PrimeCamera` before a stage's first render), takes the keys and the mouse, emits playback actions (`SetActionHandler`), Escape exits; knows no file format. The free camera (`StartFree(Flight)`): from the game's last view, not modal (binds stay live, other tools' captures are honoured), speed and look from its client (`SetFlight`; freecam's `fd_pause`). `ReadState`: pose, mode, the frame's step and keys |
| `state` | the frame's player snapshot; `SetWatchOnly` / `WatchOnly()`: the level is loaded to be watched, not played (the recordings viewer, or a later playback mode) |
| `entities` | lazy Player/Victim initialization, update and destruction observers; original calls, last-error and FP state preserved. `ObserveNativeContacts` + `AddNativeTrigger`: runtime trigger construction and contact notifications (world owns roots, service owns definitions until level entry). `FilterVictimSpot`: one filter that may move a victim's spawn spot before the floor snap (early step only; speedrun's static spawns) |
| `gfx`, `textdraw` | in-frame drawing ("Drawing" below; `core/gfx.h`, `core/textdraw.h`); `gfx::ProjectWorld` / `ScreenRay` for pointer tools; `textdraw::GameHint` reuses the original native font/widget with a timed priority window, no custom surface; `gfx::ClearWorldSubmissions` drops queued world lists after a frame, before a scene change |
| `profile` | run/practice gates + watermark (reads `SetPracticeOnly`) |
| `testpipe` (Debug) | `Register("<name>", handler)` for harness requests (32 channels; one more is a logged error) |
| `log`, `crashlog`, `patches`, `levelwatch` | logging, crash reports, the core's startup patches, level events |

## Rules (enforced)

| Rule | What | Enforced by |
|---|---|---|
| R1 | Core files (`core/`) never include a feature header. The core never names a feature: no core string holds a variable, command or alias a feature registers (also as the start of a longer name). | arch_check |
| R2 | A feature includes core headers and its own folder's headers only. Features talk to each other only through core services. | arch_check |
| R3 | Each feature folder has exactly one `FDCORE_MODULE(...)`, in a file whose `// Tests:` line names existing tests (paths from the repository root). | arch_check |
| R4 | No game addresses or module-base+RVA in code: every address/offset is an `addresses.json` entry, used by `addr::Id`. Exceptions need `// arch-allow: R4 <why>` on the line. | arch_check |
| R5 | Only `gfx` calls the Genesis drawing/bitmap API. Features draw with `textdraw` panels (or `gfx` blocks). | arch_check |
| R6 | Harness channels are Debug-only (`#ifdef FDCORE_DEBUG` around `testpipe::Register`). | Release build fails otherwise |
| R7 | No dynamically initialised globals or exit-time destructors: there is no CRT, they never run. Globals must be constant-initialisable (`= {}`, `constexpr`). | `-Wglobal-constructors -Wexit-time-destructors -Werror` |
| R8 | The draw order is gfx's: only gfx subscribes to the late frame step (`hooks::OnFrameEndLate`); everything else draws from a gfx layer (D40). | arch_check |
| R9 | The player reference (`docs/quick-reference.md`) is built from the source (D41). Each feature module has a `// User: <Title> -- <one sentence>` line. A feature that reads keys itself (not a bind) declares them with `binds::DescribeKey`. Registrations take string literals. | arch_check, `tools/docgen.py` (in `build.ps1` and the pre-commit hook, which also stages `docs/quick-reference.md`), `tests/game/docs_registry.yaml` |
| R10 | A feature's `Init()` logs why it refuses: every `return false` follows a `log::` / `console::Print` call in the same or the previous statement. A refusal logged elsewhere (an early step) carries `// arch-allow: R10 <where>`. | arch_check |

Not machine-checked, still rules:
- **Service ownership.** A Genesis/exe function is hooked by exactly one owner. Shared hooks belong to a core service that offers subscriptions/filters (frame events → `hooks`, camera → `camera`, rendered world → `gfx`, the game window's messages → `binds`). A feature that needs a hooked function asks the owning service; if none exists, add the service to the core first.
- **Practice-only** variables/commands are declared by the feature (`cmd::SetPracticeOnly`). Anything that shows information the game doesn't, or moves the player, is practice-only. Speedrun-category options (static spawns, RescueBot) are not; they show on the watermark.
- **Drawing order:** state updates in `hooks::OnFrameEnd`; drawing happens in a gfx layer (the table below, R8).
- **Freestanding C++17:** no CRT, no STL, no exceptions/RTTI, no `<initializer_list>`; no 64-bit integer division (use doubles or `MulDiv`); large locals are `static` (no stack probes). Windows API via kernel32/user32/gdi32/bcrypt only.
- **No player runtime dependencies.** What ships is the game + the ASI loader + `FDNYCore.asi` (+ its cfg). No Python, .NET, companion exe or VC++ redist; 32-bit only.
- **Watched levels.** While `state::WatchOnly()` is set, a feature that acts on play stands down: it records nothing, opens no editor, draws no practice overlay, and refuses to pause or take the camera (recorder, checkpoints, triggers, freecam). A new feature of that kind checks it too (D59).
- **Console-first.** Every feature is an `fd_` command or variable (a native menu page's own actions, such as renaming a recording, may be menu-only: owner 2026-10-28), saved to the config and runnable from `autoexec.cfg` (the reference lists them all, R9).
- **Piggyback first.** Before building a subsystem, look for a fixture the game or engine already has and reuse it (the core's console is the game's own; the camera, clock and entity list are the game's). Record what you found either way.
- **Genesis exports are hooked by name** (GetProcAddress + MinHook); never proxy or replace the DLL. Check every Genesis3D 1.1 SDK signature against the installed `Genesis.dll`'s export table: this build differs from stock 1.1 in places.
- **In-memory patches only**, and only through `addr` / `hooks`: never the exe or Genesis.dll on disk.

## Drawing: pick the block and the layer

Every drawing belongs to a gfx layer, run in this fixed order each frame (D40, `core/gfx.h`):

| Layer | When | Seen | For |
|---|---|---|---|
| `LAYER_WORLD` | right after the level geometry | hidden by walls and floors; the game's sprites, actors and HUD go over it | 3D things in the level: trigger boxes, markers, paths |
| `LAYER_OVERLAY` | after everything the game draws | over walls | x-ray views |
| `LAYER_HUD` | last | over everything | screen panels (textdraw does this) |

- Anything depth-tested must be drawn in `LAYER_WORLD`. Later in the frame, the game's sprites and particles have written depth over whole screen rectangles, which cut holes in it (D39, `docs/evidence/trigger_depth/`).
- `tests/game/draw_layers.yaml` checks it. Changing the order is a `docs/decisions.md` entry plus that test.

| Need | Use |
|---|---|
| A text panel | `textdraw::Create(...)` (HUD layer): `STYLE_TRANSLUCENT` (default; opaque fallback without a world) or `STYLE_OPAQUE` (always visible, e.g. the watermark). Own colours: `textdraw::SetPalette`. |
| Boxes in the level | `gfx::SubmitWorldBoxes(boxes, n, LAYER_WORLD or LAYER_OVERLAY)` from `OnFrameEnd`, every frame |
| Lines in the level | `gfx::SubmitWorldLines(lines, n, layer)`: polylines as mitered strips, colour per point, width in px or world units, optional glow and thin points (the replay; D50) |
| Your own drawing in a layer | `gfx::OnLayer(layer, fn)` (a new kind of drawing is a gfx block first, R5) |
| Opaque image / rect | `gfx::Decal`, `gfx::SolidRect` |
| Per-pixel alpha | `gfx::AlphaSurface` (256 px 4444 tiles, world-attached; colours are 4 bits per channel) |
| Screen size | `gfx::ViewSize` (the camera rect, never the window) |

## Owner decisions in force

- Players run FDNYCore alone; the core doesn't detect or defer to the old tools (FOV fix, alttabfix, FDNYDeterministic), and the VM's game folder doesn't have them (D55). A patch site someone else changed is a signature mismatch: left alone, with a warning.
- The watermark stays opaque (decal path).
- The practice-only list is the agent's default; the owner still has to confirm it.
- No extra log files for overlay output: it goes to `FDNYCore.log` (`glitch|` lines).
