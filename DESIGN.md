# openjkdf2_libretro — Design

OpenJKDF2 (Jedi Knight: Dark Forces II + Mysteries of the Sith) compiled as a libretro
core. This document is the working design for the port. Product decisions (ROM model,
basefolder, mods gating, v1 scope) live in [devdocs/00-project-brief.md](devdocs/00-project-brief.md);
this file covers *how* the port is built. All engine line references are against the
vendored upstream tree in this repo (upstream master `0dbd1ea8`, verified 2026-08-18).

## Repo structure

This repo **is** upstream OpenJKDF2 (fetched from `shinyquagsire23/OpenJKDF2`, remote
name `upstream`) with the port added as commits on top:

- `cmake_modules/plat_libretro.cmake` — new platform module (`-DPLAT_LIBRETRO=TRUE`),
  following the existing four-macro platform contract.
- `src/Platform/Libretro/` — all new code: the `retro_*` API implementation, the
  libretro window/input/audio glue, and a vendored `libretro.h`.
- Engine edits are minimal and `#ifdef LIBRETRO_BUILD`-gated so upstream merges stay
  trivial (`git fetch upstream && git merge upstream/master`).

Dependencies are upstream's git submodules under `lib/` (SDL3, SDL_mixer, OpenAL Soft,
GLEW, zlib, libpng, freeglut). GNS/protobuf/physfs are not used (multiplayer excluded).

## Engine configuration choice (the big decision)

Upstream supports two families of platform config:

1. **Full SDL config** (`plat_feat_full_sdl2.cmake`, used by MSVC/Linux/macOS): SDL3 +
   `SDL2_RENDER` define + `Platform/SDL2/*` + `Platform/GL/std3D.c` (GL 3.3 core) +
   OpenAL + SDL_mixer. This is the known-good desktop configuration.
2. **No-SDL homebrew config** (TWL/Dreamcast): `TARGET_RETRO_HOMEBREW` +
   `PLAT_MISSING_WIN32`, own `Window_*.c`/`stdControl.c`/sound backend.

**The core uses config (1), windowless.** Rationale:

- The engine compiles *exactly* as the proven MSVC standalone build — no new
  engine-code permutation to debug. The no-SDL config on MSVC would activate legacy
  native-Win32 paths (`#if !defined(SDL2_RENDER) && defined(WIN32)` in ~14 files:
  `types.h`, `stdGdi.c`, `Windows.c`, `stdMci.c`, `jkDev.c`, …) and conflict with real
  `windows.h` vs `PLAT_MISSING_WIN32` fake types — a much bigger first step.
- SDL3 is initialized **without the video subsystem and without any window/GL context**.
  The engine's SDL window calls degrade safely (`displayWindow == NULL`;
  `SDL_GL_SwapWindow(NULL)` is a no-op error return), and the few that matter are
  neutralized under `LIBRETRO_BUILD`.
- The TWL/Dreamcast pattern (`Platform/TWL/Window_Twl.c` replaces `Window_Main_Loop`
  wholesale) remains the model for a later fully-SDL-free core (post-v1 refinement,
  tracked as M4 below) once everything else is stable.

## retro_* lifecycle mapping

Verified engine facts this maps onto: `main()`'s restart loop calls
`OpenJKDF2_Globals_Reset()` then `Window_Main_Linux()` ([src/main.c:751](src/main.c#L751));
`Window_Main_Linux` ([src/Win95/Window.c:1437](src/Win95/Window.c#L1437)) = SDL init → GL
3.3 core context → `glewInit()` → `Main_Startup(cmdLine)` → fullscreen/hidpi from
registry → synthesized `WM_CREATE`/`WM_ACTIVATE`/`WM_ACTIVATEAPP`/`WM_SHOWWINDOW`/`WM_PAINT`
→ `while(1) Window_Main_Loop()`; `Window_Main_Loop()`
([src/Win95/Window.c:1420](src/Win95/Window.c#L1420)) = `jkMain_GuiAdvance()` +
`Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0)` = exactly one frame.

| libretro entry point | What the core does |
|---|---|
| `retro_set_environment` | declare `need_fullpath`, no-game=false, core options (v2 API), log interface |
| `retro_init` | nothing heavy (no game yet, no GL) |
| `retro_load_game` | derive `basefolder = dirname(dirname(rom))`; validate `<basefolder>/resource/` exists; `chdir(basefolder)`; set `OPENJKDF2_ROOT`/`OPENJKMOTS_ROOT` env to basefolder (pins InstallHelper); `.goo` → `Main_bMotsCompat = 1`; build cmdline; request HW render (GL core 3.3); **no engine init, no GL** |
| `context_reset` | mark GL usable; (re)load GL entry points; if engine was already running: rebuild GPU state (`std3D_Startup` path); first time: arm deferred engine startup |
| first `retro_run` | deferred init with the frontend's context current: `SDL_Init(AUDIO\|JOYSTICK\|GAMEPAD)` (no VIDEO) → `OpenJKDF2_Globals_Reset()` → `glewInit()` → set `Window_xSize/ySize` → `Main_Startup(cmdline)` → registry fullscreen values ignored → synthesize `WM_CREATE`/`WM_ACTIVATE`/`WM_ACTIVATEAPP`/`WM_SHOWWINDOW` |
| every `retro_run` | poll input → inject into engine (see Input) → set `std3D_windowFbo = get_current_framebuffer()` → `Window_Main_Loop()` → `video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0)` → submit audio batch; if `g_should_exit`: `RETRO_ENVIRONMENT_SHUTDOWN` |
| `context_destroy` | `std3D_FreeResources()` + free the core's cursor GL objects (engine already supports full GPU teardown/rebuild — used for window recreation). Empirical: RetroArch sends this BEFORE `retro_unload_game` on close-content, so this is where engine GL teardown actually happens; the unload quiesce must not render |
| `retro_unload_game` | inline engine quiesce: resume the fiber once and run `jkPlayer_WriteConf` → `Main_Shutdown` at its park point (closes the OpenAL device, joining the threads that would otherwise pin the DLL against dlclose's unmap), then `DeleteFiber`. See "Engine quiesce" below |
| `retro_reset` | cooperative unwind quiesce (context alive): `g_should_exit` + force-pop one modal menu per frame → same WriteConf → `Main_Shutdown` at the frame boundary → `DeleteFiber` → re-arm boot (`OpenJKDF2_Globals_Reset` on the fresh fiber — upstream's own in-process restart path) |
| `retro_serialize_size` | 0 — no save states in v1; native saves in `<basefolder>/player/` are the save mechanism |

**Init is deferred to the first `retro_run`** because `Main_Startup` initializes GL
(std3D) partway through, and a HW-render core may only touch GL inside
`retro_run`/`context_reset`. Frontends tolerate a slow first frame; RetroArch shows the
game as loaded immediately after `retro_load_game` returns.

### The engine fiber (modal-menu inversion)

The devdocs' "nested modal loops" warning turned out to be the defining constraint: the
**entire menu system is modal** — every menu and dialog blocks in
`jkGuiRend_DisplayAndReturnClicked` ([src/Gui/jkGUIRend.c:333](src/Gui/jkGUIRend.c#L333)),
pumping whole engine frames via `Window_MessageLoop()` until an element is clicked (the
desktop build pumps SDL inside that loop; the WASM build presumably survives via
asyncify). Called directly from `retro_run`, the first menu would never return.

So the core runs the engine on its own **fiber** (Win32 fibers; same OS thread, so no
locking and the GL context stays current — swap in libco for Linux at M3):

- `retro_run` = poll/inject input → set FBO + rebind VAO → `SwitchToFiber(engine)` →
  on yield: reset leaked GL state → `video_cb` → `audio_batch_cb`.
- The engine fiber boots the engine on first resume, then loops
  `Window_Main_Loop(); yield;` — one frame per resume.
- `Window_MessageLoop()` (the single call site all modal loops pump through,
  [src/Win95/Window.c](src/Win95/Window.c)) yields to the frontend at the end of each
  iteration under `LIBRETRO_BUILD` — one modal-menu iteration per frontend frame, so
  menus render, receive input, and animate at the frontend's cadence.
- `jk_exit` (in-game Quit, called from inside modal loops) parks the engine fiber and
  raises `RETRO_ENVIRONMENT_SHUTDOWN` instead of `exit()`; the unload that follows
  shuts the engine down from that park point (below).

### Engine quiesce (unload / reset)

Getting the engine OUT of a session cleanly has two shapes (lifecycle sprint,
`086c8dc8`), both ending in the standalone's own exit sequence — WriteConf →
`Main_Shutdown`, which closes the OpenAL device and joins its mixer/event
threads (empirically the only threads the core spawns, and what used to pin
the DLL against dlclose's unmap):

- **Inline** (`retro_unload_game` — the GL context is already gone on
  RetroArch, so no frame may render): resume the fiber once; a hook at the
  yield's resume point runs the shutdown right where the fiber was parked,
  leaving the stack frames below frozen forever. Precedent: the engine's own
  WM_DESTROY handler calls `Main_Shutdown` from inside the modal pump.
- **Cooperative unwind** (`retro_reset` — context alive, frames render): set
  `g_should_exit` (the engine's clean-exit convention; menu loops like
  `jkGuiMain_Show` already return on it) and force-pop the innermost modal
  menu (`jkGuiRend_activeMenu->lastClicked = -1`) once per resumed frame,
  bounded (~120 frames), until the fiber unwinds to its frame-boundary loop;
  the modal pump's own `g_should_exit → jk_exit` call is suppressed during
  the quiesce (LIBRETRO_BUILD gate in `jkGuiRend_DisplayAndReturnClicked`).
  If the budget expires the inline shape runs instead.

Fallback (engine failed during boot): WriteConf + force-close the OpenAL
device from the frontend fiber (`libretro_ForceCloseAudioDevice`). GL
teardown (`std3D_FreeResources`) runs in `context_destroy` or in the quiesce,
whichever the frontend sends first, guarded by
`libretro_std3D_HasGlResources()`. `retro_deinit` runs `ConvertFiberToThread`
so the frontend's main thread doesn't remain a fiber after the core unloads.
- Frontend-side GL calls in `retro_run` are gated on a `s_gl_ready` flag set after
  `glewInit` (which runs on the engine fiber): GLEW's function pointers are NULL before
  that, and calling one is a jump to address zero.

The in-game Mods menu and DF2↔MoTS switching set `openjkdf2_restartMode` and exit the
loop; under libretro v1 these are left non-functional (menu entries hidden if cheap to
do). `-path` mods are out of scope for v1; `mods/*.gob` overrides are the supported path.

## Video: hardware render (GL 3.3 core)

`RETRO_ENVIRONMENT_SET_HW_RENDER` with `RETRO_HW_CONTEXT_OPENGL_CORE` 3.3,
`bottom_left_origin = true` (GL convention), `depth`/`stencil` = true. If the frontend
refuses (d3d/vulkan video driver), `retro_load_game` returns false with a log message
telling the user to switch RetroArch's video driver to `gl`/`glcore`.

Integration points (all verified in source):

- **Final render target**: std3D renders the world/menu into its own FBOs and composites
  to `std3D_windowFbo` — an existing global (`GLint std3D_windowFbo = 0;`
  [src/Platform/GL/std3D.c:97](src/Platform/GL/std3D.c#L97)) captured at startup via
  `glGetIntegerv(GL_FRAMEBUFFER_BINDING, &std3D_windowFbo)`
  ([std3D.c:462](src/Platform/GL/std3D.c#L462)). The core sets it (and
  `std3D_pFb->window.fbo`, copied at [std3D.c:339](src/Platform/GL/std3D.c#L339)) to
  `hw_render.get_current_framebuffer()` every frame before painting. Because startup
  runs inside `retro_run` with the frontend FBO bound, even the startup capture gets the
  right value.
- **Present**: `SDL_GL_SwapWindow(displayWindow)` at
  [Window.c:1169](src/Win95/Window.c#L1169), [:1176](src/Win95/Window.c#L1176),
  [:1255](src/Win95/Window.c#L1255) — `displayWindow` is NULL so these are no-ops in
  SDL3; presentation is the core returning from `retro_run` and calling
  `video_cb(RETRO_HW_FRAME_BUFFER_VALID, Window_xSize, Window_ySize, 0)`.
- **No window recreation**: `Window_RecreateSDL2Window()` and the `Window_needsRecreate`
  path ([Window.c:1180](src/Win95/Window.c#L1180)) are guarded under `LIBRETRO_BUILD`.
- **GL function loading**: `glewInit()` on the frontend's current context inside the
  deferred init (GLEW resolves via `wglGetProcAddress` on whatever context is current).
  If this proves flaky on other frontends, switch to `hw_render.get_proc_address`.
- **Context loss**: `context_destroy` → `std3D_FreeResources()`; `context_reset` →
  re-run the std3D startup path (the engine already uses exactly this pair for window
  recreation, [Window.c:1553](src/Win95/Window.c#L1553)).
- **State leakage**: RetroArch's GL driver expects sane state at frame end. After
  `Window_Main_Loop()` returns, the core unbinds VAO/FBO/program/textures it can
  cheaply reset (lesson inherited from prior libretro-host work: current-context and
  leaked-binding bugs are the #1 HW-core failure mode).
- **Resolution**: engine renders at `Window_xSize`/`Window_ySize`, which the core owns.
  M0 fixes 640×480; M3 adds a core option (`max_width`/`max_height` sized to the
  largest option; resolution changes via `RETRO_ENVIRONMENT_SET_GEOMETRY` +
  `Window_resized = 1` so the engine re-runs its mode-fix path).

## Audio

**Implemented (M2 consolidation).** One mix at 48000 Hz S16 stereo per frame;
the engine opens **no real audio device** under `LIBRETRO_BUILD`:

1. **OpenAL loopback** (`stdSound.c`): `stdSound_Startup` opens
   `alcLoopbackOpenDeviceSOFT(NULL)` (OpenAL Soft is built in-tree, so the
   extension is always present) with context attrs
   `ALC_FORMAT_CHANNELS_SOFT=ALC_STEREO_SOFT`, `ALC_FORMAT_TYPE_SOFT=ALC_SHORT_SOFT`,
   `ALC_FREQUENCY=48000`. Every `retro_run` pulls exactly 800 frames
   (48000/60, integer — no drift) via `libretro_stdSound_RenderAudio` →
   `alcRenderSamplesSOFT` and submits them as the single `audio_batch_cb`.
   Loopback devices spawn no mixer threads; the engine fiber shares the
   frontend thread, so the render is race-free by construction. A real device
   remains as a loud-logged fallback if the extension is ever missing.
2. **Music** (`stdMci.c`, SDL2_RENDER branch): the SDL3_mixer mixer becomes a
   device-less `MIX_CreateMixer()` (48kHz S16 stereo), with
   `SDL_HINT_AUDIO_DRIVER=dummy` set before `MIX_Init` so SDL's WASAPI backend
   never initializes (its init alone spawns a device-notification thread).
   Once per `retro_run` the core calls `libretro_stdMci_Pump()`:
   `MIX_Generate()` pulls the decoded music mix (all the existing
   `MUSIC/Track*.ogg` path-resolution and track-advance logic is unchanged)
   and queues it on an OpenAL **streaming source** (8 × 800-frame buffers,
   ~133 ms depth), which the loopback render folds into the same final mix as
   SFX — one mixer, no manual sample summing. Engine play/stop/volume calls
   land in SDL_mixer track state (`MIX_SetTrackGain` etc.) exactly as before.
3. **SMUSH/cutscene audio** already plays through `stdSound` OpenAL buffers
   ([jkCutscene.c:400](src/Main/jkCutscene.c#L400)) — captured by the loopback
   for free.

Consequences: RetroArch volume/recording apply to everything, pause is
hard-silent (no `retro_run` → no samples), fast-forward pitches naturally,
and content unload leaves zero engine audio threads (nothing to pin the DLL).
JK1 music note: levels drive music volume dynamically via COG `setmusicvol`
(silent while exploring, swells in combat) — music being inaudible in a quiet
area is engine-correct.

## Input

The engine has two input consumers (verified):

1. **Menus/GUI**: windows-style messages into `Window_msg_main_handler`
   ([Window.c:172](src/Win95/Window.c#L172)) — `WM_KEYFIRST`/`WM_KEYUP` with VK codes,
   `WM_CHAR`, `WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP` with packed mouse coords — plus
   the `Window_mouseX/Y`, `Window_bMouseLeft/Right` globals fed by the SDL pump
   ([Window.c:397](src/Win95/Window.c#L397) `Window_HandleMouseMove`).
2. **Game**: scancode-level keyboard state via
   `stdControl_SetSDLKeydown(scancode, bDown, timestamp)`
   ([Platform/SDL2/stdControl.c:275](src/Platform/SDL2/stdControl.c#L275) — SDL
   scancodes), mouse deltas + buttons via stdControl axis/key state, and
   SDL_GameController for pads.

Core-side mapping (`Platform/Libretro/` glue, replaces the SDL event pump which never
fires — SDL has no window):

- **Keyboard**: `RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK`. Each event maps
  `retro_key` → (a) VK code + `WM_KEYFIRST`/`WM_KEYUP`/`WM_CHAR` for the GUI (mirroring
  the exact key list the SDL pump translates, [Window.c:820–1040](src/Win95/Window.c#L820)),
  and (b) SDL scancode + `stdControl_SetSDLKeydown` for gameplay. The `character` field
  of the callback feeds `WM_CHAR` for text entry (player name, cheats, console).
- **Mouse**: `RETRO_DEVICE_MOUSE` relative deltas each frame → accumulate into a
  core-owned absolute position clamped to `Window_xSize/ySize` for the menu (feed
  `Window_mouseX/Y` + button messages), and feed deltas to stdControl's mouse axes for
  mouse-look. Buttons → `WM_L/RBUTTONDOWN/UP` + `Window_bMouseLeft/Right`.
- **RetroPad**: M1. Mimic `Platform/SDL2/stdControl.c`'s SDL_GameController mapping
  (axes `AXIS_JOY1_X/Y/...`, buttons → `KEY_JOY1_*`) by writing the same
  `stdControl_aAxisPos`/keystate entries from RetroPad state. Input descriptors declared
  for sane RetroArch binds. SDL's own gamepad support stays unused (no events without
  a window).
- **Game Focus note for the README**: keyboard-heavy game; RetroArch captures full
  keyboard only in Game Focus mode (Scroll Lock).

## Core options

Registered via `RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2` at `retro_set_environment`:

| Option | Values | Default | Wiring |
|---|---|---|---|
| `openjkdf2_use_mods` | disabled/enabled | disabled | new global `jkRes_bAllowModsDir` consulted by the `mods/` scan block at [src/Main/jkRes.c:268](src/Main/jkRes.c#L268) (~3-line gated patch); applied before engine startup, change requires restart |
| `openjkdf2_resolution` | 640x480 … 1920x1440 | 640x480 | `Window_xSize/ySize` + `SET_GEOMETRY` (M3) |
| `openjkdf2_autostart_episode` | disabled/enabled | disabled | derive episode from ROM filename → `-episode <name> -autostart` on the synthesized cmdline (M3) |
| `openjkdf2_hires_assets` | enabled/disabled | enabled | investigate at M3: `Res1hi.gob` loads by wildcard; "disabled" would skip it in the resource scan |
| `openjkdf2_cursor_autohide` | enabled/disabled | enabled | **implemented** — core-drawn menu pointer appears only after pointer activity and hides after ~3 s idle; read per-frame via `GET_VARIABLE_UPDATE` |
| `openjkdf2_boot` | menu/episode/resume | menu | **implemented** — `jkSession_ConfigureBoot`/`ArmBoot` (src/Main/jkSession.c, ported from AAOpenJKDF2 per devdocs/06): `episode` autostarts the ROM's episode at its first level entry; `resume` re-enters the last session's map and pose from `<basefolder>/openjkdf2_lastsession.json`, falling back to `episode` then `menu` |
| `openjkdf2_boot_game_type` | singleplayer/multiplayer | singleplayer | **implemented** — direct boot's mode; `multiplayer` solo-hosts a local session (works against `Networking/None`; needed for MP episode GOBs like JK1MP) |
| `openjkdf2_resume_position` | enabled/disabled | enabled | **implemented** — disabled resumes the map at its default spawn (session records are still written either way) |
| `openjkdf2_skip_intro` | disabled/enabled | disabled | **implemented** — menu boot skips the pre-title intro movie (engine's own disable-cutscenes skip in `jkSmack_SmackPlay`, without touching the profile setting) |

## Filesystem & saves

- `chdir(basefolder)` at `retro_load_game` = the whole resource system works unmodified
  (every open falls through `jkRes_FileOpen`'s search chain relative to CWD).
- `OPENJKDF2_ROOT` (or `OPENJKMOTS_ROOT` under MoTS) is set to basefolder so
  `InstallHelper` never goes hunting or `chdir`s elsewhere (it honors the env override
  above all other detection, [src/Main/InstallHelper.c:231](src/Main/InstallHelper.c#L231)).
- Writes land in the basefolder: `player/<name>/` (profiles + saves), `persist/`
  (checkpoints), and the file-backed `wuRegistry` JSON (MSVC builds already swap in
  `Platform/Posix/wuRegistry.c` — verified [cmake_modules/config_platform_deps.cmake:271](cmake_modules/config_platform_deps.cmake#L271)).
  This matches how DOSBox/ScummVM-style cores treat game dirs. Redirect to the libretro
  save dir is a possible later refinement, not v1.
- **Testing basefolders must be writable.** The local read-only Steam install
  (`H:\SteamLibrary\...\Star Wars Jedi Knight Vanilla`) is never used directly; a test
  basefolder is assembled elsewhere with copies of `episode/*.gob` and `resource/*.gob`.

## Build system

- `PLAT_LIBRETRO` cache var → `cmake_modules/plat_libretro.cmake`; added to the
  top-level dispatch and excluded from `PLAT_AUTO`.
- `plat_initialize` mirrors `plat_msvc.cmake` on Windows (`WIN64_STANDALONE`, `WIN64`,
  `ARCH_64BIT`, `WIN32`, `TARGET_WIN32`, full-SDL feature set) minus curl/updater, plus
  the `LIBRETRO_BUILD` define. `TARGET_NO_MULTIPLAYER_MENUS` is deliberately NOT set for
  now — MSVC standalone doesn't set it, and staying byte-identical to the known-good
  config matters more at M0 than trimming dead menus (networking is already the
  `Platform/Networking/None` backend). Revisit at M3. Linux support later (M3) via the
  same module taking the plat_linux_64 baseline.
- The final target is `add_library(openjkdf2_libretro SHARED src/Platform/Libretro/*.c)`
  linking `sith_engine` (which `CMakeLists.txt` already builds as an OBJECT library with
  `main.c` excluded). Exports: `RETRO_API` already carries `__declspec(dllexport)` on
  Windows — **no** `WINDOWS_EXPORT_ALL_SYMBOLS` (the fork skeleton's approach; fragile at
  this codebase's symbol count).
- Output: `openjkdf2_libretro.dll` (no `lib` prefix, no version suffix).
- Known deps chain (all built from `lib/` submodules as upstream does): SDL3, SDL_mixer,
  OpenAL Soft, GLEW, zlib, libpng, freeglut, nlohmann_json (in-tree `3rdparty/json`).
  Python 3 is required at configure time for the `cogapp` globals generation.

## Testing

```
retroarch -L openjkdf2_libretro.dll "<basefolder>\episode\JK1.gob" --verbose
```

RetroArch video driver must be `gl` or `glcore`. `--verbose` logs every environment
call. Test basefolder: copy of the Steam install's `episode/` + `resource/` into a
writable directory.

## Milestones

- **M0 — boots to menu**: MSVC x64 core builds; HW GL context negotiated;
  `retro_load_game` on `JK1.gob` → main menu renders into the frontend FBO; enough
  keyboard/mouse to click through; silence in `audio_batch_cb` (engine audio may
  incidentally work via its own devices).
- **M1 — playable**: in-game stable; full keyboard/mouse; RetroPad mapped; native
  saves verified round-trip; in-core menu entries that can't work (Exit→shutdown env
  call, display mode switching) neutralized.
- **M2 — audio consolidation** (done): OpenAL loopback + music into one
  `audio_batch_cb` (SMUSH already rode stdSound); engine no longer opens real
  audio devices; fast-forward behaves.
- **M3 — polish**: core options wired (mods toggle, resolution, autostart episode,
  hi-res assets); MoTS `.goo` boot path verified; `retro_reset`; Linux build; frame
  time callback → `sithTime` for fast-forward correctness.
- **M4 (stretch) — de-SDL**: replace the windowless-SDL config with a true
  `Platform/Libretro` platform (TWL/Dreamcast pattern) to drop the SDL3/SDL_mixer
  dependency entirely.
