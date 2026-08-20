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

## Lifecycle sprint — clean unload + frontend Restart (DONE)

Shipped in `086c8dc8`. What the sprint proved and built (empirics reshaped the
plan — see DESIGN.md "Engine quiesce"):

- [x] **Thread inventory at unload** (empirical): exactly five threads
      survived the old drop-the-fiber unload, all OpenAL device threads (CRT
      `_beginthreadex` stubs starting in ucrtbase.dll) — they pinned the core
      DLL against dlclose's unmap. Nothing else (no SDL timers). Verified via
      external thread→module mapping (NtQueryInformationThread start
      addresses); after the fix the DLL actually unmaps and zero core/OpenAL
      threads remain.
- [x] **Engine quiesce** — split in two by an empirical finding: RetroArch
      calls `context_destroy` BEFORE `retro_unload_game`, so no frame may
      render during an unload quiesce. Unload uses an **inline shutdown at
      the fiber's park point** (WM_DESTROY-handler precedent: the engine
      already calls `Main_Shutdown` from inside the modal pump); `retro_reset`
      (context alive) uses the planned **cooperative unwind** — g_should_exit
      + force-pop one modal menu per resumed frame, bounded, then the
      standalone's own WriteConf → `Main_Shutdown` sequence at the frame
      boundary. Both end with `DeleteFiber`.
- [x] **Fallback** (boot-failure edge / quiesce refusal): WriteConf +
      `libretro_ForceCloseAudioDevice()` from the frontend fiber; logs loudly.
- [x] **GL ordering**: `std3D_FreeResources` runs in `context_destroy` or in
      the quiesce, whichever arrives first, guarded by
      `libretro_std3D_HasGlResources()`; the global VAO is deleted/zeroed on
      teardown so nothing rebinds stale handles.
- [x] **retro_deinit hygiene**: `ConvertFiberToThread` (+ last-chance quiesce
      for frontends that skip `retro_unload_game`).
- [x] **retro_reset**: quiesce + re-arm (fiber recreated lazily; boot re-runs
      `OpenJKDF2_Globals_Reset` — upstream's own in-process restart path).
- [x] **Acceptance**: in one RetroArch session — load → play → Close Content →
      load again → play, twice; Restart Content from the main menu, in-game,
      and mid-cutscene; audio stops on close; saves persist across the cycle;
      no crash on frontend exit. (Baseline contrast: the OLD build crashed in
      ucrtbase on frontend exit with content loaded.)

## Adopted from AAOpenJKDF2 (devdocs/06) — DONE

Shipped in `25860a97` + `543fe5a2`, all verified in RetroArch:

- [x] Episode GOB list cap raised 64 → 512 (`JKEPISODE_MAX_EPISODES`),
      with a log line when the cap is hit instead of the stock silent drop.
- [x] `jkSession` (port of the fork's aaSession, save warts fixed): last-session
      record in `<basefolder>/openjkdf2_lastsession.json`, captured at gameplay
      exit / level switches / the core's engine quiesce.
- [x] Core options: `openjkdf2_boot` (intro [default, stock flow] / menu [movie
      skipped] / straight-into-episode / continue-from-last-level /
      resume-exact-spot) and `openjkdf2_boot_game_type` (episode-type FILTER for
      direct boot — the SP/MP mode itself always follows the episode's own TYPE,
      probed from its episode.jk). Direct boot resolves the episode's STARTING
      level by walking the episode.jk decision path; resume always restores
      exact position+look via the engine's canonical teleport with
      never-into-the-void validation, and a ROM-mismatch guard falls back to
      direct boot. Intro-skip, cursor auto-hide, and position-restore are folded
      into the modes / fixed behaviors, not separate options.
- [x] ~~Session resume is map+pose only by design~~ — superseded by the
      full-state resume sprint below (devdocs/08); pose-only remains the MP
      behavior and the SP fallback tier.
- [x] devdocs/07 cherry-picks (`b44e6c98`): cog no-change-rebuild fix (~90 s →
      1.8 s), SDL_mixer Debug/Release CRT-clobber fix (paths adapted to 3.2.4),
      Release PDBs + `/GL`+`/LTCG`, `std3D_DoTex` GL-state dedup, and
      `stdPlatform_Printf` → `retro_log` mirror with duplicate suppression +
      `OPENJKDF2_LOG` file mirror. `/arch:AVX2` deferred until near release
      (old-hardware audience — devdocs/07 §6).

## Full-state SP resume (devdocs/08) — DONE

`openjkdf2_boot = resume` in singleplayer restores full world state (kills,
pickups, inventory, health) through the engine's own savegame system,
automatically — no new core option (`level` mode is the "fresh level" choice).
All verified in RetroArch:

- [x] Per-episode session save `player/<name>/_JKSESSION_<episodeStem>.jks`
      (stem sanitized — it comes from the GOB filename), co-written by
      `jkSession_SaveCurrent` at every point that captures a valid SP pose
      (gameplay exit, level switches, engine quiesce): quicksave-shaped
      `sithGamesave_Save` + immediate `Process` (write points are past the
      last `sithUpdate` tick). Guards: `bPlayerValid` (boot-time no-op calls
      never write), SP only, engine save/load not already in flight.
- [x] Resume boot loads the save through the engine's own no-world savegame
      flow (`jkMain_sub_4034D0` → `JK_GAMEMODE_UNK` → gameMode 1 →
      `sithGamesave_Restore`) — the Load Game menu's cold path, exactly as
      doc-08's gui-state warning demanded (verified from cold boot; the
      direct `jkPlayer_LoadSave` shortcut was not needed). The save's own
      position is used; the pending pose teleport is cleared.
- [x] Fallback chain verified: missing/unreadable session save → pose resume
      (fresh level + teleport); stale save (map differs from the pose
      record's — the record is ground truth for where the user last played)
      → pose resume; no record → episode start → title.
- [x] `level` mode ignores the session save (default spawn, no restore); MP
      resume keeps the pose path (MP has no saves; both `SaveCurrent`'s .jks
      write and the boot load are SP-gated).
- [x] Empirical extras: death after a resume reloads the session save (the
      engine's stock "restore last saved position" semantics — the loaded
      save is the last save); seeded round-trip carried inventory across
      relaunch (ammo 72 vs fresh-start 50) from a record with no position.
- [x] Bug found & fixed in round-trip testing: the no-world load flow parks
      the save FILENAME in `jkMain_aLevelJklFname` and never restored the
      real map, so post-restore session records said
      `map_jkl = _JKSESSION_*.jks` and poisoned the next resume.
      `SaveCurrent` now records the live world's map name, and
      `sithGamesave_RestoreFile` (LIBRETRO_BUILD) writes the real map back
      after a successful restore.

## Frontend savestates (devdocs/09) — DONE

Save State / Load State in the frontend work as a bridge over the engine's
own savegame system — a state is an engine .jks in a fixed 8 MiB envelope
(zero padding compresses to ~nothing; real state file ≈ 47 KB). Load is a
restore *signal*: it queues the Load Game menu's own flow and completes over
the following frames. All verified in RetroArch via UDP commands:

- [x] `retro_serialize` = the Save Game menu's `sithGamesave_Save` +
      immediate `Process` into a hidden scratch save (`~`-less display name
      keeps it out of the Load Game list), read back into the frontend
      buffer. Death-respawn target (`sithGamesave_autosave_fname`) preserved
      around the capture. Works in gameplay and in the ESC menu; refused
      (empty state) in MP / no-world / mid-save-load.
- [x] `retro_unserialize` = validate the embedded save header, park
      `_JKSTATE_PENDING.jks`, arm the menu's exact load branching
      (`jkPlayer_LoadSave` live same-map; `jkMain_sub_4034D0` otherwise);
      pending pose teleport cleared. Not-ready loads (pre-boot auto-load,
      title screen without a profile) park in the core and self-arm from
      `retro_run` when the engine is ready (60 s budget, OSD give-up).
- [x] Serialize FAILS when there is nothing to capture, so no unloadable slot
      is ever written (frontend-team request 2026-08-20). Supersedes the
      earlier "empty state" hack, which rested on a wrong inference — a
      frontend's failed pre-load undo snapshot does NOT abort the load
      (RetroArch logs it and calls unserialize anyway), and auto-load-state
      never serializes at all. Verified both ways.
- [x] Multiplayer states: the engine has no MP savegame (`sithGamesave_Save`
      refuses while the MP submode bit is set), so MP states carry level +
      pose + character — the same payload MP resume uses — restored through
      the same `jkSession_ApplyPendingPosition` teleport. Envelope gained a
      `payload_kind` field (old states read back as kind 0).
- [x] Quirks declared (`INCOMPLETE | PLATFORM | ENDIAN`) + savestate-context
      check: rewind/run-ahead/netplay never route through this path.
- [x] Packaging: new core info file
      (`src/Platform/Libretro/openjkdf2_libretro.info`,
      `savestate_features = "basic"`) — RetroArch ≥ 1.15 disables savestates
      entirely for cores without one, and only matches it when the DLL is in
      the frontend's cores directory (harness now deploys there).
- [x] Bugs found in testing: `pLowLevelHS->fileSize` is NULL on this build
      (POSIX host services never set it — crashed RetroArch at first
      capture; now fseek/ftell), and `jkPlayer_bLoadingSomething` is not a
      transition signal (stays set through gameplay after a direct boot —
      dropped from the guards).
- [x] Regressions checked: session resume still writes `_JKSESSION_JK1.jks`
      at quit; `resume` boot follows the last savestate load by design.

## Boot default + profile resolution (owner feedback 2026-08-20) — DONE

- [x] `openjkdf2_boot` defaults to `episode` — loading content goes straight
      in-game; intro/menu click-through is opt-in. (Verified: no options
      file → direct boot into the level.)
- [x] Quick-start profile chain: record-bound profile (resume) → registry's
      last-used (if its .plr exists) → first profile on disk
      (`jkSession_FindAnyProfile`) → CANCEL the autostart so the title flow
      forces the character-creation dialog (uncancellable with no profiles;
      screenshot-verified it lands directly on New Player). Profiles must
      exist for in-game option changes to persist.
- [x] `jkGui_Shutdown` no longer persists an empty profile name — an
      unattended title visit used to wipe the registry's last-used profile
      and silently degrade every save feature on later boots. (Verified:
      registry self-repairs to the loaded profile on quit.)
- [x] "Disable episode confirmation" (the `menu_bFastMissionText` cvar — the
      mission-text OK gate on the level loading screen, Setup → General)
      defaults ON for LIBRETRO_BUILD: new profiles go straight in-game after
      a level load. Per-profile cvar; existing profiles keep their saved
      choice.
- [x] Quick-start MP character resolution (`jkSession_ResolveMpCharacter`):
      resumed MP sessions keep the record's restored character; direct-boot
      MP hosts get the record's last-used character, else the profile's
      first .mpc (verified: JK1MP direct boot hosts as the profile's
      character), else the stock Kyle default.

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

## M2 — audio consolidation (DONE)

Was: engine opened its own OpenAL device (SFX + cutscene audio outside the
frontend's pipeline); music inaudible; core submitted silence to keep frontend
pacing. Now (see DESIGN.md "Audio" for the implemented shape):

- [x] OpenAL Soft **loopback device** (`ALC_SOFT_loopback` /
      `alcRenderSamplesSOFT`): device creation swapped in `stdSound` under
      `LIBRETRO_BUILD`; exactly 800 frames (48000/60, integer — no drift)
      rendered per `retro_run`, submitted as the single `audio_batch_cb`.
      Loopback devices spawn no mixer thread — thread inventory with content
      running went from 5 OpenAL device threads to zero.
- [x] Music: routed through an OpenAL **streaming source** under
      `LIBRETRO_BUILD` — SDL_mixer stays the decoder (device-less
      `MIX_CreateMixer` + `MIX_Generate`, SDL audio forced to the `dummy`
      backend so WASAPI never spawns threads), pumped once per `retro_run`
      into queued AL buffers. All the stock `MUSIC/Track*.ogg` lookup and
      track-advance logic is untouched. Empirical find: music was never
      "SDL not initialized"-dead — the old build opened a real SDL WASAPI
      device and played into it; JK1's per-level COGs (`setmusicvol`) duck
      music to 0 outside combat, which is why it read as silent.
- [x] ~~SMUSH cutscene audio: redirect its device output~~ — verified stale
      (devdocs): cutscene audio already plays through `stdSound` OpenAL buffers
      ([src/Main/jkCutscene.c:400](src/Main/jkCutscene.c#L400)); loopback
      captures it for free. Pause/volume verified at M2.
- [x] Engine no longer opens any real audio device (loopback + dummy SDL
      backend); RetroArch recording captures the core's audio (verified
      non-silent via ffmpeg volumedetect); pause is hard-silent; fast-forward
      pitches up (ear-verified). Note: GPU recording encodes on the CPU and
      can drag the frame loop below 60fps on this machine — choppiness while
      recording is a recording artifact, not an audio-path bug.

## M3 — options, platforms, lifecycle polish

- [x] **`openjkdf2_resolution`** — DONE (2026-08-20, pulled forward from M3).
      Internal render size, applied live: `Window_xSize/ySize` +
      `Window_resized = 1` (the engine's own resize path) + `SET_GEOMETRY`.
      4:3, 16:9 and 16:10 sizes up to the declared 1920x1440 maximum, so no
      AV-info reinit. Verified booting at 1280x720 (frontend reports aspect
      1.778, widescreen FOV rendered natively — not stretched or cropped)
      and at 1920x1440 (aspect 1.333).
- [x] ~~`openjkdf2_autostart_episode`~~ — DROPPED: fully superseded by
      `openjkdf2_boot`, which already autostarts the ROM's episode in
      `episode`/`level`/`resume` and offers `intro`/`menu` as the opt-outs.
- [x] **`openjkdf2_use_mods`** — DONE, as a default-ON escape hatch (owner
      decision): mods/ overriding resource/ stays the normal behavior (and
      matches upstream), the option only lets a user play unmodded without
      moving files. Gates the scan via `jkRes_bAllowModsDir`
      ([src/Main/jkRes.c](src/Main/jkRes.c)); pre-boot decision, so it needs
      a content restart.
- [x] ~~`openjkdf2_hires_assets`~~ — DROPPED (owner decision): `Res1low.gob`
      is the one that always shipped and `Res1hi.gob` was the optional
      high-res install, but OpenJKDF2 already exposes the choice in its own
      in-game options, and the resource scan loads whatever is present by
      wildcard. A core option would just duplicate an in-game setting.
- [x] **MoTS**: `.goo` boot verified end-to-end (2026-08-20) — `JKM.GOO`
      loads, `Main_bMotsCompat` is set from the extension, the resource layer
      switches to `goo`, and the game is playable (test basefolder
      `testdata/mots`, assembled from the Steam install). A fresh MoTS
      basefolder has no profile, so the direct boot correctly cancels into
      character creation first. Savestate/resume matrix against MoTS still
      to run.
- [x] **Save provenance**: session records and savestates now record which
      game they belong to (`game` = `jk1`/`mots`; savestates also keep the
      flags bit) and the mods/ manifest that was loaded. Resume refuses a
      record from the other game; savestate loads refuse the other game and
      log (without blocking) a changed mod set. Note: the engine's own
      `sithGamesave_Header` records neither — it has only version, episode,
      map, health, bins and the display name.
- [x] ~~retro_reset + clean unload~~ — promoted to the lifecycle sprint above.
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
| ~~Music silent; SFX bypasses frontend audio (no FF pitch, plays while paused)~~ — fixed at M2 (loopback consolidation); JK1 music is COG-ducked outside combat by design | done |
| Middle/extra mouse buttons dead | M1 |
| ~~`retro_reset` is a no-op; unload leaks the parked fiber's engine state and may leave OpenAL threads pinning the DLL~~ — fixed in the lifecycle sprint (`086c8dc8`) | done |
| OpenAL32.dll stays mapped in the frontend after core unload (no threads, inert; next load reuses it cleanly) | cosmetic — release note only |
| Display options menu shows non-functional entries | M1 |
| Mods menu entry present but restart-based (non-functional) | M1 |
| RetroArch "Game Focus" can be toggled off by Scroll Lock, muting hotkey-bound keys | docs |
| RetroArch's own menu may not regain the mouse over a running game (frontend grab state; Alt+F4 still exits cleanly) | docs |
