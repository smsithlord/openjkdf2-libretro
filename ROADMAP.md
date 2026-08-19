# openjkdf2_libretro — Roadmap to release

Loose working plan from today's state to a releasable core. Companion to
[DESIGN.md](DESIGN.md) (architecture) and [README_LIBRETRO.md](README_LIBRETRO.md)
(build/usage). Checked boxes = done and verified in RetroArch.

## Where we are

- [x] M0 — core builds (MSVC x64), HW GL 3.3 render, boots to the GUI
- [x] Engine-on-a-fiber lifecycle (modal menus yield per frame)
- [x] Keyboard (events + polled fallback + character synthesis), mouse, core-drawn cursor
- [x] In-game verified: movement, mouse-look, weapon select, activate
- [x] Friendly on-screen error for wrong/missing game data layouts
- [x] In-game Quit → clean frontend shutdown

## M1 — playable v1 (finish line for "it's a real core")

### Hide/neutralize features that can't work under a frontend
The pattern: `#ifdef LIBRETRO_BUILD` removing elements from the menus' element
arrays (or `bIsVisible = 0`), same as existing `TARGET_*` menu gating.

- [ ] **Display options** (`jkGUIDisplay.c`): hide fullscreen, resolution list
      (enumerated via SDL video — empty/no-op), HiDPI, vsync. See the disposition
      table below for where each removed option goes; keep gamma/brightness-style
      options that pure-GL paths honor.
- [ ] **Mods menu** (`jkGUIMods.c`, entry in `jkGUIMain.c`): relies on `-path` +
      full process restart (`openjkdf2_restartMode`) — hide the entry; `mods/*.gob`
      auto-override is the supported path.
- [ ] **DF2 ↔ MoTS switching** entries: also restart-based — hide (each game loads
      via its own ROM instead).
- [ ] Audit remaining `jkGuiMain`/setup screens for anything touching windowing,
      update checker, or process restart; hide or stub each.
- [ ] Multiplayer menus: networking is already `Platform/Networking/None`; either
      hide the menu entries or build with `TARGET_NO_MULTIPLAYER_MENUS TRUE`
      (needs a link check — MSVC standalone never builds that combination).

### Input completion
- [ ] **RetroPad mapping**: drive `stdControl`'s joystick axes/buttons
      (`stdControl_aAxisPos` / `KEY_JOY1_*`) from RetroPad state, mirroring
      `Platform/SDL2/stdControl.c`'s SDL_GameController layout; declare input
      descriptors so RetroArch shows sane bind names. Menu navigation via
      `jkGuiRend_UpdateController` should come along for free. Honor the port-0
      device selection (SET_CONTROLLER_INFO is already declared: "Keyboard +
      Mouse" / "RetroPad"): pad injection active on RetroPad (the frontend
      default), disabled when the user picks Keyboard + Mouse; keyboard/mouse
      injection stays on in both modes.
- [ ] Mouse buttons 3/4/5 (currently dead — engine reads them from
      `SDL_GetMouseState`): wire `RETRO_DEVICE_MOUSE` middle/4/5 into the same
      `stdControl` keys.
- [ ] Mouse wheel: verify weapon-cycling / menu scrolling both directions.
- [ ] Text edge cases: Quake console (`~`), cheat entry, save-name entry.

### Correctness
- [ ] **Native save round-trip**: save in-game → quit → reload content → load save.
      Also checkpoint/`persist/` behavior across content reloads.
- [ ] **Intro-crawl corner artifact**: ghost of the previous frame appears top-left
      during the crawl. The engine assumes the window framebuffer's contents persist
      frame-to-frame (partial menu redraws); RetroArch's HW-render FBO may be
      double-buffered. Likely fix: force full redraw under `LIBRETRO_BUILD`, or blit
      the previous frame's FBO first when only dirty-rects were drawn.
- [ ] Cutscene pause (Space) — verify video+audio pause/resume path.
- [ ] Long-session soak test (memory growth, GL resource leaks across level loads).

### Option-disposition rule

An in-game option becomes a **core option** only if the frontend can't already do
it and the engine can't decide it from inside the game. Everything removed from
the in-game menus lands in exactly one bucket:

| In-game option | Disposition |
|---|---|
| Resolution list | **Core option** `openjkdf2_resolution` — core owns `Window_xSize/ySize`; include 16:9/16:10 sizes (engine renders widescreen FOV natively) and update the aspect via `SET_GEOMETRY` on change |
| Fullscreen | Frontend-native (RetroArch video settings) — hide |
| VSync | Frontend-native — hide |
| HiDPI / window scaling | Meaningless under a frontend — hide |
| Mods enable | **Core option** `openjkdf2_use_mods` (pre-boot decision) |
| Episode autostart | **Core option** `openjkdf2_autostart_episode` |
| Hi-res assets (`Res1hi.gob`) | **Core option** `openjkdf2_hires_assets` |
| Gamma/brightness, FOV, filtering, bloom/SSAO, hi-poly | Stay in-game — pure GL paths that work, persisted per player profile |
| Sound volumes, control binds | Stay in-game — per-profile settings |
| Mods menu (`-path` restart), DF2↔MoTS switch | Neither — restart-based; hidden with no replacement (`mods/` folder + per-game ROMs cover them) |

## M2 — audio consolidation (the last big architectural piece)

Today: engine opens its own OpenAL device (SFX + cutscene audio audible, outside
the frontend's pipeline); music/`stdMci` silent (SDL audio never initialized);
core submits silence to keep frontend pacing.

- [ ] OpenAL Soft **loopback device** (`ALC_SOFT_loopback` /
      `alcRenderSamplesSOFT`): swap device creation in `stdSound` under
      `LIBRETRO_BUILD`; render exactly 800 frames (48000/60, integer — no drift)
      per `retro_run`, submitted as the single `audio_batch_cb`. Loopback
      devices spawn no mixer thread: audio stays synchronous/deterministic.
- [ ] Music: `stdMci` is the ONLY non-OpenAL audio path. Route it through an
      OpenAL **streaming source** under `LIBRETRO_BUILD` (SDL_mixer decode-only
      or stb_vorbis on `MUSIC/Track*.ogg` → queued AL buffers) so the loopback
      render captures everything — one mixer, no manual sample summing.
- [x] ~~SMUSH cutscene audio: redirect its device output~~ — verified stale
      (devdocs): cutscene audio already plays through `stdSound` OpenAL buffers
      ([src/Main/jkCutscene.c:400](src/Main/jkCutscene.c#L400)); loopback
      captures it for free. Just verify pause/volume behavior at M2.
- [ ] Engine no longer opens any real audio device; verify fast-forward
      pitches correctly, pause silences, and RetroArch recording captures audio.

## M3 — options, platforms, lifecycle polish

- [ ] **Core options** (v2 API):
      `openjkdf2_use_mods` (gates the `mods/` scan via a small engine flag at
      [src/Main/jkRes.c:268](src/Main/jkRes.c#L268); default off per project brief —
      flip current always-on behavior),
      `openjkdf2_resolution` (internal render size → `SET_GEOMETRY` +
      `Window_resized = 1`),
      `openjkdf2_autostart_episode` (derive `-episode <rom name> -autostart`),
      `openjkdf2_hires_assets` (skip `Res1hi.gob`).
- [ ] **MoTS**: verify `.goo` boot end-to-end (`Main_bMotsCompat`, `JKM.goo`).
- [ ] **retro_reset** + clean unload: cooperative engine shutdown from a parked
      fiber (request-flag at the frame-boundary yield; modal-parked = decline),
      then `Main_Shutdown` + `OpenJKDF2_Globals_Reset` re-init. Also makes
      in-process content reload safe for non-RetroArch frontends.
- [ ] Frame-time callback (`SET_FRAME_TIME_CALLBACK`) → `sithTime`, so
      fast-forward/slow-motion scale game time instead of wall clock.
- [ ] **Linux build**: extend `plat_libretro.cmake` from the `plat_linux_64`
      baseline; replace Win32 fibers with libco (or ucontext); case-sensitivity
      pass (`fcaseopen` is already in-tree).
- [ ] Cursor: consider using the game's real cursor bitmap instead of the wedge.

## Release engineering

- [ ] **Core info file** (`openjkdf2_libretro.info`): display name, `gob|goo`
      extensions, `needs_fullpath`, database/system lines, the "video driver must
      be gl/glcore" and Game Focus notes. Required for good frontend UX.
- [ ] `library_version` from the OpenJKDF2 version + core revision (currently
      inherits `OPENJKDF2_RELEASE_VERSION_STRING`).
- [ ] **OpenAL32.dll**: static-link OpenAL Soft into the core if licensing/build
      allows, else ship the DLL next to the core and document it.
- [ ] CI (GitHub Actions): Windows x64 build on push; Linux once M3 lands;
      artifact = zip of core (+ info file, README).
- [ ] License review for distribution (upstream's LICENSE + OpenAL/SDL/GLEW
      notices in the release zip).
- [ ] User docs pass: install guide with screenshots, troubleshooting
      (wrong video driver, Game Focus, data layout), mods how-to.
- [ ] Upstream hygiene: PR the generic MSVC fixes upstream (`strtok_r`,
      case-range — their MSVC build is broken on master); periodically
      `git merge upstream/master`.

## Stretch (post-1.0)

- De-SDL the core entirely (TWL/Dreamcast-style platform files) — drops the
  SDL3/SDL_mixer dependency; prerequisite for exotic libretro platforms.
- Optional save relocation: redirect `player/` + `persist/` + settings writes
  into the frontend's save directory (`GET_SAVE_DIRECTORY`) so RetroArch's
  backup/cloud-sync tooling covers JK saves. Native files stay the mechanism —
  the SRAM (.srm) interface is deliberately unused (fixed-size blob, wrong
  shape for file-based saves; `retro_get_memory_size` returns 0 on purpose).
- Save states: no engine snapshot support, and the engine-on-a-fiber design
  makes true snapshots impossible (a parked C stack isn't serializable).
  Wrapping native saves would break rewind/run-ahead/netplay expectations;
  `retro_serialize_size() == 0` is the honest contract. If ever revisited:
  RETRO_SERIALIZATION_QUIRK_* flags are the only defensible shape. Not 1.0.
- RetroAchievements: viable via the PrBoom/TyrQuake pattern — a curated,
  **append-only** synthetic memory block (player health/shields/force, weapon,
  current episode+level, kill/secret counters, difficulty, cheats-active,
  mods-active) repopulated from engine globals each frame and exposed as
  `RETRO_MEMORY_SYSTEM_RAM` for rcheevos triggers. Layout offsets are a
  forever-contract once sets are published. Hardcore-mode notes: no save
  states (already true), cheat and `mods/` flags must be in the block so set
  authors can guard them (mod GOBs alter gameplay without changing the episode
  GOB that RA hashes for identity). Post-1.0.
- MoTS/DF2 dual-install QoL, playlist/thumbnail assets for RetroArch.

## Known issues (tracked)

| Issue | Milestone |
|---|---|
| Ghost-frame artifact, top-left corner, during opening crawl | M1 |
| Music silent; SFX bypasses frontend audio (no FF pitch, plays while paused) | M2 |
| Middle/extra mouse buttons dead | M1 |
| `retro_reset` is a no-op; unload leaks the parked fiber's engine state | M3 |
| Display options menu shows non-functional entries | M1 |
| Mods menu entry present but restart-based (non-functional) | M1 |
| RetroArch "Game Focus" can be toggled off by Scroll Lock, muting hotkey-bound keys | docs |
