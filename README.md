# FDNYCore

Speedrun and practice tooling for **FDNY Firefighter: American Hero** (2003). FDNYCore is a
single in-process plugin (`.asi`) that adds practice tools and speedrun options to the game,
built on a Ghidra decompilation of it.

- **Practice:** teleport save/load, a pause with a free camera, trigger boxes, practice
  checkpoints that show the elapsed level time, recordings with replays and ghosts, the warp
  HUD, the fire-juggling arrow, player info.
- **Speedrun options:** fixed victim spawns and the RescueBot switch, shown on the watermark.
- **Fixes:** the classic field-of-view fix (Hor+ on wide screens), and the alt-tab fix that
  stops the game holding the mouse while unfocused.
- **Profiles:** `practice` allows everything; `run` refuses practice-only settings and always
  shows the watermark.

Everything is a console command or variable, with default keys. The full list of keys,
features, variables and commands is the [quick reference](docs/quick-reference.md), generated from the
source on every build.

## Install

1. Download `FDNYCore-<version>.zip` from the
   [releases](https://github.com/RichardFranksComputer/fdny-tas/releases).
2. Unzip it and copy `FDNYCore-<version>.asi` next to `FDNYFirefighter.exe`. It loads through
   Ultimate ASI Loader (`dinput8.dll` in the game folder). Keep `quick-reference.md` from the
   zip: it lists that build's keys and commands.
3. Remove any older `FDNYCore*.asi`, and don't run other FDNY `.asi` mods alongside it
   (FOV fix, alttabfix, FDNYDeterministic): FDNYCore replaces them.

Open the game's console with the <code>`</code> key; `cvarlist`, `cmdlist` and `bindlist`
list everything in the game.

## Build from source

```
.\build.ps1             # build\Debug and build\Release FDNYCore.asi
.\tests\unit\run.ps1    # unit tests, no game needed
```

Needs Zig 0.16.0 and Python 3. See [CONTRIBUTING.md](CONTRIBUTING.md) for the layout,
tests and releases, and [docs/architecture.md](docs/architecture.md) for how the core and
its feature modules fit together.

## Repository

| Path | What |
|---|---|
| `core/` | Core services, entry point, module registry, address table |
| `features/` | One folder per feature module |
| `tests/` | `game/` (game-driven, run on the decomp VM) and `unit/` |
| `tools/` | Build checks, the reference generator, the game-test harness |
| `ghidra/` | The decompilation's labels and the script that exports them |
| `docs/` | Architecture, decisions, the generated reference, game research, evidence |
| `thirdparty/` | MinHook |

Release notes: [CHANGES.md](CHANGES.md).
