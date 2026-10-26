# Practice checkpoints

## Use

In a rescue mission, run `fd_checkpoints_edit 1`. WASD/EQ and mouse look fly;
Space places a box. Left-drag a box or colored arrow to move it, or a face cube
to resize with the opposite face fixed. Custom editor boxes show through walls;
hovered/active handles have white borders. The camera stays still during a drag.
Ctrl+C/V/D copies/pastes/duplicates, Delete removes, Home/End undo/redo.
`fd_checkpoints_name "Display text"` names the selection. Edits autosave to
`Checkpoints/<mission>.txt` beside `FDNYCore.asi`, with one `.txt.bak` backup.
The folder is created on the first save.
P exits; restart the mission to apply edits. `fd_checkpoints 1` enables timestamps
(N toggles; saved). Each native contact shows display text + `mm:ss:ddd` once per
attempt for four seconds; newer checkpoints replace older hints. A box containing
the spawn must be exited before it can fire. Commands and keys: root `README.md`.

## File format

The filename/header use the internal stage key (`training`, `bar_r`, `boat_a`, etc.).
Zero to 32 rows contain min-X/Y/Z, max-X/Y/Z and 1-80 ASCII display characters,
excluding parentheses and `|`. The BSP viewer reads/writes this same format.

```text
FDNY_CHECKPOINTS_V1|training|1
-1380|-300|4580|-1150|0|4810|End of hall
```

## Implementation and verification

- BoundsTrigger factory/admission/initialization: EXE `0x441d70`, `0x429490`,
  `0x441040`. Game-owned roots retain service-owned definitions until teardown.
  The contact gate (`0x440ce0`/`0x440dc0`) requires a nonempty event table; one
  unused binding enables it. FireEvent (`0x4705b0`) observes only these roots.
- Native hints (`0x4564c0`, Player root+`0x5f4`) preserve the bitmap font. The optional
  text-widget hook (`0x48c780`) shifts checkpoint rectangles up 180 source pixels.
  Original calls, String lifetimes and observer last-error/FP state are preserved.
- Timing uses `clock.elapsed_ms` from the first playing frame; player initialization
  distinguishes a new attempt from menu resume.
- C++ unit suite: 148 checks pass under Wine (67 checkpoint checks). Architecture
  and docgen Python suites: 12 + 10 pass. Debug/Release x86 cross-builds pass with
  Zig 0.14.1; the Windows build uses Zig 0.16.0.
- `tests/checkpoints.yaml` covers native contacts, one-shot timing, toggle/profile
  gates, file editing, clipboard/undo, autosave/backup and restart application.
