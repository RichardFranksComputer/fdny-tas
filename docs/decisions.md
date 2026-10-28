# FDNYCore decisions

Why things are the way they are. One entry per non-obvious choice: the decision,
the reason, and what was rejected. Add an entry whenever you make a choice a
later session might "simplify" away; change one only with the owner's approval
(say so in the entry). Evidence paths are under `docs/evidence/`.

## Toolchain and runtime

- **D1 Zig (clang) freestanding C++17, no CRT.** Owner: keep Zig, MSVC only if needed. One small `.asi` with no runtime dependency, like the earlier `asi/` plugins. Consequences: no STL or exceptions, no 64-bit integer division helpers, no stack probes, and **no static constructors or exit-time destructors** (D2).
- **D2 No dynamically initialised globals (rule R7), enforced with `-Wglobal-constructors -Wexit-time-destructors`.** Nothing runs them. When this was enforced (0.16.1) it found two real cases:
  - `textdraw`'s panel array: benign, all-zero by luck;
  - `glitch_bsp`'s `ON_EPSILON = OnEpsilon()`: never initialised, so the BSP on-plane tolerance could be 0 instead of 0.1.
- **D43 One core per process: a second FDNYCore*.asi stays inert** (owner 2026-10-24: "Do 1 and 2"). Releases are named `FDNYCore-<version>.asi`, so an update dropped beside an older copy loads two cores, and both would hook the same functions.
  - **What:** the first copy's DllMain creates the named mapping `Local\FDNYCore.instance.<pid>` holding its path, and keeps it open. A later copy finds it, appends one WARN to `FDNYCore.log` ("... stays inert: another copy is already loaded (<path>)") and returns without patching, hooking or starting a thread. The first copy to load wins (alphabetical, ASI loader order), not the newest version.
  - **Supporting changes:** the log is opened append-only with write sharing, so the inert copy's line isn't overwritten. The watermark skips the running core by module handle, not by the name `FDNYCore.asi`, and lists an inert copy among the other asi. The Debug address-override path is found the same way.
  - **Test:** `tests/game/core_single_instance.yaml` (two renamed copies).
  - **Rejected:** a per-process mutex (it can't carry the first copy's path); picking the newest version (both copies would have to load far enough to compare, and the loser has already run its DllMain); refusing to start at all (the player loses every feature for a packaging mistake).
- **D3 MinHook (vendored) for every inline hook; one owner per hooked function.** Two MinHook hooks on one target in one instance aren't possible, and two owners would fight over semantics. Shared hooks live in a core service that offers subscriptions or filters (`hooks` frame events, `camera`, `gfx` world).

## Game access

- **D42 Trajectory recording consumes shared entity lifecycle observations.** The
  `entities` service owns Player initialization/update and Victim spawn/update/
  destruction through `hooks`; the recorder owns JSONL files and settings.
  This preserves Player-update sampling and victim identities without duplicating
  hooks or changing the frame snapshot service. `fdny-trajectory-v1` retains
  wall-clock time, raw fields/keys and measured bounds. Passive recording is
  available in both profiles. Default output is `Recordings` beside the ASI.
  A standalone `FDNYRecorder.asi` disables this feature to prevent duplicate capture.

- **D44 `entities` owns all of SpawnAtSpot, including the spot filter** (owner 2026-10-24: "Do 1 and 2"). Speedrun used to patch 0x0045D717 inside the function whose entry `entities` hooks (D3: one owner per function).
  - **What:** `entities::FilterVictimSpot(fn)` installs the trampoline before the floor snap and holds one filter. It must be called from a module's early step, since the patch goes in at DllMain, before any level exists. Speedrun calls it.
  - **One filter, not a list:** two features moving the same victim would need an ordering rule. A second request fails, like camera's FOV filter.
  - **Rejected:** routing the pin through the entry hook's spawn observer. That runs after SpawnAtSpot returns, so after the floor snap, and the moved victim would float or sink.

- **D4 Every address lives in `addresses.json`** (rule R4). It is versioned by exe + Genesis SHA-256, and every entry is signature-checked at startup. A bad signature disables only what needs that entry, and the game runs on. This replaced raw addresses so an unknown game version degrades instead of crashing.
- **D5 Defer to other mods that own a patch site** (alttabfix, FOV fix, FDNYDeterministic). Superseded by D55.
- **D6 Ghidra work goes through subagents**, read-only; the primary session never calls GhidrAssist MCP. Project rule (memory: loop owns Ghidra writes).

## Architecture

- **D7 Core services + self-registering feature modules, in one binary** (0.16.0, owner chose "option A").
  - **Rejected for now:** separate plugin DLLs ("option B": versioned C API, load-order waits, a runtime per DLL, a bigger test matrix). A is shaped to map onto B later: module descriptor = plugin entry, service headers = API.
  - **Proof:** `build.ps1 -Without *` builds and runs the core alone.
- **D8 Registry by COFF section (`.fdmod$a/$m/$z`), not a list or constructors.** A list in the core would make the core name features (R1). Constructors never run (D2). lld merges `$`-suffixed sections in name order, so the core walks pointers between its two markers (nulls skipped).
- **D9 Practice-only is declared by the feature (`cmd::SetPracticeOnly`), enforced by `profile`.** Previously `profile` hard-coded feature names, a service knowing features. The classification itself is the agent's default and still **awaits owner confirmation**.
- **D10 `camera` is a core service** owning `geCamera_SetAttributes`. `fov` is a filter on it, and `gfx` reads the view size from it. Before, `gfx` (a service) depended on `fov` (a feature).

## Drawing

- **D11 Text is GDI-rendered into one bitmap per panel, re-rendered only when it changes** (≥ `minFrames` apart). One blit or poly set per frame. Rejected:
  - `geEngine_Printf`: one ~29 µs blit per character, max 40 strings × 80 chars; a full box ran at 12 fps;
  - an external overlay window: no fullscreen or OBS capture, and a second UI thread.

  Evidence: `foundation/` "In-frame text investigation".
- **D12 `geEngine_FlushPipeline` once before the frame's first immediate draw.** Without it, the cached 3D is drawn over the 2D inside EndFrame. This was the cause of the early "invisible bitmaps".
- **D13 Placement from the camera rect, never the window client size** (0.14.1). With dgVoodoo scaling, the window is the desktop size and the render is smaller, so right- and bottom-anchored panels landed off-frame.
- **D14 Translucency = `gfx::AlphaSurface`: 256-px 4444 tiles attached with `geWorld_AddBitmap`, drawn with `geEngine_RenderPoly`** (flags 1, z 1.0, −0.5 px offset). Spike: `alpha_spike/README.md`. Rejected or impossible:
  - untextured vertex alpha: D3DDrv ignores it;
  - engine-attached bitmaps: 2D decal handle only, crashes D3DDrv;
  - textures over 256 px: crash;
  - 32-bit ARGB: stored as 4444 anyway.

  Without a world (menus) a translucent panel falls back to an opaque decal. Tiles are re-added when the world changes.
- **D15 Text alpha is a parameter, not a separate method** (0.15.1). It costs the same, since the tile is blended either way. Strictly opaque drawing is the Decal (`STYLE_OPAQUE`).
- **D16 The watermark stays opaque** (owner), so it shows in menus too.
- **D17 Font: Consolas, non-antialiased, default 9 px (`fd_text_size`).**
  - Non-antialiased so every text pixel is exactly its colour: alpha keys on the background colour.
  - 9 px because the owner wanted smaller than 11.

## Features

- **D18 Teleporter writes position, view and zeroed motion at frame end; it does not use the game's Verb_Reset.** Verb_Reset drops held objects and the victim, resets health and zones, moves to an escape-mutated checkpoint, and SnapToFloor commits (0,1,0) without a floor. Owner: it only works on the map it was saved on. It has a single in-memory slot; stance is not saved.
- **D19 Collision-overlay output goes to `FDNYCore.log` (`glitch|` lines), no extra files** (owner).
- **D20 Speedrun options (static spawns, RescueBot off) re-implement FDNYDeterministic.** They no longer stand down when it is installed (D55).
- **D24 Trigger boxes come from live memory every level; there is no BSP scan or coordinate file** (owner). Triggers are the entities whose vtable is `trigger.vtbl`. The set is rescanned on a map change and about once a second. The exits' open flag is read every frame. Spike: `trigger_boxes/README.md`.
- **D25 Trigger classes are the game's own tests, not kind and not our own parsing rules:**
  - Kind is 8 for every trigger: the constructor writes it, so it can't separate exits.
  - Rescue zone = `trigger.escape`, the flag the game sets when a trigger binds `touch` → `tell player escape`.
  - Exit = a `touch:` line containing `tell player finalexit` or `tell player (finalexitloadmap`. These are the two has-binding tests in `PlayerRescueEnt::UpdateZonesAndWin`, which match by substring.
  - Open = `trigger.enabled`, set by `BoundsTrigger_SetEnabled(1)` when every victim is delivered.
  - An exit is also an escape trigger, and exit wins.
- **D26 World boxes are a `gfx` block (`SubmitWorldBoxes`), drawn in gfx's own late step before the text panels.**
  - Only gfx may call RenderPoly (R5).
  - A queued poly would land on top of the panels at EndFrame's flush. So gfx flushes the queued boxes itself, from an `OnFrameEndLate` subscriber registered before textdraw's.
  - Faces are clipped to the near plane (2 u) and the view, and drawn far to near.
  - One 64-px solid white 4444 tile per alpha level, tinted by vertex colour (the spike showed D3DDrv modulates textured polys).
- **D27 Trigger boxes are depth-tested by default; `fd_triggers_xray 1` draws them over walls.**
  - Depth-tested: queued polys (flags 0) with `geCamera_Project`'s z, so the world hides them.
  - X-ray: immediate polys, which skip the z-test.
  - Rejected: x-ray as the default. Big zones behind walls cover the view and hide the room you're in.
- **D28 A box around the camera is drawn at 25% instead of 50%.** The spawn often stands inside large zones, which tinted the whole view (`spike/training_bank.png`).
- **D29 Default key T (`fd_triggers_toggle`) through `binds::BindDefault`.**
  - The default applies before `FDNYCore.cfg`, so the player's own binds win. An unbound default is saved as `unbind t`.
  - The F-keys were rejected:
    - F2–F4, F6–F8 and F11 are the game's developer keys (F11 crashes, F8 wins the level);
    - F9, F10 and F12 are the collision overlay's;
    - F5 / F6 are suggested for the teleporter.
  - T isn't one of the game's controls (`GameControls::Construct`).
- **D30 A world-attached tile that settles without a driver handle is re-added** (0.17.0, gfx: world boxes and `AlphaSurface`).
  - The world can't be identified by its pointer alone: bank, loaded after Training, got Training's world address. The tiles looked attached but had no handle, so nothing was drawn and text panels would have stayed on the opaque fallback.
  - The handle check after the settle frames is the signal. Log line: `gfx: world box tile without a handle; re-adding`.
  - Rejected: re-attaching on `OnLevelLoad`. It races the first render of the new world, and the handle check also covers anything else that drops a texture.
- **D31 textdraw gains `CENTER` and `STYLE_TEXT_ONLY`** (fd_juggle, owner: "no background, 30 px below centre").
  - **`CENTER`:** the panel is centred horizontally (`dx` shifts it), and `dy` is measured from the view's vertical centre to the panel's top.
  - **`STYLE_TEXT_ONLY`:** the same `AlphaSurface` path, with background slot 0 keyed to alpha 0. The font is non-antialiased (D17), so only glyph pixels stay; there's no fringe.
  - **No world (menus):** it isn't drawn at all, rather than falling back to the opaque decal: a world-space hint has no meaning there, and a box would contradict "no background".
  - Existing anchors and styles are unchanged.
  - Rejected: a per-panel background-alpha parameter. It would be one more knob for no use beyond "none", and `fd_text_alpha` would have to be special-cased for it anyway.
- **D32 fd_juggle observes the safe spot; it doesn't evaluate any conditions** (0.18.1, owner ruling; replaces the 0.18.0 gate).
  - Rule: following = this frame's spot equals the player's position at the end of the previous frame (Think's write copies it, spike `fire_juggling/round2_frame_order.txt`); anything else is frozen and shows the arrow. Read at frame end. A new player object starts as following.
  - **Why (owner):** a gate (flinch / burn >= 0.1 / O2 <= 0.5 / passed out) bakes our current understanding into the tool, so a condition we don't know about would show "-" while the spot was really frozen. Observing the write shows every cause, known or not, and players work out the conditions themselves.
  - **Consequences, accepted:** a jump shows the arrow for its whole length and landings blip it for 1-2 frames (both are real: the game doesn't save then; the 0.18.0 gate hid airborne on purpose). Standing still, a spot frozen under your feet reads as "-".
  - Transitions are logged with burn / O2 / flinch / state / on-ground as data, not as a verdict. A write to anywhere but the last position is logged and counted as "written elsewhere" (the respawn's checkpoint mirror is the known case).
  - The arrow aims at **spot - 30 X** (where Verb_Reset puts the survivor), before the floor search. Direction is relative to the view yaw, with axes measured live (`fire_juggle_overlay/yaw_probe.log`).
  - Rejected: "more than 5 u away" (a few frames late and wrong at the start of a freeze); the 0.18.0 gate (above).
- **D33 Test hooks that move or burn the player are Debug-only commands in the feature** (`fd_dbg_setpos`, `fd_dbg_o2`, `fd_dbg_tofire`; R6).
  - `fd_dbg_tofire` searches the fire nodes live, because fire nodes move along their paths: the spike's coordinates burnt only sometimes.
  - The glyph check holds an O2 lock on clean ground and stands the player 30 u off the frozen spot, so every view direction has an exact expected answer.
  - The harness's `assert_juggle` recomputes the arrow from its own memory reads, rather than trusting the pipe.

- **D34 Pause = skip `GameSession::TickSubsystems` + `GameClock::Pause` (core service `pause`); the free camera is a world-transform override in `camera`, applied from gfx's `geEngine_RenderWorld` detour** (fd_pause; spike `freecam/README.md`).
  - **Why this site:** RunFrame runs the clock tick, the input poll, TickSubsystems (every entity list, the spawn/callback queues, the sound update), then RenderFrame. Skipping the one call froze the player, victims, the mission timer and the clock while rendering went on (probe runs p2-p5).
  - **The clock:** the game's own Pause/Resume on the active subtimer (as on alt-tab).
    - It stops elapsed time and the render-side particle keyframes (`ParticleNode::AdvanceSpawn` reads `g_gameTimeMs`).
    - The first frame after resume has a normal delta.
    - Its 60 FPS Sleep cap is in the paused `ComputeFrameDelta`, so the service caps paused frames at ~60 fps itself.
    - The game's alt-tab Resume would un-pause the subtimer, so it is paused again each paused frame.
  - **Input:** `controls.block_input` is held while paused. It only gates the game's action queries; polling and the mouse sums go on. On resume it is released unless the console is open.
    - Window-message keys bypass it: Esc while paused opened the in-game menu (freecam run fc5). So the service also hooks `GameSession::OnKey` (`session.on_key`, reached from WM_KEYDOWN after the console) and drops key-downs while paused.
    - Key-ups pass, so the game's Esc latch still clears.
    - The console and FDNYCore binds come before it, so they still work.
  - **Hook state:** the hook is enabled only while paused (no cost otherwise; group toggle).
  - **Ending a pause:** leaving the level (game state != 1, a level load) ends it.
  - **Camera:** a transform set on the render camera at frame start reads back but is rewritten inside the render before the world is drawn (p2-p4). Set on the camera passed to `geEngine_RenderWorld` it shows (p5). gfx owns that hook, so it calls `camera::BeforeWorldRender` (core to core). Clearing the override hands the camera back to the game's per-frame set.
  - **Known side effect:** the collision overlay's tick counter (`glitch.tick`, after TickSubsystems in RunFrame) still counts paused frames; nothing it records moves then.
  - Rejected:
    - dt = 0: per-frame logic and 1/dt;
    - the game's own spectator mode (F7): it moves the player, needs Think, and changes the timer, burn and triggers;
    - a camera set at RenderFrame entry (above).
- **D35 View-relative panels hide while paused (`textdraw::HideWhilePaused`)**. An arrow relative to the player's view (fd_juggle) means nothing to a free camera. The panel declares it, and textdraw skips it while `pause::Active()`, so the pause feature never names another feature (R2).

- **D36 fd_pause: default key P; free camera W/A/S/D + E/Q, Shift ×4 / Ctrl ×0.25, mouse from the game's own sums** (owner-agreed design, 2026-10-22).
  - **Keys:** P is not a game control (GameControls::Construct binds W A S D C, 1-3, Ctrl, Shift, Space, arrows, LMB/RMB, and F2-F4/F6-F8/F11 only in dev mode) and not an FDNYCore default (T, F9/F10/F12; F5/F6 suggested for the teleporter). It is bound through `binds::BindDefault`, so the player's own binds win. E/Q (up/down) are free too. Shift and Ctrl are the game's USE / CROUCH, harmless while input is blocked.
  - **Mouse:** `controls.mouse_x/y` are the game's per-frame DirectInput sums, still polled while paused. The Windows cursor is useless: the game re-centres it every frame. Default 0.075°/count (`fd_freecam_sens`; owner: 0.15 was twice too fast). Yaw grows to the left (the measured convention), so mouse right lowers it (owner: left and right were swapped at first).
  - **Speed:** `fd_freecam_speed` 400 u/s (the run speed) × real frame time (QPC; the game clock is paused), capped at 0.1 s per frame; no acceleration.
  - **Start pose:** the game's last camera transform. Genesis camera columns are right / up / back (the view is −column 2). The rebuild error against the game's matrix is checked by the test (0.000 in fc4).
  - `fd_pause` is not archived: a session never starts paused. Practice-only, so `fd_profile run` refuses it and resets it to 0, which resumes.

- **D37 Every feature has a default letter key; no F keys** (owner, 2026-10-22).
  - **Map:** P pause; J juggle; H player HUD; G collision HUD with its B / Z / X (were F9 / F10 / F12); T trigger boxes (cycling x-ray, D38) / Y all; K / L teleporter save / load.
  - **Excluded letters:**
    - the game's keys (W A S D C, Shift, Ctrl, Space; GameControls::Construct);
    - the free camera's (W A S D E Q);
    - O (held to free the cursor).
  - **Not bound, on purpose:** the speedrun options, profile, watermark, FOV and text settings. A stray press would change a run's setup.
  - **Mechanism:** toggles run the new `toggle <var>` builtin through `binds::BindDefault`, so the player's binds win (D29) and the profile gate applies.
  - **The collision HUD's three hotkeys stay edge-triggered inside its frame step** (only the key codes changed). The clear must keep its place between the frame's hotkeys and build() for the replay parity (the 0xF5 marker). Its note text "history cleared (F12)" is the original overlay's and stays identical.
  - Supersedes the F-key part of D29 (F5/F6 are no longer suggested for the teleporter).
  - **FDNYCore.cfg records only changes from the defaults** (owner 2026-10-22: every default bind was written out).
    - A default key still on its default line is not written.
    - A rebind is written as `bind`, and a removed default as `unbind`.
    - Why: a later default change then reaches existing players, whose files hold only their own choices.
    - Old files with default lines clean themselves on the next save.
  - **Turning `fd_hud_glitch` on always shows the boxes** (it clears B's hide and Z's freeze). Owner: a B hide outlived G off/on, so G alone looked dead.

- **D38 One player-facing name: the warp HUD** (owner 2026-10-22: "collision" in one place, "glitch" in another, and it is really about warping).
  - **Renamed:** `fd_hud_warp`, `fd_warp_hide` / `fd_warp_freeze` / `fd_warp_clear`, and every doc and help text.
  - **The old names stay as aliases** (`cmd::RegisterAlias`): old configs and habits keep working, and the config is written under the new name.
  - **Unchanged on purpose:**
    - the module and file names (`glitch*.cpp`);
    - the `glitch|` log lines;
    - the parity files and the test pipes.
    The log and box text must stay byte-identical to the original Python overlay (glitch_parity_*), and the player never types those names.
  - **Trigger boxes: T cycles off -> on -> on + x-ray -> off** (owner). The separate U key for x-ray is gone; `fd_triggers_xray` remains a variable.

- **D39 Depth-tested world boxes are drawn right after `geEngine_RenderWorld`, as immediate polys with the projected z** (supersedes D26/D27's queued late-step polys for the depth-tested path; owner report 2026-10-23, oil_rig_A).
  - **What was wrong:** drawn at the late step, the boxes lost large screen-aligned rectangles, even over open sky. The game draws more after the world (sprites, particles), and that writes depth over whole rectangles.
  - **Evidence:**
    - x-ray showed the true box;
    - immediate + projected z at the late step lost the same rectangles;
    - halving z filled most of them;
    - drawn right after the world render, the holes went away, and the box was cut only where world geometry really is in front.
    - `docs/evidence/trigger_depth/`.
  - **Mechanics:**
    - gfx's RenderWorld detour (gfx already owns it) draws the list the feature submitted at the previous frame end. The boxes are static and the camera is this frame's, so the one-frame lag doesn't show.
    - gfx flushes the game's queued polys first.
    - Immediate polys don't write depth, so what the game draws later (actors, sprites, its HUD) goes over the boxes. The game's HUD is no longer tinted by a box you stand in (an old known gap).
  - **X-ray is unchanged:** late step, z 1.0, over everything and under the text panels.

- **D40 Named draw layers, run by gfx in a fixed order: WORLD, OVERLAY, HUD** (owner 2026-10-23, after D39).
  - **Why:** the order used to depend on registration order (gfx's late step "registered before textdraw's") and on which hook a drawing happened to use. D39 showed the cost: one wrong moment and boxes lose rectangles or show through walls.
  - **The layers:**
    - WORLD runs from gfx's RenderWorld detour;
    - OVERLAY then HUD run from gfx's single late-frame subscriber;
    - features subscribe with `gfx::OnLayer` or submit boxes with a layer. Several features can submit boxes in one frame.
  - **Enforced:** R8 (arch_check: no `OnFrameEndLate` outside gfx) and `tests/game/draw_layers.yaml`.
    - The test uses a Debug test box at the oil-rig wall corner: WORLD behind the wall changes the picture by 0.00, OVERLAY there by 129, WORLD in the open shows.
    - The layer order comes from the `layers` pipe.
    - It runs paused (`fd_pause`): unpaused, two shots of the same view differ by 13 (moving sky, clock).
  - **Rejected:** keeping the registration-order convention (silent when broken); a priority number per subscriber (more freedom than three meaningful moments need).

- **D41 The player reference (was the key sheet) is generated from the source on every build** (owner 2026-10-23: "built each time so we don't have something that can go stale").
  - **What:** `tools/docgen.py` reads the string literals at the registration calls: variables, commands, practice-only, aliases, `BindDefault`, `DescribeKey`, and each file's `// User: <Title> -- <sentence>` line. `build.ps1` writes `build/<Config>/README.md` every build; the release workflow ships the Release one in each release's zip (D45). Hand-written release notes are the root `CHANGES.md` (was `dist/CHANGES.md` until 2026-10-24). The sheet for the current source is the repository's `README.md` (tracked; was `core/KEY_SHEET.md` until 2026-10-24, owner: the front page): every build and the pre-commit hook rewrite it, and the hook adds it to the commit, so GitHub always shows the source's sheet. Since 2026-10-27 (D56) it is `docs/reference.md` (owner: "reference", it has the keys and every variable), `build/<Config>/reference.md` and `reference.md` in the release zip; `README.md` is a hand-written front page.
  - **Rules (R9, arch_check):** a feature module has a `// User:` line; a feature that reads keys itself (`GetAsyncKeyState`) declares them with `binds::DescribeKey` (also shown by `bindlist`). docgen stops the build on a registration it can't read (arguments not literals, a `RegisterCvar(&x)` with no `cmd::Cvar x = {...}`, a registering file with no `// User:` line). (Until 2026-10-24 `build.ps1` and the hook also checked that `dist/README.md` named the SHA-256 of `dist/FDNYCore.asi`; `dist/` is gone, D45.)
- **D45 The GitHub release is the only download** (owner 2026-10-24, "Yes", to making the release the single source). Since the release workflow (0.18.1), every version existed twice: the workflow's build from source, and a VM build committed to `dist/` by `build.ps1 -Dist`. They came from different machines and weren't checked to be byte-identical, so the two SHA-256s could disagree.
  - **What:** `dist/` is removed (the `.asi`, its sheet, `-Dist` and `check-dist`). The release notes move to the root `CHANGES.md`, and the workflow puts its `## <version>` section into the release's notes. The release's sheet is its own build's (in the zip).
  - **Rejected:** keeping `dist/` as a mirror (two artifacts per version and a hash to keep in step); having the workflow commit its build back to `dist/` (a bot commit on every release, and a binary in history each time).
  - **Checked against the build:** `tests/game/docs_registry.yaml` has the Debug build write what it registered (`fd_dbg_registry`) and compares it with docgen's reading, item by item, so a registration the scanner misses fails a test.
  - **Values:** the variable's help text is its value list. Help texts say the values ("0-255", "practice | run", "1 = ..., 0 = ..."); no separate values field.
  - **The sheet is the player documentation:** the hand-written player guide was removed (owner, 2026-10-23); module-author notes are `CLAUDE.md`'s "Core services".
  - **Rejected:** a hand-maintained table (goes stale, as the old guide's did); dumping the sheet from the running game (needs the game at build time, and the sheet must be built on every build).

## Process

- **D59 One flying camera; a watched level is a core state; the menu owns the way back** (owner 2026-10-28, reviewing PR #31: "Rebuild freecam in this pr"; on the viewer state, "If the viewer state is something that can be more generic for future additions let's add that as a state"; "No console for rename is fine since there's a menu"; "Move return to briefing into menu").
  - **One camera:** the core `spectator` service is the only free-flying camera. The recordings viewer uses it modally (`Start`: it takes the keys and the mouse, follows runs, Escape exits). `freecam`'s `fd_pause` uses its free mode (`StartFree`): not modal, so binds stay live (P resumes) and keys or the pointer another tool captures (the checkpoint editor) are left to that tool. `freecam` keeps the pause, `fd_pause` / `fd_freecam_speed` / `fd_freecam_sens`, its log lines and its test measurements, which it now takes from the service's pose. Flight moves at one speed in every direction (the old freecam added the keys, so diagonals were faster); pitch is limited to +-89 in both modes.
  - **Watched level:** `state::WatchOnly()` means the level is loaded to be watched, not played. The viewer sets it before the stage loads and clears it when it leaves. The recorder records nothing, the checkpoint editor and the trigger overlay stay off, and `fd_pause` is refused. These four checked the spectator camera before; that stopped meaning "a recording is being watched" once `freecam` used the same camera. Any later playback mode sets the same state.
  - **The way back:** `menu::ReturnFromLevel` leaves the level (the game's own Escape through `GameSession::OnKey`) and shows the page over the briefing. The replay feature used to send that key itself. The game values the menu relies on (title screen 2, 10 native screens, MenuScreen_List's backgrounds 11/17) are named constants citing the evidence.
  - **Console:** a native menu page's own actions (renaming, favoriting a recording) may be menu-only; watching has `fd_replay_view`.
  - Rejected: keeping two cameras (two copies of flight, look and capture rules); a viewer flag owned by the spectator service (a camera is not the reason a level is loaded); leaving level exit in the feature (a game-wide action belongs to a core service). Tests: `freecam`, `checkpoints`, `spectator_camera`, `recordings_menu`, `recorder`, `triggers`.
- **D58 Binds act only inside a level; menus read the keyboard themselves** (owner 2026-10-28: "we could just do binds in game only and listen for keyboard input when in menu, I don't think we'll need menu hotkeys"). A bind fires only while a level is loaded (game state 1), the game has focus and the console is closed. A menu screen that needs keys takes them through its own input handler (`menu::Callbacks::input`: the Recordings page's Escape, PageUp/PageDown and rename typing), so typing in a menu never runs a gameplay bind. The game window's messages have one owner, `binds` (`OnWindowMessage` filters): PR #29's menu and spectator each subclassed the window and found it their own way; they now register filters (one subclass, one way to find the window). Every default key is a level action, and the menu-safe commands (settings, `exec`, `echo`) need no hotkey at the menu: `FDNYCore.cfg` and `autoexec.cfg` already apply them at startup. Rejected: a per-command level-only flag, and a second bind table for menus (no command needs a menu hotkey). Tests: `autoexec`, `vars_binds_persist` (a bound key does nothing at the menu and fires once in a level).
- **D57 Recordings extends replay; native menu and spectator are services.** The briefing browser and its metadata stay in `features/replay/`, sharing the existing JSONL reader and recording directory. The menu service owns native widgets and the briefing hook, while the spectator service owns the paused camera and modal input. Features exchange no headers across groups. Renames and favorites use atomic `.fdny-meta` sidecars; trajectory names and data stay unchanged. Incomplete files replay a validated snapshot through the last complete sample. Viewer exits happen after the engine closes its frame, before returning to the browser. Rejected: a companion ASI with its own hooks, logging and replay bridge; a second replay parser; inferring mission success from recording closure. Evidence: `docs/evidence/recordings/README.md`.
- **D56 Repository layout: one top-level folder per component** (owner 2026-10-27: the old layout "is not structured in an industry standard way, and makes reading the code confusing"; the proposal: "this looks way better").
  - **What:** `core/` (services, entry point, module registry, address table) and `features/<name>/` (one folder per module) side by side, like Godot's `core/` + `modules/` or OBS's `libobs/` + `plugins/`; `thirdparty/`, `tests/game` + `tests/unit`, `tools/` (checks, generators, `harness/`, `templates/`), `ghidra/` (export + scripts), `docs/` (architecture, decisions, reference, game research, the VM plan, evidence). Build output and test runs go to `build/` (untracked).
  - **Feature folders:** a feature's folder is its group (R2, R3), instead of the `glitch_*` file-name prefix. `-Without <name>` leaves a folder out.
  - **Guidance in one place each:** `docs/architecture.md` (services, rules, drawing) and `CONTRIBUTING.md` (build, test, done means, releases) are the rules for people and agents; `CLAUDE.md` imports both and adds only agent and VM facts. `core/CLAUDE.md` is gone (it repeated the VM rules and "Done means"); the `new-feature` skill points to the docs instead of restating them.
  - **Rejected:** `src/core` + `src/features` (one more level for no gain); moving each feature's tests and evidence into its folder (the harness and the evidence topics span features).
- **D55 The core doesn't defer to the old tools** (owner 2026-10-27: "Players will be instructed to only use the specific .asi for fdnycore so any failures from additional tooling is on them"). Replaces D5.
  - **What:** no detection of alttabfix, the FOV fix, FDNYDeterministic or FDNYVictimSpawns. Speedrun no longer stands down when FDNYDeterministic.asi is in the game folder. A patch site another mod has detoured is a plain signature mismatch (`addr`): still never patched, now a warning. The on-demand tests that ran the old tools are gone, and the VM's game folder no longer has them (moved to `C:\fdny-decomp-oldtools-20261027`).
  - **Kept:** signature checks. They are what stops a double patch, and they don't depend on knowing which mod got there first.
  - **Rejected:** keeping the deferral for players who mix tools (the owner: unsupported); deleting the old tools from the VM (moved, not deleted).
- **D54 Rules an agent missed become checks** (owner 2026-10-27, after the review of PR #20: "add that on a third branch").
  - **R1 covers strings:** a core string holding a name a feature registers (also as a prefix) fails arch_check. PR #20's entities service carried `fd_checkpoints_contact`; the include-only check let it through. The `bind` usage example named `fd_tp_save` and now uses `echo`.
  - **R10:** a feature's `Init()` logs why it refuses (PR #20 returned false silently; `modules.cpp` said "init failed; see above" with nothing above). Refusals logged in an early step say so with `// arch-allow: R10` (speedrun, window).
  - **Version:** the PR that releases bumps `VERSION` and writes `CHANGES.md`; a feature PR that waits for a release PR doesn't. This is what PRs #17-#19 already did; the old wording said every feature PR.
  - **Rejected:** wording alone (both rules were already written, in `docs/architecture.md` and the skill, and were missed); CI checking that a feature PR bumps `VERSION` (contradicts batching releases).
- **D53 Practice checkpoints use native triggers and hints** (Jacob Pilawa, PR #20). The entities service
  owns BoundsTrigger creation/contact observation and retains definitions until
  world teardown; one binding for an event nothing fires (core-generic, it names no feature)
  enables native contact processing.
  Textdraw uses the native bitmap font with upward positioning and four-second
  priority. The existing free camera, input capture and gfx projection support
  the editor. Drafts autosave to `Checkpoints/<mission>.txt`; restart applies them.
  Player initialization starts timing; menu resume preserves it. Rejected: custom
  overlap polling, replacing BSPs and custom hint surfaces. Evidence:
  `docs/evidence/checkpoints/README.md`.
- **D52 No parity test against the Python collision overlay** (owner 2026-10-27: "we no longer need to test parity against the python script if the warp overlay is now superseded by the fdny native implementation"). The warp HUD is the reference now. Removed: the overlay (`tools/collision_overlay/`; its game research write-ups moved to `docs/evidence/warp_hud/`), the Debug `.glitchrec` / `.glitchbox` dumps and the `glitch_replay` pipe, `glitch_replay.cpp`, the harness's replay/fuzz/compare scripts, and the tests `glitch_parity_log`, `ondemand/glitch_parity_fuzz` and `tests/unit/test_glitch_parity.py`. Supersedes the parity parts of D38 and D47. The warp HUD keeps its game tests (`glitch_records`, `glitch_box_cost`, `text_panels`, `keys_default`).
- **D51 Choosing replays without file names** (owner 2026-10-26: "i like 1-4"; ghost "Cvar picks"; picker "No default key"; tab completion set aside).
  - **Why:** recorder file names (`<map>_<UTC start>_pid<n>_run<n>.jsonl`) are long, and remembering them would leave the feature unused (owner).
  - **One folder:** the recorder and the replay both resolve `fd_record_directory` with `console::GamePath` (absolute, or under the game folder; default `Recordings`). They used to have two copies, which differed on a leading `/`.
  - **The index (`replay_data`, unit-tested):**
    - from the name: the map, the UTC start, the pid and the run number;
    - from the last 64 KB of the file only: the duration (the last sample's `t_ms`) and whether the end record is there (complete, not being written);
    - **finished:** the same game's (pid's) next run is on another map. This works on files already recorded. It also counts leaving through the menu for another level, because the recorder has no level-complete event (that would need Ghidra work).
  - **V:** with nothing loaded for the map, it loads the best finished run and the latest complete run, then plays.
  - **`fd_replay`:** opens the picker: this map's 9 newest complete runs (the best keeps the last row if it is older), in local time, with duration, finished, best and latest. It pauses the game, which blocks game input; keys 1-9 load or unload a run, read only while the console is closed. V or `fd_replay` closes it.
  - **`fd_replay <text>`:** the first file, by name, whose name contains the text, ignoring case.
  - **`fd_replay_ghost`:** 1 = best (else latest), 2 = latest, at every spawn.
    - The spawn is `entities::ObservePlayer`, the same moment the recorder's clock starts. The ghost starts at the time since the spawn, so it stays in step with the recording's clock: 1047 ms in the test, a level becoming playable after its spawn.
    - It loads at the frame end after the spawn, once the recorder has closed the previous run.
  - **Module order 110 (after the recorder):** with the replay at 85, it created and enabled the player hooks first. The recorder's victim hooks were then patched while the game ran through them, and startup hung, which made `keys_default` fail 3 times in 3 runs (slow frames, a missed key, then a hung window). At 110 it only adds a listener: 3 of 3 passed.
  - **Rejected:**
    - Tab completion: it needs the game console's input handling and edit buffer reverse-engineered first; set aside by the owner.
    - Numbered console lists: the picker shows the same thing in the game.
    - Friendlier recorder file names: with the above, names are rarely typed.
    - Reading whole files to index them: a full recording is 0.5 MB, and only the end is needed.
- **D50 World lines: one projection per point, mitered strips; the replay draws what it needs** (owner 2026-10-26: "Do em all", after a review of how other games and tools draw player paths).
  - **Measured first:** Debug CPU timer on world lines (`gfx::WorldLineCost`, replay pipe `us` / `mean_us`). On the reference run, xray on, after playing to the end (`docs/evidence/replay_lines/`):
    - whole trace from above: 2665 strips, 3351 us per frame before; 525 strips, 554 us after;
    - the player's view at the start: 296 strips, 660-747 us before; 87 strips, 471-481 us after.
    - The frame-ratio test can't show this: under the display's refresh cap, 3.4 ms fits in a frame's spare time (D46's limit).
  - **gfx (`DrawChunk`):**
    - every point is transformed and projected once per line, and the line's glow is drawn from the same points (it was a second line);
    - consecutive segments share mitered corners, held to 2 half widths: a translucent strip no longer overlaps itself at every point, where the glow built up to nearly solid far away, or notches on the outside of turns;
    - a point under 1.5 px on screen from the last one kept is skipped;
    - a strip inside the view skips the 4 clip passes;
    - optional width in world units, held between a least and a most px, and half width per point (`thin`).
    - The line is drawn before its glow. In `LAYER_WORLD`, the glow drawn first hid the line (its depth), seen in the game.
  - **Replay:**
    - The line runs through `Simplify`'s samples: Douglas-Peucker at 0.5 units (under a pixel from 160 units away), plus every point where it turns thin and the half-time sample, where the colour fade turns blue. 497 of 1391 on the reference run. Playback and the box still use every sample; the line ends at the interpolated playback point. This replaces D49's "no decimation": the samples are untouched, the drawn line stays within 0.5 units of them.
    - 8 units wide, held to 3-8 px, so distance reads as depth.
    - Half width in the air: off the ground for 3 samples or more (the recorder's `ground_contact_raw`; on the reference run these are jumps, the jump key held and the vertical speed rising and falling). One or two samples off the ground on stairs don't count.
    - A white 10-unit tick every second (up to 60 per recording; sparser for long runs): the spacing shows speed, which the colour fade doesn't.
    - `fd_replay_xray 1`: the trace stays in the world layer and adds a 25% copy over walls, so what's behind a wall reads as dimmed. The box and arrow are over walls at full strength, as before.
    - Cleared recordings are freed at the next frame end: the world layer draws the list submitted the frame before, which points into them (a crash, seen in `replay` once the trace was in the world layer with x-ray on).
  - **Rejected:**
    - additive glow (no blend mode in the poly API the core uses, as D49);
    - round joins and caps (more polygons, for strips 3-8 px wide);
    - simplifying in screen space each frame alone (the per-frame merge does that for far points; the load-time pass removes points everywhere for nothing per frame);
    - dashes in the air (a dash pattern needs distance along the line and more polygons; half width says the same).
- **D49 Replay reads recordings the fdny-maps website's way; lines are a gfx block** (owner 2026-10-25).
  - **Data:** `replay_data.cpp` follows `map_explorer/recording-data.js`:
    - samples in file order, positions untouched;
    - the same gap rule (500 ms / 2000 u);
    - the same box defaults, and the same interpolation.
    - Facing is blended without trigonometry (normalised blend), which matches at the recorder's ~30 ms steps.
    - Unit-tested on the reference recording against numbers computed with the site's rule.
  - **Colours follow the owner's spec, not the site's:**
    - one run fades red → blue → green over its duration (the site uses a violet → red hue ramp);
    - several on a map are ranked by duration, green (fastest) → blue → red (slowest). The site colours by upload order.
  - **Drawing:**
    - `gfx::SubmitWorldLines` draws each segment as a camera-facing strip, with the world boxes' tiles, layers and per-point colours. The glow is a wide 25% strip under each line.
    - Measured on the reference run at 64x (the whole trace drawn, x-ray): frames 0.997 of the trace-cleared rate.
    - The owner allowed decimating to points every 0.25–0.5 s if needed. It wasn't.
  - **Rejected:** additive blending (no blend mode exposed by the poly API the core uses); decimating the data (both sides must draw the same samples).
- **D48 Game tests share sessions; launch and exit only when a session can't be reused** (owner 2026-10-25).
  - **What:**
    - `tools/harness/fdh/session.py` groups tests by what's fixed at launch: the build, the game cfg, files read at load (.asi, the address override, autoexec.cfg), env switches, a restart, and the Training setup. One game runs per group.
    - Before each later test, the core is reset to the defaults docgen reads from the source, the test's `FDNYCore.cfg` is exec'd, and Training is reloaded.
    - In a session a test sees only its own `FDNYCore.log` lines plus the session's startup, so an assertion can't pass on another test's output.
    - `own_session: true` opts a test out.
  - **The `loadmap: <map>` step** waits for the core's `level_load` event instead of a fixed 25–45 s.
  - **Measured, 2026-10-25 (34 tests):** 15 launches instead of 34. The suite took 1577 s, against about 2900 s before. A reload in a session takes about 4 s, against about 40 s for a launch and load.
  - **Rejected:** a core reset command. The harness resets through the console, and a default key re-bound to its default line isn't written to `FDNYCore.cfg`.
- **D47 Unit tests: core logic built for the host, run without the game** (owner 2026-10-24: "go 1-5", item 1 of the suite review).
  - **What:**
    - `tests/unit/run.ps1` builds `build/unit/fdcore_unit.exe`, an x86 host program, from chosen core sources with `-DFDCORE_UNIT`. `stubs.cpp` stands in for `log`.
    - It runs the C++ tests, then every `tests/unit/test_*.py` (`python -m unittest`).
    - CI runs it on every push and PR (`.github/workflows/unit.yml`). It takes seconds without the game's maps and about a minute with them (VM).
  - **First tests:**
    - **`addr`:** fake exe/Genesis images built from the table's own signatures, covering version by hash, mismatches, Genesis relocation, patch sites taken by another mod, and pointer chains.
    - **Warp HUD parity:** the replay driver moved to `glitch_replay.cpp`, shared with the game's Debug pipe so both run the same code. Its output is compared with the original Python overlay over seeded fuzz streams.
    - **`arch_check`:** each rule has a test that seeds one violation of it.
    - **`docgen`:** the release-notes section the workflow publishes (now `docgen.py notes`; a missing section stops the release) and the sheet.
  - **Rules:**
    - A logic change that a unit test can cover gets one there. Game-driven tests (`tests/game`) prove the behaviour in the game.
    - Production code changes for unit tests only through `#ifdef FDCORE_UNIT` hooks such as `addr::ResetForUnitTest`.
    - Each unit test was checked to fail when the logic it guards is broken: relocation tolerance off, one log label changed.
  - **Rejected:** a test framework dependency (a 30-line `unit.h` is enough, and the code under test is freestanding); unit-testing through the game's Debug pipe (that's the 60 s per test this replaces).
- **D46 Cost tests compare against a baseline in the same run, not a fixed frame rate** (owner 2026-10-24: "Do the tests need to be fps dependent? ... can they be redesigned?", then "implement").
  - **Why:** the game is locked to the display's refresh rate: about 57 fps on the VM console, 34 on a 32 Hz remote display session. `glitch_box_cost` (>= 250 ticks in 6 s) and `text_panels` (>= 300 frames) passed or failed with the session, whatever the code did (`docs/evidence/fps_independent_tests/`).
  - **What:**
    - feature on >= 0.9 x feature off, in 6 s windows back to back (`assert_sample_ratio`);
    - CPU-only costs under a budget (the warp HUD's box build, `draw_us_avg` <= 1 ms);
    - a floor of 60 frames per window (the game runs).
  - **The HUD layer's own time:** it's recorded (Debug pipe `textdraw_cost`), not asserted. It includes `geEngine_FlushPipeline`, which can wait on the GPU, so it moves with the display too (about 2.5 ms per frame at 34 fps, with no frame-rate cost).
  - **Limit:** under a refresh cap, a cost that fits in the frame's spare time doesn't show in the ratio; the regression these tests exist for (Printf boxes, ratio 0.24) is far beyond that.
  - **Rejected:** keeping the absolute thresholds and pinning the session to the console (`tscon`), which disconnects the owner's viewer and stays fragile; lowering them to fit 34 fps (weaker, and the next display mode breaks them again).
- **D21 Tests are never weakened or deleted to pass.** An environmental failure (slow VM frame rate, a race in a read) is rerun and reported with evidence, for example the previous version failing identically.
- **D22 Spikes don't touch working code:** standalone probes (`probes/`) and probe tests (`tests/game/probes/`, outside the suite). Once the result is written up in `docs/evidence/`, the probe is deleted (owner 2026-10-27: the repo holds only what the tooling needs); git history keeps it.
- **D23 Patterns live in the repo, enforced where possible** (this file, `CLAUDE.md`, `tools/arch_check.py`, the `new-feature` skill). Session memory is machine-local and doesn't transfer.
