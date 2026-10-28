# Replay line hidden along straight stretches; trigger boxes missing after a replay (2026-10-26)

**Owner report:** in the first hallway on mbase the replay line was missing, while the tick cubes along it showed.

## 1. Line depth near the eye (gfx)

**Cause:** Douglas–Peucker (D50) turns a straight hallway into one long segment. When that segment starts behind the eye, it is cut at the near plane, and the cut end lands far off screen. Clipping to the screen then set each edge vertex's depth by interpolating linearly in screen space. Depth isn't linear in screen space (1/depth is), so the visible part took nearly the far end's depth all along, and the floor covered it.

**Fix:** clip on 1/z, both for strips (`EmitQuad`) and for box faces (`DrawBoxes`).

**Repro:** probe `tests/game/probes/replay_straight.yaml` (deleted, D22; `git show cc1f700:tests/game/probes/replay_straight.yaml`) with `mbase_straight.jsonl`. It is a straight, 700-unit recording that starts 40 units behind the mbase spawn and runs along the view.

`before_after.png`, left to right:
1. Before: only the last stretch shows; the tick has no line under it.
2. With the depth fix: the line runs to the bottom of the screen.
3. With the line raised as well (part 3).

## 2. Box tiles ran out (gfx)

**Cause:** `gfx` kept 4 tile slots, one per alpha level, and never freed them. The replay uses levels 15, 4 and 2, and `draw_layers` uses one more. After that, the trigger overlay's level 8 got no tile and drew nothing.

**Repro:** `replay`, `replay_pick`, `draw_layers`, `triggers` in one shared session failed `triggers.polys = 0, want >= 1`. It failed twice with today's changes and once on PR #19 as it stood (`2efe5f4`). `draw_layers` + `triggers` alone passed on both main and the branch.

**Fix:** 16 slots indexed by alpha level, 8 KB each. The same four tests then pass.

## 3. Line height (owner request)

**Measurement:** the eye is 72 units above the recorded origin (freecam pipe: camera y −110.0, player y −182.0). The origin is 30 above the box bottom.

**Change:** the line and its ticks are drawn 36 units up, halfway to the eye. The box and arrow stay where they were recorded.

## Results

- Game tests, one shared session: `replay_straight` (probe), `replay`, `replay_pick`, `draw_layers`, `triggers` all PASS.
- `triggers` alone: PASS.
- Unit tests: `tests\unit\run.ps1` all pass (24 Python tests).
