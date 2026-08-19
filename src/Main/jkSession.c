#include "jkSession.h"

#include "General/stdJSON.h"
#include "General/stdString.h"
#include "General/stdFnames.h"
#include "stdPlatform.h"
#include "Main/Main.h"
#include "Main/jkRes.h"
#include "Main/jkMain.h"
#include "World/jkPlayer.h"
#include "World/sithThing.h"
#include "World/sithSector.h"
#include "World/sithWorld.h"
#include "Engine/sithIntersect.h"
#include "Engine/sithCamera.h"
#include "Engine/sithPhysics.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "Gui/jkGUINetHost.h"

#include <string.h>

#define JKSESSION_FNAME "openjkdf2_lastsession.json"
#define JKSESSION_VERSION 1

jkSessionMode jkSession_currentMode      = SESSION_MODE_NONE;
int           jkSession_pendingMpHosting = 0;
int           jkSession_bResumed         = 0;
char          jkSession_resumeShortName[32] = {0};
int           jkSession_bSkipIntroVideo  = 0;

// Several engine globals live in generated globals.c / .c files without a
// header-declared extern. Re-declare the ones we need here (types match
// symbols.syms / Main.c) to avoid spreading externs.
extern char    jkRes_episodeGobName[32];
extern char    jkMain_aLevelJklFname[128];
extern int32_t Main_bAutostart;
extern int32_t Main_bAutostartSp;
extern char    Main_strEpisode[129];
extern char    Main_strMap[128+4];

// Boot configuration handed in by the core before the engine starts.
static int  jkSession_bootMode = JKSESSION_BOOT_MENU;
static int  jkSession_bootMp = 0;
static char jkSession_romEpisode[32] = {0};
static int  jkSession_bRestorePosition = 1;

// Pending teleport state -- populated by LoadAndApply, consumed once by
// ApplyPendingPosition after the player thing becomes valid.
static int        jkSession_bPendingPosition = 0;
static rdVector3  jkSession_pendingPos;
static rdMatrix34 jkSession_pendingLookOrient;
static rdVector3  jkSession_pendingEyePYR;
static int        jkSession_pendingSectorIdx = -1;
static char       jkSession_pendingMapJkl[128] = {0};

static const char* jkSession_ModeStr(jkSessionMode mode)
{
    switch (mode)
    {
        case SESSION_MODE_SP:    return "sp";
        case SESSION_MODE_DEBUG: return "debug";
        case SESSION_MODE_MP:    return "mp";
        default:                 return "";
    }
}

static jkSessionMode jkSession_ModeFromStr(const char* s)
{
    if (!s || !s[0]) return SESSION_MODE_NONE;
    if (!strcmp(s, "sp"))    return SESSION_MODE_SP;
    if (!strcmp(s, "debug")) return SESSION_MODE_DEBUG;
    if (!strcmp(s, "mp"))    return SESSION_MODE_MP;
    return SESSION_MODE_NONE;
}

// Case-insensitive compare of episode GOB names ignoring any extension
// ("JK1" == "jk1.gob").
static int jkSession_EpisodeStemEquals(const char* a, const char* b)
{
    while (*a && *a != '.' && *b && *b != '.')
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (!*a || *a == '.') && (!*b || *b == '.');
}

void jkSession_SaveCurrent(void)
{
    const char* fpath = JKSESSION_FNAME;

    if (!jkRes_episodeGobName[0] && !jkMain_aLevelJklFname[0])
        return;

    // Position snapshot -- only when the local player thing is alive and the
    // world is available so we can compute the sector index.
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    int bPlayerValid =
        pLocal && pWorld
        && (pLocal->flags & SITH_TF_DEAD) == 0
        && pLocal->actorParams.health > 0.0
        && pLocal->sector
        && pWorld->aSectors
        && pLocal->sector >= pWorld->aSectors
        && pLocal->sector <  pWorld->aSectors + pWorld->numSectors;

    // A player in the void keeps a STALE sector pointer (the engine doesn't
    // null it), so the bounds check above passes even out in empty space.
    // Require some sector to actually contain the position (same
    // point-in-sector lookup the restore path uses) -- otherwise don't
    // persist it, so next launch spawns at the default point instead of back
    // in the void.
    if (bPlayerValid && !sithSector_FindSectorAtPos(pWorld, &pLocal->position))
        bPlayerValid = 0;

    // Mode: prefer the explicit tag from the menu handlers; if a session was
    // entered some other way (e.g. loading a save from the main menu), derive
    // it from the live game state so resume still works.
    jkSessionMode mode = jkSession_currentMode;
    if (mode == SESSION_MODE_NONE)
    {
        if (!pLocal || !pWorld)
            return; // nothing identifiable to record
        mode = sithNet_isMulti ? SESSION_MODE_MP : SESSION_MODE_SP;
        jkSession_currentMode = mode;
    }

    // Fork wart #2 fix: a save with no valid position must never clobber an
    // existing record that has one for the same episode+map (e.g. the
    // teardown-time save that runs after GameplayLeave already captured a
    // good pose).
    if (!bPlayerValid)
    {
        char oldEpisode[128] = {0};
        char oldMap[128] = {0};
        stdJSON_GetString(fpath, "episode_gob", oldEpisode, sizeof(oldEpisode), "");
        stdJSON_GetString(fpath, "map_jkl",     oldMap,     sizeof(oldMap),     "");
        if (stdJSON_GetBool(fpath, "has_position", 0)
            && jkSession_EpisodeStemEquals(oldEpisode, jkRes_episodeGobName)
            && !__strcmpi(oldMap, jkMain_aLevelJklFname))
        {
            return;
        }
    }

    char shortName[32];
    memset(shortName, 0, sizeof(shortName));
    stdString_WcharToChar(shortName, jkPlayer_playerShortName, 31);
    shortName[31] = 0;

    // Start from a clean file so stale MP fields don't leak into an SP
    // session (or vice versa).
    stdJSON_EraseAll(fpath);

    stdJSON_SaveInt  (fpath, "version",           JKSESSION_VERSION);
    stdJSON_SetString(fpath, "mode",              jkSession_ModeStr(mode));
    stdJSON_SetString(fpath, "episode_gob",       jkRes_episodeGobName);
    stdJSON_SetString(fpath, "map_jkl",           jkMain_aLevelJklFname);
    stdJSON_SetString(fpath, "player_short_name", shortName);
    stdJSON_SaveInt  (fpath, "force_rank",        0); // reserved; profile restore carries rank via .plr

    stdJSON_SaveBool(fpath, "has_position", bPlayerValid);
    if (bPlayerValid)
    {
        int sectorIdx = (int)(pLocal->sector - pWorld->aSectors);
        stdJSON_SaveInt  (fpath, "sector_idx", sectorIdx);
        stdJSON_SaveFloat(fpath, "pos_x",      pLocal->position.x);
        stdJSON_SaveFloat(fpath, "pos_y",      pLocal->position.y);
        stdJSON_SaveFloat(fpath, "pos_z",      pLocal->position.z);
        stdJSON_SaveFloat(fpath, "look_rvec_x", pLocal->orient.rvec.x);
        stdJSON_SaveFloat(fpath, "look_rvec_y", pLocal->orient.rvec.y);
        stdJSON_SaveFloat(fpath, "look_rvec_z", pLocal->orient.rvec.z);
        stdJSON_SaveFloat(fpath, "look_lvec_x", pLocal->orient.lvec.x);
        stdJSON_SaveFloat(fpath, "look_lvec_y", pLocal->orient.lvec.y);
        stdJSON_SaveFloat(fpath, "look_lvec_z", pLocal->orient.lvec.z);
        stdJSON_SaveFloat(fpath, "look_uvec_x", pLocal->orient.uvec.x);
        stdJSON_SaveFloat(fpath, "look_uvec_y", pLocal->orient.uvec.y);
        stdJSON_SaveFloat(fpath, "look_uvec_z", pLocal->orient.uvec.z);
        stdJSON_SaveFloat(fpath, "eye_pyr_x",  pLocal->actorParams.headPYR.x);
        stdJSON_SaveFloat(fpath, "eye_pyr_y",  pLocal->actorParams.headPYR.y);
        stdJSON_SaveFloat(fpath, "eye_pyr_z",  pLocal->actorParams.headPYR.z);
    }

    if (mode == SESSION_MODE_MP)
    {
        stdJSON_SetWString(fpath, "mp_char_name",       jkGuiMultiplayer_mpcInfo.name);
        stdJSON_SetString (fpath, "mp_char_model",      jkGuiMultiplayer_mpcInfo.model);
        stdJSON_SetString (fpath, "mp_char_sound",      jkGuiMultiplayer_mpcInfo.soundClass);
        stdJSON_SetString (fpath, "mp_saber_side_mat",  jkGuiMultiplayer_mpcInfo.sideMat);
        stdJSON_SetString (fpath, "mp_saber_tip_mat",   jkGuiMultiplayer_mpcInfo.tipMat);
        stdJSON_SaveInt   (fpath, "mp_char_jedi_rank",  jkGuiMultiplayer_mpcInfo.jediRank);

        stdJSON_SaveBool  (fpath, "mp_was_hosting",     jkSession_pendingMpHosting);
        stdJSON_SetWString(fpath, "mp_server_name",     jkGuiNetHost_gameName);
        stdJSON_SaveInt   (fpath, "mp_session_flags",   jkGuiNetHost_sessionFlags);
        stdJSON_SaveInt   (fpath, "mp_multi_mode_flags",jkGuiNetHost_gameFlags);
        stdJSON_SaveInt   (fpath, "mp_max_players",     jkGuiNetHost_maxPlayers);
        stdJSON_SaveInt   (fpath, "mp_max_rank",        jkGuiNetHost_maxRank);
        stdJSON_SaveInt   (fpath, "mp_score_limit",     jkGuiNetHost_scoreLimit);
        stdJSON_SaveInt   (fpath, "mp_time_limit",      jkGuiNetHost_timeLimit);
        stdJSON_SaveInt   (fpath, "mp_tick_rate",       jkGuiNetHost_tickRate);
    }
}

int jkSession_LoadAndApply(const char* pExpectedEpisode)
{
    const char* fpath = JKSESSION_FNAME;

    int version = stdJSON_GetInt(fpath, "version", 0);
    if (version < 1)
        return 0;

    char modeStr[16] = {0};
    stdJSON_GetString(fpath, "mode", modeStr, sizeof(modeStr), "");
    jkSessionMode mode = jkSession_ModeFromStr(modeStr);
    if (mode == SESSION_MODE_NONE)
        return 0;

    char episode[128] = {0};
    char mapJkl [128] = {0};
    stdJSON_GetString(fpath, "episode_gob", episode, sizeof(episode), "");
    stdJSON_GetString(fpath, "map_jkl",     mapJkl,  sizeof(mapJkl),  "");
    if (!episode[0] || !mapJkl[0])
        return 0;

    // ROM consistency: if the frontend loaded a different episode GOB than
    // the record describes, the ROM wins (the caller falls back to a direct
    // boot of that episode).
    if (pExpectedEpisode && pExpectedEpisode[0]
        && !jkSession_EpisodeStemEquals(episode, pExpectedEpisode))
    {
        stdPlatform_Printf("jkSession: last session is for '%s', loaded ROM is '%s' - not resuming\n",
                           episode, pExpectedEpisode);
        return 0;
    }

    // Drive the existing -autostart path in Main_StartupDedicated.
    Main_bAutostart   = 1;
    Main_bAutostartSp = (mode == SESSION_MODE_SP || mode == SESSION_MODE_DEBUG) ? 1 : 0;
    Main_bDevMode     = (mode == SESSION_MODE_DEBUG) ? 1 : 0;

    stdString_SafeStrCopy(Main_strEpisode, episode, sizeof(Main_strEpisode));
    stdString_SafeStrCopy(Main_strMap,     mapJkl,  sizeof(Main_strMap));

    // Hand the profile short-name off to Main_StartupDedicated via our module buffer.
    memset(jkSession_resumeShortName, 0, sizeof(jkSession_resumeShortName));
    stdJSON_GetString(fpath, "player_short_name",
                      jkSession_resumeShortName,
                      sizeof(jkSession_resumeShortName), "");

    if (mode == SESSION_MODE_MP)
    {
        memset(&jkGuiMultiplayer_mpcInfo, 0, sizeof(jkGuiMultiplayer_mpcInfo));
        stdJSON_GetWString(fpath, "mp_char_name",
                           jkGuiMultiplayer_mpcInfo.name, 32, u"");
        stdJSON_GetString (fpath, "mp_char_model",
                           jkGuiMultiplayer_mpcInfo.model, 32, "ky.3do");
        stdJSON_GetString (fpath, "mp_char_sound",
                           jkGuiMultiplayer_mpcInfo.soundClass, 32, "ky.snd");
        stdJSON_GetString (fpath, "mp_saber_side_mat",
                           jkGuiMultiplayer_mpcInfo.sideMat, 32, "sabergreen1.mat");
        stdJSON_GetString (fpath, "mp_saber_tip_mat",
                           jkGuiMultiplayer_mpcInfo.tipMat, 32, "sabergreen0.mat");
        jkGuiMultiplayer_mpcInfo.jediRank =
            stdJSON_GetInt(fpath, "mp_char_jedi_rank", 0);

        stdJSON_GetWString(fpath, "mp_server_name",
                           jkGuiNetHost_gameName, 32, u"OpenJKDF2 Server");
        jkGuiNetHost_sessionFlags = stdJSON_GetInt(fpath, "mp_session_flags",    0);
        jkGuiNetHost_gameFlags    = stdJSON_GetInt(fpath, "mp_multi_mode_flags", 144);
        jkGuiNetHost_maxPlayers   = stdJSON_GetInt(fpath, "mp_max_players",      4);
        jkGuiNetHost_maxRank      = stdJSON_GetInt(fpath, "mp_max_rank",         4);
        jkGuiNetHost_scoreLimit   = stdJSON_GetInt(fpath, "mp_score_limit",      100);
        jkGuiNetHost_timeLimit    = stdJSON_GetInt(fpath, "mp_time_limit",       30);
        jkGuiNetHost_tickRate     = stdJSON_GetInt(fpath, "mp_tick_rate",        180);

        jkSession_pendingMpHosting = stdJSON_GetBool(fpath, "mp_was_hosting", 0);
    }

    // Pull the saved position data (if any) into the pending-teleport state.
    // It'll be consumed by jkSession_ApplyPendingPosition after the first
    // level load completes -- if the loaded map matches mapJkl.
    jkSession_bPendingPosition = 0;
    if (stdJSON_GetBool(fpath, "has_position", 0))
    {
        jkSession_pendingSectorIdx = stdJSON_GetInt(fpath, "sector_idx", -1);
        jkSession_pendingPos.x     = stdJSON_GetFloat(fpath, "pos_x", 0.0);
        jkSession_pendingPos.y     = stdJSON_GetFloat(fpath, "pos_y", 0.0);
        jkSession_pendingPos.z     = stdJSON_GetFloat(fpath, "pos_z", 0.0);
        jkSession_pendingLookOrient.rvec.x = stdJSON_GetFloat(fpath, "look_rvec_x", 1.0);
        jkSession_pendingLookOrient.rvec.y = stdJSON_GetFloat(fpath, "look_rvec_y", 0.0);
        jkSession_pendingLookOrient.rvec.z = stdJSON_GetFloat(fpath, "look_rvec_z", 0.0);
        jkSession_pendingLookOrient.lvec.x = stdJSON_GetFloat(fpath, "look_lvec_x", 0.0);
        jkSession_pendingLookOrient.lvec.y = stdJSON_GetFloat(fpath, "look_lvec_y", 1.0);
        jkSession_pendingLookOrient.lvec.z = stdJSON_GetFloat(fpath, "look_lvec_z", 0.0);
        jkSession_pendingLookOrient.uvec.x = stdJSON_GetFloat(fpath, "look_uvec_x", 0.0);
        jkSession_pendingLookOrient.uvec.y = stdJSON_GetFloat(fpath, "look_uvec_y", 0.0);
        jkSession_pendingLookOrient.uvec.z = stdJSON_GetFloat(fpath, "look_uvec_z", 1.0);
        jkSession_pendingLookOrient.scale.x = 0.0;
        jkSession_pendingLookOrient.scale.y = 0.0;
        jkSession_pendingLookOrient.scale.z = 0.0;
        jkSession_pendingEyePYR.x  = stdJSON_GetFloat(fpath, "eye_pyr_x", 0.0);
        jkSession_pendingEyePYR.y  = stdJSON_GetFloat(fpath, "eye_pyr_y", 0.0);
        jkSession_pendingEyePYR.z  = stdJSON_GetFloat(fpath, "eye_pyr_z", 0.0);
        stdString_SafeStrCopy(jkSession_pendingMapJkl, mapJkl, sizeof(jkSession_pendingMapJkl));
        jkSession_bPendingPosition = 1;
    }

    jkSession_bResumed    = 1;
    jkSession_currentMode = mode;
    return 1;
}

void jkSession_ApplyPendingPosition(void)
{
    if (!jkSession_bPendingPosition) return;

    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pLocal || !pWorld || !pWorld->aSectors) {
        jkSession_bPendingPosition = 0;
        return;
    }

    // Only teleport if the currently-loaded map matches the one the position
    // was captured in. Prevents wild coordinates on a mismatched world.
    if (jkSession_pendingMapJkl[0] && jkMain_aLevelJklFname[0]
        && strcmp(jkSession_pendingMapJkl, jkMain_aLevelJklFname) != 0) {
        jkSession_bPendingPosition = 0;
        return;
    }

    // Auto-detect the sector from the saved position rather than trusting the
    // stored index; the index is only a fallback, and only if that sector
    // ACTUALLY contains the saved point -- otherwise the position is in the
    // void and blindly using the stale sector would drop the player into
    // empty space. Keeps resume robust when geometry was edited between
    // sessions.
    SithSector* pSector = sithSector_FindSectorAtPos(pWorld, &jkSession_pendingPos);
    if (!pSector
        && jkSession_pendingSectorIdx >= 0
        && jkSession_pendingSectorIdx < (int)pWorld->numSectors) {
        SithSector* stored = &pWorld->aSectors[jkSession_pendingSectorIdx];
        if (sithIntersect_IsSphereInSector(&jkSession_pendingPos, 0.0, stored))
            pSector = stored;
    }
    if (!pSector) {
        stdPlatform_Printf("jkSession: saved position is in the void (no sector) - using default spawn\n");
        jkSession_bPendingPosition = 0;
        return;
    }

    // The engine's canonical teleport sequence (sithCogFunctionThing_TeleportThing).
    if (pLocal->attach_flags)
        sithThing_DetachThing(pLocal);
    rdMatrix_Copy34(&pLocal->orient, &jkSession_pendingLookOrient);
    rdVector_Copy3(&pLocal->position, &jkSession_pendingPos);
    sithThing_SetSector(pLocal, pSector, 0);
    if (pLocal->moveType == SITH_MT_PHYSICS && (pLocal->physicsParams.flags & SITH_PF_FLOORSTICK))
        sithPhysics_FindFloor(pLocal, 1);
    pLocal->actorParams.headPYR = jkSession_pendingEyePYR;
    sithCamera_Update(sithCamera_g_pCurCamera);

    stdPlatform_Printf("jkSession: resumed player at (%f, %f, %f)\n",
                       jkSession_pendingPos.x, jkSession_pendingPos.y, jkSession_pendingPos.z);

    // One-shot -- subsequent level loads during the same session get normal spawn.
    jkSession_bPendingPosition = 0;
}

void jkSession_ConfigureBoot(int bootMode, int bMultiplayer,
                             const char* pRomEpisode, int bRestorePosition,
                             int bSkipIntroVideo)
{
    jkSession_bootMode = bootMode;
    jkSession_bootMp = bMultiplayer;
    jkSession_bSkipIntroVideo = bSkipIntroVideo;
    memset(jkSession_romEpisode, 0, sizeof(jkSession_romEpisode));
    if (pRomEpisode)
        stdString_SafeStrCopy(jkSession_romEpisode, pRomEpisode, sizeof(jkSession_romEpisode));
    jkSession_bRestorePosition = bRestorePosition;

    // Fresh boot: clear one-shot state from any previous engine run.
    jkSession_bResumed = 0;
    jkSession_pendingMpHosting = 0;
    jkSession_bPendingPosition = 0;
    jkSession_currentMode = SESSION_MODE_NONE;
    memset(jkSession_resumeShortName, 0, sizeof(jkSession_resumeShortName));
}

void jkSession_ArmBoot(void)
{
    if (jkSession_bootMode == JKSESSION_BOOT_MENU)
        return;

    int resumed = 0;
    if (jkSession_bootMode == JKSESSION_BOOT_RESUME)
    {
        resumed = jkSession_LoadAndApply(jkSession_romEpisode);
        if (resumed)
            stdPlatform_Printf("jkSession: resuming last session (episode '%s', map '%s')\n",
                               Main_strEpisode, Main_strMap);
    }

    if (!resumed)
    {
        // Direct boot: autostart the ROM's episode from the top. SP leaves
        // the map empty so Main_StartupDedicated starts the episode via its
        // own new-game path (jkMain_LoadFile); MP leaves it empty for
        // jkMain_loadFile2 to resolve to the episode's first entry.
        Main_bAutostart   = 1;
        Main_bAutostartSp = jkSession_bootMp ? 0 : 1;
        stdString_SafeStrCopy(Main_strEpisode, jkSession_romEpisode, sizeof(Main_strEpisode));
        Main_strMap[0] = 0;
        jkSession_currentMode = jkSession_bootMp ? SESSION_MODE_MP : SESSION_MODE_SP;
        if (jkSession_bootMp)
            jkSession_pendingMpHosting = 1;
        stdPlatform_Printf("jkSession: direct boot into episode '%s' (%s)\n",
                           Main_strEpisode, jkSession_bootMp ? "multiplayer" : "singleplayer");
    }

    if (!jkSession_bRestorePosition)
        jkSession_bPendingPosition = 0; // resume the map, use its default spawn
}
