# Ečstatica

Decompilation and portable C99 reimplementation of **Ecstatica 1 & 2**, the 1994/1997 ellipsoid-rendered adventure games by Andrew Spencer Studios.

## Buy the games

Both games are back on sale, more than 30 years after the original Ecstatica.
SNEG and Andrew Spencer Studios release them on **26 October 2026**. It is the
first digital release of Ecstatica II. This project needs the game data, so
buying the games is how you get it, and it supports the people who made them.

| Game | GOG (DRM-free) | Steam |
|---|---|---|
| Ecstatica (1994) | [gog.com/game/ecstatica](https://www.gog.com/en/game/ecstatica) | [store.steampowered.com/app/4834840](https://store.steampowered.com/app/4834840/Ecstatica/) |
| Ecstatica II (1997) | [gog.com/game/ecstatica_2](https://www.gog.com/en/game/ecstatica_2) | [store.steampowered.com/app/4834850](https://store.steampowered.com/app/4834850/Ecstatica_II/) |

The GOG version has no DRM. On Steam there are free demos of both games, and
this project runs from the re-release files as installed (see [Data Folder](#data-folder)).

## Overview

Primary reverse-engineering target is `E2WIN95.EXE` (Ecstatica 2 Win95 build, which includes high-res mode). Ecstatica 1 is a secondary target and runs from the same binary — game version is auto-detected at runtime.

The goal is a single native executable that runs both games on modern systems (macOS, Linux, Windows) using the original game data.

![Ecstatica 1](docs/e1.png)
![Ecstatica 2](docs/e2.png)

## Goals

* Disassemble Ecstatica 1 & 2 with IDA Pro 9.1 + Open Watcom debug symbols
* Use `E2WIN95.EXE` as decomp base
* Translate Hex-Rays / Watcom pseudocode into portable C99
* Single binary for both games (E1 + E2), runtime version detect
* Native platform builds via thin platform layer (macOS, Linux, Windows)
* Optional: retarget DOS build with Open Watcom (`wcl386`)

## Tools

### Viewer

![Ecstatica 1](docs/e1-viewer.png)
![Ecstatica 2](docs/e2-viewer.png)

### Scenes

![Ecstatica 1](docs/e1-scenes.png)
![Ecstatica 2](docs/e2-scenes.png)

## Community
* Discord: https://discord.gg/msQB6yBP9

## Status

* Build system: CMake + Makefile wrapper
* Platforms: macOS (Cocoa/NSView framebuffer), Linux (X11/GLX), Windows (Win32) — all tested
* Input: keyboard + mouse + gamepad (Steam Deck, Xbox, PlayStation, generic HID)
* Renderer: 8-bit indexed palette framebuffer, palette expansion + scale in `platform_blit`
* Modules ported: init, display, ellipse, tri, asm_f (fixed-point + rasterizers), edit, move, map, topo, anim, file (.FAN I/O), music, menu, req, icon, chars, win, game
* Fixed-point math: 14-bit fraction (`FIXED_POINT_SHIFT`)
* Structs packed to original binary layout (`#pragma pack(push, 1)`)

## Build

```bash
make          # cmake build → build/bin/ecstatica
make e2       # build + stage into data/e2 + run
make e1       # build + stage into data/e1/W + run
make e1-dos   # build + stage into data/e1 (bundled DOS)
make clean    # remove build/
```

CMake project lives in `src/CMakeLists.txt`. C99 strict, plus ObjC for the macOS platform layer.

## Model, animation & scene viewer

```bash
make e2-viewer   # browse Ecstatica 2's models
make e1-viewer   # browse Ecstatica 1's models
make e2-scenes   # browse Ecstatica 2's scripted scenes
make e1-scenes   # browse Ecstatica 1's scripted scenes
# or: ./ecstatica --viewer  /  ./ecstatica --scenes
```

Browses every model in the archives with a free or orbit camera, playing any
of its animations. Both games use the same code: rendering goes through the
game's own `prepare_parts` / `draw_parts` / `show_parts`, so a model looks
here exactly as it does in play. Press `H` in the viewer for the key list.

The animation pane starts from the model's repertoire — the actions the game
itself plays for it. `TAB` runs a full archive scan and adds every other
action whose events only name parts this model has.

Playback is at the engine's rate, except that actions too short to read on
repeat (E2's `herojump` is 16 ticks, under a quarter second) are stretched to
0.6s. The status line says `SLOW` when that applies; `F` turns it off.

`V` switches to the scene browser (`--scenes` opens straight there). A scene
is a set of parallel scripts, one action per actor; it plays through the
original Watcom editor's own preview path, `advance_selected_scene_or_action`,
which is still in `edit.c`. Scenes are framed by the camera they were shot
with — painted background, scene palette and depth mask all loaded by
`check_view`, so actors occlude against the plate as they do in game. `O`
detaches into the free/orbit camera over a flat backdrop to read the staging
the shot hides.

## Disassembly Requirements

* **IDA Pro 9.1** — connected via `ida-mcp` MCP server for live pseudocode / xref queries
* **Open Watcom 2.0** (`wcl386`) — original compiler; used to regenerate debug symbols and optionally target DOS
* `wdump` — for dumping the original PE

Building Open Watcom on Apple Silicon: <https://retrocoding.net/building-for-dos-os2-and-dos-on-a-macbook-apple-silicon>

## Code Style

* C99 strict
* `snake_case`
* 4-space indent
* No comments except non-obvious "why"
* All structs / enums / typedefs in `types.h`
* Module names mirror original Watcom source files (`init`, `display`, `edit`, `game`, …)
* Original Watcom symbol pattern: `module_function_name_hexaddr` (e.g. `init_init_mouse_410100`)

## Layout

```
src/
  main.c            entry point, game loop
  init.c            input, display primitives, palette, timing
  game.c            script execution, actors, combat
  display.c         skeletal hierarchy, view pipeline
  ellipse.c         ellipsoid rendering, shade_map
  tri.c             polygon/quad rendering, textures
  asm_f.c           fixed-point math, bitmap unpack, rasterizers
  edit.c            entity management, scene/action control
  move.c            actor movement, collision, behavior state
  map.c             camera, view loading, visibility
  topo.c            terrain, height, gravity
  anim.c            keyframe / action directory loading
  file.c            .FAN archive I/O, save/load
  music.c           MIDI playback, ambient, SFX
  menu.c            main/pause menus, settings
  req.c             dialogs, file picker, game-over
  icon.c            resolution constants, VGA/SVGA config
  chars.c           font glyphs
  win.c             window/platform stubs
  platform.h        platform abstraction interface
  types.h           structs, enums, constants, forward decls
  platforms/
    macos.m         Cocoa NSView framebuffer + input + timing
    linux.c         X11/GLX, ALSA, FluidSynth music
    windows.c       Win32

decomp/
  parse.py          extract Watcom debug symbols → JSON + IDC
  modules.txt       module list with descriptions
  dump/             wdump outputs, parsed symbols, IDC scripts
  ida/              IDA Pro database files
```

## Data Folder

Game data is not committed. Place original game files under `data/`:

* `data/e1/W/`    — Ecstatica 1 Win95 (640×480)
* `data/e1/`      — Ecstatica 1 bundled DOS (320×200)
* `data/e1-dos/`  — Ecstatica 1 original DOS release (320×200)
* `data/e2/`      — Ecstatica 2 Win95 (640×480) + DOS (320×200)

The modern re-release folders (`Code/`, `Views/`, `Graphics/`, `Text/`) work
as they are installed: point the game at the folder that holds `Code/`.
Sound effects, speech and MIDI music come from its `Audio/` folder, and its
bundled GeneralUser GS soundfont plays the music on Linux, web, Android and iOS.
It also runs in 16:9 on desktop and PS Vita: the original 4:3 view stays in the
middle and the re-release's wider renders of each camera fill the sides. Switch
back to 4:3 in Enhanced menu → Aspect Ratio, or with `widescreen = 0` in
`ecstatica.cfg`.

`.FAN` archives hold game resources (actors, graphics, scripts).

Assets are read relative to the working directory, so the game normally runs
from the folder holding them. When it is launched from somewhere else — an
AppImage, a `.desktop` entry, a Steam shortcut — it also looks for the folder
next to the AppImage, in the launch directory, and beside the executable.
`ECSTATICA_DATA` overrides all of them:

```bash
ECSTATICA_DATA=/path/to/ecstatica2 ./ecstatica-linux-x86_64.AppImage
```

## Debug

* `DBG_LOG(level, ...)` — `1` = errors/warnings only, `2` = verbose tracing
* `debug_verbose` global toggles level
* Log file: `ecstatica_debug.log`
* Compile flags: `SKIP_START_LOGO`, `SKIP_DRAW_TRIANGLE`, `APP_ALWAYS_ACTIVE`

## License

Reverse engineering / preservation project. Original game assets and executables are © their respective owners and are **not** included in this repository.
