#ifndef _JKSESSION_H
#define _JKSESSION_H

// Last-session record + resume. Ported from AAOpenJKDF2's aaSession module
// (fork commit a96def37, researched in devdocs/06) with its two known save
// warts fixed; same openjkdf2_lastsession.json schema (version 1), so records
// are interchangeable with the fork. Consumed by the libretro core's boot
// options; all engine hook sites are LIBRETRO_BUILD-gated.

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum jkSessionMode
{
    SESSION_MODE_NONE  = 0,
    SESSION_MODE_SP    = 1,
    SESSION_MODE_DEBUG = 2,
    SESSION_MODE_MP    = 3,
} jkSessionMode;

// How the next engine boot should start. Set by the core (from core options)
// via jkSession_ConfigureBoot before Main_Startup runs.
typedef enum jkSessionBoot
{
    JKSESSION_BOOT_INTRO  = 0, // stock flow: intro video, then the title menu
    JKSESSION_BOOT_MENU   = 1, // title menu directly (intro video skipped)
    JKSESSION_BOOT_DIRECT = 2, // autostart the ROM's episode from its start
    JKSESSION_BOOT_LEVEL  = 3, // last session's level at its normal start; falls back to DIRECT
    JKSESSION_BOOT_RESUME = 4, // last session's level at the exact spot; falls back to DIRECT
} jkSessionBoot;

// Which episode types a DIRECT boot applies to. The game mode itself
// (singleplayer vs hosting a local multiplayer session) always follows the
// episode's own TYPE from its episode.jk; this only limits which episodes get
// direct-booted at all -- others fall back to the game's main menu.
typedef enum jkSessionDirectFilter
{
    JKSESSION_DIRECT_ALL     = 0,
    JKSESSION_DIRECT_SP_ONLY = 1,
    JKSESSION_DIRECT_MP_ONLY = 2,
} jkSessionDirectFilter;

extern jkSessionMode jkSession_currentMode;
extern int           jkSession_pendingMpHosting;
extern int           jkSession_bResumed;
extern char          jkSession_resumeShortName[32];
extern int           jkSession_bSkipIntroVideo; /* consumed by jkSmack_SmackPlay;
                                                   derived: every boot mode except
                                                   INTRO skips the intro movie */

// Writes openjkdf2_lastsession.json based on current globals
// (jkRes_episodeGobName, jkMain_aLevelJklFname, jkPlayer_playerShortName,
// jkSession_currentMode, and, for MP, jkGuiNetHost_* + jkGuiMultiplayer_mpcInfo).
// Position is captured only when the local player is alive and inside a real
// sector; an invalid capture never clobbers an existing good record for the
// same episode+map (fork wart #2 fixed).
void jkSession_SaveCurrent(void);

// Reads openjkdf2_lastsession.json (if present) and populates
// Main_bAutostart / Main_bAutostartSp / Main_bDevMode / Main_strEpisode /
// Main_strMap / jkGuiNetHost_* / jkGuiMultiplayer_mpcInfo so that the
// existing Main_StartupDedicated path drives the user straight back in.
// If pExpectedEpisode is non-NULL/non-empty, a record for a different
// episode GOB is rejected (the ROM the frontend loaded wins).
// Returns 1 if the session file was loaded and applied, 0 otherwise.
int jkSession_LoadAndApply(const char* pExpectedEpisode);

// After the world is initialized and the local player thing is spawned,
// teleport the player to the position/orientation/headPYR/sector captured at
// exit. Consumed once per resume; subsequent level loads get normal spawn.
// Hooked at the tail of sithOpenPostProcess (after cog CREATED messages and
// the MP checkpoint teleport, so nothing clobbers the restored pose).
void jkSession_ApplyPendingPosition(void);

// Core-facing boot configuration (called before the engine boots).
void jkSession_ConfigureBoot(int bootMode, int directFilter,
                             const char* pRomEpisode);

// Applies the configured boot mode; called from Main_Startup right after
// Main_ParseCmdLine (LIBRETRO_BUILD hook). MENU is a no-op.
void jkSession_ArmBoot(void);

// For a DIRECT boot: probe the ROM episode's TYPE (mount + parse episode.jk)
// and set the game mode from it -- singleplayer episodes boot singleplayer,
// any multiplayer type hosts a local multiplayer session. Called from
// Main_StartupDedicated before anything mode-dependent runs (LIBRETRO_BUILD
// hook). Returns 0 when the episode's type is excluded by the direct-boot
// filter -- the caller cancels the autostart and the title flow runs instead.
int jkSession_ResolveAutoBootMode(void);

// Full-state SP resume (devdocs/08): when the armed boot is RESUME with a
// singleplayer record and the profile holds a matching per-episode
// _JKSESSION_<stem>.jks (written by SaveCurrent wherever a valid SP pose is
// captured), queue the engine's own no-world savegame load -- the Load Game
// menu's cold path (jkMain_sub_4034D0 -> JK_GAMEMODE_UNK -> gameMode 1) --
// and suppress the pose teleport (the save's own position wins). Called from
// Main_StartupDedicated's SP branch, after the profile is created. Returns 1
// when the savegame load was queued -- the caller then skips the level
// loader. Returns 0 (missing/unreadable/stale save) -- caller falls back to
// the pose-resume level load.
int jkSession_StartBootSave(void);

// First player profile on disk (a player/ subdirectory containing its
// <name>.plr), same enumeration as the player-select menu. Used as the last
// step of quick-start profile resolution: record-bound profile, else the
// registry's last-used profile, else this. Returns 1 and fills pOut, or 0
// when no profile exists at all (callers cancel the direct boot so the
// title flow forces character creation).
int jkSession_FindAnyProfile(char* pOut, int outSize);

// "modA.gob|modB.gob" for the mods/ files the resource scan loaded (empty
// when none). Recorded into session records and savestates as content
// provenance; nothing consumes it yet. Points at a static buffer.
const char* jkSession_ModsManifest(void);

// Quick-start MP character resolution (rule-style, like the profile chain):
// returns 1 when jkGuiMultiplayer_mpcInfo is already correct (a resumed MP
// session restored the record's block) or was filled here -- from the last
// session record's mp_char_* keys (any episode), else the profile's first
// .mpc on disk. Returns 0 when the caller should apply the stock Kyle
// default (SP boots, or an MP host with no character anywhere). Called from
// Main_StartupDedicated in place of the unconditional default.
int jkSession_ResolveMpCharacter(void);

// Frontend savestate bridge (devdocs/09): a libretro "savestate" here is the
// engine's own savegame captured to / restored from hidden scratch files in
// the profile dir ('~'-less display names keep them out of the Load Game
// list). Capture mirrors the Save Game menu's synchronous Save+Process pair;
// restore is a SIGNAL -- it queues the Load Game menu's exact load flow and
// completes over the following frames.

// Capture the current game as raw .jks bytes into pOut. Works whenever the
// native Save Game menu would: a loaded SP world with a local player, from
// gameplay or from inside the ESC menu. Returns 1 and sets *pOutLen; returns
// 0 when there is nothing to capture, a save/load is already in flight, or
// the save doesn't fit outCap.
int jkSession_StateCapture(void* pOut, unsigned int outCap, unsigned int* pOutLen);

// Queue a restore of bytes previously produced by StateCapture. Validates
// the leading engine save header, parks the bytes as a pending save in the
// profile, arms the menu's load route (jkPlayer_LoadSave for a live same-map
// world, the no-world jkMain_sub_4034D0 route otherwise), and suppresses any
// pending pose teleport (the save's own position wins).
#define JKSESSION_STATE_OK     1  /* queued; the engine loads it over the next frames */
#define JKSESSION_STATE_RETRY  0  /* engine not ready (no profile / transition in flight) */
#define JKSESSION_STATE_BAD  (-1) /* not a usable save payload -- drop it */
int jkSession_StateRestore(const void* pData, unsigned int len);

// Multiplayer states. MP has no engine savegame system at all (the engine
// refuses sithGamesave_Save while the multiplayer submode bit is set, and
// stock JK gates the quicksave key on it), so an MP state carries what MP
// RESUME carries -- level, pose and character -- and restores through the
// same pending-teleport path. Capture needs a live MP session with the
// player somewhere real; restore applies immediately when the state's map is
// the loaded one, otherwise leaves the pose armed (JKSESSION_STATE_RETRY) so
// a load of that map picks it up, exactly like a resume boot.
int jkSession_MpStateCapture(void* pOut, unsigned int outCap, unsigned int* pOutLen);
int jkSession_MpStateRestore(const void* pData, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif // _JKSESSION_H
