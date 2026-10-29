# FDNYCore release notes

Hand-written: what changed in each version. Downloads are the GitHub releases (the
Releases section of the repository page); each one's zip carries the key sheet generated
for that build. The release workflow copies a version's `## <version>` section below into
its release notes (`CONTRIBUTING.md`, "Branches, commits, releases").

## 0.22.0

**New: the Recordings page** (by Jacob Pilawa; practice only). A **Recordings** button on
the mission briefing opens a list of your recordings for that stage:
- **List:** page through it (PgUp / PgDn, Escape goes back), mark favourites, and give
  runs your own names.
- **Watch:** pick one run, or two to compare, and the stage loads to be watched, not
  played. The game stays paused and a camera follows the run:
  - Space plays or pauses; Left / Right seek; Home restarts; Tab switches run;
  - F switches between following the run and a free camera (W A S D E Q, Shift fast,
    Ctrl slow); drag with the right mouse button to orbit or look; PgUp / PgDn zoom;
  - X shows the traces through walls; Escape goes back to the briefing and the page.
- `fd_replay_view <stage> <file> [second file]` does the same from the console, in the
  stage that is loaded.
- While watching, nothing is recorded and the checkpoint editor, trigger boxes and free
  camera stay off.

**Changed:**
- **Keys only work inside a level.** A bound key does nothing at the menus (the
  Recordings page reads its own keys).
- **Free camera** (`fd_pause`): it is now the same camera as the Recordings viewer. It
  moves at one speed in every direction (diagonals used to be faster), and looking up or
  down stops at 89 degrees.
- **Run FDNYCore on its own.** It no longer looks for the old tools (alttabfix, the FOV
  fix, FDNYDeterministic, FDNYVictimSpawns): static spawns and RescueBot are always
  offered, and a game location another mod changed is reported as a mismatch and left
  alone.
- The key sheet is now called `reference.md` (in the release zip).

## 0.21.0

**New: practice checkpoints** (by Jacob Pilawa; practice only). Boxes you place in a level
that show your elapsed level time when you reach them:
- **Timestamps:** `fd_checkpoints 1` (key N toggles, saved). Touching a checkpoint shows its
  name and `mm:ss:ddd` in the game's own hint text for four seconds, once per attempt. A box
  you spawn inside fires only after you've left it once. Restarting the mission starts a new
  attempt; going to the menu and back doesn't.
- **Editor:** `fd_checkpoints_edit 1` pauses and opens the free camera with the editor.
  - Space or Insert places a box ahead of the camera;
  - left-drag a box or its coloured arrow to move it, a face cube to resize it;
  - the arrow keys and PgUp/PgDn move the selected box (Shift resizes, Ctrl moves 1 unit);
  - Ctrl+C / V / D copy, paste and duplicate; Delete removes; Home / End undo and redo;
  - `fd_checkpoints_name "text"` names the selected box; P leaves the editor.
- **Files:** one per mission, `Checkpoints/<mission>.txt` in the game folder, saved
  automatically as you edit (with one `.bak` of the previous file). Restart the mission to use
  your edits. `fd_checkpoints_save` retries a failed save; `fd_checkpoints_status` shows
  what's loaded. The BSP viewer reads and writes the same format.
- Up to 32 checkpoints per mission. `fd_checkpoints_distance` and `fd_checkpoints_step` set
  where new boxes appear and how far a key moves them.

## 0.20.0

**New: replay** (practice only). This plays your recordings from the recorder back inside the level:
- **What it shows:**
  - the path up to the playback time;
  - a see-through box and a yellow arrow for your position and facing;
  - a white cube for each second of the run.
- **Colours:** one run fades red, then blue, then green over its length. Several runs on one map each get one colour, from green for the fastest to red for the slowest.
- **Line:** it's drawn halfway between your feet and eye level, and at half width while you're in the air. Walls hide it; `fd_replay_xray 1` shows a dimmed copy through them.
- **V:** play or pause. With nothing loaded, V first loads your best finished run and your latest run on the map.
- **`fd_replay`:** opens a list of the map's newest runs. It shows the time, the length, and which runs are finished, best or latest. The game pauses while the list is open; keys 1–9 show or hide a run.
- **`fd_replay <text>`:** loads the first recording whose name contains that text.
- **`fd_replay_ghost`:** 1 starts your best run each time you spawn, so you race it; 2 starts your latest run.
- **More controls:** `fd_replay_speed`, `fd_replay_load`, `fd_replay_list`, `fd_replay_clear`.
- **"Finished" rule:** a run counts as finished when your next recording in the same game session is on another map. Quitting to the menu and starting a different level also counts as finished.

**Fixes:**
- A line that ran from behind you into the distance could vanish under the floor, for example along a straight hallway.
- After a replay, the trigger boxes could stop drawing.
- The recorder and the replay now find the recordings folder the same way.

## 0.19.0

**Download note:** the GitHub release labelled v0.18.1 (2026-10-24) was built from `main` after
most of this work had landed, so it already contained everything up to the recorder. 0.19.0 is
the first build whose version says so, and it adds the 2026-10-24 fixes.

**New:**
- pause + free camera (`fd_pause`, key P);
- fd_juggle observing the safe spot;
- letter keys for every feature (no F keys);
- trajectory recorder (by Jacob Pilawa): player and victim paths as JSONL files in
  `Recordings` beside the `.asi`. Off by default: `fd_record 1` turns it on
  (`fd_record_victims`, `fd_record_directory`).

**Owner fixes (2026-10-22):**
- free-camera mouse at half the sensitivity, left/right no longer swapped;
- FDNYCore.cfg no longer filled with the default binds;
- G turns the warp HUD on visibly;
- one name, the **warp HUD** (`fd_hud_warp`; the old `fd_hud_glitch` names still work);
- T cycles the trigger boxes: off, on, on + x-ray, off;
- trigger boxes no longer lose rectangles or show through walls (drawn right after the world);
- player HUD: x / y / z columns, stance standing / crouched / bugged, no screen line;
- watermark pinned to the bottom-left corner;
- fire-juggling arrow: the frozen spot's coordinates above it.

**Key sheet (2026-10-23):**
- the key sheet is generated from the source on every build: every default key, feature key,
  feature, variable (default, values, saved, practice-only) and command. It's the repository's
  front page, and each release's zip has the sheet for that build;
- `bindlist` also lists the keys features read themselves (free camera W/A/S/D/E/Q,
  warp HUD B/Z/X, O to free the cursor);
- `exec` shows in `cmdlist`;
- help texts give every variable's values (`0 = ...` for each on/off setting, ranges, what the default means).

**Fixes (2026-10-24):**
- only one copy of the core runs: a second `FDNYCore*.asi` in the game folder stays inert,
  writes one line to `FDNYCore.log` naming the first, and shows on the watermark's `asi:` list;
- the watermark no longer lists the running core as another mod when its file is named
  `FDNYCore-<version>.asi`;
- the victim spawn hook has a single owner inside the core (no change in behaviour: static
  spawns pin as before, and the recorder sees every spawn).
