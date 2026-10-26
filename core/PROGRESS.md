# FDNYCore progress

Spec: `core/LOOP.md`. One line per iteration in the log; findings,
assumptions and workarounds in their sections. Newest log entry last.

**Encoding rule:** edit this file (and every UTF-8 doc) with the Edit/Write
tools, never a PowerShell 5.1 `Get-Content`/`Set-Content` round trip. PS 5.1
reads UTF-8 without a BOM as ANSI and re-encodes every non-ASCII character
on write. This file got mojibake that way on 2026-10-21, and it is the likely
cause of RESEARCH.md growing to 465 MB.

## Status

| # | Item | State |
|---|---|---|
| 1 | Core skeleton + harness + foundation research | **DONE 2026-10-21**: live-tested, regression suite green, committed |
| — | Bind feasibility check (`bind <key> <command>`), owner request 2026-10-21 | **DONE (research)**: feasible with constraints, see "Bind feasibility" |
| 2 | Address table + version check | **DONE 2026-10-21**: live-tested, suite green, committed |
| 3 | Hook manager + events | **DONE 2026-10-21**: live-tested, suite green, committed |
| 4 | Console (on the game's console) | **DONE 2026-10-21**: live-tested in player config, suite green, committed |
| 5 | Variables, config, binds | **DONE 2026-10-21**: live-tested, suite green (11/11), committed |
| 6 | fov module | **DONE 2026-10-21**: parity with the original fix measured, suite green (13/13), committed |
| 7 | window module | **DONE 2026-10-21**: alttabfix parity (same 5-cycle script), suite green (15/15), committed |
| 8 | speedrun module | **DONE 2026-10-21**: FDNYDeterministic proven live, then exact parity; suite green (18/18), committed. On-screen bot check substituted by memory evidence (see item 8) |
| 9 | Game state snapshot | **DONE 2026-10-21**: snapshot == direct reads (0 mismatches), suite green (20/20), committed. Also fixed a SafeRead race crash, added fd_skip_intro |
| 10 | hud_player | **DONE 2026-10-21**: every old-overlay field in-game via geEngine_Printf; drawn == same-frame values (also moving) and == memory at rest; suite green (21/21), committed |
| 11 | hud_glitch | **DONE 2026-10-21**: record layer, analysis, warp window, boxes + keys; log and per-frame box text identical to the original overlay (live + synthetic), suite green, committed |
| 12 | Profiles + watermark | **DONE 2026-10-21**: run/practice sets, refusals, in-frame watermark, persistence; suite green, committed. Practice-only list = agent default, owner to confirm |
| 13 | Teleporter (save location / teleport, map-gated), owner request 2026-10-21 | **DONE 2026-10-21**: fd_tp_save / fd_tp_load, map-gated, practice-only, bindable; suite green, committed |

**Mid-item:** no.

## Environment facts (verified)

- 2026-10-21: Claude Code runs in interactive session 1 (`console`, Active), so GUI apps can be launched directly. `schtasks /it` is only needed from non-interactive shells.
- Exe SHA-256 `4b3b4dc1…d2fc3e`, Genesis.dll SHA-256 `5014bb87…6c4643` (full hashes in LOOP.md / `addresses.json`). Exe base 0x00400000, Genesis 0x10000000 in every run so far.
- The game folder already has:
  - Ultimate ASI Loader (`dinput8.dll`)
  - dgVoodoo (D3D8/D3D9/DDraw/D3DImm + `dgVoodoo.conf`)
  - FOV fix and alttabfix
  - LiveSplit and 2 autosplitters

  Game cfg: D3D via dgVoodoo, 640x480.
- **Rendering works in the VM.** dgVoodoo switched to windowed (`FullScreenMode = false`, `[DirectX] Resolution = unforced`; original in `core/backup/dgVoodoo.conf`). Game window is a 640x480 client area on a 1024x768 desktop.
- **Audio was the real blocker.** The VM has no sound device, and the game fatal-errors at startup with `TSoundEngine::open(): Error opening sound engine`.
  - Fixed with the VB-CABLE virtual audio driver (`get_vbcable.ps1`/`install_vbcable.ps1`). Its install also sets Audiosrv/AudioEndpointBuilder to Automatic.
- **alttabfix.asi had never loaded on the VM.**
  - It links MSVCP140/VCRUNTIME140, and the VC++ runtime was missing: ASI Loader dialog "Unable to load alttabfix.asi. Error: 126".
  - Fixed by installing the VC++ 2015-2022 x86 redist (`get_vcredist.ps1`/`install_vcredist.ps1`). VM-only; FDNYCore imports only kernel32/user32/bcrypt.
- ffmpeg is pinned to BtbN autobuild-2026-09-30-13-08 (`get_ffmpeg.ps1`/`install_ffmpeg.ps1`); gdigrab is available.
- Harness Python deps are in `config.ps1` `PythonPipPackages`: mss, Pillow, pydirectinput, PyYAML.
- **Host follow-up:** `provision.ps1` isn't on the VM copy. The host must add get/install for vbcable, vcredist and ffmpeg (and python, from an earlier session).
- The uploaded FOV fix `.asi` and `alttabfix.asi` are byte-identical to the installed ones. The owner's FOV ini (640x480) differs from the installed one (1920x1080); the owner's is the reference (`reference/original_tools/`).
- **Game console needs `ImADeveloper = true`** in `FDNYFireFighter.cfg`. Without it, the console object (`0x00502A90`) is never created, at the menu or in a level.
  - With it, backtick opens the console in typing mode (state 2) in-level, and typed lines run script verbs (`listmaps` printed all 18 maps).
  - The flag also skips the intro movies (WinMain plays ActiValue/Mekada only when it's 0).
  - Set in the game folder's cfg since 2026-10-21 02:06; original in `core/backup/FDNYFireFighter.cfg`.
- **Front end input:**
  - The splash advances on any LMB.
  - The menus draw their own cursor, driven by DirectInput mouse deltas (1:1, clamped to the screen); absolute cursor moves are ignored. The harness `gclick` slams to the top-left corner, then moves and clicks.
  - Path to Training: splash → `game` (97,457) → career slot "Hotrod Stein" (195,165) → briefing `GO!` (568,450) → about 20 s load (`harness/setups/load_training.yaml`).
- **Movement keys:**
  - W moves forward; S/A/D are presumably back and strafe (route uses them, not yet individually verified).
  - Mouse X turns the view.
  - `player.state` toggles 3/4 with C / Ctrl.
- **Game clock:**
  - The game is hard-capped at 60 FPS: `GameClock::ComputeFrameDelta` 0x0042BF70 busy-waits with Sleep(1) while dt <= 1/60.
  - Median measured fps in the VM is about 55-57.
  - The focus-loss pause is `GameClock::Pause` 0x0042BF20, called from `MainWindow::RunMessageLoop` on WM_ACTIVATEAPP(wParam=0). This resolves R2's "WndProc ignores WM_ACTIVATEAPP" conflict: it's handled in the message loop, not the WndProc.

## Research (item 1: R1-R5)

### R1/R4 Genesis exports + exe imports (Haiku PE parse, `core/harness/pe_dump.py`)

Full lists: `core/notes/genesis_exports.txt` (536 named exports, no forwarders) and `core/notes/exe_imports.txt` (291 imports, 9 DLLs). Stdcall exports are decorated (`@name@N`); cdecl ones are bare. Hook these with GetProcAddress by their exact decorated name.

- **Frame:**
  - `geEngine_BeginFrame` (RVA 0x25B70), `geEngine_RenderWorld` (0x24950), `geEngine_EndFrame` (0x260E0).
  - The game's per-frame render is `ROCRender::RenderFrame` 0x00494670. It runs BeginFrame → draw entries → `Console::Render` → FlushPipeline → EndFrame, and it skips everything while `gameState == 2`.
  - The render is called from `GameSession::RunFrame` 0x0042AEA0, after `GameClock::Tick`. The menu/loading path (`MenuManager::TeardownToLoading`) also calls Begin/EndFrame.
- **In-frame drawing (piggyback):** the exe already imports `geEngine_Printf` (0x24B60) and `geEngine_DrawBitmap` / `FillRect`. `geFont_*` is a fallback.
- **Camera/FOV:** there's no SetFov export; FOV goes only through `@geCamera_SetAttributes@12` (0x1350). The FOV fix patches Genesis.dll+0x1383, which is inside SetAttributes.
- **Level load:** candidates are `geWorld_Create` (0x3E810) and `geEngine_AddWorld` (0x243D0) / `RemoveWorld`.
- **Input:** Genesis has no input exports. The exe uses DirectInput8 + the WM pump (see R2).

### R3 Existing console (Haiku Ghidra passes; confirmed live 2026-10-21)

- **Object:** `main\console.cpp`, instance global `0x00502A90` (Console ctor 0x00427910, `DevConsole::Shutdown` 0x00427BD0). It is only created with `ImADeveloper = true`.
- **Functions:**
  - `OnChar` 0x004274F0, `OnKeyDown` 0x00428C50, `AddLine` 0x004286E0
  - `Print` 0x00428930, `WatchPrint` 0x00429100
  - `AddHistory` 0x00428A20, `AddCompletionCandidate` 0x00428970
  - `LoadFont` 0x004275A0 (`data\sprites\confont.bmp`), `Render` 0x00427F00, `SetState` 0x004274B0
- **State `[con+0x49C]`:** 0 closed, 1 view-only (Tab), 2 typing (backtick/tilde via WM_CHAR, live-confirmed). Esc closes.
  - `SetState` also writes `[[0x503A78]+0x330] = (state==2)`. That's probably the game's input-block flag; not yet verified.
- **Enter:** echoes `--> line`, then the line goes to the object at `0x00502FB0` (`+0x78`, via 0x00470F10 / 0x00475310). It's a deferred submit, consumed by the script engine; live, `listmaps` worked.
- **Script verbs:** a string-compare chain in `ScriptTrigger::HandleVerb` 0x0047A1F0 with about 60 `Verb_*` handlers, incl.:
  - `Verb_LoadMap` 0x00478150 (`loadmap`)
  - `Verb_ListMaps` 0x00479670
  - `Verb_Exec` 0x0047A070
  - `Verb_Set` 0x004750E0
- **Swallowing:** WM_KEYDOWN reaches the game (0x00499000) only while `[con+0x49C]==0`.
- **Config table** at 0x004FF524:
  - `ImADeveloper` 0x00502A78
  - `EnableDebugLogging` 0x00502A79
  - `SkipMenus` 0x00502A7A. Useless for the harness: it fires 3 no-op activations at the splash. There are no command-line switches and no start-map cfg key.
- **Piggyback plan for item 4:** intercept the submitted line, route `fd_*` / `echo` / `cvarlist` / `exec` to our registry, and print via `Console::AddLine`. Players need the console object to exist. The core must either set the `ImADeveloper` byte before console creation or create the console itself; that's a decision for item 4. Mind the side effects: the intro movies get skipped and the debug message feed turns on.

### R2 Input path (Haiku Ghidra pass)

Input reaches the game by two parallel paths:

1. **DirectInput 8, buffered.**
   - `GameControls::PollFrame` 0x00439330 → source poll 0x004394A0 → `DrainMouseBuffer` 0x00437AB0 / `DrainKeyboardBuffer` 0x00437C80. PollFrame then calls `SetCursorPos(200,200)`.
   - Key records sit at provider+0x16+3*DIK; the action layer is `GameControls::IsHeld/WasPressed/WasReleased/GetAxis`.
2. **Window messages.**
   - `MainWindow::WndProc` 0x004996C0: WM_KEYDOWN goes to 0x00498FC0 (console first, then the game only if the console is closed). WM_CHAR goes to the console only. Mouse messages go to 0x004990D0.

**Swallow plan:** skip the poll and zero the provider state while our UI is open, and filter WM input at WndProc. The game's own console already blocks the WM key path.

### R5 Autosplitter memory paths (Haiku read of both .asl)

Both `.asl` files share one `state("FDNYFirefighter")` block: 12 vars, all main-exe-relative, no sigscans. Two static roots:

| var | type | chain | used by |
|---|---|---|---|
| gameState | int | `[0x00502AAC]+0x6F0` | start (==1), split gate, ILs reset (==0), isLoading (==2/3) |
| mapName | string10 | `[[[0x00502AAC]+0x688]]+0xC` | splits |
| totalVictims / rescuedVictims | int | `[[0x00503A78]+0x30]+0x3528/+0x352C` | declared, unused |
| 8x level-end sound flags | int | `[[[[0x00503A78]+8]]+off]` | splits (0 → non-zero) |

Live 2026-10-21: in Training, gameState = 1 and mapName = `training`. All 12 chains resolve; their final (heap) addresses differ run to run even without the core.

## Bind feasibility (owner request; Haiku Ghidra pass 2026-10-21; not yet live-verified)

**Verdict: feasible, with constraints.** `bind <key> <command>` can't piggyback as-is, because the game has no bind verb, no key-name table, and no "key runs a line" mechanism. The plumbing it needs does exist and is reusable. Item 5 builds the binding store and the verb.

- **Every console line is a script line.** `Console::OnKeyDown` (Enter, 0x00428FD3) calls `ScriptEngine::ExecLine(g_scriptEngine, line)` 0x00470F10, and that is ExecLine's **only** caller.
  - So the single interception point for **all** our console commands (item 4) is a pre-hook on ExecLine: handle `fd_*` / `bind` / `unbind` / `echo` / `cvarlist` / `exec`-of-our-cfg ourselves, pass everything else through.
  - A bound key can run a line by calling the same ExecLine with the bound text, so game verbs and `fd_` commands both work from binds.
- **Verbs in the game:**
  - `ScriptTrigger::HandleVerb` 0x0047A1F0: if/ifnot/set/inc/dec/print/once/ignore/exec/execsetsonly/sendmsg/fog*/*snd*/hint/quake/if-comparisons/listmaps/loadmap/loadhosemap/setlighttable/difficulty/quit/debugvictims/runrescuebot/listvars.
  - `ScriptActor::HandleBaseVerb` 0x0046F9D0: que/flush/wait/idle/cmd/loopinque/onmsg/onmsgappend/printmsgs.
  - There's no bind/unbind/alias. Names to avoid colliding with: `exec` is a game verb (it loads Data\Scripts / Data\Maps), so our config exec should be `fd_exec`, or `exec` should fall back to the game verb when the file isn't ours. **Decide in item 4/5.**
- **Default controls are hardcoded:** 26 `ActionMap::BindKey` 0x00437410 calls inside `GameControls::Construct` 0x004395C0, with keys as raw **DIK** codes. Mouse uses custom codes 0x101-0x105 (ANY_KEY, ANY_MOUSE_BUTTON, MOUSE_LB/MB/RB).
  - Movement: W/S/A/D + arrows.
  - CROUCH = C / LCtrl / RCtrl.
  - **USE = LShift / RShift / Space**, ACTION = mouse LB, **JUMP = mouse RB**.
  - SELECT_* = 1-4.
  - Dev keys: F2 / F3 / F4 / F6 (SKIP_HOTSPOT and ACTIVATE_RESCUEBOT share code 0x40 in the decompile) / F7 spectator / F8 WIN_LEVEL / F11 CRASH_TEST.
  - `ActionMap::BindAction` 0x00438BF0 asserts "Invalid binding" for an unregistered name; new actions need `RegisterAction` 0x00438D70 first.
- **Constraints for item 5:**
  1. We ship our own key-name → DIK table (incl. mouse4/5 and wheel).
  2. Bound-key detection reads DIK state from the DirectInput keyboard buffer (`DrainKeyboardBuffer` 0x00437C80 state) or our own WM_KEYDOWN filter. Binds must not fire while the console is open (state != 0).
  3. A bind on a key the game already uses (W, Space, ...) runs **both** our line and the game action, unless we swallow it. Default: don't swallow, and warn in the console when binding a game-used key.
  4. Backtick/tilde stays reserved for the console.
  5. Binds persist in the core's own cfg (written by `fd_writeconfig` / on exit), not the game's `FDNYFireFighter.cfg`, which the game rewrites at startup and exit.
- **Harness note:** my `baseline_route` presses Space, which is USE, not jump. Jump is mouse RB. Not a problem for equivalence, since both runs do the same; noted for future routes.

## Build / harness notes

- **`core/build.ps1`:** zig 0.16 freestanding x86 (no CRT; kernel32/user32/bcrypt), `-Wall -Wextra -Werror`.
  - Debug adds `-O0 -g -fno-sanitize=all -DFDCORE_DEBUG`, because zig turns UBSan on at -O0 and a no-CRT DLL has no sanitizer runtime.
  - `-Wno-unused-command-line-argument` is needed because zig injects `-fno-rtlib-defaultlib`.
  - Zig emits a PDB per config. Preferred base 0x3D000000, relocatable.
- **Core startup:**
  - DllMain opens `FDNYCore.log` next to the .asi and spawns an init thread.
  - The thread waits for Genesis.dll, SHA-256s the exe and Genesis (BCrypt), then logs the version line and `FDNYCore ready`, about 0.1-1.3 s after load.
- **Harness flow** (`core/harness/run_test.py` + `fdh/`):
  1. Launch and wait for the window.
  2. **SetForegroundWindow**: screenshots and video grab a screen region, and input goes to the foreground window.
  3. Wait for the ready line, attach pymem, and pick the address-table version by exe+Genesis hash.
  4. Record `run.mp4` with ffmpeg gdigrab.
  5. Run the steps, with a crash check on every step: exit, a `#32770` dialog owned by the game, or a SendMessageTimeout hang > 10 s.
  6. Kill the game and write `result.json`.

  `--core none` renames the core to `.asi.off` for baseline runs. `--suite` runs `core/tests/*.yaml` (not `tests/explore/`).
- **Step types:** `wait_s`, `key`, `type`, `click`, `gclick`, `mouse_rel`, `lclick`, `focus`, `screenshot`, `assert_screen_nonblack`, `assert_mem`, `sample_mem` (+`append`), `sample_addr`, `sample_pos`, `assert_log`, `repeat`, `console` (the console step still needs `ImADeveloper`; it becomes real in item 4).
- **Address table:** `core/addresses.json`, JSON so the build-side generator and the Python harness need no extra deps. The harness reads it with `utf-8-sig`, because PowerShell 5.1 writes a BOM.
  - It holds the 12 autosplitter chains, console globals, player position/ground/state (`[[0x503A78]+0x30]` + 0x4C/0x12C/0x18C, from the collision overlay) and `g_gameClock` 0x00502AB8 fields (`deref_root: false`: +0x44 elapsed ms, +0x4C dt float, +0x50 fps **float**, verified live as 60.0 under the cap).
  - `sig` fields are filled in item 2.
- **`harness/compare_runs.py`:** item 1(c) baseline equivalence. Two no-core runs define the noise envelope, and the core run must sit inside it.

## Item 2: address table + version check (done)

- **Signatures.** `core/tools/addrgen.py sigs <exe>` finds, for each exe entry's static root, the first code instruction that embeds that root as an absolute operand. It stores the instruction plus 6 context bytes as the entry's `sig`: `{rva, bytes, ref, ghidra}`.
  - This works because the exe has no relocations and is always at 0x400000. Matching bytes in memory prove the global is where the table says.
  - Five distinct sites cover all 23 entries; each is confirmed in Ghidra as an xref to its global (Haiku pass):

    | site | instruction | function |
    |---|---|---|
    | 0x0042465A | `MOV ECX,[0x502AAC]` | TPlayerInfo::RecordLevelComplete |
    | 0x004274C2 | `MOV EDX,[0x503A78]` | FUN_00427420 |
    | 0x00427920 | `MOV [0x502A90],ESI` | (see below) |
    | 0x0042AEC0 | `MOV ECX,0x502AB8` | GameSession::RunFrame |
    | 0x004220BE | `MOV AL,[0x502A78]` | SArray::OobFail = the debug-message dispatcher |

  - **Decomp follow-up (not ours to fix here):** Ghidra places 0x00427920 in `PlayerRescueEnt::Init @0x004277D0`, but the R3 pass called it the console ctor at 0x00427910. That's a function-boundary labelling question for the decomp loop.
- **Compiled-in table.** `build.ps1` runs `addrgen.py header` to produce `src/gen/addresses_gen.h` (gitignored, regenerated every build): an `Id` enum plus `kVersions[]`. Python is a build-time tool on the VM only; players get the table inside the .asi.
- **Startup (`src/addr.cpp`):**
  1. Select the version by exe + Genesis SHA-256.
  2. Compare each entry's signature bytes in memory via `SafeRead`, which uses VirtualQuery and never faults.
  3. Mark each entry usable or not.
  4. Unknown version → one WARN, and every module disabled.
- **Modules (`src/modules.cpp`):** each module declares required `Id`s. `StartAll` enables a module only if the version is known and all its entries verified, and logs `module X: enabled` / `disabled (unknown version)` / `disabled (needs <entry>)`.
  - The first module is `levelwatch`: it logs map/state changes, polling at 4 Hz from its own thread until item 3's frame events exist. Live, it shows state −1 → 0 (menu) → 3 (loading) → 1 (in level), the seed of the level-load event.
- **Debug-only override:** `FDNYCore.addroverride.txt` next to the .asi, in the `addrgen.py text` flat format. Tests feed patched tables through it without touching the exe.
  - Fixtures come from `tools/make_override_fixtures.py`: `tests/data/addr_override_unknown.txt` (hashes zeroed) and `addr_override_badsig.txt` (first sig byte of `game.map_name` flipped).
  - The harness places them via the new `pre_files` test key and removes them after the run.
  - Verified absent from Release: no `addroverride` string in the Release .asi.
- **Tests:** `addr_verified`, `addr_unknown_exe`, `addr_bad_signature` (all live, in Training). `core_loads` now expects `FDNYCore 0.2.0`; this is a version bump, not a weakened check.
- **Deviation:** I didn't run the new tests red before implementing; the item-1 binary had already been rebuilt. Each one asserts log lines 0.1.0 never emitted, so all would have failed on their first assertion.

## Item 3: hook manager + events (done)

- **Hook manager (`src/hooks.cpp`).** MinHook 1.3.3 is compiled with `zig cc` (third-party, no `-Werror`) and statically linked. Both builds still import only KERNEL32/USER32/bcrypt.
- **Frame hooks.** `geEngine_BeginFrame` / `geEngine_EndFrame` are resolved **by export name**. They're in the address table as Genesis entries `genesis.begin_frame` (RVA 0x25B70) and `genesis.end_frame` (0x260E0).
  - The signature is the first 16 prologue bytes, plus a cross-check that GetProcAddress == base+rva.
  - Both are **cdecl**: undecorated exports, and the exe cleans the stack after both call sites (`add esp,4` after EndFrame at 0x004949A6).
  - The two hooks toggle together via `MH_QueueEnable/Disable` + `MH_ApplyQueued`, so a frame never sees one hooked and not the other.
- **Events.**
  - `frame_end` is dispatched inside the EndFrame detour **before** the original runs, so later in-frame drawing lands before the flip.
  - `level_load(map)` is raised by `levelwatch` (now a frame_end subscriber, no thread) when state reaches 1 with a map name. Live: `event: level_load 'training'`.
  - `victim_spawn` arrives with item 8.
- **Debug-only test pipe `\\.\pipe\FDNYCore`** (`src/testpipe.cpp`, wholly `#ifdef FDCORE_DEBUG`; verified absent from Release). Commands: `ping`, `stats` (begin/end/events/levels/toggles counters + frame on/off), `hook frame on|off`. Harness client: `fdh/pipe.py`.
- **Independent frame count.** The game has no frame counter, but `g_gameClock`'s published elapsed-ms (+0x44) changes once per `GameSession::RunFrame`. In a level each RunFrame renders exactly one Begin/End pair. The harness step `assert_frame_hooks` polls it in a tight loop and compares against the core's counters.
- **Results:**
  - `hooks_frame`: clock ticks = BeginFrame = EndFrame = frame_end events = **287/287/287/287** in 5 s (and 286 again while moving).
  - `hooks_toggle`: 50× off/on with 0 failures and no crash. Afterwards hooks fire once per frame again (286, 172). While off, all deltas stay 0 for 2 s.
  - Tolerance ±2 is for window-edge races only; actual differences were 0.
- **Red run first:** `hooks_frame` against the item-2 core (`git stash` of the new sources) failed for the right reasons: no hook log lines, and no test pipe.
- **Version single-sourced:** `core/VERSION` → `build.ps1` generates `src/gen/version_gen.h`, and the harness substitutes `{core_version}` in `assert_log`. Without the generated header, `core.cpp` refuses to compile.
  - This fixed `core_loads`, which first failed in the suite after the 0.3.0 bump. Its check is unchanged: it still matches the exact running version.
- **Release 0.3.0, hooking every frame**, is still EQUIVALENT to the item-1 baselines (fps 58.8 vs 55.6, laps within 3.5 u).

## Item 4: console (done)

- **It's the game's console.** FDNYCore doesn't draw its own: it makes sure the game's developer console exists for every player, intercepts typed lines, and prints into it. The console is drawn by the game's own `Console::Render`, inside RenderFrame, so it shows on any driver (screenshot `evidence/item04/02_console_exec.png`).
- **Console without ImADeveloper (decision, reversible).** The game creates the console only when `ImADeveloper` is set: in `roc_CreateEngineSubsystems` 0x004992D0, `JZ` at 0x00499370 skips operator new(0x4A0) + ctor 0x004277D0(640, 460) + `MOV [MainWindow+0x70],EAX`.
  - Forcing the flag would turn on about 16 debug paths: diagnostic spam into the console, dev keys, F12, a 0xDEADBEEF sentinel write per frame, extra debug HUD objects, and RescueBot/hose debug text.
  - So the core NOPs **only that JZ**, in memory, from DllMain, which runs before WinMain (verified live: `g_mainWindow` and `g_console` are still null there). The game then creates its console itself at the normal moment.
  - `addr::EarlyCodeAddress` verifies the patch site's signature before writing (no version selection yet at DllMain). `addr::Init` later counts the pre-patch verification, so it doesn't report the patched bytes as a mismatch.
- **Rejected approach (documented failure).** Calling the ctor ourselves at the first frame crashed: AV at 0x0047EAC8 reading `[g_everGame 0x00503A78]+4`.
  - Some frames render **before** WinMain reaches `roc_CreateEngineSubsystems`, which is also where `g_everGame` (the root) is allocated, at 0x0042CD65.
  - The ctor depends on that root, and also on root+4 (the 0x17C-byte subsystem, 0x00495190).
  - Evidence: `evidence/item04/attempt2_lazy_ctor_crash.FDNYCore.log`. The new crashlog (below) located it without a debugger.
- **Interception (`src/console.cpp`).** MinHook on `ScriptEngine::ExecLine` 0x00470F10, group `console`. It's thiscall, takes the game `String` by value, and the callee releases it; its only caller is the console's Enter handler, after the game has already echoed `--> line` and before it adds history.
  - Our lines (`cmd::Dispatch`) are handled and the handle released the way ExecLine does: decrement, then `String::FreeRep` 0x004011C0 at <= 0. The game never sees them.
  - Everything else is passed to the original untouched, so game verbs (`listmaps`, `loadmap`, ...) keep working.
  - A mistyped `fd_*` is answered "unknown command" and never reaches the game.
- **Output.** `console::Print` builds a game String with 0x00402790 and calls `Console::AddLine` 0x004286E0 on `[MainWindow+0x70]`. This is `Console::Print` 0x00428930 minus its ImADeveloper gate. Every line is mirrored to FDNYCore.log as `console> ...` for tests.
- **Commands / vars (`src/cmd.cpp`).**
  - `echo`, `cvarlist`, `cmdlist`, `exec`.
  - `fd_version` (read-only) and `fd_hooks_frame` (toggles the frame hook group).
- **`exec` vs the game's `exec` verb.** `exec <name>` runs `<name>` / `<name>.cfg` from the game folder if that file exists. Otherwise the line goes to the game's own `exec` (Data\Scripts / Data\Maps).
  - Lines in our cfg that aren't ours are queued and handed to the game **one per frame**, because its script engine keeps a single pending line (`+0x78`).
- **Keystrokes don't reach the game while the console is open.** Live: W held 1.5 s with the console open → player moved **0.0 u**; console closed → 258 u. That covers both input paths:
  - The WM key path is blocked natively by `[con+0x49C] != 0`.
  - The DirectInput path is also blocked natively. This is presumably `Console::SetState`'s `[[0x503A78]+0x330]` flag; the live result shows the effect.
- **Crash reporter (`src/crashlog.cpp`).** A vectored exception handler logs AV/illegal/div0/stack-overflow first-chance exceptions with module+offset, registers, and the code addresses on the stack (≤ 8 reports per run). It is log-only and never changes the game's handling.
- **Address table:** +11 entries, 35 total.
  - Code entries (signature = first 16 bytes): `con.exec_line`, `con.add_line`, `str.from_cstr`, `str.free_rep`, `con.ctor`, `crt.operator_new`, `patch.console_gate`.
  - Data entries: `str.empty_rep`, `script.engine`, `mainwindow.console` (`[g_mainWindow 0x00503A70]+0x70`).
- **Game cfg restored** to the owner's original (`ImADeveloper = false`). The whole suite now runs in the player configuration. `console_basic` stages that cfg itself through `pre_files`, which now backs up and restores a replaced game file.
- **Red first:** `console_basic` against the item-3 core failed for the right reasons (no console, no output, W moved the player 344 u).

## Item 5: variables, config, binds (done)

- **Built (0.5.0):**
  - Archived cvars (`archive` flag) and binds are persisted to `FDNYCore.cfg` in the game folder, rewritten on every change (the game can be killed at any time); `fd_writeconfig` forces a write.
  - At startup, on the game thread once the console exists: `FDNYCore.cfg` runs first (no rewrite while loading), then `autoexec.cfg`. First archived var: `fd_log_commands` (default 1).
  - `bind`/`unbind`/`unbindall`/`bindlist` (`src/binds.cpp`), per the feasibility notes:
    - own key-name table (VK codes);
    - edge-triggered polling with `GetAsyncKeyState` on frame_begin, only while the game window is foreground and the console is closed;
    - backtick reserved; binding a game key warns but doesn't swallow;
    - bound lines go through `cmd::Dispatch`, or to the game one per frame.
- **Live so far:**
  - `autoexec` PASS (autoexec ran at startup; its F6 bind fired once).
  - `vars_binds_persist`: everything up to the restart passed. After the restart, `fd_log_commands 0` was in effect (no `cmd:` lines), F5 still fired from the persisted bind, and `FDNYCore.cfg` loaded. Then the test opened the console **at the main menu**, and the game raised FATAL ERROR "Console font texture not loaded".
- **Native game bug, confirmed:** the unmodified game (no core, `ImADeveloper = true`) raises the same fatal error when the console is opened at the main menu (`runs/...-explore_menu_console_native`). The developers evidently only used the console in a level. FDNYCore exposes the console to every player, so it must make the menu safe.
- **Fix (font guard, `console.cpp` GuardFont).**
  - **Why it happens** (Haiku Ghidra pass): the font is registered only during world load (`ROCRender::LoadWorldGeometry` 0x00495210 → `Console::LoadFont` 0x004275A0, its only caller). `Console::Render` checks it via the texture lookup 0x00493D00 (ECX = `[g_everGame]+4`, id = `[console+0x108]`) **only while the console is open**.
  - **The guard:** Render runs after `geEngine_BeginFrame` inside `RenderFrame`, so the core's frame-begin hook, if the console is open and the same lookup says the font isn't registered:
    - closes it again (`[con+0x49C] = 0`);
    - clears the game's input-block byte `[[g_everGame]+0]+0x330`, which the open path set (GameControls object, ctor 0x004395C0);
    - prints "console: not available until a level has loaded".
  - **Rejected alternative:** calling LoadFont at the menu, because the device may be null there (placeholder texture → later crash risk), and registrations look per-level.
  - **Cost:** no console at the menu, only in a level.
- New address-table entries: `gfx.find_texture`, `everGame.render`, `console.font_id`, `controls.block_input` (39 total).
- **Tests:**
  - `console_menu_safe` (new): backtick and Tab at the menu → state stays 0, input not blocked, refusal printed, menu still works; then in a level the console opens and echoes.
  - `vars_binds_persist`: now checks the refusal at the menu after the restart, then enters Training through the new `run_setup` step to verify the restored values in the console. This adds coverage; no check was dropped.
  - Both PASS.
- **Harness fixes:** `restart` step added (kill + relaunch inside one test); `Game.launch` now clears the stale HWND, which otherwise read as "hung". Also added `assert_file`, `assert_log_count`, `pre_files` with `src: null` (file must be absent, restored after), and F1-F12 scancodes.

## Item 6: fov module (done)

- **Original FOV fix provenance** (Haiku web pass plus my own clone):
  - It's by **AlphaYellow**, not Lyall: PCGamingWiki community file 3287, v1.0. Same toolchain as AlphaYellow's other fixes (inipp, spdlog, safetyhook, Ultimate ASI Loader).
  - AlphaYellow's MIT repo `github.com/alphayellow1/AlphaYellowWidescreenFixes` (shallow clone, 1,952 files) has **no FDNY / Line of Duty fix**, so the source isn't public there. Parity comes from the binary: disassembly of `geCamera_SetAttributes` and of the fix's hook (Haiku, capstone).
  - PCGamingWiki describes the game as Vert- and the fix as uncropped Hor+, with FOV-factor controls.
- **Engine facts** (capstone disassembly of Genesis.dll; Haiku plus my own read):
  - `@geCamera_SetAttributes@12` is fastcall: ECX = geCamera*, EDX = geRect*, Fov on the stack; `ret 4`.
  - It stores Fov raw at `[cam+0]`, Width/Height at +4/+8, and the screen scales `[cam+0x1C] = W/Fov`, `[cam+0x20] = H/Fov`. So **horizontal FOV = 2·atan(Fov/2)**.
  - The exe passes Fov = tan(deg·π/360) from its camera controller (`Camera::SetFov` 0x00491D20).
  - The old fix mid-hooks Genesis+0x1383 (`mov eax,[esp+0x10]`, the Fov load; its pattern is `8B 44 24 10 D8 05 ?? ?? ?? ?? 89 01`).
- **Old fix behaviour, measured live** with an fov-module hook logging Fov in vs stored:
  - Without the fix, stored = the game's value. It changes over loading (2.0 → 1.732 → 2.0), and in Training it is **1.428148 = 71.1° horizontal** (Vert-).
  - With the fix, stored is a **constant**, whatever the game passed: **1.43 × (W/H)/(4/3) × FOVFactor**, with W/H from *its ini*. Data points: 1920×1080 ini, factor 1.4 → 2.669333; 640×480, factor 1.0 → 1.43; owner's 640×480, factor 1.4 → **2.002 = 90.0°**.
  - The installed ini is the stale 1920×1080 one, so the owner's game currently runs at 2.669 = 106°. The owner's reference ini (640×480) is the parity target.
- **fov module (`src/fov.cpp`).** MinHook on `@geCamera_SetAttributes@12` (function entry; no overlap with the old fix's +0x33 mid-hook), group `fov`.
  - **`fd_fov`** (archived, default `0`):
    - `0` = classic, exactly the old fix: 1.43 × aspect/(4/3) × `fd_fov_factor`, with the aspect from the **real** render rect instead of an ini.
    - `N` = N° horizontal at 4:3, Hor+ for wider screens: 2·tan(N·π/360) × aspect/(4/3).
    - `-1` = leave the game's FOV untouched.
  - **`fd_fov_factor`** (archived, default 1.4).
  - **`fd_fovinfo`** shows the game and camera values.
  - Changes apply at once: re-call SetAttributes on the live camera with the game's last rect/FOV.
  - No-CRT math: `fptan` inline asm, plus a small float parser/formatter.
- **Camera address:**
  - `camera.fov` = `[[[[g_everGame 0x00503A78]+0x288]+0x8]+0x5C]+0` (f32). The chain to the camera controller came from a live **pointer scan** (`harness/pointer_scan.py`, from the static roots to the geCamera* the hook logged). Controller+0x5C = geCamera* is Ghidra-confirmed (0x00491BE0).
  - Dead end, documented: Haiku's static chain via `GameSession+0x64` is actually the **entity list** (dozens of elements, `+0x18` = 4/8); the player chain `[[g_everGame]+0x30]+0x7C` is not it either.
  - Bug found and fixed: Reapply first used a camera pointer cached in the detour and cleared on level_load, but level_load fires *after* the camera is configured, so changes never applied. Reapply now resolves the live camera through `camera.fov`.
- **Tests (live):**
  - `fov_parity_reference`: the **original fix** with the owner's ini, core told to leave the FOV alone (`fd_fov -1` via FDNYCore.cfg), gives camera.fov **2.001999855**.
  - `fov_sets_camera`: original fix removed. Default **2.001999855** (parity); `fd_fov -1` → 1.428148 (game stock); `fd_fov 90` → 2.0; `fd_fov 120` → 3.464102; `fd_fov 0` → 2.002, and FDNYCore.cfg holds no fd_fov line again (default).
  - Screenshots: 71° stock vs 120° shows a clearly wider view.
  - Red run against the item-5 core: no fov module, so the assertions fail.
- **Both fixes active at once:** the old fix's mid-hook runs inside the original, so its value wins and the owner's current behaviour is unchanged. The old .asi stays installed until the owner retires it; the profiles item (12) should decide what ships.

## Item 7: window module (alttabfix parity)

- **alttabfix.asi, reverse-engineered** (Haiku, capstone; MSVC, no source).
  - In DllMain it replaces the 16 bytes at `0x00439347` with 11 NOPs + `jmp` to its routine. Those bytes are `push 200; push 200; call [SetCursorPos]` in `GameControls::PollFrame`, which runs every frame, **even when the game isn't focused**: that's the "mouse trapped after alt-tab" bug.
  - Its routine (pushad): if the foreground window is the game window and `GetAsyncKeyState('O') == 0`, `SetCursorPos` to the centre of the window's **outer rect**.
  - A thread finds the window once, via `FindWindowA(NULL, "FDNY Firefighter")`, polling every 100 ms.
  - Nothing else: the clock pause and rendering are untouched.
- **window module (`src/window.cpp`).**
  - **Patch:** at DllMain, the same 16 bytes become 11 NOPs + a 5-byte `call CursorWarp` (stdcall, no args, so the stack is balanced like the original call; the caller ignores EAX/ECX/EDX after SetCursorPos). It's guarded by the signature of the original bytes (`patch.cursor_warp`).
  - **Behaviour:** CursorWarp mirrors alttabfix exactly, 'O' key included. It finds the window by title too, but also requires it to belong to our process.
  - **`fd_window_cursor`** (archived, default 1): 0 = the game's own (200,200) warp.
  - **Coexistence:** if alttabfix (loaded earlier, alphabetically) has already patched the site, the signature doesn't match. The module logs it and stays out, so the two never fight; the owner's current setup behaves as before.
- **Corrected research:** `[g_mainWindow 0x00503A70]+0x74` is **not** the HWND (live it read `03570048`, a heap pointer, while the real HWND was `001E00D6`). The entry was removed. `+0x70` = console is still verified every run by the console self-check.
- **First live run failed.** The cursor was never recentred because of the bad HWND, so the menu `gclick` relative moves walked the OS cursor to the bottom-right corner, and the click there hit "show desktop", minimizing the game. With alttabfix (and now FDNYCore) the cursor is held at the window centre, so this can't happen.
- **Harness:** `alt_tab` (a real Alt+Tab via SendInput), `assert_foreground`, `set_cursor`, `assert_cursor` (point, or `window_centre` from GetWindowRect); the shared script is `setups/alttab_cycles.yaml`.
- **Tests (live):**
  - Red run, game default: the cursor stays trapped at **(200,200)** while away and while back, which demonstrates the bug.
  - `window_parity_reference` (original alttabfix, same script): **PASS**, all 5 cycles.
  - `window_alttab` (alttabfix removed, FDNYCore): **PASS**, all 5 cycles. Away, the cursor stays where the user puts it (900,700); back, it's held at the window centre (330,261); the screen is non-black; afterwards W still moves the player.

- **Suite finding → address-table rule.** In the default game folder (alttabfix installed), `patch.cursor_warp` held alttabfix's detour, so `addr_verified`/`console_basic` failed on "signatures: all OK" (41/42).
  - New rule: a `patch.*` code site whose live bytes start with another mod's detour shape (NOP run, or JMP/CALL rel32) is logged "occupied by another mod". It stays unusable and is excluded from the mismatch count (`all OK (41/41, 1 patch site(s) occupied by other mods)`).
  - Every other byte difference is still a real mismatch. The `addr_bad_signature` test covers a non-patch entry.

## Item 8: speedrun module (FDNYDeterministic parity)

- **Baseline first: `asi/FDNYDeterministic.asi` verified live for the first time** (`speedrun_reference_deterministic`).
  - Setup: skeleton ini (every victim on spot 0), Training loaded 3× (initial + 2× console `loadmap training`, the game's own verb).
  - Both victims at identical positions every time, each on its spot 0: victim1 (1104, −230.9, −4976), victim2 (1656, −230.9, −4968).
  - Negative control without any spawn mod (explore, not in the suite since it's random): positions changed across loads (victim1 1300→1300→1104, victim2 1820→1972→1820).
- **Measurement independent of either implementation.** Victims = entities of kind 2 (`entity.kind` +0x18, per `EndMission` 0x0044FBA0) in the level entity list `[GameSession]+0x64` (node: elem +0, next +4).
  - Fields: `victim.pos` +0x4C, `victim.state` +0x120, `victim.def` +0x150 → name +0x10, `victim.spots` +0x200 / `victim.spot_count` +0x204.
  - These are new **field** entries in the address table (module `field` = struct offset; always usable; the core reads them via `addr::Field()`).
  - Harness steps: `sample_victims`, `assert_victims_equal`, `assert_victims_at_spot0`.
- **RescueBot enabled byte resolved live.**
  - Haiku found two candidates (PRE+0x340C+0x18 from the ctor site vs +0x32CC+0x18 from the Think call site).
  - In `bank` (bot ships ON): stock `[PRE+0x3424]` = 1, with FDNYDeterministic = 0. +0x32E4 stayed 0 in both.
  - So `rescuebot.enabled` = `[[g_everGame]+0x30]+0x3424`. Training reads 0 (its script runs `setrescuebot 0`).
  - research.db's Think note ("+0x3424 blocking-surface SArray") conflicts with this and needs correcting in the decomp loop.
- **speedrun module (`src/speedrun.cpp`)**, a direct port:
  - **Spawn hook:** a byte-assembled trampoline at `patch.victim_spawn` 0x0045D717, installed in DllMain. It runs pushfd/pushad, calls the handler with EBP = victim, then executes the displaced `mov ecx,ebp; call SnapToFloor` and jumps back.
  - **Handler** (same semantics as the original): ordinal reset after a >2 s gap; map from `game.map_name`; copy spot[i] x/y/z before the game's floor snap; RNG untouched.
  - **`fd_static_spawns`** (archived, default 0). **`fd_spawns <map> <spot …>`** pins per map: −1/x = random, `default` resets; with no pins, every victim gets spot 0, exactly the original's shipped skeleton ini. Persisted to FDNYCore.cfg through a new `cmd::RegisterConfigWriter`.
  - **`fd_rescuebot`** (archived, default 1 = as shipped). 0 makes `TryStartRescue` byte 0 `6A→C3`, ctor imm8 `01→00`, and clears the live level's enabled byte. 1 restores the code bytes; the live byte follows the next level load. These are single-byte runtime writes.
- **Coexistence:** if FDNYDeterministic is installed (it hooks/patches the same sites), the spawn site reads as a foreign detour (E9) and the RescueBot sites as 1-byte patches. A new address-table rule treats "only byte 0 differs" as occupied by another mod. The core's features then stay off, so the two never fight.
- **Suite finding → deferral rule.** With both installed, the ASI loader loads plugins **alphabetically**: `FDNYCore.asi` before `FDNYDeterministic.asi`.
  - The core grabbed the spawn site first, FDNYDeterministic aborted its own hook, and the victims went random (the reference test failed in the suite). A player who kept FDNYDeterministic would silently lose static spawns.
  - Fix: if `FDNYDeterministic.asi` or `FDNYVictimSpawns.asi` is in the game folder, the speedrun module defers to it entirely (no hook, no cvars) and logs why. `speedrun_reference_deterministic` now asserts that deferral line.
  - alttabfix loads before the core, and the FOV fix hooks a different spot, so neither had this problem.
- **Tests (live):**
  - `speedrun_static_spawns` (FDNYDeterministic absent, `fd_static_spawns 1`): identical positions across 3 loads, each at spot 0, **the same numbers as the original plugin**.
  - `speedrun_rescuebot`: bank with `fd_rescuebot 0` → enabled 0, claimed 0. `fd_rescuebot 1` + reload → 1 (as shipped). `fd_rescuebot 0` live → 0 at once; claimed 0.
  - Red (item-7 core): bank enabled = 1.
- **Deviation from the done-when "absent on screen":** the research found **no bot body or actor**. RescueBot is invisible logic whose only on-screen trace is the "RESCUEBOT: Rescuing …" HUD subtitle (string 0x004E1CC8, posted only by `BeginRescue`). With `TryStartRescue` returning immediately, `BeginRescue` can't run, so the subtitle can't appear.
  - Proving it on screen would mean waiting minutes on the easiest difficulty for a random claim in a stock run; it's not scripted. Memory evidence (enabled 0, claimed 0, byte 0 = C3) stands in. Flagged for the owner.

## Item 9: game state snapshot

- **Fields researched** (Haiku Ghidra pass) **and verified live** with `harness/probe_state.py`:
  - **Velocity** `PRE+0xFC/+0x100/+0x104`, units/s (written by `Entity::IntegrateVelocity` 0x0049DBC0). Live: 367 u/s with W held, 0 idle. Displacement +0x90 = v·dt (6.6 ≈ 367×0.018).
  - **View** pitch `+0x3430`, yaw `+0x3434` (degrees).
  - **Mission timer** `+0x343C`, float seconds, decremented by Think. Live it counts down in real time. Training's limit is about 12.5 h (44979 s), and the HUD shows mm:ss within the hour ("29:39").
  - **Menu screen**: the current screen `[GameSession+0x3C8]+0x10`, indexed in the screen array at MM+0x158/+0x15C. In a level it stays on the last menu screen (2 = Title).
  - **Fires:** FireRescueEnt, identified by vptr 0x004E1780. They are entity **kind 6** (84 in bank), with mode +0x70 (0 idle, 4 burning, 5 out) and intensity +0x128. WindowFire is kind 7 (hose maps).
  - **Open:** fire position. FireRescueEnt's +0x4C reads 0 for all of them, so the root position lives elsewhere.
  - Training reports 7 kind-6 entities; whether all of them are fires is unverified. A vptr filter would be stricter but needs a data-pointer table entry; left for when the glitch HUD needs fires.
- **research.db corrections to apply in the decomp loop:**
  - 0x00456090 "MissionSummary::SetTimeRemaining (+0x3538)" is wrong: it's `RescueHud::DamagePanels_Update`, and PRE+0x3538 is the fire-burn accumulator.
  - The "+0x3424 blocking-surface SArray" note conflicts with the live-verified RescueBot enabled byte.
- **`src/state.cpp`:** `state::State`, a POD refreshed at every frame end on the game thread, read through address-table entries only.
  - Contents: frame, game state, map, menu screen; player pos/vel/XY speed/pitch/yaw/move state/ground; time left; victims total/rescued plus up to 24 listed (pos, state); fires total/burning/out; bot enabled/claimed.
  - One entity-list walk per frame. Modules read it with `state::Current()`.
  - The Debug pipe gains `snapshot` (key=value line; 4 KB response buffer). It runs under a critical section because the pipe thread formats while the game thread refreshes.
- **Test `state_snapshot`** (harness `assert_snapshot_matches`: snapshot via pipe, then the harness's own direct reads straight after):
  - Training idle, Training after movement, and bank twice: **24 / 24 / 28 / 28 fields, 0 mismatches**.
  - Real values in bank: 6 victims (one roaming, state 7), 83 fires, bot enabled 1.
  - Tolerances: exact for static fields; the timer ±0.5 s (it counts down between the two reads); motion fields ±1 u (the player is idle).
- **Suite finding: a crash in our own `SafeRead`** (`FDNYCore.asi+0x3822`, AV reading 0x0700010B, at the menu). Located with the crashlog + capstone, no debugger needed.
  - The VirtualQuery check said readable, but the page changed before the byte-copy loop ran. That's a race with another thread, most likely the D3D wrapper.
  - The snapshot triggered it by walking the entity list **at the menus**, where the list head is stale and every frame chased up to 20,000 junk pointers. That also slowed the menus.
  - Fixes:
    1. `SafeRead`/`WriteU8`/`WriteI32` use `ReadProcessMemory`/`WriteProcessMemory` on our own process. The kernel copies and reports a fault as failure, atomically.
    2. The snapshot walks entities only in a level (game state 1), capped at 4096 nodes.
- **Harness hardening (a flake surfaced by the same suite run):** Windows' foreground lock sometimes refused `SetForegroundWindow` at launch. Clicks then went to another window, and a click in the screen corner minimized the game.
  - `inputs.focus` now restores the window, sends a synthetic Alt press (lifts the foreground lock), and retries 6×.
  - If focus still fails, the run aborts as "harness: could not focus" and `run_test.py` retries the test once. This is an environment flake, so it isn't reported as a test result.
  - **Correction:** the synthetic Alt press itself broke menu setup. A lone Alt activates the window's system menu ("menu mode"), which swallows the next clicks. Replaced with the `AttachThreadInput` technique, which sends no keystrokes.
- **More menu-setup flakes, diagnosed from the run videos** (ffmpeg contact sheets):
  1. **Start menu.** `gclick`'s relative moves can leave the **OS** cursor outside the game window (e.g. if something warps it mid-move), and the click then lands on the taskbar. `gclick` now parks the OS cursor at the window centre with an absolute `SetCursorPos` before clicking. That produces no DirectInput delta, so the game's menu cursor is unaffected.
  2. **Intro-movie stall.** With `ImADeveloper` off (player config, since item 4), WinMain plays `Data\Movies\ActiValue.avi` + `Mekada.avi`. On the VM playback sometimes freezes on the "GENESIS" frame forever (DirectShow + the virtual audio device), so the game never reaches its menus.
- **New feature `fd_skip_intro`** (archived, default 0, takes effect next launch):
  - At DllMain it replaces WinMain's `jne` at 0x0042CC63 (`patch.intro_movies`) with `nop; jmp` to the same target. That's the branch developer mode takes, skipping exactly the movie block.
  - It must act before WinMain, long before the console runs FDNYCore.cfg, so it peeks `fd_skip_intro` in FDNYCore.cfg and then autoexec.cfg directly.
  - Debug builds also honour `FDNYCORE_TEST_SKIP_INTRO=1`. The harness sets it on every launch, and a test can override it via a new `env:` key.
  - Test `skip_intro` (env switch **off**, player cfg line `fd_skip_intro "1"`): movies skipped, menus and Training reached, value shown in cvarlist. Its first run caught a case-fold bug: `'_' | 0x20` = 0x7F, so the key never matched.
  - Useful for speedrunners anyway: no unskippable intro.

## Item 10: hud_player (the old Python player overlay, in-game)

- **Drawing:** Genesis' own `geEngine_Printf` (export, RVA 0x24B60, cdecl variadic; the exe already imports it; table entry `genesis.printf` with a prologue signature).
  - Called with `"%s"` and a string the core formats itself.
  - Called from frame_end, **before** the original `geEngine_EndFrame`, with the `geEngine*` the hook manager now keeps from the Begin/EndFrame arguments (`hooks::CurrentEngine()`).
  - Renders in-frame in the engine font on the D3D/dgVoodoo driver (screenshot `hud_after_move`).
- **`src/hud.cpp`:** `fd_hud_player` (archived, default 0).
  - Five lines at (8, 52), below the game's mission timer: `pos x y z`, `vel x y z`, `speed <xy>`, `state <name> (<n>) ground|air`, `screen <in level|loading|menu-screen name>`.
  - These are exactly the owner's field list for the old overlay: X/Y/Z position, player state, screen state, X/Y/Z velocity, XY speed.
  - Values come from the item-9 snapshot, and the HUD registers after it, so the HUD shows that frame's values.
  - The Debug pipe `hud` returns the lines actually drawn last frame plus their frame number.
- **Test `hud_player`** (`assert_hud_matches`: drawn lines vs direct reads; numbers have 1 decimal, so the tolerance is 0.06 + one frame of motion). Three checks pass:
  - idle;
  - right after moving (vel 349.4 drawn; HUD frame == snapshot frame);
  - after a state change (crouched (3) → standing (4)).

  Screenshots show the text in-frame, plus one with `fd_hud_player 0`.
- No red run against item-9: the test's first assertion is the `hud: ready` log line, which item-9 never emits, and its pipe `hud` command didn't exist.
- **Suite finding (test design, not a HUD bug):** the first version compared the HUD (frame N) with a snapshot and memory read *later*. The player was still decelerating after W was released (350 → 298 u/s), so the reads spanned 3 frames. A Python harness can't sample memory at the exact frame the HUD drew.
  - Fix: the pipe `hud` now returns, with the drawn lines, the **same-frame source values** they were drawn from (3 decimals).
  - `assert_hud_matches` checks (1) drawn == same-frame source (±0.051 for 1-decimal rounding), **including mid-motion** via new `key_down`/`key_up` steps and `{moving: true}`: 366.8/367.2 u/s drawn == source. It checks (2) drawn == direct memory **at rest** (the test waits 1 s after moves).
  - Together with item 9's snapshot == memory, that is "equals memory within one frame".

## Item 11: hud_glitch (port of tools/collision_overlay), split

Structural map (Haiku, line-referenced) of `fdny_collision_overlay.py` (1748 lines):

- **Hooks:** 8 hook sites write 128-byte records (`REC_FMT "<11I3f3ff3fI3f7I"`) into a 16384-entry ring:
  - entry / x_box / x_ray / x_miss / boxw / rayw in Genesis (`geWorld_Collision` body + writers);
  - commit `0x0043F9C7` and tick `0x0042AF0F` in the exe.
- **Analysis seam:** `GameSource.poll` turns records into `Ev` → `FrameAnalyzer.feed` / `end_of_poll` → Model / TextLog / `Overlay.build()`.
- **Determinism:** given the record stream plus per-poll snapshots (player pos/state/box/disp, tick, slot plane ny, map), the log and box text are deterministic apart from timestamps.
- **Bsp:** about 175 lines (literal port of Genesis' box clip + `.bsp` parser). MapFinder matches the live plane array against the `.bsp` files.
- **Doc/code discrepancies, kept as in the CODE** (parity is with the code): "Bugged" text only in state 3; no "too low" status; "Target -" fallback row.

**Parity strategy (the owner's "same outputs as the old overlay"):** identical-input replay.

- The core produces the same raw record stream.
- A Python replay driver feeds that stream, with the core's per-frame snapshots, through the **original overlay's own classes** (Model / FrameAnalyzer / TextLog / Bsp / `build()`).
- Its outputs are diffed against the core's for the same frames.
- In-process, a "poll" is one frame.
- Known risk: Python `round(x,1)` on doubles is used for writer-lookup keys; the C++ must round identically or attributions can differ on ties.

**Sub-items:**

| # | What | Done when |
|---|---|---|
| 11a | Record layer: the 8 hooks in the core, same record format, per-frame snapshot; Debug dump `FDNYCore.glitchrec` | Records flow live in Training (player sweeps, commits, ticks = frames); hook sites signature-verified; no crash; suite green |
| 11b | Analysis port (Ev/Model/FrameAnalyzer/classify/TextLog) | Replay of a recorded stream through the Python classes == core log lines |
| 11c | Bsp + MapFinder port (WARP prediction) | Replay: status text / gap / sweeps identical |
| 11d | In-game boxes (WARP / LAST WARP / HISTORY) + `fd_hud_glitch` + F9/F10/F12 equivalents | Box text == `build()` on replay; screenshots |

**11a (record layer): done.**

- **`src/glitch.cpp`:** the 8 hook sites are address-table code entries (Genesis + exe), signature-verified. The Genesis data globals `genesis.impact` / `.plane` / `.planes_ptr` have signatures from Genesis code that embeds them (addrgen now handles Genesis data).
- **Cave code:** a byte-for-byte port of the Python `Asm` emitter: same layout (TICK/HEAD/INCALL/FRESH, scratch record, ring of 16384 × 128 bytes), the LOG routine, entry/exit/boxw/rayw/commit/tick tails, displaced bytes taken live.
  - Installed by `fd_hud_glitch 1` (archived, default 0) and removed by `0`.
  - Every other thread is suspended while patching (`Freeze` checks that no thread's EIP is inside a site; retries).
  - The cave stays allocated after removal, as in the Python tool.
- **Reading:** the ring is read at frame_end; in-process, a "poll" = one frame.
- **Debug dump:** `FDNYCore.glitchrec` (records + one KIND 0xF0 snapshot per frame: tick, player ptr/state/ground/spectator, pos/mins/maxs/disp, slot impact/plane, Genesis base) for the 11b replay. Pipe `glitch` returns counters.
- **Relocation-aware Genesis signatures** (`addr::VerifySig`): Genesis really relocates (seen at 0x02390000), and sigs such as `glitch.rayw`, `glitch.exit_box` and **`genesis.printf`** contain absolute Genesis pointers.
  - A 4-byte window that differs by exactly the base delta (and held a 0x100xxxxx address) is accepted as relocated.
  - Before this, a relocated Genesis would have silently disabled the HUD.
- **Test `glitch_records`:** hooks install; +353 traces and +174 ticks over 3 s of movement, 0 dropped; off → 0 new traces; on → flowing again; no crash.
  - Two test-authoring errors fixed: commits ≥ 10 was a wrong expectation (commits are rare on flat ground, so they're left to the 11b replay), and the YAML label `off` parsed as boolean.
- **Stream check** with the overlay's own `REC_FMT` (`harness/glitchrec_check.py`): kinds trace / engine-ray / commit / snapshot.
  - Per frame, traces carry tick N and engine rays N+1. That matches the game loop: TickSubsystems → tick hook at 0x0042AF0F → RenderFrame, the same as the Python poll order.

**11b (analysis port): done.**

- **`src/glitch_analysis.{h,cpp}`:** C++ port of Ev / Model (writers = insertion-ordered map, 50000 FIFO; history 500) / FrameAnalyzer / `iteration_outcome` / TextLog / `GameSource._classify` / `poll` record decoding / `track_crouchbug`.
  - Python `%.Nf` (half-even on the double of the float32) and `round(x, 1)` writer keys reproduced exactly.
  - `glitch.cpp` starts a session (TextLog header) on every hook install and runs `Poll` per frame. The log goes to `FDNYCore.log` as `glitch| ` lines (new `log::Raw`); there's no separate file (owner rule).
  - 11b has no prediction on either side: the warp line ends `; predicted: none` until 11c.
- **Parity tools:** `harness/glitch_replay.py` subclasses the overlay's `GameSource` and swaps only the data source; everything else runs unmodified, with timestamps stubbed. It's session-aware (Debug dump marker 0xF1 on each install). `harness/glitch_parity.py` diffs session by session.
- **Live test `glitch_parity_log`** (Training → bank, two sessions): identical, but a live run only yields header lines and crouchbug notes. Warp-capable hits and warps are rare in normal play.
- **Synthetic test `glitch_parity_fuzz`:** `harness/glitch_fuzz.py` writes a seeded stream in the exact dump layout, covering:
  - capable hits, including repeats;
  - warps with known and unknown writers;
  - every iteration outcome and re-sweeps;
  - engine rays, Genesis callers (relocated and not) and foreign commits;
  - crouchbug edges, player-less frames, tick gaps, and floats on the .x5 rounding edges.
  - The core replays it offline through the Debug pipe (`glitch_replay <path>`, refused while the live hooks run); the reference replays it through the original classes.
  - Result: **2075 lines identical** (3 sessions: 1495 capable, 458 WARP, 92 notes, 30 header).
  - Mutation check: a one-character edit to the core log is reported as a mismatch.

**11c (warp window: Bsp + MapFinder): done.**

- **`src/glitch_bsp.{h,cpp}`:** C++ port of `gbsp_chunks` / `Bsp` (expanded hulls, walk, the literal Genesis box clip, sweep, inside, predict) / `MapFinder` / `describe_window`.
  - Python's double arithmetic is reproduced in the same operation order, with float32 rounding at the same points (`f32`), IEEE sqrt (SSE2 `sqrtsd`), and Python's `min`/`max` argument semantics.
  - Verified: zig's x86 default target has SSE2 and no FMA, so doubles are SSE2 scalars with no x87 extended precision and no contraction.
  - `%.Nf` is now exact for any double (integer scaling of the exact binary value, half-even). The 11b version relied on float32-sourced inputs, but gap/window are computed doubles.
- **Map recognition in-process:** `[genesis.planes_ptr]` first 0x4000 bytes vs each `<exe dir>\Data\Maps\*.bsp` plane chunk prefix (the old tool's default dir), checked every 1 s (MAP_CHECK_SEC).
  - Each prediction goes into Model.window / predictions / pred_stats, and warp lines now end `; predicted: <describe_window>`.
  - On `fd_hud_glitch 0` the core logs what the old tool logged on exit ("hooks removed…" and the prediction summary).
- **Dump / replay:** KIND 0xF2 = a map check ran this frame (+ the 0x4000 plane bytes, or unreadable); 0xF4 = session end.
  - `glitch_replay.py` drives the original MapFinder by opening its throttle exactly on the core's check frames and handing it those bytes. The original Bsp / describe_window run unmodified.
  - `_predict` is GameSource's with the two live reads swapped for the stream (documented in the file).
- **Fuzz:** `glitch_fuzz.py` now uses real maps:
  - plane prefixes, map switches, unreadable and unknown arrays;
  - positions in model bounds and in solid leaves, plus "spots" found by nudging inside-solid points up to undersides. The reference `Bsp` is used only to choose inputs.
  - Per-frame windows covered: WARP NEXT FRAME, both LINED UP variants, blocked above, floor stops it, start moving, move toward open space, too high (many gap values), in bounds, not recognised, spectator.
  - **Not reached:** "blocked ahead", "almost lined up", "nothing here to line up with".
- **Results:**
  - `glitch_parity_fuzz`: 2537 lines identical (463 WARP lines with full predictions, 564 notes incl. map / stats notes).
  - `glitch_parity_log` live: Training and bank recognised from the live plane array (1160 / 2492 solid leaves), 5242 predictions in one session, identical.
- **Cost (Debug -O0, game thread):**
  - 23 µs/frame average.
  - One ~22 ms frame per level load (the map check that loads the .bsp, plus its INFO line), at a point where the game is loading anyway.
  - Found and fixed along the way: `log::Raw` called FlushFileBuffers per line, which cost 20-140 ms frames. It now relies on WriteFile (survives a game crash).
- **Timing visibility:** the pipe `glitch` reports `poll_us_avg` / `poll_us_max`; the uninstall line logs the same, and frames over 10 ms are logged.

**11d (boxes + keys): done. Item 11 complete.**

- **Box text:** `analysis::BuildBox` ports `Overlay.build()` (WARP / LAST WARP / HISTORY), with the original Text widget's tags per character.
  - It reproduces tk's tag priority: source and player colours outrank the warp/flash background; plain tags turn white on it.
  - The model gained what build() reads: the snapshot slot fallback (`last_player_trace_tick`), `last_warp` kept apart from history eviction, and Model.player / tick.
- **Display:** like the original, a click-through, never-activating layered window, 0.88 alpha, the original's colours, Consolas 11 px with an exact cell grid. It's owned by the game window (stays above it, minimises with it) and placed at its client top-right with the 12 px margin.
  - GDI rendering runs on the core's own UI thread, only when the lines change. The game thread builds the text and hands it over (~57 µs/frame, Debug).
  - Needs windowed or dgVoodoo fake-fullscreen, the same requirement as the original.
- **Two in-frame attempts, rejected with measurements:**
  - `geEngine_Printf` (proportional engine font with cell artefacts) cut simulated ticks from ~346 to 84 per 6 s.
  - Genesis bitmap decals (`geBitmap_Create` / `LockForWriteFormat` / `geEngine_AddBitmap` / `geEngine_DrawBitmap`, ABI read by Haiku in Ghidra) attached and returned TRUE but never showed:
    - tried 398×346, 256 and 64 tiles; XRGB and 565; with a colour key; opaque alpha;
    - `AddBitmap` only flags the engine, and blitting in the same frame crashed in D3DDrv (null driver handle at D3DDrv+0x7E09);
    - D3DDrv's decal function (RVA 0x7DE0, table +0x88) is an immediate DDraw `Blt` with DDBLT_KEYSRC that ignores errors;
    - diagnostic dumps showed our THandles match the game's visible timer digits except the bitmap's source format (8-bit palettized).
    - Left as a lead if in-frame drawing is ever wanted (e.g. for OBS game capture): test a palettized bitmap.
- **Keys:** F9 hide/show, F10 freeze (logging continues), F12 clear history + last warp (logged "history cleared (F12)").
  - Edge-triggered via GetAsyncKeyState, only while the game has the keyboard (`binds::KeysLive`: focused, console closed); the game still receives them.
  - Also console commands for binds: `fd_glitch_hide` / `fd_glitch_freeze` / `fd_glitch_clear`.
  - Discrepancy kept as in the code: the title's "FROZEN" never shows in the original (build() only runs unfrozen).
- **Parity:** the Debug `FDNYCore.glitchbox` (env `FDNYCORE_TEST_GLITCHBOX=1`) holds every frame's build() text; `glitch_replay.py` runs the ORIGINAL `Overlay.build` on a stand-in with `m` / `frozen`. F12 is dump marker 0xF5 (replayed as `m.clear()` + note).
  - `glitch_parity_fuzz`: 2400 frames / 59953 box lines identical, covering LAST WARP + from, HISTORY warp rows, repeat counts, Target rows, Bugged, LINED UP / WARP NEXT FRAME and 9 F12 clears.
  - `glitch_parity_log` live: 6870 frames / 164882 lines identical, plus F9 / F10 / F12 asserted through the pipe (`glitch_box`: hidden / frozen / shown_tick).
  - Colours are not parity-checked (text only); screenshots in evidence.
- **`glitch_box_cost`:** ≥ 250 ticks / 6 s with the boxes shown (guards against the printf regression).
- **Ghidra project side effects (Haiku lookups, read-only intent):**
  - instruction units were created in the Genesis.dll listing at the bitmap exports;
  - D3DDrv.dll was imported as a new program (raw block, not PE-loaded).
  - No renames, comments or patches.

## Item 12: profiles + watermark

- **`src/profile.{h,cpp}`:** `fd_profile practice` (default) / `run`.
  - `cmd` gained gates: `SetGates(CvarGate, CommandGate)`; `SetCvarUngated` for profile switches.
  - In `run`, practice-only variables can't leave their default and practice-only commands are refused, each with the console message "<name> is practice-only (fd_profile is run)" and a log line.
- **Switching sets:**
  - → `run`: practice-only vars not at default are remembered and reset.
  - → `practice`: they come back.
  - The remembered set (`fd_profile_saved <var> "<value>"`) and `fd_profile "run"` are written last in FDNYCore.cfg, so a restart restores both.
  - A switch also runs `profile_<name>.cfg` if present (the player's own per-profile set, through the existing exec).
- **Classification (agent default, OWNER TO CONFIRM):**
  - Practice-only vars: `fd_hud_glitch`, `fd_hud_player`, `fd_watermark` (the watermark can't be hidden in run).
  - Practice-only commands: `fd_glitch_*`, and `fd_tp_save` / `fd_tp_load` reserved for item 13.
  - Allowed in run and listed on the watermark: fov, window cursor, skip intro, static spawns / spawn pins / RescueBot (FDNYDeterministic's speedrun features), binds.
  - The list is one table in profile.cpp.
- **Watermark:** one in-frame line (geEngine_Printf, engine font, so recordings and OBS game capture show it) at the bottom-left above the tool icons.
  - Example: `FDNYCore 0.12.0 RUN | fov 95 | asi: alttabfix FDNYFirefighterAmericanHeroFOVFix`.
  - It shows the version, the profile (RUN in capitals), every FDNYCore variable not at its default (`fd_` stripped), and every other .asi module loaded (toolhelp snapshot at init).
  - On by default in practice too (`fd_watermark 0` hides it there).
- **Test `profile_run`:**
  - practice shows hud_player; → run resets it;
  - refusals: fd_hud_player / fd_hud_glitch (hooks stay off) / fd_watermark / fd_glitch_clear;
  - fov allowed and listed;
  - restart keeps RUN and the fov, without hud_player;
  - → practice restores hud_player 1;
  - screenshots of both profiles.
  - New harness step `assert_pipe_contains`; Debug pipe `watermark`.

## Item 13: teleporter (owner request)

- **Research (Haiku, Ghidra, read-only):**
  - **No native teleport verb.** PlayerRescueEnt::OnScriptCommand's verbs are jump, crouch, uncrouch, push, climb, unclimb, oxyon/oxyoff, hint, equip, select, touchhose, toolcmd, setashdensity, addbonus, printstats, finalexit(loadmap), setrescuebot, reset and escape. "moveto" is FireRescueEnt's NPC path walk. So the owner's default names stand: `fd_tp_save` / `fd_tp_load`.
  - **The game's reset isn't reusable.** `Verb_Reset` 0x0044FF10 → `ResetLocomotionState` 0x0044CD10 does all of this:
    - moves to the spawn anchor (+0x534.., which Verb_Escape mutates at runtime);
    - sets yaw from the anchor heading (+0x530);
    - drops held liftables and the carried victim;
    - sets +0x3450 = 1.0, move state 3 and the on-ground latch;
    - resets SFX volumes, rebuilds zone occupants and calls OnLevelWon;
    - calls `SnapToFloor` 0x0049DA00, which commits (0,1,0) when the floor trace misses (confirmed).
  - +0x24 is an inline geXForm3d (rotation +0x28..+0x48, translation = position +0x4C); no other position cache was found.
  - Side note: the reset's position mirror at +0x3400 overlaps the RadioChatter object at the same offset (decompile reading, impact not verified).
- **Implementation (`src/tp.{h,cpp}`):**
  - `fd_tp_save` reads position + pitch + yaw and tags them with the current map (state snapshot).
  - `fd_tp_load` is refused without a save, outside a level, or on any other map ("saved on X, this is Y -- refused", no state change). Otherwise it queues the load for frame end (after Think, before the next).
  - The load writes position, pitch and yaw, and zeroes velocity (+0xFC), displacement (+0x90) and the impulse accumulator (+0x5B8). Nothing else (health, carried victim, checkpoint) is touched.
  - New field entries: `player.f_vel` / `f_impulse` / `f_pitch` / `f_yaw`. New `addr::SafeWrite` (kernel-copied, like SafeRead).
  - Practice-only via the item-12 gate.
  - Stance is not saved: loading while crouched keeps the crouched box.
- **Test `tp_teleport`:**
  - save → move 730 u and turn → load: position back to 0.0 u (x/z) and y, yaw −270 restored; still there 1 s later;
  - `bind f5 "fd_tp_load"` works;
  - refused in run;
  - on bank: "saved on training, this is bank -- refused", position unchanged.
  - Screenshots saved and loaded are identical views.

## Owner decisions after the backlog (2026-10-21)

- **Old tools stay installed** (FOV fix, alttabfix, FDNYDeterministic, Python overlays): no further development on them, but they work, so they're not deleted yet.
- **Revisit on-screen text drawing:** investigate how the game draws text, measure where the cost is (e.g. updating every 5 ticks), and find a way to make in-frame text affordable. See "In-frame text investigation" below.

## In-frame text investigation (owner request, 2026-10-21)

Debug-only benchmarks in `src/bench.cpp` (`fd_bench_text`, `fd_bench_blit`, `fd_bench_bmp`) and `tests/bench/*.yaml`, which aren't in the suite (the suite globs `tests/*.yaml` only). Frame rate comes from pipe `stats` frame_end deltas over 6 s, in Training with the watermark off. Base is 57.5 fps (the game's frame cap).

- **How Genesis draws Printf text** (Haiku, hand-decoded from bytes):
  - `geEngine_Printf` 0x10024B60 only queues: store 0x10024AE0 keeps at most 40 strings of at most 80 characters per frame (entries at engine+0x33AB4, 0x58 bytes, count at +0x34874). Anything beyond is dropped silently.
  - The drawer 0x10025C60 then calls `geEngine_DrawBitmap` once per character, spaces included. It reads the glyph origin from engine+0x336B4[c], blits from the font bitmap at engine+0x336B0 (1024×14, 8-bit palettized, colour key 0, i.e. data\sprites\confont.bmp) and advances 8 px.
  - Each DrawBitmap is one immediate DirectDraw `Blt` in D3DDrv (slot +0x88, RVA 0x7DE0), which dgVoodoo emulates.
- **Cost model (measured): about 29 µs per blit; area doesn't matter.**
  - Printf 31×65 characters = 12 fps; 8×65 = 35 fps; 1×65 = no visible cost.
  - Blits of font rects: 2000 = 11.8 fps; 400 = 45 fps; 40 = 57.5. 40 blits of 128×128 = 57.7, so area is free.
  - "2000 × 1-char Printf" was free only because the queue keeps 40 strings.
- **"Update every N ticks" with Printf doesn't work.** The queue is drained every frame, so text exists only in frames where it was printed. Every 5 frames = text missing on 4 of 5 frames (screenshot shows none), and the cost becomes a periodic spike: 36 fps average for the 31-line box.
- **Why our own bitmaps never showed (items 11d / this pass): no `geEngine_FlushPipeline` before the blit.**
  - At the EndFrame hook the engine's 3D polys are still cached; they're flushed inside EndFrame and paint over an immediate Blt done earlier.
  - With `geEngine_FlushPipeline(engine)` first, blits of the engine font bitmap and of our own bitmaps are visible.
  - GE_Rect is {Left, Right, Top, Bottom}, inclusive.
  - The Printf text survives because its drawer runs after that flush. The exe imports FlushPipeline for its own HUD.
- **Our own GDI-rendered bitmap, one blit per frame after the flush:** a 400×346 box (26 text lines, changing every frame) runs at 57.5 fps.
  - 32-bit XRGB, uploaded every frame: 1.6 ms upload (Debug).
  - 565, uploaded every frame: 0.6 ms.
  - Every 5 frames: same per-upload cost, 1/5 as often.
  - No colour key needed (57.3 fps, visible).
  - AddBitmap still needs a couple of frames before the first blit (earlier crash).
- **Conclusion:** in-frame text is affordable. Render text with GDI into one geBitmap when it changes (or at most every N frames), then FlushPipeline + one blit per frame.
  - This can replace the overlay window for hud_glitch (and lift the windowed / fake-fullscreen requirement).
  - It should also replace the Printf path for hud_player (~150 chars ≈ 150 blits ≈ 4 ms/frame) and the watermark (~70 ≈ 2 ms/frame). Those costs are hidden today by the frame-cap headroom on this VM.
- **New address entries:** `genesis.flush_pipeline`, the bitmap API (create / lock / unlock / bits / info / destroy / add / remove / set_color_key / draw_bitmap), and engine fields `engine.f_font_bitmap` / `f_font_glyphs` / `f_printf_count`.

## Cut-over: all on-screen text in-frame (owner request "cut it over", 2026-10-21)

- **`src/textdraw.{h,cpp}`:** in-frame text panels.
  - Each panel is GDI-rendered (Consolas 11 px, the original overlay's palette and bold tags) into one 565 Genesis bitmap, only when its text changes and at most every `minFrames` frames.
  - Every frame: one `geEngine_FlushPipeline`, then one `geEngine_DrawBitmap` per visible panel.
  - Anchors: top-left / top-right / bottom-left, from the game's client size.
  - Bitmaps grow in 64×32 steps; AddBitmap waits 2 frames before the first blit.
  - Registered last, so its frame-end draw runs after every module updated its panel.
- **Panels:**
  - **hud_glitch boxes:** top-right, 12 px; re-rendered at most every 3 frames, about the original's 50 ms REFRESH_MS (the text is still built every frame for the log/box parity).
  - **hud_player:** top-left 8,52.
  - **Watermark:** bottom-left, above the tool icons; the asi list wraps to a second line beyond 100 cells.
- **Removed:** the item-11d overlay window (`glitch_draw`, its UI thread) and the remaining geEngine_Printf use. The windowed / fake-fullscreen requirement is gone.
- **Test `text_panels`:** all three panels on: 346 frames / 6 s in Training and in bank, the frame cap (12 fps before with Printf boxes). F9 hides the boxes. The panels survive a level change.
- **Still open:** the player HUD calls move state 3 "crouched" while the collision box says "Standing" (movement research: 3 = stand). Side by side on screen now, for the owner to rule on.

## Assumptions / workarounds

- **"Behaves the same" (item 1c) definition**, chosen by the agent at the owner's request. These are LOOP.md's 7 criteria, with two calibrations:
  - **(4)** Autosplitter final addresses are heap pointers that move run to run even without the core. So the check requires all 12 chains to resolve in every run, plus an exact match only where the two baselines agree with each other.
  - **(6)** Lap-position tolerance = max(32 u, 2× the baseline-pair spread). Scripted wall-clock input isn't frame-exact; the observed core-vs-baseline distance was 0.6-5.8 u.
- **Saves:** backed up to `core/backup/Saves` after several harness runs had already started the game, and the game rewrites `player*.rb`. The pristine originals are in the repo-root `FDNYSP1.1_0e4c1e9f.zip`. Harness runs only enter Training on the "Hotrod Stein" slot.
- **Game cfg history:**
  - Items 1-3 ran with `ImADeveloper = true` (the console needed it then). That covers the baseline_route base_a/base_b no-core runs used as the equivalence envelope.
  - From item 4 on, the cfg is the owner's original (`ImADeveloper = false`) and the core provides the console.
  - The item-4 Release core with the flag off is still inside the envelope (fps 55.6 vs 55.6). A future re-baseline in player config is cheap if a check ever becomes ambiguous.

## Log

- 2026-10-21 it1: created PROGRESS.md and dispatched 4 Haiku research subagents (R1/R4, R5, R3, R2).
  - R1-R5 done. MinHook 1.3.3 vendored to `core/third_party/minhook/`.
  - Smoke launch #1 hit the FMOD fatal error. Installed VB-CABLE; smoke #2 renders the splash windowed.
- 2026-10-21 it2: FDNYCore 0.1.0 skeleton (Debug+Release clean) loads live and logs version, hashes and ready. Harness v1 written; `core_loads` PASS. Fixed VM gaps: VC++ x86 runtime, ffmpeg.
- 2026-10-21 owner request: added backlog item 13 (teleporter, map-gated) and a bind feasibility check (before item 5) to LOOP.md.
- 2026-10-21 it2 (cont.): mapped the front end (DirectInput cursor → `gclick`) and found `ImADeveloper` enables the console (`listmaps` works in-level). Added player/clock entries.
  - Wrote `baseline_route` (10 laps ≈ 2.5 min in-game) and `compare_runs.py`.
  - Baseline equivalence: **EQUIVALENT** (runs `2026-10-21-022020-base_a`, `022253-base_b`, `022526-core`; fps 55.6 vs 57.2; laps within 5.8 u). The first attempt read fps as int; fixed to f32 and re-ran.
  - Regression suite (`baseline_route`, `core_loads`) PASS, Debug. **Item 1 DONE**, committed with evidence in `core/evidence/item01/`.
- 2026-10-21 bind feasibility (Haiku): feasible with constraints; recorded above.
- 2026-10-21 it3: **item 2 DONE**. FDNYCore 0.2.0 has the signature-verified address table (23/23 OK live), version selection by hash, the module registry plus `levelwatch`, and a Debug override for failure-path tests.
  - Tests `addr_verified` / `addr_unknown_exe` / `addr_bad_signature` PASS.
  - Suite 5/5 PASS.
  - The Release 0.2.0 core is still EQUIVALENT to the item-1 baselines (fps 58.8 vs 55.6, laps within 5 u).
  - Evidence in `core/evidence/item02/`.
- 2026-10-21 it4: **item 3 DONE**. FDNYCore 0.3.0 adds the MinHook hook manager, by-name Genesis frame hooks (exactly once per frame vs an independent clock), frame_end + level_load events, and the Debug test pipe.
  - Red run first, then green; suite 7/7 PASS; Release EQUIVALENT.
  - Evidence in `core/evidence/item03/`.
  - Next: item 4 (console on the game's console; hook `ScriptEngine::ExecLine` 0x00470F10).
- 2026-10-21 it5: **item 4 DONE**. FDNYCore 0.4.0 gives players the game's own console without ImADeveloper (1 JZ NOPped at DllMain).
  - `fd_`/`echo`/`cvarlist`/`cmdlist`/`exec` are intercepted at ExecLine; output goes through `Console::AddLine`. Input is blocked while the console is open (0.0 u vs 258 u). Crashlog VEH added.
  - The first approach (calling the ctor at the first frame) crashed; it's diagnosed and documented.
  - Red then green; suite 8/8 PASS in player config; Release EQUIVALENT (fps 55.6 vs 55.6). Game cfg restored to `ImADeveloper = false`.
  - Evidence in `core/evidence/item04/`.
- 2026-10-21 it6: **item 5 DONE**. FDNYCore 0.5.0:
  - Archived vars and binds persist to `FDNYCore.cfg`; `autoexec.cfg` runs at startup; `bind`/`unbind`/`unbindall`/`bindlist`.
  - Fixed a native game crash: console opened before a level → FATAL "Console font texture not loaded". The core now refuses to open it until the font exists.
  - Red runs first; suite 11/11 PASS; Release EQUIVALENT (fps 55.6 vs 55.6).
  - Evidence in `core/evidence/item05/`.
- 2026-10-21 it7: **item 6 DONE**. FDNYCore 0.6.0 adds the fov module.
  - The old fix's behaviour was measured live: a constant 1.43·aspect/(4/3)·factor, which is 2.002 = 90° with the owner's ini.
  - FDNYCore's default reproduces it to the float (2.001999855). `fd_fov N` sets 2·tan(N/2) Hor+; `-1` gives the game default.
  - `camera.fov` chain found by pointer scan.
  - Red then green; suite 13/13 PASS; Release EQUIVALENT.
  - Evidence in `core/evidence/item06/`.
- 2026-10-21 it8: **item 7 DONE**. FDNYCore 0.7.0 adds the window module, which replaces alttabfix:
  - The same 16-byte site becomes NOPs + a call to the core's cursor routine (alttabfix logic, 'O' key included).
  - `fd_window_cursor`; it stands aside if alttabfix already patched the site.
  - New address-table rule: patch sites occupied by other mods are reported, not counted as mismatches.
  - Wrong HWND entry removed.
  - Red (cursor trapped at 200,200) → reference (alttabfix) PASS → FDNYCore PASS; suite 15/15; Release EQUIVALENT.
  - Evidence in `core/evidence/item07/`.
- 2026-10-21 it9: **item 8 DONE**. FDNYCore 0.8.0 adds the speedrun module (FDNYDeterministic port):
  - `fd_static_spawns` / `fd_spawns` / `fd_rescuebot`; victim_spawn event; field entries in the address table.
  - FDNYDeterministic was verified live for the first time, then the core matched its victim positions exactly. The RescueBot enabled byte was resolved live.
  - Suite-found coexistence bug fixed: the core defers if FDNYDeterministic is installed.
  - Suite 18/18 PASS; Release EQUIVALENT.
  - Evidence in `core/evidence/item08/`.
- 2026-10-21 it10: **item 9 DONE**. FDNYCore 0.9.0 adds the state snapshot (`state::Current()`, Debug pipe `snapshot`). It matches direct memory reads with 0 mismatches in Training and bank.
  - The suite found a race crash in SafeRead → kernel-copied reads/writes; the entity walk now runs in-level only.
  - Harness hardened, diagnosed from videos: focus without keystrokes; `gclick` parks the OS cursor in the window; intro movies skipped via the new player feature `fd_skip_intro`.
  - Equivalence: Genesis.dll may legitimately relocate (seen at 0x02390000). `baseline_route` stages the dev cfg like the item-1 baselines.
  - Suite 20/20 PASS; Release EQUIVALENT.
  - Evidence in `core/evidence/item09/`.
- 2026-10-21 it11: **item 10 DONE**. FDNYCore 0.10.0 adds hud_player (`fd_hud_player`): pos / vel / XY speed / player state / screen state, drawn in-frame with Genesis' `geEngine_Printf`.
  - Test redesigned after a suite-found measurement race: same-frame source check, also mid-motion.
  - Suite 21/21 PASS; Release EQUIVALENT.
  - Evidence in `core/evidence/item10/`.
- 2026-10-21 it12: **11a DONE** (FDNYCore 0.11.0): collision-overlay hooks in-process, Debug `.glitchrec` dump; suite green; evidence in `core/evidence/item11a/`.
- 2026-10-21 it13: **11b DONE** (FDNYCore 0.11.1): analysis port; the core's `glitch|` log is identical to the original overlay classes on the same stream.
  - Live parity (`glitch_parity_log`) only covered header and notes, so I added the seeded synthetic stream (`glitch_parity_fuzz`): 2075 lines identical, including 1495 capable and 458 WARP. The mutation check is caught.
  - Suite 24/24 PASS (Debug); Release EQUIVALENT (fps 57.2 vs 55.6, laps within 4.1 u).
  - Evidence in `core/evidence/item11b/`.
  - Next: 11c (Bsp + MapFinder → "predicted:" text; replay with `--predict`).
- 2026-10-21 it14: **11c DONE** (FDNYCore 0.11.2): the warp window (Bsp / MapFinder / describe_window port), with map recognition from Genesis' live plane array.
  - Fuzz parity: 2537 lines identical, with real-map spots reaching WARP NEXT FRAME and LINED UP. Live: Training and bank recognised; 5242 predictions, identical.
  - Fixed the per-line log flush (20-140 ms frames) → 23 µs/frame average.
  - Suite 24/24 PASS (Debug); Release EQUIVALENT (fps 58.8 vs 55.6, laps within 6.2 u).
  - Evidence in `core/evidence/item11c/`.
  - Next: 11d (boxes WARP / LAST WARP / HISTORY drawn in-game + F9/F10/F12 equivalents; box text == `build()` on replay).
- 2026-10-21 it15: **11d DONE → item 11 DONE** (FDNYCore 0.11.3): WARP / LAST WARP / HISTORY boxes as the original's overlay window (GDI, own UI thread), F9/F10/F12 + console commands.
  - Per-frame box text identical to `Overlay.build()`: 2400 synthetic + 6870 live frames.
  - Two in-frame attempts measured and rejected (printf: 84 vs 346 ticks / 6 s; Genesis decals: never visible). Findings recorded above.
  - New test `glitch_box_cost` (345 ticks / 6 s shown and hidden).
  - Suite 25/25 PASS (Debug); Release EQUIVALENT (fps 58.8 vs 55.6, laps within 4.3 u).
  - Evidence in `core/evidence/item11d/`.
  - Next: item 12 (profiles + watermark).
- 2026-10-21 it16: **item 12 DONE** (FDNYCore 0.12.0): fd_profile run/practice (gated vars/commands, remembered practice set, persisted, profile_<name>.cfg), in-frame watermark (version, profile, non-default vars, other .asi). Test `profile_run`. Suite 26/26 PASS (Debug); Release EQUIVALENT with the watermark on (fps 57.2 vs 55.6). Practice-only list is the agent's default, owner to confirm. Evidence `core/evidence/item12/`. Next: item 13 (teleporter).
- 2026-10-21 it17: **item 13 DONE** (FDNYCore 0.13.0): fd_tp_save / fd_tp_load (position + view, frame-end write, velocity/displacement/impulse zeroed; refused on another map and in run). Test tp_teleport. Suite 27/27 PASS (Debug); Release EQUIVALENT (fps 55.6 vs 55.6). Evidence core/evidence/item13/. **Backlog complete; loop stopped.**
- 2026-10-21 it18: in-frame text investigation (eca4653) then **cut-over** (FDNYCore 0.14.0): textdraw panels for the collision boxes, hud_player and watermark (one bitmap blit each after FlushPipeline); overlay window and Printf removed. Test text_panels (346 frames / 6 s with all three on, Training and bank). Suite 28/28 PASS (Debug); Release EQUIVALENT (fps 57.2 vs 55.6). Evidence core/evidence/text_cutover/.
- 2026-10-21 it19: **0.14.1** (b92cd99): panel placement from the camera rect, not the window (owner's dgVoodoo fullscreen scaling put the glitch box / watermark outside the frame); `fd_textdraw`.
- 2026-10-21 it20: **translucent-panel research spike** (owner: "try C as a spike, don't modify working code"). Standalone probe `core/probes/alphaprobe.cpp` + `tests/probes/` (not in the suite); core/src untouched. **Feasible:** 2x2 world-attached (`geWorld_AddBitmap`) 256 4444 tiles drawn with `geEngine_RenderPoly` (flags 1) after FlushPipeline give true per-pixel alpha, live text (0.74 ms per 4-tile upload), survive a level change. Untextured vertex alpha is ignored; engine-attached bitmaps and textures > 256 crash D3DDrv; alpha textures are 4444 only. Evidence core/evidence/alpha_spike/README.md.
- 2026-10-21 it21: **0.15.0** (owner: smaller font, watermark stays opaque, reusable helpers per rendering type).
  - New `src/gfx.{h,cpp}`: Canvas (GDI grid text, shared `fd_text_size` font, default 9 px), Decal (565 engine bitmap), AlphaSurface (256 px 4444 world-attached tiles, RenderPoly, dirty-tile uploads, re-attach on world change), SolidRect, BeginDraw (one flush per frame), World (RenderWorld hook), ViewSize.
  - textdraw rebuilt on gfx: panels are STYLE_TRANSLUCENT (default; opaque decal fallback without a world) or STYLE_OPAQUE (watermark). `fd_text_alpha` (default 160). Address table +5 (render_poly, render_world, world_add/remove_bitmap, bitmap.f_driver_handle).
  - Suite 25/28 then console_basic PASS on rerun (input flake). glitch_box_cost / text_panels fail their >=50 fps thresholds, but **0.14.1 fails identically tonight** (204/207 vs 201/206 ticks per 6 s; VM at ~34 fps all evening, control without any core change 36-37): environmental, thresholds unchanged. Release not re-measured for the same reason.
  - Evidence core/evidence/text_015/ (screens: translucent in Training and after loadmap bank; log shows the opaque fallback during the load).
- 2026-10-21 it22: **0.15.1**: `fd_text_fg_alpha` (text opacity on translucent panels, default 255). No separate drawing method: the per-pixel alpha is a parameter of the same AlphaSurface upload (constant -> variable) and the tiles are blended either way, so cost is identical; strictly opaque drawing stays STYLE_OPAQUE (Decal). New test text_alpha (PASS): brightest box pixels 233 -> 191 at 96, back to 233 at 255. Evidence core/evidence/text_015/fg255_zoom.png, fg96_zoom.png.
- 2026-10-22 it23: **0.16.0 core / feature-module boundary** (owner: "core functionality with a layer of separation, like plugins"; option A = in-binary boundary, shaped to map onto a later DLL plugin API).
  - Features moved to `src/features/` (fov, window, speedrun, hud, glitch + analysis/bsp/palette, tp, bench) and self-register with `FDCORE_MODULE` (COFF section `.fdmod$m` between core markers: no static constructors needed). `core.cpp` starts services only, then `modules::StartAll`; DllMain runs each module's `early` step. Log: `modules: N registered (...)`, `module <name>: enabled|disabled (reason)` (levelwatch keeps its lines).
  - Services no longer know features: practice-only is declared by each feature (`cmd::SetPracticeOnly`), profile only enforces; textdraw is palette-agnostic (attr = colour index / bold / bg slot, `SetPalette`; the overlay colours live in `features/glitch_palette.h`); Debug pipe handlers are registered by their features (`testpipe::Register`); new core `camera` service owns the geCamera_SetAttributes hook (view size for gfx; fov is a filter on it); `hooks::OnFrameEndLate` for drawing.
  - `build.ps1 -Without <features|*>` (separate output folder). Core-only build (98.5 KB vs 188 KB) runs in-game: core_loads + console_basic PASS, `modules: 1 registered (levelwatch)`.
  - Suite: all pass except glitch_box_cost / text_panels (the VM fps thresholds, unchanged, failing identically since it21), plus two reruns: addr_verified (level never loaded; PASS on rerun) and state_snapshot (fire count grew between the snapshot and the harness's direct read, 87 vs 88; PASS 2/2 on rerun -- a race in the test's "read immediately after" design, left unchanged). Evidence core/evidence/modules_016/.
- 2026-10-22 it24: **0.16.1 durable patterns** (owner: future additions must follow the established patterns, durably and transferably). `core/CLAUDE.md` (rules, done-means, report format, owner decisions), `core/DECISIONS.md` (D1-D23 with reasons), `tools/arch_check.py` (R1-R5, run by build.ps1 + `.githooks/pre-commit`), R7 via `-Wglobal-constructors -Wexit-time-destructors`, `new-feature` skill + `core/templates/`. R7 found and fixed two never-run initialisers (textdraw panels: benign; glitch_bsp ON_EPSILON: could be 0 instead of 0.1). Parity tests still identical. Template dry run built and ran (`module sample: enabled`). Evidence core/evidence/guardrails_0161/.
- 2026-10-22 it25: **0.17.0 trigger boxes** (owner: color-coded 50% boxes over level triggers, recognised live from the loaded map, no BSP pre-scan or coordinate file). New feature `triggers` (`fd_triggers` / key **T** via the new `binds::BindDefault`, `fd_triggers_all` grey others, `fd_triggers_xray`, `fd_triggers_list`; practice-only):
  - **Finding and classifying (D24, D25):** triggers are entities with vtable `trigger.vtbl`. Classes use the game's own tests:
    - blue = `trigger.escape`;
    - red/green exit = a `touch: tell player finalexit` or `tell player (finalexitloadmap` line, open = `trigger.enabled`.
    - research.db's "kind 8 = final exit" is wrong: the constructor sets 8 on every trigger.
  - **Core drawing (D26-D28, D30):** new gfx world boxes (`SubmitWorldBoxes`; depth-tested queued polys or x-ray; 64 px 4444 tiles + vertex colour; drawn before the text panels; 25% when the camera is inside). A real gfx bug found and fixed: a new level's world can reuse the old world's address, so tiles (box and `AlphaSurface`) that settle without a driver handle are now re-added.
  - **Addresses:** 9 address entries (signatures all OK). Spike: probe `probes/triggerprobe.cpp`, probe tests `tests/probes/probe_trigger*.yaml`.
  - **Tests:** `tests/triggers.yaml` PASS. Training 25 triggers / 5 rescue; bank 14 / 1 rescue / 1 exit. The bank exit goes red -> green when 6x `runrescuebot` makes the game open it (screenshots).
  - **Suite:** 28/31. glitch_box_cost / text_panels are VM fps. state_snapshot is a roaming-victim race: 0.16.1 fails it the same way (1/2). Evidence `core/evidence/trigger_boxes/` (README spike verdict, STATUS loop log, feature/, suite_0170.md).
- 2026-10-22 it26: **0.18.0 fire-juggling arrow** (owner: a small ASCII arrow pointing to the last safe spot, with the distance below; started with `fd_juggle 1`). New feature `juggle` (practice-only):
  - **Lock gate:** it evaluates the game's own safe-spot write gate at frame end (flinch / burn >= 0.1 / O2 <= 0.5 / passed out; airborne excluded, owner). It shows `-` while the spot follows the player.
  - **When locked:** an 8-way view-relative arrow to the landing point (spot - 30 X), the horizontal distance, ` ^`/` v` beyond 48 u, and `o` under 16 u. It stays up while passed out.
  - **Look:** 30 px below the screen centre, no background. textdraw gained `CENTER` and `STYLE_TEXT_ONLY` (D31).
  - **Decisions:** D32 gate choice, D33 test hooks.
  - **Addresses:** player.f_safe / f_burn / f_flinch / f_o2 and fire.center. The yaw convention was measured live (`probes/yaw_probe.py`).
  - **Tests:** `tests/fire_juggle.yaml` PASS (3/3 after the fix below).
    - O2 lock: 8 yaws give the computed sector at exactly 30 u.
    - Real fire via the Debug hook `fd_dbg_tofire` (fire nodes move, so it searches): locked and frozen after stepping out, `-` once the meter cools, the arrow on every passed-out frame, `-` after the respawn, selfcheck 0, refused in run.
    - New harness steps: `wait_log`, and `assert_juggle` (it recomputes the arrow from its own memory reads).
  - **Suite:** 31/32. fire_juggle failed `selfcheck = 1` because `fd_dbg_o2` wrote O2 mid-frame. It is now applied at frame end like `fd_dbg_setpos`, and reruns PASS 3/3. glitch_box_cost, text_panels and state_snapshot passed this time.
  - **Also this session:**
    - harness focus fallback for an unattended console (title-bar click);
    - test paths for the owner's `asi-archive/` move.
  - **Evidence:** `core/evidence/fire_juggle_overlay/` (STATUS loop log, yaw_probe.log, glyphs.png, it4/, suite_0180.md).
- 2026-10-22 it27: **0.18.1** fd_juggle observes the safe spot instead of evaluating conditions (owner ruling, D32 rewritten).
  - **Rule:** `-` when this frame's spot equals last frame's position (Think's write copies it), the arrow otherwise. No burn / O2 / airborne logic remains; transitions are logged with those fields as data, and a write anywhere else is logged as `spot written elsewhere`.
  - **Seen live:** walking never freezes; a jump freezes for its whole airborne span and landings blip 1-2 frames with `ground 1` (hidden by the old gate); O2 0.10 holds the spot until it refills to exactly 0.50; the respawn's checkpoint mirror shows as `elsewhere`.
  - **Harness:** `rclick` (JUMP is the right mouse button), `wait_pipe`, `wait_log since_console`, `assert_juggle locked: auto`.
  - **Suite s0181:** 31/32; fire_juggle failed on ~1 s VM frames (asserted before the test hook applied). Fixed by waiting for the hook; 3/3 PASS.
  - **Evidence:** `core/evidence/fire_juggle_overlay/observed/`.
- 2026-10-22 it28: **0.19.0** pause + free camera (`fd_pause`, key P) and letter keys for every feature (owner).
  - **Core service `pause`:**
    - skips GameSession::TickSubsystems (hook on only while paused);
    - pauses the game clock with GameClock::Pause/Resume on the active subtimer (re-paused
      after the game's alt-tab Resume);
    - holds controls.block_input and drops GameSession::OnKey key-downs (Esc opened the
      menu otherwise);
    - caps paused frames at ~60 fps;
    - resumes on leaving the level, including from its own detour (a `loadmap` while
      paused deadlocked before that fix).
  - **camera:** a world-transform override applied at geEngine_RenderWorld (set any
    earlier, the render rewrites it).
  - **textdraw:** HideWhilePaused (the fd_juggle arrow).
  - **Feature `freecam`:**
    - start pose from the game camera (rebuild error 0);
    - W/S/A/D/E/Q, Shift x4, Ctrl x0.25, speed 400 u/s on QPC time;
    - mouse from the game's DirectInput sums;
    - fd_pause practice-only.
  - **Spike:** `core/evidence/freecam/README.md` (probe runs p1-p5).
  - **Tests:**
    - `pause_service` and `freecam`: 15 state fields frozen and proven live while
      running; measured 400/1600/100 u/s exactly along the view; 30.0 deg turn;
      resume dt 17 ms; Esc/W/jump during the pause without effect;
    - `keys_default`.
  - **Keys (D37):** J juggle, H player HUD, G collision HUD with B/Z/X (were F9/F10/F12),
    T/Y/U trigger boxes, K/L teleporter; new `toggle <var>` builtin.
  - DECISIONS D34-D37.
  - Suite s0190: see `core/evidence/freecam/suite_0190.md`.

- 2026-10-23 it29: **key sheet** generated from the source on every build (owner: "built each time so we don't have something that can go stale"; D41, R9). No version bump: rides in the 0.19.0 pre-release.
  - **`tools/docgen.py`:** reads the literals at every registration (variables, commands, practice-only, aliases, default binds, `DescribeKey`) and each file's `// User:` line; `build.ps1` writes `build/<Config>/README.md` every build and stops on anything it can't read. `-Dist` copies the Release `.asi` and its sheet to `dist/`, and every build checks they match. Hand-written notes moved to `dist/CHANGES.md`.
  - **Code:** `binds::DescribeKey` for keys features read themselves (free camera W/S/A/D/E/Q/Shift/Ctrl/mouse, warp HUD B/Z/X, window O), shown by `bindlist`; `exec` registered so `cmdlist` shows it; `// User:` lines on every registering file; glitch's practice-only list as plain calls; `fd_profile` help no longer says "collision overlay".
  - **Tests:** new `docs_registry` (the Debug build's own registry vs docgen, all 23 variables / 30 commands / 4 aliases / 8 binds / 10 feature keys / 10 modules identical); docgen's error cases checked on a scratch copy of the source; `keys_default`, `vars_binds_persist`, `console_basic`, `window_alttab` PASS; `core_loads` PASS on Release. `freecam` failed once (player y moved 0.9 units during the 4 s pause) and passed on rerun; this change doesn't touch the pause path.
  - **Evidence:** `core/evidence/key_sheet/`.
  - **Follow-up (same day):** help texts reworded so every variable states its values (`0 = ...` on each on/off setting, ranges, defaults explained; `bind` lists the key names; `fd_spawns` the spot numbers). The pre-commit hook now runs docgen too: it stops a commit on an unreadable registration, rewrites the tracked source sheet `core/KEY_SHEET.md` and adds it to the commit, and on a `dist/` commit checks `dist/README.md` against the `.asi`. `docs_registry` PASS again; `core_loads` (Release) PASS; `dist/` rebuilt.
- 2026-10-23 it30: **QUICKSTART.md removed** (owner). The generated key sheet (`KEY_SHEET.md`, `dist/README.md`) is the player documentation; the module-author services table moved to `core/CLAUDE.md` ("Core services"); the reading list, Done-means step 4, the new-feature skill and D41 point at the sheet and the code's help texts. Historical evidence STATUS logs that mention it are left as records.

- 2026-10-23: **Recorder feature**: automatic `fdny-trajectory-v1` JSONL output,
  shared entity lifecycle observers and normal-exit flushing. Player/victim data,
  bounds and sampled keys retained; settings documented in the generated key sheet.
  - Checks: Debug/Release compilation, architecture/doc generation and JSONL
    validation. Training capture and playback confirmed; output accepted by the
    trajectory viewer.
  - Test: `tests/recorder.yaml` checks live capture, restarts, opt-out, error recovery
    and JSONL validity through the existing harness. Evidence: `evidence/recorder/README.md`.
- 2026-10-24: **fix (Debug core): `hooks_toggle` crash.** D40's Debug-only layer-order tracker `MarkLayer` appended to `g_order[32]` without a bound; with the frame hooks off, `FrameBegin` stopped resetting it while the RenderWorld detour kept appending, and the overflow corrupted the box lists (`DrawBoxes` read a list at 0x1). Bisected to 549b1fe (D40). Release unaffected. Bounded append; `hooks_toggle` PASS x3, `draw_layers` PASS. Evidence: `evidence/hooks_toggle_crash/`.
- 2026-10-24: **fix (core): single-instance guard (D43).** A second FDNYCore*.asi (releases are `FDNYCore-<version>.asi`) finds the first copy's per-process mapping, logs one WARN naming it and stays inert. The log opens append-only with write sharing; the watermark and the Debug address override find the core by module handle, not by name. No version bump (the paused 0.19.0 pre-release sits in `dist/`). `core_single_instance` PASS; `core_loads`, `addr_bad_signature`, `recorder`, `draw_layers` PASS. Evidence: `evidence/single_instance/`.
- 2026-10-24: **refactor (core): SpawnAtSpot has one owner (D44).** The speedrun feature's spawn-spot trampoline (0x0045D717) moved into the `entities` service as `FilterVictimSpot`, beside the entry hook the recorder's observers use; speedrun's spot logic and defer check are unchanged. No version bump. `speedrun_static_spawns` (now also asserts the entities line and the recorder alongside), `speedrun_reference_deterministic`, `speedrun_rescuebot`, `recorder`, `core_single_instance` PASS. Evidence: `evidence/entities_spawn_filter/`.
- 2026-10-24: **0.19.0** release: VERSION bump (pause + free camera, fd_juggle, letter keys, recorder, the owner fixes, the single-instance guard and the SpawnAtSpot owner). The GitHub release becomes the only download (D45): `dist/` removed with `build.ps1 -Dist` and docgen `check-dist`; release notes moved to the root `CHANGES.md`, whose `## <version>` section the workflow puts in the release notes; the release sheet names `FDNYCore-<version>.asi`. Full suite 36/38: `glitch_box_cost` and `text_panels` are the known slow-VM thresholds (same margins on rerun and on `main` at 3c5aa54). Evidence: `evidence/release_0.19.0/`.
- 2026-10-24: **tests (0.19.0): `glitch_box_cost` and `text_panels` no longer depend on the display's refresh rate (D46).** Their absolute 250-tick / 300-frame thresholds passed at 57 fps (VM console) and failed at 34 (32 Hz remote session) whatever the code did. Now feature on >= 0.9 x off in back-to-back windows (new harness step `assert_sample_ratio`), the warp HUD's CPU box build <= 1 ms, a 60-frame floor; the HUD layer's time per frame is recorded through a new Debug pipe `textdraw_cost`, not asserted (it includes the GPU flush). Both PASS on the 32 Hz display (0.990, 0.953). Evidence: `evidence/fps_independent_tests/`.
- 2026-10-25: **tests: unit tests without the game (D47; suite review item 1).** `core/unit/run.ps1` builds `fdcore_unit.exe` (core sources for the host, `-DFDCORE_UNIT`, `log` stubbed) and runs it plus `core/unit/test_*.py`; CI runs it on every push (`.github/workflows/unit.yml`). First tests: `addr` on fake exe/Genesis images (7 tests), warp HUD parity over 5 fuzz streams through the replay driver now shared with the game (`glitch_replay.cpp`), every `arch_check` rule, and `docgen` (new `notes` command, used by the release workflow; a missing section stops a release). Each checked to fail on a deliberate break. Live `glitch_parity_fuzz` / `glitch_parity_log` PASS after the refactor. Evidence: `evidence/unit_tests/`.
- 2026-10-25: **tests: `state_snapshot` compares with the simulation paused (suite review item 2).** Each of its four comparisons now runs under the Debug pipe `pause on`, so a spreading fire or a roaming bank victim can't move between the core's snapshot and the harness's reads (the known race; last seen on `main` 2026-10-24, v5 1.5 u off). Same fields compared. 3/3 PASS. Evidence: `evidence/state_snapshot_paused/`.
- 2026-10-25: **tests: Release in the suite, and `baseline_route` asserts "behaves the same" (suite review item 3).** A test may name its build (`core: Release`). New `core_release`: the Release core loads, verifies, starts every module and reaches Training with no ERROR lines (PASS 15/15). `baseline_route` now runs Release plus two runs without the core and asserts `compare_runs.compare`'s 7 checks (`expect: baseline_equivalent`): PASS 7/7, fps 55.6 vs 58.8 (-5.5%, limit 10%), laps within 1.0-3.6 u. Evidence: `evidence/release_and_baseline/`.
- 2026-10-25: **tests: `keys_default` narrowed; reference tests on demand (suite review item 4).** `keys_default` now proves the binds themselves: every default key dispatches its command, each press seen (`bind: <key> -> <command>` counted), J's toggle runs both ways; the features' effects are left to their own tests (it re-checked all of them, so a failure didn't say whether the bind or the feature broke). Kept the three checks nothing else has (G off/on after a B hide, only changed binds in FDNYCore.cfg, a practice-only key refused in run). `fov_parity_reference`, `speedrun_reference_deterministic`, `window_parity_reference` (they measure the old plugins) and `glitch_parity_fuzz` (now in the unit tests) moved to `tests/ondemand/` (`run_test.py --ondemand`); none deleted or changed. `keys_default` PASS; `fov_parity_reference` PASS from its new place.
- 2026-10-25: **tests: gaps closed (suite review item 5).** `text_alpha` asserts the opacity, with the world paused so only the text changes (255 vs 96 = 4.5 in the HUD panel, 255 vs 255-again = 0.00). New `crashlog` test: Debug hook `fd_dbg_fault` (a caught access violation) and the report's exception, registers and a stack walk into FDNYCore. R3 extended: every core source names its tests (19 `// Tests:` lines; arch_check enforces it, 2 new unit tests). Stale test descriptions fixed (`hud_player`, `glitch_records`). Unit tests, `docs_registry`, `crashlog`, `hud_player`, `glitch_records` PASS. Evidence: `evidence/test_gaps/`.
- 2026-10-25: **tests: trimmed for efficiency (owner: "The root needs to be engineering efficiency").** The crash-report check moved from a game test with a Debug console command to a unit test (`core/unit/test_crashlog.cpp`, no command, no game); the every-core-file `// Tests:` rule and its 19 lines were dropped; the 8-minute triple `baseline_route` moved to `tests/ondemand/` (before a release). The stack's evidence was cut to READMEs; `core/CLAUDE.md` says evidence is a README with the numbers.
- 2026-10-25: **tests: game sessions shared between tests (D48).** `harness/fdh/session.py` groups tests by their launch conditions; within a group one game runs, and each later test starts with the core reset to its defaults (from docgen), its cfg exec'd and Training reloaded, seeing only its own log lines. New `loadmap:` step waits for the level_load event instead of a fixed 25-45 s. Suite: 15 launches instead of 34, 1577 s instead of ~2900 s; all 34 PASS after two session fixes (`text_alpha` checks the panel via the pipe, `core_release` is `own_session`) and a rebuild (`docs_registry` had a stale Debug build). Harness +~280 / -94 lines.
- 2026-10-25: **feature: replay (D49).** Recorder .jsonl files played back in the level, read the fdny-maps website's way: the trace up to the playback time (one run red -> blue -> green; several ranked green fastest -> red slowest), a see-through box and a yellow facing arrow; `fd_replay_load` / `_clear` / `_list` / `_toggle` (key V), `fd_replay_speed`, `fd_replay_xray`; practice-only. New gfx block: world lines. Unit tests: the reference recording parses to the site's numbers (1391 samples, 33847 ms, gaps before samples 3 and 1390), bad files rejected, colours. Game test `replay`: loads on mbase, plays to the end, 10/12 lines drawn, frames 0.997 of the cleared rate.
- 2026-10-26: **replay: line drawing reviewed against other games' path traces (D50; owner: "Do em all").** Measured first (Debug timer on world lines), then: one projection per point with mitered strips and the glow from the same points, sub-pixel points merged, on-screen strips unclipped; the replay line runs through Douglas-Peucker's samples (497 of 1391 at 0.5 units), 8 units wide held to 3-8 px, half width in jumps, a white tick every second, x-ray as a dimmed copy over walls. Whole-trace view 3351 -> 554 us per frame (2665 -> 525 strips); player view 660-747 -> 471-481 us. Fixed on the way: cleared recordings freed while the world layer still drew them (crash), the glow hiding the line in the world layer, PR #17's unit check (arch_check fixture lacked unit/). Unit 70 checks + 24 Python pass; game `replay`, `keys_default`, `draw_layers`, `docs_registry` PASS; `replay`'s segs floor 100 -> 50 awaits the owner. Evidence `evidence/replay_lines/`. +435 / -96 lines.
- 2026-10-26: **replay: choosing runs without file names (D51; owner: "i like 1-4").** V with nothing loaded plays the map's best finished and latest run; `fd_replay` opens a picker (this map's newest runs, local time, duration, finished / best / latest; pauses; 1-9 load or unload); `fd_replay <text>` loads by part of a name; `fd_replay_ghost` 1 best / 2 latest starts a run with each spawn, on the recorder's clock. The index reads only names and the last 64 KB of each file; "finished" = the same game's next run is on another map. Recorder and replay share one folder rule (`console::GamePath`). Fixed on the way: the replay enabling the player hooks before the recorder hung startup (`keys_default` 0/3 -> 3/3 at module order 110). Unit 81 checks + 24 Python; game `replay_pick` (new), `replay`, `recorder`, `keys_default`, `draw_layers`, `docs_registry` PASS. Evidence `evidence/replay_pick/`. +458 / -39 lines plus the new test, its data and the evidence README.
- 2026-10-26: **replay: line hidden in a hallway; trigger boxes lost after a replay; line raised (owner report, mbase).** gfx clipped strips and box faces to the screen interpolating depth linearly in screen space, so a long segment starting behind the eye took the far end's depth and sank under the floor: clipped on 1/z now. gfx's 4 box-tile slots (one per alpha level, never freed) ran out after the replay and draw_layers, so triggers drew nothing: 16 slots by level. The line and ticks are drawn 36 units above the recorded origin (halfway to the eye, 72 above). Tests: probe replay_straight, replay, replay_pick, draw_layers, triggers PASS in one session (triggers failed there before, also on 2efe5f4); unit pass. Evidence: core/evidence/replay_depth/.
- 2026-10-26: **0.20.0 released (owner: "Bump to .20, merge, release").** The replay (D49-D51) and today's fixes. Full suite: 35/36; triggers failed on a log race (an exit opening on a rescan frame went unlogged), fixed in 5eb3622 and passing on rerun. baseline_route failed 1/7 because the shared session (D48) was still open when the runs without the core started; the harness now closes it (7cdf3de), 7/7. Unit tests pass.

- 2026-10-26: **practice checkpoints:** native checkpoint contacts with elapsed game-clock hints; in-game box placement, move/resize, clipboard and undo/redo. The `fd_checkpoints_*` editor commands use per-mission text files that autosave beside FDNYCore.asi with a backup and apply on restart; failed writes retain their error and wait for an edit or explicit retry. C++ unit suite 148/148; architecture/doc generation and Debug/Release cross-builds pass. Evidence: `evidence/checkpoints/README.md`.
