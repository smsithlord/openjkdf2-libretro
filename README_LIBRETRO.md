# OpenJKDF2 libretro core

Jedi Knight: Dark Forces II (and eventually MoTS) as a libretro core, built on
[OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2). Status: **M0 bring-up**
(see [DESIGN.md](DESIGN.md) for the design and milestones).

## Build (Windows, MSVC x64)

Requirements: Visual Studio 2022 (C++ workload), CMake 3.20+, Python 3 with
`cogapp` (`pip install cogapp`), git submodules initialized:

```
git submodule update --init lib/SDL lib/SDL_mixer lib/openal lib/glew lib/zlib lib/libpng lib/freeglut
git -C lib/SDL_mixer submodule update --init
cmake -S . -B build_libretro -DPLAT_LIBRETRO=TRUE
cmake --build build_libretro --config Release --target openjkdf2_libretro --parallel
```

Output: `build_libretro/Release/openjkdf2_libretro.dll` (plus `OpenAL32.dll`,
which must be placed next to the core or on PATH).

## Game data ("ROM") layout

The core loads an **episode GOB** as its ROM and derives the game folder
("basefolder") from its path:

```
<basefolder>/
  episode/JK1.GOB        <- load this file in RetroArch
  resource/Res2.gob      (+ Res1hi.gob, VIDEO/, cog/)
  MUSIC/Track12.ogg ...
  player/                (created by the game: profiles + saves)
  mods/                  (optional mod GOBs, override resource/)
```

Copy `Episode/`, `Resource/`, and `MUSIC/` from a legal Jedi Knight install
(e.g. the Steam version) into a **writable** folder. The engine writes saves,
settings, and checkpoints into this folder.

There is no separate asset-folder setting: the loaded GOB's location *is* the
configuration. If the folder above the GOB doesn't contain `resource/`, the
core refuses to load and shows an on-screen message explaining the expected
layout (plus a detailed diagram in the frontend log).

## Running

The standard test environment is a portable RetroArch at `tools/RetroArch-Win64/`
(gitignored; RetroArch 1.21.0), preconfigured with the `gl` driver, auto Game
Focus, and the UDP command interface (`SCREENSHOT` on port 55355 is how automated
tests capture frames):

```
tools\RetroArch-Win64\retroarch.exe -L build_libretro\Release\openjkdf2_libretro.dll "testdata\jk1\episode\JK1.GOB" --verbose
```

- RetroArch's **video driver must be `gl` or `glcore`** (Settings → Drivers).
  The core requests an OpenGL 3.3 core context; d3d/vulkan drivers refuse it.
- The game is keyboard+mouse heavy: **Game Focus** (Scroll Lock, or the
  preconfigured auto mode) is required so keys like ESC (skip cutscene, menu
  back) reach the game instead of RetroArch's hotkeys.
- MoTS: load a `.goo` episode file (untested until M3).

## Current limitations (M0)

- Audio plays through the engine's own OpenAL device, not RetroArch's audio
  pipeline (no fast-forward pitch; audio consolidation lands at M2). Music and
  cutscene audio are silent (their SDL audio path is inactive).
- No RetroPad mapping yet (M1); keyboard + mouse only.
- No save states (by design — the game's native save system is used).
