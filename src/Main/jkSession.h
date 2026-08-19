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
    JKSESSION_BOOT_MENU   = 0, // stock title flow
    JKSESSION_BOOT_DIRECT = 1, // autostart the ROM's episode from the top
    JKSESSION_BOOT_RESUME = 2, // resume last session; falls back to DIRECT
} jkSessionBoot;

extern jkSessionMode jkSession_currentMode;
extern int           jkSession_pendingMpHosting;
extern int           jkSession_bResumed;
extern char          jkSession_resumeShortName[32];
extern int           jkSession_bSkipIntroVideo; /* consumed by jkSmack_SmackPlay */

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
void jkSession_ConfigureBoot(int bootMode, int bMultiplayer,
                             const char* pRomEpisode, int bRestorePosition,
                             int bSkipIntroVideo);

// Applies the configured boot mode; called from Main_Startup right after
// Main_ParseCmdLine (LIBRETRO_BUILD hook). MENU is a no-op.
void jkSession_ArmBoot(void);

#ifdef __cplusplus
}
#endif

#endif // _JKSESSION_H
