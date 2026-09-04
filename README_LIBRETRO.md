# OpenJKDF2 libretro core

Star Wars: Jedi Knight - Dark Forces II, and Mysteries of the Sith, as a
libretro core for RetroArch and other libretro frontends. Built on
[OpenJKDF2](https://github.com/shinyquagsire23/OpenJKDF2), the open-source
reimplementation of the engine. **Windows x64, keyboard + mouse.**

You need your own copy of the game. The core contains no game data.

## Install

1. Unzip the release. Put `openjkdf2_libretro.dll` and `OpenAL32.dll` in
   RetroArch's `cores/` folder and `openjkdf2_libretro.info` in its `info/`
   folder. (`OpenAL32.dll` must sit next to the core or be on `PATH`.)
2. The core needs the Microsoft Visual C++ 2015-2022 x64 redistributable. Most
   machines have it; if the core refuses to load with no message, install it
   from Microsoft.
3. In RetroArch, **Settings > Drivers > Video** must be `gl` or `glcore`. The
   core asks for an OpenGL 3.3 core context; the d3d and vulkan drivers refuse
   it.
4. Make a **writable** game folder with the game's data in it:

```
<game folder>/
  episode/JK1.GOB        <- load this file as the content
  resource/Res2.gob      (+ Res1hi.gob or Res1low.gob, VIDEO/, cog/)
  MUSIC/Track12.ogg ...  (the GOG/Steam soundtrack; disc layouts MUSIC/1, MUSIC/2 also work)
  player/                (created by the game: profiles, settings, saves)
  mods/                  (optional: .gob files here override resource/)
```

Copy `Episode/`, `Resource/` and `MUSIC/` from a legal install (GOG or Steam).
The Steam install folder itself is read-only for this purpose; copy it
somewhere the game can write. For Mysteries of the Sith, do the same with its
files and load `episode/JKM.GOO`.

The loaded file's location **is** the configuration: the folder above it must
contain `resource/`. If it does not, the core refuses to load and shows what it
expected on screen (with a fuller diagram in the frontend log).

5. Load the core, then load `episode/JK1.GOB` as content.

## Playing

- **Game Focus** (Scroll Lock in RetroArch) must be on, so keys such as ESC
  reach the game instead of RetroArch's hotkeys. Turn it on once the game is
  up; if ESC or the console stop working, it was toggled off.
- Keyboard and mouse only for now. Mouse buttons 1-5 and the wheel work
  (button 3 is secondary fire by default; rebind in the game's Setup >
  Controls). A RetroPad mapping is planned for a later release; the frontend's
  controller menu already offers "Keyboard + Mouse" as the port 1 device.
- The game's own Setup menus still hold the options that make sense inside a
  frontend (FOV, texture filtering, bloom, SSAO, gamma, HUD scale, sound,
  controls). Fullscreen, vsync, HiDPI and the Expansions & Mods screen are
  hidden because the frontend owns those; use RetroArch's video settings and
  the `mods/` folder instead.

## Core options

| Option | What it does |
|---|---|
| Boot mode | `episode` (default) starts the loaded episode directly, skipping intro and menu; `intro` and `menu` give the stock click-through; `level` boots straight into the first level; `resume` continues from the auto-resume position |
| Direct boot episode types | Limit direct boot to single- or multiplayer episodes |
| Internal resolution | 4:3, 16:9 and 16:10 sizes up to 1920x1440, applied live; the engine renders widescreen natively |
| Load mods folder | On by default: `mods/*.gob` override `resource/`. Off plays unmodded without moving files (takes effect on the next content load) |
| Multiplayer saves | Allow saves and states in multiplayer sessions |
| Netplay | Enable the frontend's netplay handling |
| Fast-forward speed limit / Slow-motion speed | Bounds for the frontend's speed controls; game time follows |
| Portable mode (no writes) | The engine creates and modifies nothing; for read-only media or several copies sharing one folder |
| COG Factory debug signals | For content development only. Leave disabled. |

## Saves and savestates

- **Saves are the game's own.** The in-game Save Game / Load Game screens
  write `.jks` files into `<game folder>/player/<profile>/`, exactly as the
  standalone game does. Nothing goes through the frontend's `.srm` mechanism,
  and RetroArch's save-directory setting is not used.
- **Savestates are engine savegames in disguise.** Saving a state works
  wherever the native Save Game menu would; loading one restores through the
  engine's own load flow over the following frames, so it is not
  frame-exact. Rewind, run-ahead and netplay-style state use are not
  supported and the core says so to the frontend. States record which game
  (JK1 or MoTS) and which mods were loaded, and refuse to load across games.
- **Auto-resume**: single-player sessions save their position on exit;
  `Boot mode = resume` continues from it.

## Known issues (0.1.0)

- During the opening crawl, a ghost of the previous frame can appear in the
  top-left corner under RetroArch. Cosmetic; not reproducible outside
  RetroArch so far.
- `OpenAL32.dll` stays mapped in the frontend after the core is unloaded. Inert;
  the next load reuses it.
- RetroArch's own menu may not regain the mouse over a running game. Alt+F4
  still exits cleanly.
- Scroll Lock toggles Game Focus off as easily as on; see Playing above.

## Building (developers)

Visual Studio 2022 (C++ workload), CMake 3.20+, Python 3 with `cogapp`
(`pip install cogapp`), submodules:

```
git submodule update --init lib/SDL lib/SDL_mixer lib/openal lib/glew lib/zlib lib/libpng lib/freeglut
git -C lib/SDL_mixer submodule update --init
cmake -S . -B build_libretro -DPLAT_LIBRETRO=TRUE
cmake --build build_libretro --config Release --target openjkdf2_libretro --parallel
```

Output: `build_libretro/Release/openjkdf2_libretro.dll` with
`openjkdf2_libretro.info` and a `.pdb` beside it, and `OpenAL32.dll` one level
up in `build_libretro/`. The GitHub Actions workflow
`libretro-win64.yml` builds the same and packages the release zip; tags named
`libretro-v*` publish it.

Design notes are in [DESIGN.md](DESIGN.md), the milestone log in
[ROADMAP.md](ROADMAP.md), and per-topic notes in [devdocs/](devdocs/). The
headless test frontend and content tooling used to develop the core are a
separate repository (see `devdocs/README.md`), not part of this one.

## Licensing

OpenJKDF2's terms are in [LICENSE.md](LICENSE.md); the libraries the core is
built from are listed with their licenses in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Jedi Knight is a trademark
of Lucasfilm Ltd.; this project is not affiliated with Lucasfilm, LucasArts or
Disney.
