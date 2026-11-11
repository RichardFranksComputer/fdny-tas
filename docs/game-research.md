# docs/game-research.md

High-level, living understanding of the game itself — not the build
infrastructure (`docs/decomp-vm.md`) and not per-symbol research records (the
Phase 3 SQLite DB, once it exists). This is the master top-down context:
what this game *is*, how its pieces fit together, and the current
strategy for decomposing it. Update it in place as understanding grows
— this file's job is to let a fresh session (or a fresh VM) get
oriented in one read, without re-deriving everything from scratch.

**Since 2026-10-27** the per-symbol research database (`research/research.db`,
`research.html`, `research_report.ps1`) and the VM scripts under `environment/` are
local-only on the VM, not in the repo; references to them below point at the VM's disk.
The old plugins, splitters and Python tools named below (`asi/`, `livesplit/`,
`tools/collision_overlay`, the spawn tools) left the repo too; git history keeps them. The Ghidra labels themselves are in
`ghidra/`.

---

## Prime directive (from the project owner)

**The deliverable is clarity of labeling.** A fully and accurately
labeled Ghidra database — function names, parameters, locals, globals,
structs, enums, comments — such that a later agent can answer a
functional question about any game system ("how does the rescuebot
choose a victim?", "what ends a level?", "how is player movement
integrated?") by *reading named decompiled code*, not by re-deriving
structure every time. Renames / retypes / comments / struct creation
against the live Ghidra project are the work product here, not a
side effect — do them in labeled batches and record the reasoning.

---

## Labeling conventions (in use)

- **Functions with an identifiable C++ class** → `Class::Method`
  (Ghidra namespace form). Class name taken from the entity type string
  (`RCS_PlayerHose` → `PlayerHose`), the `T`-prefixed class in assert
  text (`TScriptTrigger` → `ScriptTrigger`), or the source file
  (`RescueBot.cpp` → `RescueBot`).
- **Free / unclassed functions** → `subsystem_VerbNoun`
  (`level_LoadMap`, `console_Exec`). Subsystem = the source directory.
- **Globals** → `g_<name>` when understood (`g_gameCtx`), left as
  `DAT_*` until then.
- **Every rename gets a plate comment** with: one-line purpose, the
  key evidence, `file.cpp:line` if an assert/trace string pins it, and
  a confidence word (high / medium / low). `[[g_name]]` cross-links a
  global that has its own note.
- **Confidence discipline**: only `high` renames drop the `FUN_`/`DAT_`
  entirely on the first pass. `medium` gets the name + a `?` suffix in
  the plate note's purpose line. `low` → comment only, no rename.

## Identity

| Fact | Value | Source |
|---|---|---|
| Title | *F.D.N.Y. Firefighter: American Hero* | MobyGames #36473, box art |
| Platform | Windows PC (also released on PS2 as a sibling SKU) | MobyGames |
| Release | 2002 | MobyGames, retail listings |
| Developer | **Mekada** | web search (MobyGames listing) — *medium confidence, page 403s to direct fetch* |
| Publisher | Activision Value | retail listings |
| Engine | **Genesis3D v1.1** (WildTangent, 1999; open source) | **confirmed** — `GENESIS.DLL` imports: `geEngine_CreateWithVersion`, `geWorld_Create`, `geActor_*`, `geCamera_*`, `geVFile_*`, `geWorld_Collision`, … |
| Internal codename | **"Everblaze 2"** — source tree root `G:\Everblaze2\Src\`, game code under `everblaze2\src\game\` | assert / `__FILE__` strings in `.rdata` |
| Audio | FMOD 3.x (`FSOUND_*` from `fmod.dll`) | imports |
| Input | DirectInput8 | imports (`DirectInput8Create`) |
| Local copy | `C:\fdny-decomp\FDNY SP 1.1\FDNYFirefighter.exe`, 1,060,864 bytes | — |

### Not game code (community add-ons shipped in the folder)

- `D3D8.dll`, `D3D9.dll`, `DDraw.dll`, `D3DImm.dll`, `dgVoodoo*.exe/.conf`
  — **dgVoodoo2**, a DirectX→modern wrapper added to run the game on
  current Windows.
- `dinput8.dll` (2.3 MB), `*.asi`, `alttabfix.asi`,
  `FDNYFirefighterAmericanHeroFOVFix.*` — **Ultimate ASI Loader**
  (dinput8 proxy) plus community `.asi` mods (FOV fix, alt-tab fix).
- The real engine/game DLLs are `Genesis.dll` (engine) and `fmod.dll`
  (audio). `Genesis.dll` is open source — prefer the published headers
  over decompiling it.

## Binary shape

- MSVC-built Win32 PE, x86 32-bit, image base `0x00400000`.
  `.text` `0x401000–0x4dbfff` (~0x db000). 5807 functions, avg ~120 B.
- **Assert-heavy build.** `.rdata` is full of `dir\File.cpp` path
  strings and human-readable assertion messages. `__FILE__` / `__LINE__`
  pin large numbers of functions to an exact translation unit — this is
  the single biggest labeling lever (see Strategy).
- Entity classes are registered with the Genesis3D entity system
  (`geEntity_GetUserData` + per-class userdata size). Class names appear
  as strings: `RCS_BoundsTrigger`, `RCS_PlayerRescue`, `RCS_VictimRescue`,
  `RCS_CamRescue`, `RCS_PlayerHose`, `ARCS_FireRescue`, …
- Dev console with `@`-prefixed commands (`@setrescuebot`,
  `DBG_ACTIVATE_RESCUEBOT`, `runrescuebot`, `norescuebot`, `botrescue`).

## Subsystem map

Derived from the 54 distinct `.cpp` source-path strings + imports.
Directory = subsystem.

### Foundation — `BluelAPI\` (`G:\Everblaze2\Src\BluelAPI\`)
In-house C++ runtime, statically linked into the exe.
- `Error\CallStack.cpp`, `main\GenErr-Everblaze.cpp` — error / callstack / assert plumbing
- `Stream\` — `ByteStream.cpp`, `StdStreams.cpp`, `Win32File.cpp`
- `String\Rope*` — string type (`Rope`, `Rope_RatStr`)
- `Container\SArray.h` — dynamic array template
- `Modules\RichBinary\` — a serialization container format: readers/writers
  for BlockFile / Image / Stream / Text. Likely backs `.rrf` / `.rb` / `Data\`.

### Bootstrap & managers — `main\`
- `main.cpp` — WinMain, window/driver setup, top-level loop
- `evergame.cpp` — top-level game object ("Ever" = Everblaze)
- `LevelManager.cpp` — level load/unload/transition
- `PlayerManager.cpp` — profile save/load: `saves\player%i.rb`,
  `EVERBLAZEPLAYERFILE`/`…DATA`, `EVERPLAYER_NAME/_LASTLEVEL/_SCORE`
- `config.cpp` — `FDNYFireFighter.cfg`
- `console.cpp` — dev console / command dispatch

### Input — `main\IO\`
- `DirectInputProvider.cpp` — raw DirectInput8 device wrapper
- `GameControls.cpp` — action mapping (candidate home of movement-input handling)

### Shared entities — `sharedents\`
- `BModelEnt.cpp` — brush-model entities
- `BoundsTriggerEnt.cpp` — **trigger volumes / level triggers** (`RCS_BoundsTrigger`, "Making zone of …")
- `MiscEmitterEnt.cpp` — particle emitters
- `PropModelEnt.cpp` — static/dynamic props

### Rescue system — `rescue\`  ← owner priority
- `RescueBot.cpp` — **the rescuebot** ("RESCUEBOT: Rescuing / No eligible victims / Already rescuing someone / Exceeded maximum of / Victim dropped off")
- `PlayerRescueEnt.cpp`, `VictimRescueEnt.cpp`, `VictimAltSpotEnt.cpp`,
  `CamRescueEnt.cpp`, `FireRescueEnt.cpp`
- `Items\ItemAnimSeq.cpp` — pickup / carry animation sequencing
- Resource: `data\resdefs\rescue.rrf`. Win/lose states `RescueWin` / `RescueLose`.

### Fire hose — `hose\`  ← owner priority
- `PlayerHoseEnt.cpp` — hose gameplay entity (`RCS_PlayerHose`)
- `stream\HoseGfx.cpp` — water-jet visuals

### Level scripting VM — `script\`  ← owner priority (triggers)
- `scriptengine.cpp`, `scriptparser.cpp`, `scriptfuncinfo.cpp` — bespoke scripting language
- `scripttrigger.cpp` — `TScriptTrigger` (has `SetLightTable`, light types 1–19)
- `scriptactor.cpp` — script-controlled actors
- Sample script verbs: `tell player escape`, `tell player finalexit`, `tell player (finalexitloadmap …`

### Gameplay primitives — `basic\`
- `ActorAnimator.cpp` — skeletal-animation wrapper over `geActor`/`geMotion`
- `MapZone.cpp` — map subdivision / zones
- `PlayerStats.cpp` — score/rescues/time (`Longest Rescue Time`, `Rescues`, …)
- `RadioChatter.cpp` — radio voice-over scheduling
- `viewrhythm.cpp` — view bob / sway
- `Region.h` — spatial regions

### ROC subsystem — `ROC\`
Sizeable actor/AI framework with its own resource pipeline. Meaning of
"ROC" not yet known (in-house vs licensed).
- `Renderer\ActorInfo.cpp`, `Renderer\ROCRender.cpp`
- `Engine\engine.cpp`
- `Parsing\resparse.cpp` — resource-definition parser (`.rrf`?)
- `Message Mapping\mmap.cpp`
- `ROC AI\Entity\` — `CreateStruct.cpp`, `ModelEntity.cpp`, `WorldEntity.cpp`

### Sound — `sound\`
- `SoundEngine.cpp`, `SoundSystem.cpp`, `SoundSample.cpp` — FMOD 3 wrappers

## Strategy

Top-down, labeling-first (see Prime directive).

**Three free-labeling levers, apply before hand analysis:**
1. **Assert strings.** For each `dir\File.cpp` string: xref it → the
   functions that reference it belong to that translation unit → name
   the cluster after the class/file. Assertion message text often names
   the method and its invariants outright.
2. **Genesis3D headers.** Every `ge*` import is public and documented
   (GitHub `RealityFactory/Genesis3D`, `genesis3d.com/old/docs`).
   Recover `geVec3d`, `geXForm3d`, `geWorld`, `geActor`, `geEntity`,
   `geMotion`, `gePath`, `geCamera`, `geBitmap`, `geBody` layouts and
   apply them as Ghidra types — resolves a large fraction of
   engine-facing code with no guessing.
3. **FMOD 3 + DirectInput8 public APIs.** Same treatment for `sound\`
   and `main\IO\`.

**Order of work:**
1. Bootstrap + frame loop (`main.cpp`, `evergame.cpp`) — get the
   skeleton and the per-frame update/render call tree named.
2. Entity-system base classes + Genesis3D entity registration — so
   every `*Ent.cpp` class slots onto a known vtable/userdata shape.
3. Owner-priority systems: `rescue\` (RescueBot first), `hose\`,
   `script\` + `sharedents\BoundsTriggerEnt`, `main\IO\` +
   movement integration.
4. Remainder by file cluster: `ROC\`, `basic\`, `sound\`, `BluelAPI\`.

**Checkpointing:** write findings here and (once it exists) to the
Phase 3 DB continuously — a labeled batch + its rationale, not a
summary at the end.

## Open questions

- **Player locomotion lives where?** No `movement.cpp` / `PlayerEnt.cpp`
  assert string surfaced. Candidates: `evergame.cpp`, `PlayerManager.cpp`,
  `GameControls.cpp`, or a `ROC` model entity. Resolve early — it's an
  owner-priority system.
- **What is "ROC"?** Own resource parser + AI entity model + message
  mapping. In-house or licensed middleware? Determines how deep to label it.
- **Import `Genesis.dll` into the Ghidra project?** Current lean:
  source-first from the open-source engine; import only if game behavior
  hinges on engine internals.
- **Container formats** — `.rrf` (resdefs), `saves\*.rb`, `texture.dat`,
  the `Data\` tree. Likely RichBinary-based. Candidate for a dedicated
  self-contained format pass (cf. `docs/decomp-vm.md` "save/load format first").
- **Exact build.** Which retail/patch build is this 1,060,864-byte exe?
  Folder name `FDNY SP 1.1` implies a 1.1 ("SP"?) revision.

## Key findings (load-bearing)

- Engine is **Genesis3D** and it's open source — engine-facing structs
  and APIs come from headers, not decompilation.
- The exe is an **assert-heavy debug-symbol-adjacent build**: source
  file paths, class names, and method invariants are visible as
  strings. Labeling is unusually well-scaffolded.
- `rescue\`, `hose\`, `script\`, `sharedents\BoundsTriggerEnt` are each
  cleanly delimited by source file — the owner's target questions map
  directly onto known translation units.

## Progress log

> Granular per-symbol records now live in `research/research.db` (Phase 3).
> `research/research.html` is the generated view
> (`environment/scripts/research_report.ps1`, `-Serve` for live). This log
> keeps only the session-level narrative.

### 2026-08-27 — session 1: infra + bootstrap spine
**Infrastructure built:**
- `research/research.db` — Phase 3 store. Schema
  `environment/scripts/research_schema.sql` (5 tables), created by
  `environment/scripts/init_research_db.ps1` (seeds the task queue).
- `environment/scripts/research_report.ps1` — regenerates
  `research/research.html` (self-contained; bare / `-Watch N` / `-Serve :8899`).
- `config.ps1` — `$ResearchDir/$ResearchDbPath/$ResearchHtmlPath/$ResearchSchema/$SqliteExe`.
- sqlite3 3.53.4 installed on the VM. **Host TODO:** fold sqlite +
  `init_research_db.ps1` into `provision.ps1` / `.pkr.hcl` (no
  `environment/packer/` on the VM copy).
- `docs/decomp-vm.md` — Phase 3 marked built, Phase 4 (viewer) pulled forward + built.

**Labels applied (17 symbols — see the DB / HTML for detail):**
- Bootstrap spine, all high-confidence:
  `WinMain` → `EverGame::Run` (vtbl+0xA4) → `EverGame::CreateSession`
  (vtbl+0x84) → `GameSession::Construct` / `GameSession::Init` /
  `GameSession::Activate`.
- Plus `CheckGameCD`, `PlayFullscreenMovie`, `MainWindow::Create`,
  `MainWindow::Init`, `main_CheckSingleInstance`, `Rand_Seed`, `Rand_Below`.
- Globals: `g_gameCtx` (`0x00503a78`, service locator — confirmed:
  `GameSession::Init` publishes subsystems into `+0x08..+0x24`),
  `g_mainWindow` (`0x00503a70`, provisional), `g_skipIntroMovies`
  (`0x00502a78`, provisional).
- `RescueBot::BeginRescue` (`rescue\RescueBot.cpp`) — documented.

**Findings:**
- Build pinned: **v2.03, compiled "Mar  8 2002"** (WinMain strings).
  Publisher Activision Value / dev Mekada re-confirmed via
  `Data\Movies\ActiValue.avi` / `Mekada.avi`.
- `GameSession` is a 0x6f4-byte per-run object at `EverGame+0xB4`;
  `GameSession::Init` builds six subsystems (sizes
  `0x98/0x1044/0x48/0x550/0x4c/0x4c`) and loads
  `data\resdefs\{sound.rrf, thegame.lev, victims.srf}`.

### 2026-08-27 — loop cycle 0: frame loop
- **Frame loop found:** `MainWindow::RunMessageLoop` (`0x00499410`) — a
  PeekMessage idle loop. Per idle frame: `EverGame::FrameUpdate`
  (vtbl+0x74) then `EverGame::FrameRender` (vtbl+0xA0). `WM_ACTIVATEAPP`
  pause via `geEngine_Activate`. Called from WinMain *after*
  `EverGame::Run` (which only registers the game +
  builds the session). `MainWindow::SetGameClient`, `g_appRunning` also
  named. Hypothesis marked **confirmed**.

### 2026-08-27 — loop cycle 1: rescue subsystem (2 subagents)
- **RescueBot fully mapped** — 7 methods (`Init`, `Think`,
  `TryStartRescue`, `BeginRescue`, `ArmChatterTimer`, `DebounceTimers`,
  `ArmRescueAttemptTimer`), plate comments on the three big ones, and a
  Ghidra **`RescueBot` struct** (`/everblaze`, 28 bytes: dropoffTimer /
  currentVictim / foundLines / urgeLines / chatterTimer / attemptTimer /
  enabled).
- **RescueBot is embedded at `GameLevel+0x340C`**, where `GameLevel =
  *(g_gameCtx+0x30)` — the 0x3580-byte scene object (vftable
  `0x004e1c48`) that also owns the child-entity list (`+0x176`),
  `RescueHud` (`+0x2160`), mission state (`+0x18c`), victim-carry cap
  (`+0x3530`), subtitle sink (`+0x5f4`). *This corrects the cycle-0
  BeginRescue note.*
- **New subsystem found: `RescueHud`** — `0x004558f0–0x00455f60` is a
  separate TU (mission HUD / results screen; strings `RC_VictPick`,
  `RC_ResLvlWin`, `RescueWin/Lose`). 15 widget funcs named `RescueHud::*`;
  class ctor `FUN_004576b0`.
- Container/runtime glue named: `SArray_*`, `EntityIter::*`,
  `EntityRef_*` (in `BluelAPI`).
- Victim eligibility (`TryStartRescue`): `Entity+0x48` type in {4,7},
  `Entity+0x1fd`==0, LOS ok, dist **>= 600.0** (min/max reading flagged
  for recheck).

### 2026-08-27 — loop cycle 2: frame internals + script VM + hose (3 subagents)
**Cycle 2a — frame internals.** `EverGame::FrameUpdate`/`FrameRender` are
**GUI-tree iterators, not the world tick.** EverGame is the root/desktop
widget; each frame it walks a child-widget linked list (head
`EverGame+0x7c`), calling each child's `vtbl+0x74` (Update, if visible
`+0x4c`) / `vtbl+0x70` (Paint), reaping children flagged `+0x4d`. Widget
interface: `+0x18/+0x1c/+0x20` mouse, `+0x6c` OnDestroy, `+0x70` Render,
`+0x74` Update, `+0x78` HitTest, `+0x9c` Close. Session-state transitions
via `GameSession::PumpPendingState` (vtbl+0x60 tail call). 3-vtable ctor
chain: `CtorBase`(0x4e3ef0) → `CtorMid`(0x4e3e40) → `Construct`(0x4df958).
**The real world sim is an unidentified child widget — priority 1.**

**Cycle 2b — script VM.** A line-oriented **string interpreter**, not
bytecode. `ScriptTrigger::LoadScriptFile` reads `Data\Scripts\<name>`
line-by-line; `ScriptParser` comma-splits with bracket nesting into an
N-D token matrix; `ScriptActor::ExecCommand` dispatches the first token
through `HandleBaseVerb` (9 base verbs) then a virtual subclass handler
(`ScriptTrigger::HandleVerb`, ~45 verbs — `if/set/tell/loadmap/playsnd/
quake/setlighttable/...`), both hand-written `stricmp` if-chains.
Cross-actor: `tell <name> <cmd>` → `ScriptEngine::FindActor` (linear
name scan of `g_scriptEngine`) → re-enter `ExecCommand`. Events: a
name→commandlist map per actor + `FireEvent`. **`BoundsTrigger`
(`RCS_BoundsTrigger`)** fires `touch`/`touching`/`untouch` each frame
from the GameLevel zone system's entered/inside/left entity lists;
default bind `touch → "tell player escape"`. ~24 funcs named.

**Cycle 2c — fire hose.** An **on-rails mini-game** entity
(`RCS_PlayerHose`, 0x1A88). View auto-advances a designer spline of
`HosePathPoint`s; at hotspot nodes you stop, aim (`STEER_PITCH/YAW`) and
spray (`MOUSE_LB`) to douse an `RCS_WindowFire` cluster before a timer.
`PlayerHose::Think` state machine: 3 Advance → 5 DrainWait → 4 AtHotspot
→ (next | 6 Win/Lose). Water (`hose\stream\HoseGfx.cpp`) is a **ballistic
projectile trail** — `HoseTrail::Eval` integrates `p = o + v·dt +
½g·dt²`; `HoseGfx::RebuildGeometry` updates 25 cached `geWorld_AddPoly`
quads via `gePoly_SetLVertex` and ray-tests each with `geWorld_Collision`
for the impact. ~42 funcs named. Cross-check: `PlayerHose::OnSpawn` calls
`ScriptActor::Init(this+0x50,"Player")` — the hose is itself a script
actor named "player".

**Totals after cycle 2: 134 symbols** (hose 42, main 28, script 24,
rescue 24, BluelAPI 11, sharedents 5).

### 2026-08-27 — loop cycle 3: services + sound + player/movement (3 subagents)
**`g_gameCtx` (0x00503a78) fully mapped:** `+0x04` renderWorld (ROC
renderer / `geWorld` host), `+0x08` `ROCSoundSystem`, `+0x10`
`EntityHandleTable` (512 generational), `+0x14` `ROCWorldEntityMgr`,
`+0x1c` `ROCMessageMapper` (64-slot), `+0x20` `EntityListB`, `+0x24`
`EntityList` (spawned), `+0x30` the `PlayerRescueEnt` singleton.

**Sound:** name-string lookup (never ids). `ROCSoundSystem::Play` →
`FindSample` (dict at `+0x08`, from `sound.rrf` via `ROCResourceParser`,
tags `ROC_SOUND_BUFFER2D/3D`) → `TSoundSample::Play`. `.mp3` streams,
else static buffer. `TSoundEngine::Open` = `_FSOUND_Init(rate,512,0)`.
4 volume categories. ~16 funcs named.

**"GameLevel" is `PlayerRescueEnt`** (ROC class `RCS_PlayerRescue`,
`rescue\PlayerRescueEnt.cpp`, 0x3580 B) — *both* the player character
*and* the mission controller, one object at `*(g_gameCtx+0x30)`.
Inheritance `WorldEntity ← ModelEntity ← BModelEnt ← PlayerRescueEnt`;
embedded `ScriptActor` at `+0x140` (vtbl `0x4e1c40`: `[0]` Think, `[1/4]`
OnScriptCommand), embedded `RescueBot` at `+0x340C`. `Construct`/`Init`/
`OnScriptCommand`/`EndMission`/`FrameUpdate` named + plate comments.

**MOVEMENT (owner priority) — fully mapped.** `PlayerRescueEnt::Think`
(`0x00452570`, reached via `FrameUpdate` vtbl+0x14 → `ScriptActor::Tick`):
input (`GameControls::GetAxis`/`IsHeld`, names `MOVE_*`/`STEER_*`) →
local wish vec (fwd ±1.0, strafe ±0.75) → rotate to world + blend into
velocity `+0xFC` (authority 0.95 grounded / 0.2 air) → platform slide vs
contact `SArray +0x3564` → gravity `accel.Y=-1000`, drag `(1000,0,1000)`
→ `Entity::IntegrateVelocity` (`vel+=a·dt`, `disp=v·dt+½a·dt²`) → `pos
+0x4C += disp`. Speed caps 400 / 260 air / 800 spectator; jump +400 vy;
crouch AABB h=85. `Verb_Jump/Crouch/UnCrouch/DoFootstep/FinalExit/
Escape`, `GameControls::*`, `PlayerStats::*` named. Full pipeline in the
`0x00452570` plate comment.

**Totals after cycle 3: 198 symbols** (rescue 47, hose 42, main 31,
script 25, sound 16, BluelAPI 16, ROC 13, sharedents 5, basic 3).

### Standing directive (owner, 2026-08-27)
The named priorities (rescuebot, triggers, movement — all now done) are
the *start*, not the finish. The loop continues autonomously to a
**fully labeled decomp**, picking targets by the `docs/decomp-vm.md` Phase 9
heuristic. Standing task in `research.db`. See memory
`full-coverage-labeling-goal`.

### 2026-08-28 — loop cycle 4: rescue entities + ROC layer + entity base + basic/ (3 subagents; wave re-run after a session-limit stall)
**Rescue entity subsystem complete.** `VictimRescueEnt` (`RCS_VictimRescue`,
0x22c): full lifecycle mapped — Construct → Spawn (resolve type in
`VictimRegistry` / `victims.srf`) → SpawnAtSpot → ValidateSpawn
(obstacle + reachability) → claimed (bot: state=6 + `"botrescue"`;
player proximity ~60u: `+0x1fd`=1, state=5) → delivered → counted →
exits enabled. `VictimAltSpotEnt`, `FireRescueEnt` (scripted objective
fire: `FloorMe`, `GotoPathPoint`), `CamRescueEnt` (scripted cam),
`ItemAnimSeq` (carry-anim splines) all named.
**Correction:** `RescueBot`'s `[0x48]` filter reads `victim+0x120`
(FSM *state*, 4/7), not `Entity+0x18` (entity *kind*: 1=player,
2=victim, 6=obstacle, 8=exit).

**ROC layer mapped.** `ROC\Engine` (`MainWindow`: `geEngine`
bring-up + `WndProc` + message loop), `ROC\Renderer` (`ROCRender` +
per-level `TWorldResources` = `geWorld` + actor/texture/bitmap tables;
`RenderFrame` = `geEngine_BeginFrame` → per-world `vtbl[0x10]` draw →
`EndFrame`), `ROC\Parsing` (`ROCResourceParser` — **`.rrf` grammar
documented**: `ROC_MODEL/BITMAP/ALPHA_TEXTURE/SOUND_BUFFER2D/3D`),
`ROC\Message Mapping` (`ROCMessageMapper` 64-slot `msgId→handler`),
`ROC\ROC AI\Entity` (`Entity ← WorldEntity ← ModelEntity` base +
`EntityCreateStruct` geEntity-UserData bridge). **Entity spawn
pipeline**: `ROCWorldEntity::LoadWorld` → iterate BSP entity set →
enqueue on `EntityList` → `DrainAndDestroy` builds each + two passes
(`vtbl+0x20` PostConstruct, `vtbl+0x24` Spawn).

**Entity base + `basic/`.** `BModelEnt` (brush-model base; `Think` =
`geWorld` sweep + commit pos; `+0x138`=16.0 push force) + its
`_Liftable` subclass. `PropModelEnt` (`RCS_PropModel`, valve),
`MiscEmitterEnt` (`RCS_MiscEmitter`), `PathPointEnt` (`RCS_PathPoint`).
`ActorAnimator` (geActor/geMotion wrapper, at PlayerRescueEnt+0x198),
`ViewRhythm` (head-bob + landing dip), `MapZone`/`MapZoneTracker`
(enter/inside/leave sets — drives `BoundsTrigger`), `TChatterSet`
(shuffled radio-VO bag), `PlayerStats` (`std::map<string,StatEntry>`,
rule-aware `SubmitStat`; loaded from a stat-def config, saved via
BluelAPI RichBinary `EVERBLAZEPLAYERFILE`).
Base entity vtable slots decoded: `+0x18` GetTransform, `+0x1c`
GetTypeId, `+0x20` PostConstruct, `+0x24` Spawn, `+0x2c` Unlink,
`+0x38` Think, `+0x64` IntegrateVelocity.

**Totals after cycle 4: 299 symbols** (rescue 79, ROC 51, hose 42,
main 32, script 25, basic 24, sound 16, BluelAPI 16, sharedents 14).

### 2026-08-28 — loop cycle 5: BluelAPI + main managers + render architecture (3 subagents)
**BluelAPI foundation.** `String` = 1-pointer COW handle (rep →
`g_emptyString` or heap RatStr block `{refCount,length,bufSize,data}`;
the 5-store `g_emptyString` init is a "String local constructed here"
marker). `SArray` (data/count/cap/dtorCtx/elemDtor/granule). `ByteStream`
+ `Win32File` (`CreateFileA`) + `XorByteStream` (obfuscated data) +
`SubStream`. **`RichBinary`** = the save/editor format:
`"RICHBINARY"` + 0x17-byte header; each block = one LE u32
`{size:29, type:2, hasName:1}`; sizes back-patched on `EndContainer`;
block names are numeric tag ids via a global registry. `NameTree` =
serializable string-keyed BST (backs `GameControls` + script event maps).

**`main\` managers.** `GenErr::Dispatch` (`0x00422060`) = the central
error handler every failed assert funnels into (Ghidra label stuck at
`SArray::OobFail` — see its plate comment). `LevelManager::LoadLevelDefs`
parses `data\resdefs\thegame.lev` → `TLevelInfo` list; **all map
switches funnel through `GameSession::RequestLevelChange`**
(`0x0042a260`, sets a pending byte + map name + `rescue.rrf`/`hose.rrf`;
BSP load deferred to the frame loop). `listmaps/loadmap/loadhosemap`
are `ScriptTrigger` verbs; `finalexit*` are `PlayerRescueEnt` verbs.
`PlayerManager`: `saves\playerN.rb` via `RichBinary` — `root → Player {
Name, LevelIndex (high-water), DifficultyLevel, Score* }`.
`Config`: `FDNYFirefighter.cfg` INI (`[DEBUG]` flags gate the console +
GenErr logging). **`Console` has no parser** — `OnKeyDown` pipes every
entered line to `ScriptEngine::ExecLine`; every console command is a
script verb, "cvars" are script-engine vars (`DAT_00503044`).

**Render architecture.** Per-view `Camera` wrapper (`geCamera*` +0x5c,
viewport rect, `projScale = tan(fov·π/360)`) + singleton
`RenderManager`/`ROCRender` (active `geCamera*` +0x170, two scene
slots). `RenderManager::RenderView` → `geEngine_RenderWorld`;
`WorldToScreen` = `geCamera_Transform`→`geCamera_Project`. Entities get
a `RenderInstance` (0x78, holds `geActor*`); 2D HUD = the `Sprite`
class (geLVertex[4] quad, virtual-res mapped via screen-metrics
`0x004ffad8`); particles = `ParticlePool` (0x30) of `ParticleEmitter`
(0x168). **Correction:** `FUN_004a13a0` etc. = `SoundCue`
(`ROCSoundSystem::Play`), not an "anim object".

**Totals after cycle 5: 420 symbols** (ROC 81, rescue 79, BluelAPI 67,
main 63, hose 42, script 30, basic 25, sound 19, sharedents 14).
Named functions in Ghidra ≈ 3500 / 5807 (many of the rest are CRT/lib
boilerplate).

### 2026-08-28 — loop cycle 6a: front-end menu + mission HUD + player tools (subagent; wave re-run after another session-limit hit)
**Front-end.** `MenuManager` embedded at `GameSession+0x3c8` (ctor from
`GameSession::Init`, parses `menu.rrf`); 10 `MenuScreen` subclasses
(Main / SlotList / Title / Options / level-select List / Credits /
Splash), nav stack, named `MenuMouse{Down,Click,Up}` sinks;
`MenuManager::GotoScreen` is the only screen switch. Menu picks a
`TLevelInfo` → `GameSession+0x688` and the session acts on the pending
flag `+0x68c`; in-level transitions use the `loadmap`/`loadhosemap`
script verbs → `GameSession::RequestLevelChange`. `GameSession::SetAudioState`
(`0x0042a730`) is the music/duck director. **Correction:** `0x0042d480`
(earlier `GameSession::Activate`) is really `MenuManager::Activate`.

**Mission HUD.** `RescueHud` at `PlayerRescueEnt+0x5f4` (ctor
`0x004576b0`). `OnEvent(evId)`: VictPick(1) / VictDrop(2) / carry(3,4)
/ tool-select(5) / tool-acquired(6) / bonus(7). `Update()` drives the
O₂ gauge + timer + compass and fires `RC_ResLvlWin/Lose` +
`RC_OxygenLow`. `MissionSummary` = the end-of-level panel.

**Player tools + oxygen.** Carried-tool `SArray` at
`PlayerRescueEnt+0x3564` (active idx `+0x357c`): **1 axe, 2
extinguisher, 3 IR goggles, 4 hose** (`touchhose` only). `equip`
creates the tool object; `select` only switches; `toolcmd` forwards a
sub-command to the tool actor; `ACTION` control edges route to the
active tool's `vtbl+0x14/+0x18/+0x1c`. **Oxygen:** budget
`PlayerRescueEnt+0x343c` = `TLevelInfo+0x10` (def 600) × difficulty
(`*(g_session+0x6e4)+0x2c`, 0/1/2 → ×1.5/×1.5/×1.0); O₂ fraction
`+0x3450`; `RC_OxygenLow` edge-fires below 0.25; `oxyon`/`oxyoff` set
the regulator next-breath at `+0x3458`. All `PlayerRescueEnt` script
verbs now named (jump/crouch/climb/push/escape/oxy/equip/select/
touchhose/toolcmd/hint/setashdensity/addbonus/printstats/reset/
finalexit*).

**Totals after cycle 6a: 461 symbols** (rescue 106, ROC 82, main 76,
BluelAPI 67, hose 42, script 30, basic 25, sound 19, sharedents 14).

### 2026-08-28 — loop cycle 6b: render-submit + particle sim + audio internals + RichBinary readers (subagent)
**Entity actor draw path.** `*(g_gameCtx+4)` = `RenderManager`/`ROCRender`
(confirmed again). Per frame `ROCRender::RenderFrame` walks each
renderable: `vtbl[1]` `PrepareForRender` composes the owner
`ModelEntity` transform (`geXForm3d_Multiply` of pos + orientation) and
calls **`RenderInstance::SetXForm`** → `ApplyPoseAndScale` →
`geActor_SetScale` + `geActor_SetPose(actor, motion, time, &xform)` —
so the entity matrix reaches `geActor` as `SetPose`'s **4th arg**, not
via `geActor_Render`. Then `vtbl[4]` `RenderInstance::Render` →
`geActor_Render`.

**Particles.** `ParticlePool` (0x30, `SArrayPtr` of `ParticleEmitter*`,
free slot = byte `+0x30`==0). `ParticleEmitter` (0x168, derives
`ParticleNode`). Per frame: `IntegrateMotion` (semi-implicit Euler pos/
vel/accel + per-axis linear damping), `AdvanceSpawn` (keyframe cursors
+ lifetime cull), `Render` (`geWorld_AddPolyOnce` billboard, poly mode
`+0xb0`, `geWorld_GetLeaf`/`MightSeeLeaf` cull). Fields: pos `+0x5c`,
vel `+0x7c`, accel `+0x88`, damping `+0x94`, colour `+0xb4`, vertex buf
`+0xd8`.

**Sound wrap internals.** A `SoundWrap` (= what a `SoundCue` is): `+0x00`
FMOD channel, `+0x10` world pos, `+0x14` category id, `+0x18` 3D-mode/
owner, `+0x20` name. `ROCSoundSystem`: SoundWrap list `+0x00`, category
volumes `+0x18`, deferred queue `+0x3c`. `FindSample` = binary tree by
name (from `sound.rrf`). Per frame `Update` pauses finished one-shots;
`UpdateListener` pushes the camera as the FMOD 3D listener.

**Anim events** are game-side (geMotion event API not imported). The
name table `0x004e18ec` (`FootStep1-4`, `Breathe`, `OxyRelease`,
`PassOut_O2/Fire`, `LandingImpact`, `GrabItem`, `ValveHookUp`, …) binds
via `MessageSink::Subscribe` to a list on `*(g_gameCtx+8)` — the same
singleton that owns `SoundCue`s + the FMOD listener. `AnimSeqPlayer::
Advance` (undefined code) posts the name when playback crosses a
keyframe.

**Totals after cycle 6: 508 symbols** (ROC 108, rescue 106, main 76,
BluelAPI 75, hose 42, sound 31, script 30, basic 26, sharedents 14).
~3600 / 5807 Ghidra functions named.

### 2026-08-28 — loop cycle 7: mission logic + fire/ash sim + create_function sweep (2 subagents)
**MAJOR CORRECTION — no mission controller.** `*(g_session+0x6e4)` is
the active `TPlayerInfo*` (profile: `+0x2c` difficulty 0/1/2, `+0x30`
`PlayerStats`). The **entire per-mission ruleset lives inside
`PlayerRescueEnt`**:
- **Mission timer `+0x343c`** = `(TLevelInfo+0x10` or `600s) ×
  difficulty` (idx 0/1 → 1.5, idx 2 → 1.0), drains by `dt` each frame.
  Hitting 0 → outcome `+0x3438 = 2` → `RC_ResLvlLose`. **This is the
  only true level loss.** Difficulty scales *only* this timer.
- **O₂ `+0x3450`** drains −0.05 per 3000 ms (~60 s, difficulty- and
  smoke-independent). 0 → `PassOut(0)`.
- **Burn `+0x3538`** += `dt·1.2·severity` from fire msg `0x7531`. >1 →
  `PassOut(1)`. Cooled −`dt·0.1` when safe.
- `PassOut` → state 6 → ~4 s fade → `Verb_Reset`: **respawn at the
  rolling checkpoint `+0x3400`** (written every frame by
  `UpdateCheckpoint` while safe), O₂ refilled, carried victim dropped
  — *not* a level loss. No HP scalar exists. Hard mode: an unforced
  pass-out spawns a rescuable "herodown" clone (cap 3).
- **Win**: rescued ≥ quota → `UpdateZonesAndWin` arms `Entity+0x18==8`
  final-exit zones → touch → `Verb_FinalExit` → `EndMission` →
  `+0x3438 = 1` → `RC_ResLvlWin`.

**Fire extinguish (hose).** `Hotspot` (`+0x20` = `fires_remaining`)
owns ≤8 child `RCS_WindowFire`. `Hotspot::AdvanceActiveFire` round-robins;
`WindowFire::Think` drives `douseProgress +0x258 -= dt / state+0x08`;
cursor wrap → `fires_remaining--`; `IsCleared` (`+0x20 < 1`) →
`PlayerHose::WinLevel`. Per-hotspot timer `hose+0x69f` → `LoseLevel`.

**Ash/smoke** (`AshSim` at `PlayerRescueEnt+0x55c`) — 100 drifting
motes; density `+0x54` (script `setashdensity`) has **no located
consumer** → tentatively cosmetic. Engine fog is separate.

**`create_function` sweep** — 24 undefined vtable-method bodies defined,
named, commented: `BModelEnt::Think`, `ModelEntity::{GetCollisionBlock,
ApplyDisplacement}`, `RenderInstance::{Render,GetBoundsSphere,
PrepareForRender}`, `PlayerRescueEnt::{Think,HandleMessage,GetName}`,
`TReader_Image::read*` ×11, `AnimSeqPlayer::{Bind,GetScaledBoundingBox,
IsEnabled,Unbind}`. **Known gap:** `PlayerRescueEnt::FrameUpdate`
(`0x0044fe20`) got a 1-byte body from an MCP `create_function` fluke —
needs a manual Ghidra GUI "Re-create Function" (flagged as a task).

**Totals after cycle 7: 548 symbols** (rescue 120, ROC 110, BluelAPI
86, main 76, hose 52, sound 31, script 30, basic 29, sharedents 14).
29 research tasks done, 17 open.

### 2026-08-28 — loop cycle 8: Genesis3D types + coverage survey (2 subagents)
**Genesis3D types applied** (see the separate `retype` event): `geVec3d`
/ `geXForm3d` / `GE_LVertex` / `geRect` / `GE_RGBA` + 15 opaque handle
types; real prototypes on 26 `ge*` import thunks; `ModelEntity+0x24` &
`RenderInstance+0x10` → `geXForm3d`, `Sprite+0x10` → `GE_LVertex[4]`.

**RESOLVED — the "3D game-view child widget" does not exist.** `EverGame`
is the GUI-tree root; its `+0x7c` child list holds **UI clients only**
(menu screens, console, modal dialogs). The per-frame world tick+render
is wired as `EverGame` vtbl slot `+0x60` → `GameSession::PumpPendingState`
(`0x0042aea0`), which each frame calls a subsystem tick,
**`GameSession::TickSubsystems`** (`0x00429520` — the entity/world sim),
and `ROCRender::RenderFrame`. (`PumpPendingState`'s name understates it;
plate comment corrects this.)

**Other cycle-8 names:** `GameSession::{RegisterEntityClasses,
TickSubsystems,ReturnToFrontend}`, `GameControls::Construct` (the 6 KB
input action + default-binding table), `Region::{EnsureValidated,
DispatchShapeQuery}`, `RichTextLabel::SetText`, `MenuScreen_Options::
PopulateVideoModes`, `EverGame::{ShowClient,RunClientModal}`,
`FireRescueEnt::{Spawn,HandleScriptCommand}`, `PropModelEnt::
SpawnScripted`, `MiscEmitterEnt::Spawn`. Globals `g_devMode` (was
`g_skipIntroMovies` — it's the single cfg-`[DEBUG]` flag: intro-skip +
console + `DBG_*` binds + debug paths), `g_screenViewport`,
`g_musicManager`, `g_displayModes`/`Count`.

**Totals after cycle 8: 573 symbols** (rescue 122, ROC 112, main 86,
BluelAPI 86, hose 52, sound 31, script 31, basic 31, sharedents 16,
engine 6). 33 tasks done, 16 open.

### Coverage status (2026-08-28)
All major subsystems are mapped end-to-end. What remains is the long
tail, in decreasing value:
1. **Per-entity Spawn/Update/Use bodies** — the 4 `Tool_*` classes,
   `FireRescueEnt::Think`, sharedents scripted-spawn siblings, the two
   undefined `BModelEnt` liftable code regions (need manual
   `disassemble_at` first).
2. **~75 small MenuScreen widget ctors / button handlers** — low
   individual value but they're the bulk of the remaining `FUN_` in the
   `0x0042e000–0x00437000` range.
3. **Struct-creation + `this`-param retype pass** — mechanical; the
   field maps are all in `research.db` symbol summaries + plate comments.
4. **CRT / BluelAPI template boilerplate** — deliberately left `FUN_`;
   not worth bespoke names.
The autonomous loop continues on 1→2→3; 4 is the natural stop line for
a `get_functions` sweep ("no meaningful `FUN_`/`DAT_` left").

### 2026-08-28 — loop cycle 9: menu-widget layer + LEVEL-START PATH (subagent; wave re-run after a 3rd 429)
The **campaign level-start path is now fully traced**:
`MenuScreen_Main[id 0]` → `SlotList` (`PlayerManager::SelectSlot`) →
`MenuScreen_Title` (idx 2) → **"Begin Mission" (id 0)** →
`LevelManager::FindByIndex(session+0x6ac, Title+0x2c)` maps the
selection index to a `TLevelInfo*` → `MenuManager::TeardownToLoading`
(loading screen) → **`GameSession::LoadLevel` (`0x0042ab00`,
`evergame.cpp` ~245)** which *synchronously* sets `session+0x688`,
parses the level `.rrf`, loads `<name>.bsp` (`FUN_00471310`), builds
the `"World"` `EntityCreateStruct` (type `0x2711`) and flushes the
spawn queue. `GameSession::RequestLevelChange` (`0x0042a260`) is only
the **in-game deferred** path (sets `session+0x68c`; the main loop
performs the load). Post-mission: `MenuScreen_LevelResults[id 1]` →
`Title` (next mission) → `LoadLevel`; or `End.avi` + `Credits` at
campaign end. **Correction:** `MenuScreen_List` (idx 4) is a read-only
mission-records/stats browser, not level select.

Also named: the `MenuWidget` layer — base + `ImageButton`/
`ImageButtonGlow`/`ListRow`/`Slider`/`OptionCycler` (`+0x04` commandId,
`+0x08..+0x14` LTRB, `+0x18` owner, `+0x1c` enabled; `OnClick` →
`owner->OnButtonActivate(commandId)`); `MenuScreen::{OnActivate,Draw,
DrawWidgets,AddStandardButton}`; the Options screen's slider→cvar
writes (`geEngine_SetGamma`, `GameSession::SetSfx/MusicVolume`) +
`Config::Save`-on-leave; `MenuScreen_VideoConfirm` + `ReinitVideo`.

### 2026-08-28 — loop cycle 9b: carried-tool bodies + RCS_FireRescue + scripted BModel (subagent)
The four **carried tools** are now mapped end to end. They share a
`Tool` base (`0x0045f7e0`); `PlayerRescueEnt::UpdateLocomotionAndTools`
(`0x0044f340`) drives them: `+0x14/+0x18/+0x1c` press/hold/release
(edge, active tool only), `+0x20/+0x24` tick/post-tick (every tool,
every frame), `+0x38` bool "busy". The active tool exports a recoil
vec `+0x1e4` that adds into player locomotion; **active tool index 4 =
the water tool** ("Gallons Sprayed" stat).
- **Axe** = a composite of **two `AxeSwing` hit-test effectors**
  (`0x004600f0`, vtable `0x004e27f0`). Press arms a 100 ms wind-up;
  hold swings — plays `"AxeSwing"`, bumps the `"Axe Hits"` stat, and at
  the arc apex proximity/Z-tests a nearby-breakables list and fires
  `"AxeHitWood"`/`"WoodDoorBreak"` (or `"AxeHitOther"`), clearing the
  target BModel's latch byte `+0x30` so the door/board releases.
- **Extinguisher** = a short-range foam **cone** (FSIN/FCOS, 3 emitters)
  that douses `class-6` fire in range by stamping `[fire+0xa4]=0x20`.
  **Limited charge** at `+0x8c8` draining at `+0x8cc` while spraying;
  auto-stops empty; refilled by the `"refill"` message. 3-part cue
  `ExtStart`/`ExtMiddle`/`ExtEnd`.
- **Goggles** = thermal toggle: scans for nearby **fire *and* live
  victims**, attaches screen-space highlight Sprites that track them
  through smoke for ~8.5 s, plus a full-screen tint overlay.
- **Tool_Hose** = the *carried* hose water jet (distinct from the
  on-rails `PlayerHose`): 100 ms nozzle spin-up, continuous particle
  jet each post-tick, tick ray-tests the stream vs world to douse the
  `class-6` fire it's aimed at, nozzle recoil shoves the player;
  `+0xc68` = hydrant-connect state.

**`RCS_FireRescue`** is a *spreading-fire node*: a level fire is a chain
of `FireRescueEnt` nodes crawling designer **`PathPoint`** splines,
each node factory-spawning a child as it advances
(`FireRescueEnt::AdvanceSpread` `0x00449340` — **mislabelled `::Spawn`
in Ghidra**, corrected via plate comment + DB). Default behaviour is
**ambient** auto-crawl; it becomes mission-relevant through `PathPoint`
script lines and the `moveto`/`stopmove` script commands. Modes
(`+0x70`/`+0x4c`): 0 idle, 4 spreading, 5 extinguished. The orange
light colour / radius / flicker is the front half of `Think`
(`0x00449fe0`), not a separate function.

**`PathPoint`** (`0x00442c90`) = a path node that is *also* a
`ScriptTrigger` — carries `name` + up to 10 script-line keyvalues that
fire as a follower arrives. **`TE_BModel`** (`sharedents\BModel*.cpp`,
vtable `0x004e0de4`) = the moving-brush entity (doors / lifts /
liftables / throwable debris): `Spawn` derives the open/closed Z limits
from the brush's MapZone span; `Think` eases it along a 2-keyframe
`gePath` toward a target path param; verbs `open`/`openaway`/
`opentowards`/`close`/`drop`; activation is script/trigger/`use`, never
push. A separate lighter `ScriptedBModelEnt` (`0x00444a40`) handles
`hide`/`show`/`setscale` only.

**Caveat:** 13 of these bodies (all the Axe/Ext/Hose action slots,
`FireRescueEnt::Think`, the 4 `TE_BModel` methods) live in Ghidra
*undefined regions* — MCP `create_function` fails on every one. Each
carries a full plate comment; a task tracks the manual GUI
"Create Function" pass.

### 2026-08-28 — loop cycle 10: struct-layout pass (subagent)
Created three Ghidra **structs** from decompiled ctors + the undefined
bodies: `Tool` (`0x1fc`, `/rescue`), `FireRescueEnt` (`0x1d8`,
`/rescue` — Ghidra auto-binds it to the `FireRescueEnt::` namespaced
functions, so their `this` is now typed), `TE_BModel` (`0x274`,
`/sharedents`). Full offset→type→name maps are in the struct symbol
rows + a carried-tool-family hypothesis in `research.db`.

Key structural facts recovered:
- **`Tool` base** ends at `0x1fc`; `wielder` Pawn* at `+0x54` (anim
  table at `wielder+0x34a4`), `WorldEntityBinder` muzzle transform at
  `+0x58`, `deployed` byte `+0x158`, deploy anim `+0x15c`. `Tool_Axe`
  embeds **two `AxeSwing` objects** (`+0x200` primary, `+0x648`
  secondary-always-ticked); `AxeSwing` is itself a `Tool` subclass
  (`0x448`) with `swing_state` at `+0x368`.
- **`FireRescueEnt` has two base origins**: the movement methods use the
  primary base; `Think`/`HandleScriptCommand` use the RCS "think"
  sub-object at `this+0x24` and call movement fns with `this-0x24`.
  `B1+0x4c == B0+0x70 == mode`; `B0+0x4c` is separately the `String`
  length of the name at `+0x48`.
- **`TE_BModel` plate offsets corrected**: motion channels are at
  `+0x178` (not `+0x170`); `+0x1c4` is the **MapZone count**, not a
  path param; `+0x124` is a `SoundCue` built from a keyvalue. The
  animated door position / speed / limits are `current_z` / `speed` /
  `closed_z` / `open_z` at primary `+0x1d8` / `+0x1dc` / `+0x1e8` /
  `+0x1ec`.

Still pending: the `this`-param retype for the non-namespaced `Tool_*`
and `TE_BModel` action-slot functions — blocked until the manual-GUI
`create_function` pass gives those undefined regions function objects.

### 2026-08-28 — loop cycle 11: the level-script command set (subagent)
Mapped the **ScriptTrigger verb table** — 42 renames, `script\` count
31 → 74. The level "script VM" is a **line-oriented string
interpreter** (`ScriptActor::ExecCommand`), *not* bytecode: no program
counter, no `goto`/labels, no `else`. `ScriptActor::Tick` drains a
per-actor queue of command strings one per frame; **`wait N` is the
only time primitive** (sets a wake-time checked against the global ms
clock). Conditionals take a quoted command string and run it **inline,
immediately**, via recursive `ExecCommand`.

`ScriptActor::HandleBaseVerb` handles `que`/`flush`/`wait`/`idle`/`cmd`/
`loopinque`/`onmsg`/`onmsgappend`/`printmsgs`; then
`ScriptTrigger::HandleVerb` (a hand-written `stricmp` if-chain, ~40
verbs, one `Verb_*` worker each):
- **flow/logic**: `if`/`ifnot` (string eq), `if<`/`if<=`/`if>`/`if>=`
  (numeric), `iffacing`/`ifnotfacing`, `ifclear`/`ifnotclear` (zone
  occupant count), `once`, `ignore N`, `exec`/`execsetsonly`,
  `sendmsg`, `tell`/`action`.
- **vars**: `set`, `inc`, `dec`, `print` on the global dict
  `g_scriptVars` (`0x00503044`; ref-counted `String` values, default
  `"0"`). `once`/`ignore` use a **per-trigger** dedup map at
  `this+0x54`.
- **world**: `playsnd`/`loopsnd`/`ambientsnd` family, `hint <sec>
  "<text>"` (→ `PlayerRescueEnt::Verb_Hint`), `fogenable`/`fogdisable`/
  `quake` (atmosphere entity), `difficulty`, `runrescuebot`, `quit`
  (**shuts the game down**, not end-level), `debugvictims`.

**There is no `spawn`/`camera`/`endlevel` verb in `ScriptTrigger`** —
those are `Verb_*` methods on `PlayerRescueEnt` and other actors,
reached with `tell <actor> <verb>`. A `ScriptTrigger` *fires* when
something calls `ScriptActor::FireEvent` on it — its volume subclass
detecting player entry, or another script's `sendmsg`.

### 2026-08-28 — loop cycle 12a: the level win/lose + trigger model (subagent)
The `PlayerRescueEnt` verb dispatcher and its verbs were already named
from an earlier cycle; this pass added the **behavioural detail** as
plate comments + `research.db`, plus 5 new renames
(`ScriptedBModelEnt`/`PropModelEnt`/`VictimRescueEnt::ExecCommand`,
`VictimRescueEnt::ForceSpawnSpot`, `PlayerRescueEnt::OnLevelWon`).

**Level win/lose is entirely on `PlayerRescueEnt`.** `+0x3438` =
mission result (`0` active / `1` win / `2` lose); `FrameUpdate` ticks
gameplay only while it's `0`; `RescueHud::Update` consumes it (win →
`SetAudioState(3)` + `"RC_ResLvlWin"` + score handoff; lose →
`"RC_ResLvlLose"` + failure panel).
- **WIN** is set by (a) `tell player finalexit` when outstanding
  victims `== 0`, or (b) `UpdateZonesAndWin` automatically once every
  victim is delivered *and* every final-exit `BoundsTrigger` volume
  (entity type tag `+0x18==8`) is clear of fire. Rescuing all victims
  does not itself win — you must then reach a fire-clear exit.
- Victim math: `outstanding = +0x3528(total, counted lazily by
  EndMission) − +0x3530(lost) − +0x352c(delivered, bumped in
  DeliverCarriedSurvivor)`.
- **MAP TRANSITION** only via `tell player finalexitloadmap <map>` →
  `GameSession::RequestLevelChange(…,"data\resdefs\rescue.rrf")`.
- **LOSE** (`+0x3438=2`) is set on the timeout/death path, not by any
  verb. `ScriptTrigger`'s `quit` shuts the whole game down — not
  end-level.

**Correction:** `PlayerRescueEnt::TeleportToCheckpoint` (`0x00451160`)
is misnamed — it's `DeliverCarriedSurvivor`, the `escape`-verb helper
that counts a rescue and releases the carried victim. **Trigger-volume
correction:** the player-entry volume is `RCS_BoundsTrigger`
(`BoundsTrigger::DispatchTouchEvents` `0x00470840`, fires
`touch`/`touching`/`untouch` via `FireEvent`, re-fires every
transition); the 3 `ExecCommand` subclasses are scripted movers/props/
victims, not volumes.

### 2026-08-28 — loop cycle 12b: the sharedents level-entity family (subagent)
28 renames — the designer-placed level entities now have their
registrar / ctor / think / verb families named. `sharedents` count
24 → 60.
- **`RCS_BoundsTrigger`** (the level-trigger volume): *embeds* a
  `ScriptTrigger` at `ent+0x24` (doesn't subclass it). Keyvalues at
  `ent+0xfc`: brush-model index = the volume, a touch mask, 10
  script-line slots. `DispatchTouchEvents` fires `touch` on entry,
  `touching` every frame inside, `untouch` on exit — for *any*
  entity, non-latching (scripts use `once`). Default `touch` binding
  is `"tell player escape"`.
- **`RCS_MiscEmitter`**: ambient particles/sound. Keyvalue `type`
  1..6 selects one of 4 impl classes at `ent+0x78` (types 5/6 =
  positional/ambient sound). Distance² + PVS culled. Verbs
  `turnon`/`turnoff`/`setstrength`/`setsize`/`setrot`/`setpitch`.
- **`RCS_BlinkLight`**: the blink pattern *is the entity's own
  script* (a `wait`-loop toggling `SetLightParams`) — no C++ blink
  code.
- **`RCS_PathPoint`**: a path node that is also a proximity trigger.
- **`RCS_PropModel`** = `ScriptedBModelEnt`: placed mesh prop with
  optional rotation, optional `use` interaction, full touch/use
  script; `makevalve` turns it into a "turn the valve" objective.

Corrections: `0x00440d60` (Ghidra `BoundsTrigger::Construct`) is
`BoundsTriggerEnt::RegisterClass`; `0x004424d0` (Ghidra
`MiscEmitterEnt::Spawn`) is `MiscEmitterEnt::HandleScriptCommand`. The
cinematic camera rig (`RCS_CamRescue` `0x004e1544`, `RCS_CamHose`
`0x004e2a10`) is *not* in this range — it's above `0x00446000`,
queued as its own cluster.

### 2026-08-28 — loop cycle 13a: ActorAnimator + MapZone (subagent)
16 renames. Two resolutions:
- **`ActorAnimator`/`AnimSeqPlayer`** is a thin *time-derived* wrapper
  over `geActor_SetPose` — **no per-frame clock, no blending, no event
  track**. `ApplyPose` recomputes `t` from `(now − startMs) mod
  period` each frame. `PlayMotionLooped` (the only "play" entry)
  looks a motion up by *index* and hard-cuts to it. Fields
  `+0x34…+0x4c` are a stubbed/removed blend path — nothing reads
  them. The tool "`+0x15c` deploy anim" is a *separate* class,
  `XFormAnimMover` (`0x00483720` Tick), that eases an xform offset
  toward a motion-sampled pose.
- **Cycle-9 open question — the anim-event fire site — RESOLVED**:
  keyframed notifies don't live in the animator. They're a named-string
  pub/sub: `MessageSink::Subscribe` (`0x004a12f0`); `Tool_Axe::Construct`
  subscribes four sinks to `"AxeSwing"`/`"AxeHitWood"`/`"AxeHitOther"`/
  `"WoodDoorBreak"`; footsteps use `"FootStep1".."FootStep4"`. The
  publish site is in `ItemAnimSeq` — located, not yet byte-pinned
  (new task).

**`MapZone`** = a *named set of `BoundsTrigger` AABBs* (not a brush or
BSP leaf). `MapZoneSet` is an arena-backed case-insensitive
string-keyed balanced BST; the manager is `PlayerRescueEnt+0x5d8`.
Membership is built at load by `BoundsTrigger::MakeZone`. Occupant
lists (`zone+0x20`/`+0x24`) are rebuilt every frame by
`MapZoneManager::RebuildZoneOccupants` and contain **victims only** —
fire entities are never zone members. `MapZoneTracker` keeps
current+previous `{zone,entity}` snapshots; `CommitFrame` flips them —
that's the enter/leave diff behind every `touch`/`untouch` callback.

**Correction:** the cycle-12a plate on `UpdateZonesAndWin` claimed win
needs "every exit zone clear of fire" — **wrong**, zones don't track
fire. Actual: `local_b0` = count of distinct zones hosting an active
final-exit trigger; `== 0` (no exit gate) → auto-win; `> 0` → HUD
"Exit through: …" + enable the exit boxes.

### 2026-08-28 — loop cycle 13b: camera rig + RadioChatter + RescueBot (subagent)
25 renames.
- **No camera script verb exists.** `RCS_CamRescue` / `RCS_CamHose`
  are ordinary ROC entities (kind 5) **auto-spawned by their owning
  player entity** (`PlayerRescueEnt::Init` / `PlayerHose::OnSpawn`).
  `CamHoseEnt` is the on-rails chase cam (follows a named target at
  ~200u standoff). `CamRescueEnt` has no bespoke motion — its
  `FrameUpdate` only runs `Camera::UpdateShake` +
  `Camera::UpdateTimedMove` (a scripted 2-point lerp over a duration
  with fog/far-clip animated). The first-person↔EntityCam view
  switch isn't pinned (likely a mode on the `+0x34a4` view-director);
  new task.
- **`RadioChatter`** is the game's primary objective/status narration
  channel (there's no HUD text). `RadioChatter::Play(cue, immediate)`
  is the sole entry: a cue name maps to a `TChatterSet` of
  `<cue>0`/`<cue>1`/… clips enumerated at load; playback is random
  with single-step anti-repeat. **No priority, no interrupt, no
  cooldown** — lines just stack. Cue→trigger map recorded (e.g.
  `RC_Pain` ← `TakeFireDamage`, `RC_ResLvlWin/Lose` ← `RescueHud::
  Update`). **Original-game bug found**: `RC_HoseTimeWarn` is emitted
  by the hose warn-timer but isn't in `RadioChatter::Init`'s roster →
  `FindSet` returns null → silent no-op.
- **RescueBot** labeling is effectively complete: `FUN_004524f0`
  turned out to be `MapZoneManager::Destruct` (not a bot method), and
  `0x004552f1` is a disassembly artifact inside `TryStartRescue`.

### 2026-08-28 — loop cycle 14: save/load + the ROC entity base (2 subagents)
**14a — campaign save/load.** 6 fixed profile slots → `saves\playerN.rb`
(BluelAPI RichBinary). `TPlayerInfo` (0x54) holds furthest-unlocked
level index, difficulty byte, and one `TLevelScore` per level touched;
each `TLevelScore` carries a `PlayerStats` best-for-that-level map.
`PlayerStats` is a **string-keyed BST**, not a struct — 13 code-
registered keys (`Rescues`, `Axe Hits`, `Gallons Sprayed`, …) each with
a type (int/time/volume) and min/max/overwrite merge rule. On mission
win, `RecordLevelComplete` merges the live-run map into the per-level
record (keeping best-ever), advances the unlock index, and saves
immediately. Level list is the **plain-text `data\resdefs\thegame.lev`**
(not a `.rrf`); `<id>` in `level:<id>` is the `.bsp` base name.
*Corrections:* no per-level difficulty scaling exists; `0x00423fa0`
"GetOrCreateStat" is really `AcquireLevelScoreSlot`.

**14b — the ROC entity base** (touches every entity). Hierarchy:
`Entity` (`0x004e4288`) ← `WorldEntity` (`0x004e4338`, adds the xform
sub-object at `+0x24`, world pos at `+0x4c`, binder at `+0xd0`) ←
`ModelEntity` (`0x004e43f0`, adds the physics set `+0xf0…+0x12d`).
`ROCWorldEntity` is a separate `geWorld` wrapper off `Entity`.
**`Entity+0x18` is the kind tag** used for filtering everywhere (1
player / 2 victim / 5 camera / 6 fire / 8 final-exit). Handles are
`{slot, generation}` pairs — `EntityHandleTable::Deref` returns null
once the generation moves on, so stale refs are safe. Per-frame
think/draw is *not* on these vtables — it's driven by
`ROCRender::RenderFrame` over `TWorldResources.worldList` via a
renderable interface. *Corrections:* `Entity::IntegrateVelocity`
(`0x0049dbc0`) is actually a `ModelEntity` slot; `GetCollisionBlock`
(`0x0049d790`) is `WorldEntity`'s.

### 2026-08-28 — loop cycle 15: hose mini-game + particles + HUD (2 subagents)
**15a — the on-rails hose mini-game.** The `PlayerHose`/`Hotspot`/
`WindowFire` cluster was already named by an earlier cycle; this pass
confirmed it and pinned the **mechanic**: the douse runs on a *fixed
timed schedule* — `Hotspot::AdvanceActiveFire` extinguishes the
window fires on a fixed round-robin regardless of input; aiming and
firing drive only the visible stream, splash particles and sound. The
player's job is purely to survive the per-hotspot timer (budget ×
difficulty 1.8 / 1.3 / 1.0). Win/lose live on the entity at `+0x198`
(not `session+0x6ec`), polled by `RCS_DouseHose`. The truck auto-drives
a closed spline at 500 u/s.

**15b — the particle system** (referenced by fire/foam/water/smoke
everywhere). `FUN_00480090` is `ParticleSystem::Emit` — one point-spawn
burst, velocity-box spread, constant world accel, self-recycling
*unbounded* pool. Per-frame tick+draw is the RenderManager calling
`ParticlePool` vtable slot 1; particles draw as BSP-culled engine polys
(`geWorld_AddPolyOnce`). **There is no decal pool** — foam/scorch/wet
marks are one persistent `ParticleNode` per tool reprojected onto the
hit surface each frame. Also mapped **`RescueHud`**: a flat widget bag
(status text line, transient banner, O2 `HudGauge` bar fed a smoothed
value, burn vignette panels, compass, objective markers, edge
damage-hit sprites, hose GPM readout). No objective-*list* widget —
the status line + radio narration carry it. The `HudGauge` widget is
shared with the tool charge gauges.

### 2026-08-29 — loop cycle 16: input + victims/RescueBot; Knowledgebase tab added
**Input.** `GameControls` (`g_gameCtx+0x00`) — gameplay talks to the
ActionMap at `+0x334`. DirectInput8 keyboard + mouse only (**no
joystick**). ~30 named actions with **hard-coded** default bindings in
`Construct`; **no rebind path**, no config file, no controls screen.
`STEER_YAW/PITCH` = mouse delta × sensitivity; `ACTION` = LMB,
`JUMP` = RMB, `USE` = Shift/Space.

**The view switch is a Camera "enabled" byte.** `ROCRender::RenderFrame`
renders from whichever `Camera` instance is `alive && enabled`
(`Camera::ShouldRender` = `+0x64 && +0x08`). First-person = the
`EntityCam` at `PlayerRescueEnt+0x3460`; cinematic = the one in
`CamRescueEnt+0x24`. Not byte-pinned: what flips `Camera+0x08` on the
cinematic cam (no camera verb; likely `tell <camname> …`).

**Victims.** `VictimRescueEnt` is a **4-state FSM** on `+0x120`:
4 idle (default; leaves 4 only on a `roam` script verb), 5 carried
(frozen where picked up), 6 bot-claimed / delivered (fades out),
7 roaming. **No dead / knocked-out / panic state, and no fire response
at all** — nothing writes the burn field, the message handler is a
no-op stub. Pickup is **automatic on AABB contact** (no key, no
facing); the carry is **abstract** (no bone-parent) with **no O2 or
speed penalty**. Deliver warps the victim ahead of the rescuer and
blinks it out.

**RescueBot has no pathfinding.** It's a pure timer: pick the first
eligible victim ≥ 600 u away with clear LOS, wait a random 4–8 s, then
credit the rescue by flipping the victim to the fade-out state. The bot
never moves.

**Knowledgebase.** `research.html` now has a searchable **Knowledgebase
tab** — 26 prose articles (`kb_articles` table) on every system and how
they interact, rendered like the Hypotheses tab. Additive only: no
existing DB rows or tabs changed. Standing rule: **never delete
`research.db` rows**; each cycle keeps the KB current.

**Totals after cycle 16: 856 symbols**, 76 hypotheses, 26 KB articles.
21 tasks open.

### 2026-08-29 — loop cycle 17a: the BluelAPI foundation library (subagent, read-only)
27 renames (applied by a commit subagent). `String` is a **1-pointer
COW handle** — `{ RatStr* rep }` where `RatStr` is
`{refCount, length, bufSize, data[]}`; every non-empty value is heap,
COW on mutate; the earlier "COW rope" phrasing was imprecise.
`SArray` (~0x1c hdr) owns values, `SArrayPtr` (0xc) is a pointer vector;
a third refcounted inline variant is COW (`SArrayRB::DetachCOW`).
`RichBinary` is the named-node tree behind `.rb` saves and `.rrf`/`.srf`
resource defs — 12-entry field-type enum, `RBField` variant node,
CONTAINER (12) locks the tag. `NameTree` is a name→value BST at
`NameTree+0x08`. **`GenErr::Dispatch` (`0x00422060`, Ghidra-stuck as
`SArray::OobFail`) is the universal assert sink** — every
`dir\File.cpp:line` check in the binary lands there; fatal →
`MessageBoxA` + abort. New KB article `bluelapi` (27 articles now).
Totals: **883 symbols**, 79 hypotheses.

### 2026-08-29 — loop cycle 17b: the sound system (subagent, read-only)
10 renames (commit subagent). Audio = FMOD 3.x behind
`ROCSoundSystem` (`*(g_gameCtx+8)`). **FMOD init is lazy** — no
`ROCSoundSystem::Init`; the first `LoadSound` brings up FMOD
(`FSOUND_Init(_,512,0)`). Sample bank parsed from
`data\resdefs\sound.rrf`. `ROCSoundSystem::Frame` (from
`TickSubsystems`) updates the 3D listener + repositions 3D cues each
frame. A `SoundCue`'s first dword *is* an FMOD channel; play mode
0/1/2 = one-shot / loop-if-idle / loop + listener-relative. 4 category
buses; **cat 3 is the untouched voice/`RadioChatter` bus**. Music is
just a `TSoundSample` under `Data\snd\music\` (streams only if `.mp3`);
`GameSession::SetAudioState` is a 7-state crossfade/mute machine (state
4 = "interior/muffled", cat 2 ×0.3). `0x004a12f0` is a `/OPT:ICF`
COMDAT fold of the `SoundCue` rebind method and
`MessageSink::Subscribe`. KB `sound` article upgraded to stable.
Totals: **893 symbols**, 83 hypotheses, 27 KB articles.

### 2026-08-29 — loop cycle 18: stop-line recon
Live Ghidra: **5835 functions, 3974 named (68%), 1861 still `FUN_`
(32%)**. Of the 1861, ~55–60% is genuine boilerplate — a **360-function
MSVC CRT/STL/locale blob** at `0x4a2aae–0x4db750` is 100% runtime;
~350 are BluelAPI container methods; ~130 are RichBinary/NameTree
mechanical families. Only **~300 functions (~15–20%)** still hide
subsystem behaviour.

**Plan to the stop line: ~3–4 focused cycles left** on —
(1) player-rescue/hose collision & reachability math + mission debrief
(`0x480000–0x488900`); (2) carried-tool entities + HUD overlays
(`0x45c310–0x464cd0`, `rescue\Items\ItemAnimSeq.cpp`);
(3) RenderManager/ROCRender frame + debug-draw internals
(`0x489e40–0x496c30`); (4) the Sprite/2D HUD-widget layer
(`0x456310–0x45b520`) — then a mechanical bulk-tag pass over the CRT
blob and the BluelAPI/RichBinary container families, then stop.
Confirmed non-issues: no water/`AshSim`/atmosphere subsystem exists;
`Config`/`Console`/resource-parser/renderer-core already named; the
`.avi` intro is three filenames. New KB article `coverage`.

### 2026-08-29 — loop cycle 18b: HUD widget primitives + the RichBinary save-file reader (subagent)
~35 renames (commit subagent). The RescueHud **builder** layer was
already named; this pass named the **primitive** layer beneath —
`Sprite` (bitmap quad), `FloatTrack` (keyframed rect animator),
`SlideSprite`, `HudAnimQuad`, `RichTextLabel` (splits on `\n`),
`ScreenFade` (the `+0x824` win/lose fade-wipe, anim 0x47/0x48). And the
**RichBinary stream node-walk**: `TReader_Stream::IterateNode` is the
cursor; typed reads (`readString`/`readInt`/… one per type enum) each
assert the node type; there is **no `FindChildNamed`** — the "Expected
node named" check is an inlined macro. **The `.rb` save schema is now
confirmed end to end**: `root → Player{ Name, LevelIndex,
DifficultyLevel, Score{ LevelName, stats{ stat{ name, value,
idealValue, updateRule, betterIf, displayIndex } } } }`. `PlayerStats`
serializes inside the save and is also a reusable RichBinary
custom-type handler — no separate `.rrf` parser here. KB `save-load`
article updated with the schema. Totals: **923 symbols**, 86
hypotheses, 28 KB articles.

### 2026-08-29 — loop cycle 19: player collision/reachability math + mission debrief + a copy-protection find (subagent)
~30 renames. Key results:
- **Player-vs-world collision is the engine**, not custom — Genesis3D
  `geWorld_Collision` via `World::TraceGroundHeight`; only the impact Y
  is used. The `0x00483xxx` range is a *generic* geometry kit
  (sphere/sphere, swept segment/sphere, ray/sphere, segment/AABB slab,
  Vec3 helpers) shared by `rescue\` and `hose\`. Corrected:
  `PlayerRescueEnt::UpdateCheckpoint` (`0x00441e90`) is a decompiler
  mis-binding — a trivial 3-float store that tests nothing.
- **Damping helpers** named — `Damp` / `CriticalDampStep` /
  `Damp_RateFromFraction` drive the view mount, tool deploy, and
  movement blend; plus the `XFormAnimMover::SetThresholdMode` /
  `SetDampedMode` / `SetSnapOnArrival` setters.
- **The end-of-mission debrief has no medal/grade system.**
  `PlayerStats` is an **AVL tree**; `BuildStatSheetText` just formats
  `"<label>: <value> [/ <ideal>]"` rows with separator blank lines.
  `betterIf` is only the keep-min/keep-max submit rule. The results
  screen animates a count-up but shows raw values.
- **Copy protection found**: `File_LoadObfuscatedU32Blob` loads
  `texture.dat` — an XOR-obfuscated `u32` blob next to the EXE — as a
  disc-check token; `CheckGameCD` prints
  `OutputDebugStringA("Don't blame Mekada")`.
- New strings: `hose\stream\HoseGfx.cpp` owns the `StreamTube` cluster;
  `basic\MapZone.cpp` referenced from an undefined region (`MapZone::
  Load`?). KB `player-movement` / `save-load` / `overview` updated.
  Totals: **953 symbols**, 90 hypotheses, 28 KB articles.

### 2026-08-29 — loop cycle 20: the carried-tool entity classes + equip path (subagent)
~11 renames. The five tools derive from `ItemAnimSeq`/`Tool` (base
vtable `0x004e2790`); slot `+0x08` `OnEquip(owner)` builds each tool's
HUD/FX "view" (`AxeSwing::OnEquip`, `Tool_Goggles::OnEquip`,
`Tool_Hose::OnEquip` named; Axe/Extinguisher OnEquip are undefined
regions). Tools live in an `SArray` at `PlayerRescueEnt+0x3564` (axe 1 /
extinguisher 2 / goggles 3 / hose 4); `Verb_Equip` news+Constructs then
calls `OnEquip`; `SetActiveTool` toggles via `Tool::SetActive`.
`DoFootstep` (`0x0044ea90`) + `ToolNameToIndex` named.

**Cycle-9 anim-event question — narrowed decisively:** it is *not* in
`ItemAnimSeq` (its keyframes are plain 3-vec3 records, no tags). Tools
*subscribe* `SoundCue`s by event name into a global sink registry
(`SArrayPtr` at `*(g_gameCtx+8)`) via the `0x004a12f0` fold. The
*broadcast* walk over that array is the fire site — a focused subagent
is hunting it now. KB `animation` / `carried-tools` updated. Totals:
**965 symbols**, 92 hypotheses.

### 2026-08-29 -- loop cycle 20b: the cycle-9 anim-event question, CLOSED
**There is no pub/sub broadcaster.** Every gameplay/anim "notify" sound is
played **directly by pointer** -- `SoundCue::Play(this + <fixed cue offset>)`
-- from the owning object's own state machine. `MessageSink::Subscribe`
(`0x004a12f0`) only binds a `SoundCue`'s FMOD sample-bank key and parks it in
the ROCSoundSystem array; it is not a topic registry, and the four axe event
strings each have exactly one xref (the `Subscribe` call). The axe hit sound
is a **collision result**: `AxeSwing::ResolveHit` (`0x00460b90`, undefined
region) sphere-tests breakable world entities (tag `+0x18==4`) and reads
`BreakableEnt::GetMaterialFlags` -- bit0 `"WoodDoorBreak"`, bit1 `"AxeHitWood"`,
else `"AxeHitOther"`. `XFormAnimMover` has no keyframe-notify table either. KB
`animation` / `carried-tools` / `open-questions` corrected. Totals: **968

### 2026-08-29 -- loop cycle 21: RenderManager / ROCRender internals + the Path spline API (subagent)
~24 renames. RenderManager (singleton at `*(g_gameCtx+4)`) holds two
`TWorldResources` sets (static / per-level); each has a **heterogeneous**
`renderNodeList` of `RenderInstance*` (geActor wrappers) and `Camera*`
(geCamera wrappers) with a shared vtable slot map (+0x04 prep / +0x0c cull /
+0x10 draw). `RenderFrame` walks it. **No batched debug-draw API exists** --
just `DrawDebugQuad` (safe-area overlay) and `Gfx_DrawTube3D` (goggles/hose
FX). `Path` is a Catmull-Rom spline with an arc-length resample table + a
secondary aux vec3 channel per control point (shared by PlayerHose /
FireRescueEnt / camera rails). Corrections: the wave-18 recon mis-guessed
`Path::AddPoint`/`Finalize` (`0x00490d00`/`0x004905a0`) as "debug-vertex
buffers"; `0x00490ab0` "ViewRhythm::SampleBobCurve" is really
`Path::SamplePos`. New KB article `rendering` (29 articles). All four
wave-18 "meaningful clusters" are now done. Totals: **992 symbols**, 97
hypotheses.
symbols**, 93 hypotheses.

### Operating rule (owner, 2026-08-29)
The primary session **never** calls GhidrAssist MCP tools directly. All
Ghidra reads/writes go through loop-dispatched subagents. The loop
cycle: primary adjudicates the read-only analysis subagents' findings →
writes `research.db` (sqlite is fine) + the KB + regenerates
`research.html` → dispatches a **commit subagent** that applies the
vetted rename/comment list via MCP → re-arms the wakeup.

### Loop reliability note
Both waves 4 and 6 lost their subagents to account **session usage
limits** (HTTP 429). This is the only failure mode — the loop itself
keeps firing and re-dispatches on the next tick; nothing is lost
(everything's in `research.db`). Mitigations in place: smaller waves,
and on a 429 the loop waits for the known reset time rather than
retrying into the wall.

### 2026-11-11 -- native text labels and the menu's widgets (two read-only subagents; for D66 and the settings page)

**RichTextLabel** (== the DB's `HudAnimQuad` / `HudWidget`, vtable 0x004E399C, 0x104 bytes) is
self-contained: no owner pointer, no refcount. Ctor 0x0048C120 (ECX only; mallocs +0xC8 and
+0x100 on the game heap), `Init(fontIdx, registerWithRenderMgr, bitmapId)` 0x0048ABB0,
`SetText(String, rect*, hAlign, vAlign)` 0x0048C780 (consumes the String), `Render`
0x0048B7C0 (ECX only), non-deleting dtor 0x0048C1D0 (never 0x0048C760 on memory the game
didn't allocate). Unregistered labels are drawn by calling Render, as the menu screens do.
Corrections to earlier notes: **the rect is {left, right, top, bottom}**, not a Win32 RECT;
**hAlign is 0 left, 1 right, 2 centre** (vAlign 0 top, 1 bottom, 2 centre); row+0xEC
(`menu.f_font`) is the label's ParticleNode (colour ramp and bitmap), the font descriptor is
label+0xF4.

Fields: +0x04 "dead" byte (RenderFrame frees a registered label when set), +0x10 visible,
+0x14 cached geBitmap* (acquired once when 0), +0xB8..+0xC0 current R G B (reset to 255 each
Render), +0xD0..+0xDC rect, +0xE0 / +0xE4 alignment, +0xE8 bitmap id, +0xF8 / +0xFC float
scale. Markup in SetText: a literal backslash-n or 0x0A breaks the line; `^(r,g,b)` (0-255)
colours the glyphs after it and resets every Render; word wrap at the rect's width.
`SetTextLayout` 0x0048C2A0 is the same without wrap and markup.

**Fonts** (table 0x005030F0, stride 0x14: charset, cell w, cell h, bitmap id, advance
reduction): 0 = 8x12, bitmap 0x3FB, the HUD status line and a ListRow's second label; 1 =
16x24, bitmap 0x3FC, reduce 4, the hint and a ListRow's first label; 2 = 32x43, digits and
":" only (the warn timer); 3 = 8x14, bitmap 64000 `data\sprites\confont.bmp`, reduce 1, the
console. Seen in the game (2026-11-11): font 0 draws with its glyph tops cut off on the VM's
renderer, in the game's own status line too.

**Drawing:** per glyph `geEngine_RenderPoly` (flags 0x36), in a virtual 640x480 mapped per
vertex (`ScreenMetrics_MapVirtToReal` 0x0048A910); no world needed. `ROCRender::RenderFrame`
0x00494670: BeginFrame, the renderable list sorted by key (labels 11000), `Console::Render`,
EndFrame. **Colours** are vertex colours: the hint white; the status line green (0.25, 1,
0.25) alternating with white; menu rows grey 0.75 turning white when focused, their second
label grey 0.5 turning orange (1, 0.75, 0).

**The menu.** Screens built by `MenuManager::Init` 0x0042DEC0: 0 Main, 1 SlotList, 2 Title,
3 Options, 4 List, 5 LevelResults, 6 VideoConfirm, 7 Credits, 8 (unnamed), 9 Splash. A
screen's vtable has 8 slots: [0] OnActivate(bool), [1] OnButtonActivate(id), [2]
OnSliderChanged(id, float), [3] Draw, [7] HandleMouse. **Escape in a level shows the Title
(briefing) screen**, with the nav stack [0, 1, 2]; its button id 2 resumes
(`MenuManager::TeardownToLoading` 0x0042D1B0). `GameSession::Activate` 0x0042D480 is really
`MenuManager::Activate`. Widget rects are {left, right, top, bottom} too. Main has four
`ImageButtonGlow` (ids 0-3, y 440-472, art in background bitmap 10) and one invisible
hotspot (id 4); its OnButtonActivate 0x00430DF0 ignores other ids. A `ListRow` appended to
a screen is clickable when row+0x20 is 0 (its ctor doesn't set it). `OptionCycler` (0x4A8
bytes, ctor 0x00435510 `(owner, String* label, id, rect)`, options at +0x4A0, index +0x4A4,
no wrap, calls `owner->OnButtonActivate(id)`) and `Slider` (0x6F8 bytes, ctor 0x00437090,
position +0x6EC, min / max +0x6F0 / +0x6F4, steps of 0.05, calls `owner->vtbl[2](id,
value)`, no numeric readout) are what the Options screen (ctor 0x00432080) is built from;
`MenuScreen::AddStandardButton(id)` 0x004366D0 adds its Back button. Not determined: what
screen 8 is, what bitmap 10 looks like.

One subagent used `disassemble_at` (clear_existing = false) on unlabelled vtable targets;
the tool reported nothing cleared, but that nothing was written to the database wasn't proved.

## Blocked tasks

_(none yet)_

## Needs human attention

- Developer attribution ("Mekada") is from a search snippet only —
  MobyGames blocks direct fetch. Confirm if it matters for the writeup.

---

## Original orientation checklist (kept for reference)

Research the game before granular decompilation:
- Title, platform, engine, release year, developer — **done above**
- Prior art — community wikis, existing RE/decomp projects, format docs
  — *not yet searched in depth; PCGamingWiki page exists (FOV-fix mod)*
- Binary shape — compiler fingerprint, subsystems from strings/imports
  — **done above**
- Top-down strategy with reasoning — **done above**

### 2026-08-29 -- loop cycle 22: STOP LINE reached
A fresh `get_functions` sweep (cycle 22) confirms the wave-18 verdict: of
the ~1780 remaining bare `FUN_`, ~42% is pure MSVC CRT/STL/locale runtime,
~35% is BluelAPI container/serialization plumbing (`String`/`SArray`/
`RichBinary`/`NameTree`-AVL one-liners + template instantiations), and ~23%
is ctors/dtors/`RegisterClass` thunks/accessors on **already-named**
classes. **No residual `FUN_` cluster reveals a game system or mechanic**
not already mapped. Committed a ~33-entry container/serialization bulk-name
pass (`RichBinaryWriter::writeNamed*`, `RBField::Set*`, `TWriter_Text::`,
`NameTree` AVL ops, `String::Right`/`WordWrap`, `Vec3_SpringFollow`, ...).

**State: ~1025 symbols with summaries, 29 KB articles, 97 hypotheses.**
Every game subsystem an agent would be asked about is mapped, documented,
and cross-referenced. The remaining work is mechanical (bulk-tag the CRT
blobs, name the container/getter tail) and adds symbol count but no
understanding. Handed the scope decision to the owner: wind the loop down
here, or continue the mechanical grind toward literal 100% symbol coverage.

### 2026-08-29 -- loop cycle 23: gap-closing (owner challenged the stop line -- correctly)
A DAT_ pass and a resource-format pass DID surface new documented material:
- **Config / cvars** (`FDNYFirefighter.cfg`, INI, 4 sections, 10-entry cvar
  table; no key-binding persistence). New KB article `config`.
- **The `.rrf` format** -- a PRECOMP'd C-tokenized manifest declaring ONLY 5
  resource kinds (MODEL / BITMAP / ALPHA_TEXTURE / SOUND_BUFFER2D/3D) by int
  id. **No entities, no lights.** All level entities + lights come from the
  Genesis3D `.bsp` and spawn via the EntClassRegistry queue flush -- this
  resolves the long-open ".rrf vs .bsp" question.
- **The 19 light tables** = Genesis3D `geWorld_SetLTypeTable` animation
  strings (Quake-style brightness-over-time), driven by `setlighttable`.
- **`victims.srf`** -- a victim "type" IS a `ROC_MODEL` id; type `0xE` = a
  random-civilian alias over models 0xB/0xC/0xD; per-type beckon-delay range,
  random spoken-name pool, `Rescue:` anim list.
- **The dev Console** is a script teletype (300-line ring + `` ` `` toggle +
  Enter -> `ScriptEngine::ExecLine`); no native command table.
~20 data renames + ~24 function renames. New KB articles `resources`,
`config` (31 total). Totals: **~1056 symbols**, 103 hypotheses. One more
targeted pass running (actor/motion anim naming + inline tuning constants).

### 2026-08-29 -- loop cycle 24: animation naming + tuning constants (closes the last named gap)
The animation gap is now closed, and it caught three errors:
- **Two motion paths**, neither a state->name table: by-INDEX
  `AnimSeqPlayer::PlayMotionByIndex` (ordinal into the model geMotion list),
  and by-NAME `RenderInstance::SetMotionByName` (only 4 call sites: hose
  viewmodel, `PropModelEnt setmotion`, scripted cameras). **No blending**
  code anywhere. The player has **no motion-selection layer** -- FP tool
  model + literal strings + an inbound anim-event->sound table.
- CORRECTION: the velocity-blend fraction was **inverted** in old notes --
  0.2 grounded / 0.95 airborne (airborne keeps momentum).
- CORRECTION: crouch/stand AABB heights were **swapped** -- standing ~85,
  crouched ~25; the `Crouch`/`UnCrouch` function names are backwards.
- CORRECTION: `RC_ResTimeWarn` fires at **30 s** remaining, not 120.
Full confirmed tuning-constant list recorded (speed caps, gravity, jump,
O2 drain/refill, burn accrual, mission-timer x difficulty, axe reach probe,
RescueBot 600u / 4-8s). ~9 fn renames + 2 data + 3 correction plate
comments. KB `player-movement` / `animation` / `mission-win-lose` corrected.
Totals: **1066 symbols**, 107 hypotheses, 31 KB articles.

### 2026-08-29 -- loop cycle 25: adversarial final sweep -> labeling pass VERIFIED COMPLETE
An independent adversarial sweep (its brief: *disprove* the stop line) re-swept
the binary, cross-checked all 31 KB articles, and spot-checked residual `FUN_`
across every segment. Verdict: **no game system, mechanic, or level-designer-
facing feature remains undocumented.** Fresh stats: 5835 total / 4119 named /
1716 bare `FUN_` / 60 thunks -- the residue is MSVC CRT/STL, BluelAPI
container/serialization one-liners, and ctors/dtors/accessors on already-named
classes. Two wording fixes applied to the KB `coverage` article:
- `AshSim` **does exist** -- a cosmetic ~100-mote player-attached ash/ember
  emitter (7 named fns, `setashdensity` verb). Not a general weather system.
- A **difficulty mechanism** exists (`difficulty` verb + a "Difficulty" script
  var + timer/pitch/hose multiplier triples); what is absent is per-level
  difficulty data in `TLevelInfo`.
Added one symbol row: `PlayFullscreenMovie` (0x00485650, DirectShow player for
`Data\Movies\{Mekada,ActiValue,End}.avi`).
**The autonomous labeling loop is stopped.** Deliverable = the Ghidra project +
`research.db` + `research/research.html` (Knowledgebase tab, 31 articles).
Standing follow-up: a manual-GUI `create_function` pass for the ~40
undefined-region bodies that carry plate comments but no function object
(headless-VM limitation), tracked in the research_tasks queue.

### 2026-08-29 -- loop cycle 26: player-movement deep dive (jump determinism + exploit analysis)
Two independent read-only analysts fully traced the movement pipeline
(`PlayerRescueEnt::Think` input->pos, `Verb_Jump`, `IntegrateVelocity`,
`ClipVelocityToPlanes`, the `Vec3_DampToward` velocity blend,
`Damp_RateFromFraction`, `Xform_BuildYawBasis`, `GameClock::ComputeFrameDelta`).
The two reports agree on every overlapping point.

**Jump height** is deterministic on flat ground at the 60 FPS cap (vy driven
to ~0 by the prior-frame ground-clip, `vy += 400` additive, gravity -1000,
apex ~80u). Bounded deviations: dt clamp `[1/60, 0.1]` + single Euler step;
taking off from a `<=60deg` slope (retained tangential velocity folds +-vy
into the impulse -- uphill higher, downhill lower); the script-only `push`
channel `+0x5b8`; tool constraint planes; ladder-jump transition timing.
NOT moving platforms (positional depenetration only, no velocity inheritance)
and NOT crouch. No accidental double-jump (500 ms wall-clock cooldown stamp).

**Exploits:** diagonal-speed bug ABSENT (conditional pre-scale normalize ->
diagonal == forward == 400, pure strafe = 300); strafe-jump / air-accel
ABSENT (convex-LERP blend, no `dot(vel,wishdir)` term); bunnyhop speed-build
ABSENT (jump adds only vertical; cooldown + grounded drag/blend reset
overspeed each contact); pitch/view momentum ABSENT (yaw-only basis, vel.y
restored around the blend); wall-surf MARGINAL (100% tangential preserved but
no ramp geometry in the maps); scripted-freeze slide EXISTS by design (the
freeze flag `+0x345c` blanks the wish vector only, momentum coasts).

CORRECTIONS: airborne per-axis drag is `(0,0,0)` not `(1000,0,1000)` --
grounded-gated select `{grounded (1000,0,1000) / airborne (0,0,0) /
climb|spectator (1000,1000,1000)}`; this zero-airborne-drag is what
preserves horizontal air momentum. Locomotion state 3 = standing, 4 =
crouched (the `player-movement` article header had them reversed; 260 speed
is the CROUCHED cap).

8 fn renames (`GameClock::ComputeFrameDelta/Tick/Pause/Resume`, `Vec3_Negate`,
`Vec3_DampToward`, `Xform_BuildYawBasis`, `_CIpow`) + 3 data
(`g_gameClock` struct, `g_frameDeltaTime`) + 7 plate comments. New KB article
`movement-tech`; `player-movement` corrected/expanded. Totals: 1074 symbols,
110 hypotheses, 32 KB articles.

### 2026-08-29 -- loop cycle 27: correction -- 0x00502ab8 is the GameClock, not a music manager
While committing the cycle-26 movement renames, the rename of `0x00502ab8`
collided with an existing label `g_musicManager` (from a cycle-8 guess). A
tie-break analyst decompiled the whole `0x0042bexx-0x0042c2xx` cluster:
every function is pure timing (`timeGetTime`, `*0.001`, a `Sleep(1)` 60 FPS
busy-wait cap, a `dt=0.1f` 10 FPS floor clamp, pausable-accumulator
bookkeeping) with **zero** FMOD/audio calls. Cycle 8 found the right call
sites (pause on `WM_ACTIVATEAPP`, tick every frame from
`GameSession::PumpPendingState`) but named the object wrong -- audio is the
sibling `ROCSoundSystem::Frame` call.

Corrected: `g_musicManager` -> `g_gameClock` (a dual-source clock struct:
SubTimer[0] "game" time paused by menus/focus, SubTimer[1] "real"
free-running, +0x40 selector, +0x44 `g_gameTimeMs`, +0x4c `g_frameDeltaTime`).
`0x00502ad8` -> `g_gameClock_realTimer` (the 2nd SubTimer),
`0x00502af8` -> `g_gameClock_timeSource` (the selector byte) -- both were
still `DAT_` defaults in Ghidra (the "music" names had only ever been in
notes). `FUN_0042c110` -> `GameClock::SetTimeSource`, `FUN_0042c1a0` ->
`GameClock::Reset`. All 5 cycle-26 GameClock names confirmed correct.

PROCESS FIX: resolve conflicting evidence BEFORE dispatching a commit, and
every commit-subagent prompt now must say "if the current name is not the
`FUN_`/`DAT_` default, STOP and report -- do not overwrite a meaningful name."
The cycle-27 commit subagent correctly halted on 3 such items.

### 2026-08-29 -- loop cycle 28: victim/survivor spawn deep dive
Two read-only analysts fully traced the survivor spawn pipeline (one
spawn-source run was killed by a 429 and re-run). Findings:

**Positions are 100% authored.** Each `RCS_VictimRescue` entity in the `.bsp`
contributes its editor `origin` as "spawn-spot #0" (`Construct` copies the
`geEntity` user-data struct at `entity+0x150`). Each `RCS_VictimAltSpot`
marker referencing that survivor appends one more `{vec3,angle}` spot
(`VictimAltSpotEnt::Spawn` 0x00459840 -> `RegisterSpawnSpot`). Then the
two-phase `EntityManager::FlushSpawnQueue` (0x0049c670: pass 1 `Spawn` on
all, pass 2 `SpawnAtSpot` on all -- pass 2 UNCONDITIONAL) has `SpawnAtSpot`
(0x0045d5b0) pick ONE spot uniformly at random
(`idx = Ftol(Rand_FloatRange(0, count-0.01))`), copy it into `entity+0x4c`,
floor-snap, and random-roll a skin variant. One draw, never re-rolled.
No alt spots -> draw forced to 0 (spawn on the editor origin). Type `0xE`
also rolls the MODEL id (`Rand_Below(3)+0xB`); skin + idle anim are RNG too
-- none of these is ever a position.

**Count:** one survivor per editor `RCS_VictimRescue` PLUS runtime "herodown"
survivors from `Verb_Reset` (hardest difficulty, <3 FF down), each placed
deterministically at the player reset origin. Only two runtime creators; no
K-of-N / pooling / count randomization.

**Determinism / memory-mod:** the RNG is one global MT19937
(`g_mtState` 0x00501a28) seeded once by `WinMain(timeGetTime())`, never
re-seeded -- so the UNPATCHED spot index is unpredictable run-to-run and
retry-to-retry. But placement is fully pinnable: patch the spot SArray at
`victim+0x200` (STABLE), or overwrite `victim+0x4c` post-spawn and set FSM
state `+0x120=4` to kill the only self-motion (STABLE). State-7 roam is a
bounded leash (radius `+0x224` around fixed anchor `+0x218`), never re-runs
placement, never snaps back to a spot.

CORRECTIONS: `FUN_0049c670` `EntityList::DrainAndDestroy` -> 
`EntityManager::FlushSpawnQueue` (it is the spawn bring-up, not a teardown).
`Rand_PlusMinus1` (0x00404ac0) -> `Rand_Float01` (returns [0,1), not
[-1,1] -- the `|0x3f800000 ... -1.0` bit trick).

~11 renames (MT19937_* trio, EntityManager::*, VictimInstance::Construct,
Entity::PostSpawn_Nop, g_mt* globals) + 8 plate comments. `victims` KB
article rewritten with the full placement model + a determinism/memory-mod
section. Totals: 1088 symbols, 113 hypotheses, 32 KB articles.

### 2026-08-29 -- loop cycle 29: SPEEDRUN cluster 1 of 4 -- framerate x collision
New multi-cluster speedrun/TAS deep dive (owner's primary interest). Cluster 1:

**Framerate.** View rotation is FPS-INDEPENDENT by design -- `yaw/pitch +=
GetAxis(...)` with no `dt`, and the DirectInput mouse delta is *summed* over
the frame (`DrainMouseBuffer` 0x00437ab0); `GetSteerAxis` = `-MouseSens *
accum`, linear. BUT the mouse buffer is `DIPROP_BUFFERSIZE = 10` and the
drain accepts `hr==0` only -> on `DI_BUFFEROVERFLOW` it drops the ENTIRE
frame's motion ("Lost sync with mouse", no `GetDeviceState` fallback);
a high-poll mouse overflows even at 60 FPS during fast flicks. `MouseExc`
cvar is DEAD (zero readers).

Jump APEX and AIR TIME are FPS-DEPENDENT (`disp = v*dt + 0.5*a*dt^2` after
`v += a*dt`): apex 80u ideal / 73.3u at the stock 60 FPS cap / 40u at 10 FPS;
air time 0.800 / 0.767 / 0.600 s. Ground top speed is FPS-independent
(dt-corrected blend). **Removing the `Sleep(1)` 60 FPS cap in
`GameClock::ComputeFrameDelta` (0x0042bf70) is a STABLE speedrun lever:
~9% higher, ~4% longer jumps.** No low-FPS double jump.

**Collision / OoB.** The player's only per-frame world collision is
`BModelEnt::DepenetrateFromWorld` (0x0043f6d0) on the PRE-move pos, then a
RAW unclamped `pos += disp`. The swept `geWorld_Collision` box yields a
binary response: `normal.Y>=0.5` -> `disp` UNCHANGED (full move applies, no
surface clamp); `normal.Y<0.5` -> `disp`+vel zeroed (dead stop). The
push-out virtual `[vtbl+0x38]` is `0x0043f670 = RET 4`, an empty stub in
every entity vtable, so depenetration CAN NEVER eject the player.
- Clean high-speed wall clip: **ABSENT** (swept test catches even a 40u
  `dt=0.1` step -> hard stop).
- Fall through a thin floor / off a thin ledge: **EXISTS, FPS-dependent** --
  the walkable branch doesn't shorten `disp`, so one large-`dt` frame plants
  the whole AABB past a thin brush over a void. Wants a weak machine or a
  deliberately induced hitch at the spot.
- Start-solid ooze-through: **EXISTS, NON-DETERMINISTIC** (engine plane choice).
- Moving `TE_BModel` push/crush/carry: **ABSENT** (movers touch nothing on
  the player). Player has **no auto-step** (`+0x138` is `BModelEnt::Think` only).
- `SnapToFloor` miss -> teleport to world origin `(0,1,0)`, non-fatal,
  respawn-path only.

CORRECTIONS: `MouseExc` cvar dead; `GetCollisionBlock` +0x68 is the
world-space origin not "AABB half-extents"; `TraceToFloor` is a general
swept-box trace not a floor ray.

10 renames (`GameControls::PollFrame`/`GetSteerAxis`, `RawInputActions::*`,
`DirectInputProvider::Drain*Buffer`, `Entity::PushOutOfPlane_STUB`) + 12
plate comments. New KB article `speedrun-tech` (draft); `movement-tech` /
`input` / `config` updated. Totals: 1098 symbols, 116 hypotheses, 33 KB
articles. Remaining speedrun clusters: 2 sequence breaks, 3 movement boosts,
4 timing/routing.

### 2026-08-29 -- loop cycle 30: SPEEDRUN cluster 2 of 4 -- sequence breaks

**escape-verb delivery skip (EXISTS, STABLE).** `Verb_Escape` (0x004513a0)
unconditionally tail-calls `DeliverCarriedSurvivor` (0x00451160, Ghidra-
misnamed `TeleportToCheckpoint`); its only gate is "carrying a victim". No
zone / ground / distance check. `Verb_FinalExit` (0x00450a00) has NO
positional check and a strict `outstanding == 0` gate. Route: walk over each
victim (pickup is automatic on AABB overlap), `escape` from anywhere, repeat
to `delivered == total`, then one `finalexit` from any position.

**climb-verb fly (EXISTS, STABLE).** `Verb_Climb` (0x0044c0b0) guard field
`+0x3300` is the carried/thrown-object pointer, NOT an on-ladder flag --
**the game has no ladder subsystem at all** (zero "ladder" strings). State 5
= gravity 0, wish rotated by the full yaw+pitch basis (look-up + forward =
ascend), `vel.Y` driven by the wish, 400 cap, collision still active (fly
through open space, hard-stop on solids). No leash / timeout / recheck.
Exits: `unclimb`, `reset`, or JUMP (state->3, `vel.Y = 400` SET, ~73u bounce).

**dev mode via FDNYFirefighter.cfg (EXISTS, STABLE).** `[DEBUG] ImADeveloper
= 1` writes `g_devMode` (0x00502a78) before `GameControls::Construct`, which
gates the `DBG_*` BindKey block. **F7 `DBG_TOGGLE_SPECT`** = noclip
(skips depenetration) / gravity 0 / 800 cap fly + mission-timer immunity
(forced >=120s each frame) + smoke pinned clear; exit with JUMP. **F8
`DBG_WIN_LEVEL`** = force victim counters equal + `UpdateZonesAndWin` (instant
win on exit-less levels). **F6 `DBG_SKIP_HOTSPOT`** skips hose hotspots.
`ImADeveloper` does NOT enable `SkipMenus` (separate byte) -- menus/spawns
unchanged. `DBG_CLOSE_ALL_AREAS` (F4) unverified (undefined poll fn).

**Defended:** the victim-counter race -- `SpawnAtSpot` re-arms the `-1`
sentinel on every victim spawn; over-delivery softlocks the strict `==0`
gate rather than skipping it.

Access note: `escape` / `finalexit` / `climb` are in the `OnScriptCommand`
stricmp chain with no extra gating -- any level script, or the dev console
when `g_devMode` is on. Pure retail (unmodified `.cfg`, no cooperating
script) cannot reach them.

CORRECTIONS: `UpdateZonesAndWin` gate is `>=` (`Verb_FinalExit` is strict
`==`); `g_skipIntroMovies` -> `g_devMode` (skip-intro is one effect of the
`ImADeveloper` cvar, not its identity); `+0x3300` is the carried-object
pointer not an on-ladder flag. Flag: `ResetLocomotionState` calls a symbol
named `OnLevelWon` near its end -- likely a mislabel, verify later.

5 renames (`Sys_FileExistsA`, `BuildMapBspPath`, `BoundsTrigger_SetEnabled`,
`Vec3_Set`, `Xform_ComposeYawPitch`) + `g_devMode` + 9 plate comments.
`speedrun-tech` "Cluster 2" section; `mission-win-lose` / `player-movement` /
`config` updated. Totals: 1103 symbols, 120 hypotheses, 33 KB articles.
Remaining speedrun clusters: 3 movement boosts, 4 timing/routing.

### 2026-08-29 -- loop cycle 32: SPEEDRUN cluster 4 of 4 -- timing / routing (deep dive COMPLETE)

**Time during menus/loads/movies/alt-tab.** `g_gameClock` has SubTimer[0]
"game time" (pausable, feeds `g_gameTimeMs` cooldowns) and SubTimer[1]
"real" (free-running). `MenuManager::Activate` -> `SetTimeSource(1)` pauses
SubTimer[0] on menu-open. BUT the **mission countdown** (`PlayerRescueEnt`
`+0x32fc`, decremented `-= g_frameDeltaTime` per `Think` frame) is **NOT**
paused by the in-game ESC menu -- `TickSubsystems` has no pause gate, the
world keeps simulating. Alt-tab and fullscreen movies DO freeze it (no
frames run). An in-game level load costs <= 0.1 s (the `dt` clamp). No
global time-scale: `settimescale` / `addtimecmd` act only on one
`ScriptedBModelEnt` sequence clock.

**Movie skip.** `PlayFullscreenMovie` is a bare message pump; **any keyboard
key-down aborts it** (mouse clicks do NOT). Intro AVIs gated only by
`g_devMode`. `SkipMenus` (`g_dbg_skipMenus`) is MISNAMED -- it only gates
error-log writes; there is no skip-menus feature.

**Split-relevant RNG.** Fire spread is **fully deterministic**
(`FireRescueEnt::AdvanceSpread` = `progress += dt*speed` on authored
PathPoints, zero `Rand_*`). RescueBot rescue timing IS RNG
(`ArmRescueAttemptTimer`, 4-8s dropoff) and feeds `UpdateZonesAndWin` -- but
the bot is opt-in (`runrescuebot` verb / dev key). The only always-present
split-relevant draw is the victim spawn-spot pick.

7 renames (`Movie_*`, `g_moviePlaying`, `__ftol`, `ScriptSeq::AddTimedCommand`,
`g_dbg_skipMenus` -> `g_logErrorsToFile`) + 6 plate comments. `speedrun-tech`
gained "Cluster 4" + a "Summary - the speedrun picture" section and was
flipped **draft -> stable**. Totals: 1112 symbols, 125 hypotheses, 33 KB
articles, 202 events.

**The 4-cluster speedrun deep dive (cycles 29-32) is complete.** Headline
findings, all in KB `speedrun-tech`:
- `escape` x N + `finalexit` from anywhere = victim-gate + exit-walk skip
  on every rescue level (needs script/console verb access).
- `climb` verb = persistent zero-g fly (no ladder system exists); collision
  still active.
- `ImADeveloper = 1` in `FDNYFirefighter.cfg` = F7 noclip-fly + timer
  immunity, F8 win-check, F6 hose-hotspot skip.
- `finalexitloadmap <map>` accepts any real `.bsp` with no campaign-order
  check -> direct level warps.
- Uncapping the 60 FPS `Sleep(1)` -> ~9% higher / ~4% longer jumps. No
  bhop / strafe-jump / air-accel / diagonal-speed / tool-recoil / damage
  boost exists.
- Only no-access OoB: a large-`dt` (low-FPS or hitched) frame overshooting
  a thin floor over a void. No clean wall clip.
- In-game menu does not pause the mission timer; movies skip on any key;
  fire spread is deterministic.

### 2026-08-29 -- loop cycle 33: exhaustive ImADeveloper / g_devMode inspection

**Set:** `g_devMode` (0x00502a78) is written exactly once, by `Config::Load`
(0x00426650) from `FDNYFirefighter.cfg` `[DEBUG]`; a bool cvar accepts **only**
the literal `true` / `false`. Absent key -> `.bss` 0. **Sole writer** -- no
`set` verb, console, key, menu, registry, env, or command-line path (xref +
pointer byte-scan: 17 reads, 0 other writes). `Config::Save` round-trips it
but never re-loads. RNG is seeded before `Config::Load`.

**17 readers.** bind-only (poll unconditional): the `GameControls::Construct`
BindKey block @0x0043a009, and thereby F4/F7/F8 (`DBG_CLOSE_ALL_AREAS`,
`DBG_TOGGLE_SPECT`, `DBG_WIN_LEVEL`) -- their poll sites have no `g_devMode`
test. poll-time: GenErr sev-2 console routing, `Dev_PollStatsToggles` (F2/F3),
the intro AVIs, RescueBot F6, the <15fps HUD sprite, PlayerHose F6, F11
crash, **the dev-console object creation** (`roc_CreateEngineSubsystems`
@0x00499361 -- the console does NOT exist without devMode, so a retail player
cannot open it by any key or cfg), F12 quit, `geEngine_EnableAppLog`, and
the two `MainWindow::Init` video-mode-dialog branches. devMode does NOT
change assert/fatal handling, spawns, mission timer, physics, RNG, level
unlocks, or the title/briefing/slot menus.

**F4 `DBG_CLOSE_ALL_AREAS`** is a MapZone cull toggle, not a trivializer:
`TE_BModel::UpdateMapZones(this, 0)` -> `geWorld_OpenModel(...,0)` per zone,
forcing every ticking area/region-divider brush into its closed BSP state.
No fire clear, no exit open, no win.

CORRECTION: the cycle-32 `SkipMenus` / `EnableDebugLogging` byte assignment
was swapped. Definitive from the `g_cvarTable` records: **0x00502a79 =
`EnableDebugLogging`** (`g_logErrorsToFile` -- `FDNYFirefighter.log` gate);
**0x00502a7a = `SkipMenus`** (`g_cfg_skipMenus` -- `GameSession::
ReturnToFrontend` auto-advances the first 3 frontend screens, a real
feature). DB rows + `config` / `speedrun-tech` KB fixed.

8 renames (`Dev_PollStatsToggles`, `GameSession::ReturnToFrontend`,
`g_devConsole`, `g_skipMenus_frameCounter`, `g_devPerf_fps`,
`DevConsole::Shutdown`, `Config::Load`, `g_dbg_enableDebugLogging` ->
`g_cfg_skipMenus`) + 7 plate comments. New KB article **`dev-mode`** (full
reader table). Totals: 1118 symbols, 129 hypotheses, 34 KB articles.
Manual-GUI item added: `create_function` at 0x0043dc60 `TE_BModel::Think`
(holds the F4 poll) and 0x0043c1a0 `TE_BModel::UpdateMapZones`.

### 2026-08-30 -- loop cycle 34: victim spawn-selection for memory-modding + full cvar table + exact FPS-uncap patch

**Victim spot list layout.** `victim+0x200` is an `SArrayPtr` of POINTERS,
not the records: `victim+0x200` -> P (array of 4-byte ptrs); `P[idx]` -> a
separate `operator new(0x10)` record `{float x@0, y@4, z@8, angle@0xc}`. So
`record = (*(void***)(victim+0x200))[idx]` -- two dereferences (this is why
inspecting `+0x200` directly shows an SArrayPtr header, not records).
`+0x204` = count. Append-only during load: #0 = editor origin, #1..N =
`RCS_VictimAltSpot` markers in `.bsp` order.

**No "spawn group" selector exists.** `TLevelInfo`, `thegame.lev`,
`LoadLevel`, the flush, and `RCS_VictimAltSpot` user-data all lack any
group/set/layout index. `FlushSpawnQueue` pass 2 calls `SpawnAtSpot()` with
no args. A whole-level layout is purely the aggregate of N independent
per-victim `Rand_FloatRange` draws off one MT19937 stream in `.bsp` order.

**Reproducing a layout = freeze the 3 MT globals at the flush.** The spot
draws run synchronously inside the blocking load (`GameSession::LoadLevel`
0x0042ab00 -> `World_LoadBSP` -> `EntityManager::FlushSpawnQueue` 0x0049c670,
pass 1 Spawn then pass 2 SpawnAtSpot, all in `.bsp` order, before the first
frame). Seed = `Rand_Seed(timeGetTime())` in WinMain, never re-seeded, so
the unmodified layout differs every boot. K = 2 MT words per ordinary
victim (idle pose + spot index), 3 for type-0xE. Snapshot/restore
`g_mtState` (624 dw @0x00501a28) + `g_mtWordsRemaining` (0x004ff370) +
`g_mtNextPtr` (0x005023ec) at `0x0049c670` entry -> exact layout reproduces
(everything else in the flush is deterministic).

**FPS uncap -- exact patch.** `GameClock::ComputeFrameDelta` (0x0042bf70)
`while (dt <= 1/60) Sleep(1)`. Cleanest: at **VA 0x0042bfbf / file offset
0x2bfbf** change `74 3E` (`JZ`) -> `EB 3E` (`JMP`) -- entry guard becomes an
unconditional skip over the wait block (PUSH/POP EBX skipped as a pair).
Alt: zero the double at **0x004dfa20** (`11 11 11 11 11 11 91 3F` -> 0),
only 2 xrefs both in this function. Leave the separate `dt=0.1` 10-FPS
clamp (guard 0x0042c00e) alone. Effects: jump apex +9% (73->80u), air
time +4%; ground speed + mouse-look unchanged.

**Cvar table.** `g_cvarTable` @0x004ff4c0 = EXACTLY 10 records
`{u32 sectionId, char* name, void* target, u32 typeId}`, zero-terminator at
0x004ff560. `Config::Load` walks to that terminator; `Config::Save` writes
all 10; NO special-cased keys (no include/version/exec/path override). A
Config::Save-generated `.cfg` is the complete recognized set. `Driver`/
`Mode` are STRING cvars (typeId 5, `String*` targets), not int indices.

CORRECTIONS: `0x0049c670` DB row was still `EntityList::DrainAndDestroy`
with a teardown summary (stale since the cycle-28 Ghidra rename) -> fixed
to `EntityManager::FlushSpawnQueue`. `g_cvar_videoDriver`/`g_cvar_videoMode`
"s32 index" -> STRING cvars. `SpawnAtSpot` skin/material = deterministic
per-type round-robin (`DAT_00502cc0`), NOT an RNG draw.

~5 renames + 8 plate comments. `victims` KB reworked (SArrayPtr indirection
+ Q5 decision table); `speedrun-tech` got the exact FPS patch bytes;
`config` cvar list verified. Totals: 1119 symbols, 134 hypotheses, 34 KB
articles.

### 2026-08-30 -- loop cycle 35: frame-rate dependence full audit (cluster 1)

Swept all 55 `g_frameDeltaTime` (0x00502b04) readers + every per-frame tick.
**Almost nothing that matters is FPS-dependent.** FPS-DEPENDENT list:
- **Jump / gravity arc** -- `IntegrateVelocity` (0x0049dbc0) +
  `PredictDisplacement` (0x0049de70) + the collision-slide re-integrate all
  compute the move delta as `v*dt + 0.5*a*dt^2` after `v += a*dt` (class ii);
  apex 80u ideal / 73u @ 60 / 40u @ 10. Any accelerating body traces a
  different path per FPS; constant-velocity movers are fine.
- **Coyote ledge-jump window** = one frame = `dt` s (class v): ~17ms @ 60 /
  ~100ms @ 10 / ~4ms uncapped. **Uncapping for jump height shrinks the coyote
  grace** -- a real trade-off.
- **`push` / `+0x5b8` channel** -- `vel += push*dt` then zeroed each frame
  (class v); a one-shot script `push 0 N 0` delivers `N*dt`, ~6x at 10 FPS
  vs 60 (script-only).
- **`FrameThrottle::Tick`** (0x0049c630) -- pure frame counter (class iii),
  callback every N+1 frames -> gated behaviour runs at `FPS/(N+1)` Hz.
  Consumer in the ROCMessageMapper area, untraced.
- **`Tool_Hose::Tick` `+0xc60 += 40.0`/frame** (class iii) -- accumulator
  while the free-roam stream hits a surface; consumer untraced, probably VFX.

FPS-INDEPENDENT (confirmed): the entire time layer (mission timer `-= dt` =
walltime; O2 drain `g_gameTimeMs`-gated discrete; O2 refill / burn / smoke
dt-aggregate; script `wait`; footstep throttle; jump cooldown; bot timers),
the horizontal velocity blend (`Vec3_DampToward` composes exactly -> no
strafe-jump/bhop at any FPS), all camera / view / tool-deploy smoothing
(`Damp` force-sub-steps at fixed 0.01s), animation playback (continuous
float motion-time, no integer frame index exposed).

CORRECTION: **`Hotspot::AdvanceActiveFire` is FPS-INDEPENDENT** -- the
hose-minigame douse cursor is wall-clock latch-gated (`WindowFire::Think`
`-= dt/blockSec` countdown), not a per-frame constant. Douse = `sum(blockSec)`
real seconds at any FPS; the only FPS effect is a low-FPS *penalty* floor.

Also noted: moving brushes (`TE_BModel::Think`) snap to their endpoint (no
overshoot) but have **no rider-carry code** -- a fast lift at low FPS can
leave a rider behind or clip them, since the rider is only moved by the next
physics depenetration.

New "Frame-rate dependence - full audit" section in `speedrun-tech` (with a
table); `movement-tech` coyote trade-off updated. 1 rename (`Damp`) + 8
plate comments. Totals: ~1120 symbols, 137 hypotheses, 34 KB articles.
Follow-up queued: trace the FrameThrottle / Tool_Hose+0xc60 consumers +
cluster-3 overshoot quantification.

### 2026-08-30 -- loop cycle 36: frame-rate audit -- final gaps closed, AUDIT COMPLETE

Both "class (iii) frame-counted" flags from cycle 35 turned out to be
non-issues:
- **`FrameThrottle::Tick` (0x0049c630) is DEAD CODE** -- vtable slot 0,
  never invoked; `GameSession::TickSubsystems` calls slot 1 (the real
  one-shot queue drain) directly every frame; the divider N is always 0.
  Renamed `DeferredQueue::Tick_UNUSED`.
- **`Tool_Hose::Tick +0xc60`** (`+= 40.0`/frame) has **zero readers in the
  entire binary** -- a vestigial "gallons" counter never wired up.

**Thin-geometry overshoot OoB quantified -> NEAR-ABSENT**, downgrading the
cycle-29 "EXISTS, FPS-dependent" verdict. Player AABB is 60 wide / 115 tall
standing (55 crouched). Even at the `dt=0.1` clamp the per-frame step is
~40 u running / ~55 u in a half-second fall -- **smaller than the body**, so
the box always still straddles thin geometry the previous sweep caught. Only
a crouched player in a developed ~1 s fall (~105 u/frame) could clear a
<= ~55 u floor slab over a void -- and the swept `geWorld_Collision`
(start .. start+disp) defeats even that for a real in-mask solid. The
5-iteration slide cap fails safe (zeroes disp+velocity, never leaves the
player embedded-and-moving). Genuine OoB = content-flag gaps + BSP seams
(dt-independent) + the `SnapToFloor`-miss teleport.

**FRAME-RATE DEPENDENCE AUDIT COMPLETE.** Final FPS-dependent list:
1. Jump/gravity Euler arc -- apex 73u @ 60 / 40u @ 10 / 80u uncapped.
2. Coyote ledge-jump window = one frame = `dt` s (~17ms @ 60 / ~100ms @ 10 /
   ~4ms uncapped -- uncapping for jump height COSTS the grace).
3. Script-only `push`/`+0x5b8` one-frame kick = `push*dt`.
(+ situational moving-brush rider-carry gap; NPC-only `Vec3_MoveToward`
snap.) Everything time / damage / script / camera is wall-clock,
dt-aggregate, or dt-corrected. No strafe-jump / bhop / air-accel at any FPS.
No clean wall clip or overshoot OoB at any reachable frame rate.

1 rename + 5 plate comments. `speedrun-tech` "Frame-rate dependence - full
audit" table + Cluster-1 OoB section updated. Totals: 1119 symbols, 139
hypotheses, 34 KB articles, 223 events.

### 2026-09-01 -- save-state feasibility study (Half-Life-style quicksave/quickload)

Owner wants speedrun-practice save states: snapshot on a key, restore on
another, retry a section without replaying the mission. Verdicts:

- **(A) Use / extend the native save -> IMPRACTICAL.** The only native
  persistence is `TPlayerInfo` -> `saves\playerN.rb` (obfuscated RichBinary):
  furthest mission unlocked, difficulty, per-level best-score stat maps.
  Auto-written on mission win (`TPlayerInfo::RecordLevelComplete 0x00424650`
  -> `TPlayerInfo::Save 0x004243f0`). **No live-state serialization** -- no
  `Entity::Serialize` vtable slot, no `DEFINE_FIELD` tables, no `WriteFile` of
  `g_session`/entities. The generic RichBinary named-block writer/reader is
  reusable but nothing except `TPlayerInfo` is plumbed in.

- **(B) External ASI within-level memory snapshot -> tiered.** Mechanism is
  easy: EXE is **single-threaded** (zero `CreateThread` calls); sim + render
  are back-to-back on the main thread in `GameSession::RunFrame 0x0042aea0`.
  Clean **safe point 0x0042af0f** (return of `TickSubsystems`, before
  `RenderFrame`): spawn queues + deferred-callback list drained, no
  ctor/dtor in flight, renderer not yet mutated. Renderer / sound /
  geWorld-collision all **LEAVE** -- `geActor`/`geWorld`/`geCamera` handles
  are level-lifetime; `RenderInstance::ApplyPoseAndScale 0x00492450` +
  per-entity `geWorld_SetModelXForm` re-push transform/pose from game memory
  every frame; fmod tolerates stale channel handles; BSP collision is
  read-only. The hard part is the **entity lifecycle**: `operator_new`
  (0x004a5a84) is a plain malloc wrapper (no pool), every entity a separate
  heap block freed on death, tracked by a fixed 512-slot generation-checked
  `EntityHandleTable` (0x0049b9e0), graph linked by raw pointers
  (`player+0x3534` carried victim, `hotspot+0x08[]`, `EntityRef` offset-0,
  `g_mtNextPtr`).
  - **B1 (player block 0x3580 + GameSession block 0x6f4 + MT triple +
    g_gameTimeMs) -> EASY-MODERATE**, all stable-address whole-run blocks,
    pure in-place memcpy, no fixup. Covers movement/jump/route practice.
  - **B2 (+ handle table + all entity blocks via class->size table + SArray
    buffers) -> MODERATE**, correct only if entity population is unchanged
    between snapshot and restore.
  - **B3 (robust to population change) -> HARD**, needs HL-style handle-keyed
    serialization + per-class `FIELD_EHANDLE`/`FIELD_CLASSPTR` pointer-fixup
    + entity re-create/destroy.

- **(C) Full cross-level -> HARD/IMPRACTICAL.** B3 + surviving a level
  unload/reload; equivalent to implementing HL save/restore in full.

`PlayerRescueEnt::Verb_Reset 0x0044ff10` (mislabeled `TeleportToCheckpoint`
was actually `DeliverCarriedSurvivor 0x00451160`; `UpdateCheckpoint` was a
segment/sphere collision helper) restores **player position only** from a
single Vec3 anchor `player+0x3400` -- not a state primitive.

**Recommended:** ship B1 as `FDNYSaveState.asi` (same Zig toolchain as
`FDNYVictimSpawns.asi`), hook 0x0042af0f, F6 snapshot / F7 restore.

5 renames + 4 plate comments. New KB article `save-states-feasibility`
(speedrun group). Totals: ~1122 symbols, 145 hypotheses, 35 KB articles,
225 events.

### 2026-09-01 -- FreemanAPI feasibility study

Owner asked to assess gaycoderprincess/FreemanAPI as a way to host Lua /
a mod menu / trainer toggles in FDNY. **Premise corrected:** FreemanAPI is
NOT a scripting/menu/overlay framework -- it is a self-contained C-ABI DLL
(`FreemanAPI_gcp.dll`, MinGW GCC + C++20 + x86 + -static) that ports
Half-Life 1/2 `PM_PlayerMove` player-movement physics (bhop, air-strafe,
duck, water, fall damage) into an arbitrary game. Zero rendering / input /
hooking / Lua. Host-driven: consumer `.asi` calls `FreemanAPI_Process(delta)`
per frame + registers ~17 callbacks (pos, velocity, view-angle, 7 input
getters, sound, dead flag, 3 collision callbacks). Menu/ImGui/hook machinery
lives in separate sibling libs (`nya-common`, `CwoeeMenuLib`).

Verdicts:
- **Original ask (Lua/menu/trainer host): wrong tool.** Stay with focused
  ASIs (`FDNYVictimSpawns.asi`; `FDNYSaveState.asi` B1 tier). A unified menu
  = separate `nya-common`+`CwoeeMenuLib`+overlay-hook project. FDNY renders
  DirectDraw + Direct3D IM (`Drivers\D3DDrv.dll` = WildTangent "D3D Driver
  v101.1", imports `ddraw.dll`) wrapped by dgVoodoo -> internal D3D11/12, so
  `nya-common`'s custom `imgui_impl_dx8` can't attach; an overlay needs a
  dgVoodoo DXGI-swapchain Present hook or must run headless.
- **FreemanAPI's actual purpose (FDNY plays with HL movement): MODERATE.**
  ~10-14 person-days prototype / ~2-3 weeks polished. No hard blockers. The
  collision bridge is nearly free -- Genesis3D `geWorld_Collision` (IAT
  0x004dc0bc) ~= HL `PM_PlayerTrace`.
- **Full parity incl. tuning-menu UI: HARD** (movement + dgVoodoo DXGI
  overlay hook + ImGui-DX11).
- **Build `FreemanAPI_gcp.dll` with the VM's Zig toolchain: MODERATE** (~1d;
  friction = C++20 <format>/<filesystem>/<numbers> on libc++, vendor 2
  headers).

FDNY movement host interface (mapped this cycle, all on the PlayerRescueEnt
block): velocity `+0xfc/0x100/0x104` (u/s); accel `+0x108/0x10c/0x110`
(gravity .y=-1000 per frame); move-delta `+0x90/0x94/0x98`; view pitch/yaw
`+0x3430/0x3434` (degrees, Y-up LH, no roll); onground byte `+0x12c` (+prev
`+0x12d` = 1 coyote frame; set by `ClipVelocitySimple` on normal.z>=0.5);
hull mins `+0x78`=(-30,-30,-30) / maxs `+0x84` (.Y at `+0x88` = 85 stand /
25 crouch); locomotion state `+0x18c`; input via `GameControls` named-action
queries (no stored wishdir). `GE_Collision` out-struct (0x2c B): Model+0,
Impact+0xc, StartSolid?+0x18, Plane.Normal+0x1c, Plane.Dist+0x28 -- NO
fraction/endpos (glue synthesises). Contents mask 0x43 = SOLID|WINDOW|CLIP,
no water bit. Frame hook = safe point 0x0042af0f.

Biggest risk for goal B: excising the per-frame move step from the
tightly-coupled `PlayerRescueEnt::Think` state machine (`+0x18c`) without
breaking crouch headroom / ladder / passed-out / the scripted mover
`+0x34a4` / tool reaction-velocity injection into `+0xfc`. FDNY also has
zero stair-stepping, so HL step-up traces must be added.

NOT implemented -- feasibility article `freemanapi-feasibility` (speedrun
group) is the deliverable. 6 plate comments. Totals: ~1122 symbols, 151
hypotheses, 36 KB articles, 227 events.


---

## FDNY player-movement architecture -- full RE pass (2026-09-01, movement-architecture loop)

Motivated by the FDNYFreemanMovement.asi v0.1 prototype stalling on
symptom-chasing ("additive replace" = FDNY Think moves the player, glue
overwrites pos/vel after -> jitter, camera drift, origin guesswork). This
cycle mapped FDNY's movement end-to-end so v0.2 can be a single-physics
in-Think detour instead. Full details in KB article "FDNY player movement
architecture" (speedrun group); hypotheses H1-H26 (research.db ids ~152-185).

Key results:
- **No reusable "move entity" function** -- the per-frame move is hand-inlined
  in PlayerRescueEnt::Think 0x00452570. The integrate+world-resolve+commit
  window is the contiguous span **0x00453789..0x00453815** (also the state==7
  jump target): [1] velocity += impulse(entity+0x5b8)*dt inline; [2] CALL
  [primary_vtbl+0x64] = BModelEnt::IntegrateVelocity_thunk 0x0043f680 ->
  Entity::IntegrateVelocity 0x0049dbc0; [3] CALL BModelEnt::DepenetrateFromWorld
  0x0043f6d0 unless spectator (entity+0x380); [4] position += displacement
  (entity+0x90..0x98) inline.
- **FDNY has NO Quake accel/airaccel.** Horizontal control = exponential
  Vec3_DampToward(velocity, wishdir*maxspeed), retain fraction 0.2 vs 0.95 by
  ground/air. Gravity 1000, linear ground friction 1000. Max wish speed 400
  stand / 260 crouch / 800 spectator.
- **Input is string-keyed via one ActionMap singleton**:
  `am = *(uintptr_t*)(*(uintptr_t*)0x00503A78) + 0x334`; GameControls::IsHeld
  0x00438480 / GetAxis 0x004386f0 are __thiscall, plain-string NameTree::Find
  (no hash). Bindings 100% hardcoded (no rebind), no sprint action. A v0.2
  glue can read these directly instead of GetAsyncKeyState -- EASY.
- **Coordinate constants (derived, not dialed):** feet.Y = entity_Y - 30
  exactly (both stances; collision start entity+0x68 == position, offset 0).
  Basis at yaw Y: forward=(sinY,0,cosY), right=(cosY,0,-sinY), RH about +Y.
  Pitch positive = up. Cross-checked FreemanAPI's own AngleVectors +
  NyaMat4x4::Rotate: **v0.1's cb_getview had pitch/yaw SWAPPED in the array**
  (FreemanAPI default order is index0=YAW, index1=PITCH). Correct tuning-free
  feed: array[0]=-entity_yaw, array[1]=-entity_pitch, array[2]=0. Origin feed:
  FreemanAPI_game_Y = entity_Y + 6 (feet-30 + HL2-hull centre-up 36).
- **Camera needs no extra work** -- UpdateLocomotionAndTools 0x0044f340
  re-derives the eye from entity+0x24 (translation == position) AFTER the
  locomotion span, so writing entity+0x4c..0x54 is sufficient; no need to
  register FreemanAPI's SetGamePlayerViewPosition/ViewAngle.
- **Splice safety: SAFE.** UpdatePassedOut, UpdateO2, StepScriptedSequence
  (HUD prompt only), RescueBot::Think, carried-object, StandUp/Duck (names
  were inverted in the DB -- 0x0044bce0 stands up, 0x0044bcb0 ducks; neither
  moves the player), Verb_Jump (writes only entity+0x100 += 400) -- none
  depend on locomotion-span internals beyond the final committed
  pos/vel/onground. States 5 & 7 execute the same span (only 6 bypasses), so
  the v0.2 stub's state-gate must be airtight and replay the vanilla 4 steps
  for 5/6/7 + spectator (byte-exact A/B toggle). The pre-span wishdir/damp
  already writes entity+0xfc, so the stub keeps glue-authoritative velocity
  and restores entity+0xfc from it.

VERDICT: v0.2 design is complete and every constant is derived. Difficulty
MODERATE -- concentrated in the mid-function trampoline + conditional vanilla
replay. NOT auto-implemented at end of loop (fiddly asm splice + needs
playtest iteration with the owner); teed up as the next deliberate step with
a full spec in the KB article.


---

## Player "stuck" mechanics (2026-09-02, player-stuck loop)

Investigated how FDNY detects/recovers from the player being wedged in world
geometry. Full details in KB article "Player stuck -- detection, recovery, and
the checkpoint system" (speedrun group); hypotheses H27-H33.

Key results:
- **No dedicated stuck detector/recovery exists.** No kill-plane/void Y-check,
  no periodic ground-loss check in Think (full call list re-audited, zero new
  calls), zero relevant stuck/unstick/wedge/trapped string or symbol hits.
- **Collision fails safe.** BModelEnt::DepenetrateFromWorld 0x0043f6d0, after
  5 failed depenetration iterations, just calls ClipVelocitySimple (sets
  onground or zeroes vel/displacement) -- no teleport, no rollback. A player
  wedged into geometry stays wedged.
- **The only prevention**: PlayerRescueEnt::StandUp 0x0044bce0 denies the
  crouch->stand transition via roc_SegmentInSolid 0x00492df0 if the standing
  AABB would intersect solid -- a headroom gate, not a general stuck-check.
- **A real bug found**: ModelEntity::SnapToFloor 0x0049da00 unconditionally
  commits zero-initialized locals to entity position on a TraceToFloor miss,
  teleporting to world origin (0,1,0). Verified independently twice. Reachable
  for the player only via ResetLocomotionState (Verb_Reset/OnLevelWon).
- **Recovery is manual only**: Verb_Reset 0x0044ff10 teleports to a spawn
  anchor (entity+0x534), called from exactly 3 places -- dev "reset" command,
  or two O2/health pass-out branches. Never from an OOB/fall check.
- **The checkpoint system**: the spawn anchor isn't fixed per level --
  Verb_Escape 0x004513a0 overwrites it with the player's current pos+heading
  whenever onground + O2 stat > 0.5, right before delivering a carried
  survivor (reached via BoundsTriggerEnt's default "tell player escape").
  This is the mechanism behind the known escape-exploit and means Verb_Reset's
  destination drifts over a run, not level-constant.

VERDICT: stuck is handled by PREVENTION (fail-safe collision + one headroom
gate), not detection+recovery. No automatic rescue exists for a player stuck
mid-play.

### 2026-10-23 -- autosplitter: how staged levels and wins look in memory
Owner report: the LiveSplit splitters split staged levels (boat, subway, oil rig) late.
Evidence and live traces: `docs/evidence/autosplitter/`. New splitters:
`livesplit/` (v2, then v2.1; known-good test in `livesplit/test/`).

**Stage exit.**
- The exit trigger's `touch: tell player (finalexitloadmap <next>)` runs
  `PlayerRescueEnt::Verb_FinalExitLoadMap` (0x450A70): the same strict victim gate as
  `finalexit`, the .bsp existence check, then `GameSession::RequestLevelChange`
  (0x0042A260).
- RequestLevelChange writes `+0x68C = 1`, the next map's String at `+0x660`, the rrf at
  `+0x668`, and copies the old level's info into `+0x664..+0x67C`.
- The next RunFrame clears +0x68C and calls `LoadLevel(this+0x660)`. LoadLevel sets
  `+0x688` (current level) to that embedded block, sets gameState `+0x6F0 = 2`, and later
  `+0x6EC = 0`. gameState then goes 2 -> 3 -> 1.
- No win, fade, sound or results screen on a stage exit.
- Other RequestLevelChange callers: `ScriptTrigger::Verb_LoadMap` (rescue.rrf),
  `Verb_LoadHoseMap` (hose.rrf).

**Win.**
- `Verb_FinalExit` / `UpdateZonesAndWin` set the mission result `PlayerRescueEnt+0x3438 = 1`
  on the win frame.
- `RescueHud::Update` then plays the win audio and fade, and
  `GameSession::SetLevelResult` (0x4295B0) sets `+0x6EC` (1 win / 2 lose) for RunFrame's
  results screen.

**The autosplitter "end sound played" pointers** are per-level sound handle slots. They fill
at level load and change during unrelated levels. On their own map each stays 0 until the
win and fills 0-2 game frames after the win flag (measured on all eight), so the splitters
still use them, guarded by map name (v2.1, owner's method).

**gameState writers:** LoadLevel (2), `MenuManager::TeardownToLoading` (1),
`MenuManager::Activate` (0), RunFrame (3 -> 1). The writer of 3 is not found.


### 2026-10-23 -- demos and ghosts spike (docs/evidence/demos)

**No demo facility exists** in `FDNYFirefighter.exe` or `Genesis.dll` (no demo / record /
playback / timedemo / journal strings or code). Genesis3D 1.1's DirectPlay netcode is not
imported.

**Determinism, measured in-process** (Hourglass-style probe `core/probes/demoprobe.cpp`:
feeds dt and `g_gameTimeMs`, the DirectInput `GetDeviceData` results and the MT19937 state
back after a reload, compares every frame bit-exact):
- **Player movement is deterministic** given dt + input: position, velocity, view, move
  state and `mission.time_left` (+0x343C) bit-identical over ~1950 frames (32 s) on bank, in
  every run. On real dt it diverges (frame ~1280, up to 35 u).
- **The world is not yet.** Found and controlled: the load's real duration in game time,
  victims' load-time RNG draws, and presentation code drawing from the gameplay RNG.
  - `Rand_Float01` 0x00404AC0 inlines its own MT draw; ~860 draws per frame on bank, mostly
    particles.
  - `ParticleSystem::Emit`'s draw count depends on rendering, which shifts the shared
    stream. With a presentation / gameplay RNG split, victims matched for 25 s.
- **Still open:** fire spread diverges at load frame 3. It is gated on
  `FireFx::GetExtents` (the visual size), whose keyframes the level script schedules
  (`FireRescueEnt::HandleScriptCommand` -> `FUN_00447920` -> `FireFx::ScheduleSizeKeyframe`).
  The script's timing in the first frames is not traced.

**Offsets note:** from the player pointer the core uses (`[g_everGame]+0x30`), `+0x32FC` holds
the ASCII bytes `"[Cou"` (constant). The countdown there is `+0x343C` (`mission.time_left`, as in
addresses.json). The cycle-30s note "mission timer `+0x32fc`" is 0x140 lower, so it may use a
different base for the same object. Not resolved.

**Ghost assets:** the game ships `Data\Acts\firefighter.act` / `Fireman.act`; Genesis.dll
exports `geActor_DefCreateFromFile`, `geActor_Create`, `geWorld_AddActor`, `geActor_SetPose`,
`geActor_SetAlpha` and `geActor_SetBlendingType`. The last two are not imported by the
game.

**Verdicts:**

| Feature | Verdict |
|---|---|
| Full demos | MODERATE-HARD, open-ended; not a rewrite (four hook points) |
| Ghost (box) | EASY |
| Ghost (firefighter model) | MODERATE |

Ghosts need no determinism.

**2026-10-24 addendum (autosplitter v2.2):**
- `GameSession+0x68C` is the level-change request byte: set only by RequestLevelChange,
  cleared by RunFrame before LoadLevel.
- Menu starts never write `pendingMap` (+0x660).
- After an in-game load, `+0x688` points at the +0x660 block, so the current and pending
  names are the same string until the next menu start.
- The exit-touch frame can stall (2.5 s seen once) with the game clock frozen (cause not
  traced).
- `docs/evidence/autosplitter/README.md`, "v2.2".

### 2026-10-24 -- demos spike resumed: fire start-up non-determinism (docs/evidence/demos, runs 8-13)

- **Fire strength** is assigned by the fire vtable slot 18 handler 0x004483A0, which takes the
  player as its argument: strength = Chebyshev distance (`max(abs(dx), abs(dy), abs(dz))`) from
  the player to the fire (+0x7C..+0x84). It then calls `SetFireStrength` 0x00448320, which calls
  `TrySpawnSpreadFire`. The spread delay scales with the small-fire counter `DAT_00502B90`.
- **Original game bug:** the handler only runs while `FireRescueEnt+0xB8` is in 0..3, and
  +0xB8 is never initialised (its only writer is `inc` at 0x00448533). Which fires take their
  first heat, and so spread at load time, depends on leftover heap bytes, and varies per load
  in the vanilla game.
- **The level script replays identically** under controlled `g_gameTimeMs`: `ScriptActor::Tick`
  0x00470740 waits on actor+0x48.
- **With the heap zero-filled** (`HEAP_ZERO_MEMORY` in `RtlAllocateHeap` / `RtlReAllocateHeap`),
  plus time, input and RNG fed back and the presentation RNG split, a 58 s bank replay was
  identical on every reliable measure in 2 of the last 3 playbacks.
- **Remaining:** a spawned child fire's position in the entity list (frame 106 in one
  playback). Suspected: free-slot choice in the 512-slot `EntityHandleTable`.
- **Demos verdict:** MODERATE.
