#include "jkSession.h"

#include "General/stdJSON.h"
#include "General/stdString.h"
#include "General/stdFnames.h"
#include "stdPlatform.h"
#include "Main/Main.h"
#include "Main/jkRes.h"
#include "Main/jkMain.h"
#include "Main/jkEpisode.h"
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
#include "Gui/jkGUITitle.h"
#include "Dss/sithGamesave.h"
#include "General/stdConffile.h"
#include "jk.h"

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
static int  jkSession_directFilter = JKSESSION_DIRECT_ALL;
static char jkSession_romEpisode[32] = {0};
// Set when a DIRECT boot was armed: the SP/MP mode is decided later by
// jkSession_ResolveAutoBootMode from the episode's own TYPE.
static int  jkSession_bAutoModePending = 0;

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

// "_JKSESSION_<STEM>.jks" -- the per-episode full-state session save
// (devdocs/08). The stem comes from a GOB filename (or the record's episode
// field), so sanitize: uppercase, keep only [A-Z0-9_-], stop at the extension.
static void jkSession_SessionSaveFname(char* pOut, int outSize, const char* pEpisode)
{
    char stem[32];
    int j = 0;
    for (const char* p = pEpisode; *p && *p != '.' && j < (int)sizeof(stem) - 1; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            stem[j++] = c;
    }
    stem[j] = 0;
    stdString_snprintf(pOut, outSize, "_JKSESSION_%s.jks", j ? stem : "EPISODE");
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

    // A record without a map can never be resumed -- and at direct-boot time
    // the loader-hook save fires while the level-name global is still empty
    // (before first-level resolution), which would clobber a good record with
    // a useless one.
    if (!jkRes_episodeGobName[0] || !jkMain_aLevelJklFname[0])
        return;

    // The map to record: prefer the live world's own name. The level-name
    // global is a scratch buffer for the gui state machine -- the savegame
    // flows (jkMain_sub_4034D0, gameMode 1) park the save's FILENAME in it,
    // which must never become a record's map_jkl.
    const char* pMapJkl = (sithWorld_g_pCurrentWorld && sithWorld_g_pCurrentWorld->map_jkl_fname[0])
        ? sithWorld_g_pCurrentWorld->map_jkl_fname
        : jkMain_aLevelJklFname;

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
            && !__strcmpi(oldMap, pMapJkl))
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
    stdJSON_SetString(fpath, "map_jkl",           (char*)pMapJkl);
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

    // Full-state session save (devdocs/08): at every point that just captured
    // a valid SP pose, also write the engine-native savegame the RESUME boot
    // restores from -- same world-state snapshot as a quicksave. MP stays
    // pose-only (it has no savegame system), and sithGamesave_Save needs a
    // live player, which bPlayerValid guarantees (the boot-time SaveCurrent
    // no-ops never reach here). Skipped when the engine already has a
    // save/load in flight so that operation is never clobbered.
    if (bPlayerValid && mode == SESSION_MODE_SP && !sithNet_isMulti
        && sithGamesave_state == SITH_GS_NONE)
    {
        char saveFname[64];
        char16_t saveName[256];
        jkSession_SessionSaveFname(saveFname, sizeof(saveFname), jkRes_episodeGobName);
        // Same "<level>~<label>" shape as the quicksave: the Load Game list
        // only shows entries containing '~' and displays the label part.
        jk_snwprintf(saveName, 256, u"%s~%s",
                     jkGuiTitle_quicksave_related_func1(&jkCog_strings, pWorld->map_jkl_fname),
                     u"Auto-Resume");
        // Save only arms SITH_GS_SAVE; every write point here is past the
        // last sithUpdate tick, so flush it now (the save menu's own
        // Save+Process precedent).
        if (sithGamesave_Save(saveFname, 1, 0, saveName))
        {
            sithGamesave_Process();
            stdPlatform_Printf("jkSession: session save written (%s)\n", saveFname);
        }
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

void jkSession_ConfigureBoot(int bootMode, int directFilter,
                             const char* pRomEpisode)
{
    jkSession_bootMode = bootMode;
    jkSession_directFilter = directFilter;
    // Every mode except the stock INTRO flow skips the pre-title movie --
    // including the autostart modes' fallback-to-menu paths.
    jkSession_bSkipIntroVideo = (bootMode != JKSESSION_BOOT_INTRO);
    memset(jkSession_romEpisode, 0, sizeof(jkSession_romEpisode));
    if (pRomEpisode)
        stdString_SafeStrCopy(jkSession_romEpisode, pRomEpisode, sizeof(jkSession_romEpisode));

    // Fresh boot: clear one-shot state from any previous engine run.
    jkSession_bResumed = 0;
    jkSession_pendingMpHosting = 0;
    jkSession_bPendingPosition = 0;
    jkSession_bAutoModePending = 0;
    jkSession_currentMode = SESSION_MODE_NONE;
    memset(jkSession_resumeShortName, 0, sizeof(jkSession_resumeShortName));
}

void jkSession_ArmBoot(void)
{
    if (jkSession_bootMode == JKSESSION_BOOT_INTRO || jkSession_bootMode == JKSESSION_BOOT_MENU)
        return; // title flow (with or without the intro movie); no autostart

    int resumed = 0;
    if (jkSession_bootMode == JKSESSION_BOOT_RESUME || jkSession_bootMode == JKSESSION_BOOT_LEVEL)
    {
        resumed = jkSession_LoadAndApply(jkSession_romEpisode);
        if (resumed && jkSession_bootMode == JKSESSION_BOOT_LEVEL)
        {
            // Continue on the last level, but from its normal start point.
            jkSession_bPendingPosition = 0;
            stdPlatform_Printf("jkSession: continuing on last level (episode '%s', map '%s', default spawn)\n",
                               Main_strEpisode, Main_strMap);
        }
        else if (resumed)
        {
            stdPlatform_Printf("jkSession: resuming last session (episode '%s', map '%s')\n",
                               Main_strEpisode, Main_strMap);
        }
    }

    if (!resumed)
    {
        // Direct boot: autostart the ROM's episode from the top with an empty
        // map (the loaders resolve the episode's first level entry). The
        // SP-vs-MP mode is NOT fixed here: jkSession_ResolveAutoBootMode
        // probes the episode's own TYPE once the resource system is up and
        // sets it (singleplayer episodes boot singleplayer, any multiplayer
        // type hosts a local session).
        Main_bAutostart   = 1;
        Main_bAutostartSp = 1; // placeholder until the type probe runs
        stdString_SafeStrCopy(Main_strEpisode, jkSession_romEpisode, sizeof(Main_strEpisode));
        Main_strMap[0] = 0;
        jkSession_bAutoModePending = 1;
        stdPlatform_Printf("jkSession: direct boot armed for episode '%s' (mode from episode type)\n",
                           Main_strEpisode);
    }
}

int jkSession_ResolveAutoBootMode(void)
{
    if (!jkSession_bAutoModePending)
        return 1; // resume boot (mode came from the record) or no direct boot armed

    jkSession_bAutoModePending = 0;

    // Probe the episode's TYPE bitmask from its episode.jk. The level loaders
    // re-mount and re-parse right after, so this costs one extra parse and
    // leaves no state they don't rebuild anyway.
    int type = 0;
    jkRes_LoadGob(Main_strEpisode);
    if (jkEpisode_Load(&jkEpisode_mLoad))
        type = (int)jkEpisode_mLoad.type;
    else
        stdPlatform_Printf("jkSession: could not read episode.jk for '%s'; assuming singleplayer\n",
                           Main_strEpisode);

    int bSp = !type || (type & JK_EPISODE_SINGLEPLAYER) != 0;

    if (jkSession_directFilter == JKSESSION_DIRECT_SP_ONLY && !bSp)
    {
        stdPlatform_Printf("jkSession: episode '%s' is multiplayer (TYPE 0x%x) and direct boot is limited to singleplayer - booting to the menu\n",
                           Main_strEpisode, type);
        return 0;
    }
    if (jkSession_directFilter == JKSESSION_DIRECT_MP_ONLY && bSp)
    {
        stdPlatform_Printf("jkSession: episode '%s' is singleplayer (TYPE 0x%x) and direct boot is limited to multiplayer - booting to the menu\n",
                           Main_strEpisode, type);
        return 0;
    }

    Main_bAutostartSp = bSp ? 1 : 0;
    jkSession_currentMode = bSp ? SESSION_MODE_SP : SESSION_MODE_MP;
    jkSession_pendingMpHosting = bSp ? 0 : 1;
    stdPlatform_Printf("jkSession: direct boot into episode '%s' (TYPE 0x%x -> %s)\n",
                       Main_strEpisode, type, bSp ? "singleplayer" : "multiplayer (local host)");
    return 1;
}

int jkSession_StartBootSave(void)
{
    if (!jkSession_bResumed
        || jkSession_currentMode != SESSION_MODE_SP
        || jkSession_bootMode != JKSESSION_BOOT_RESUME)
        return 0;

    char saveFname[64];
    char fpath[128];
    jkSession_SessionSaveFname(saveFname, sizeof(saveFname), Main_strEpisode);
    sithGamesave_GetProfilePath(fpath, sizeof(fpath), saveFname);

    if (!stdConffile_OpenReadBytesBypass(fpath))
    {
        stdPlatform_Printf("jkSession: no session save (%s) - pose resume\n", fpath);
        return 0;
    }
    static sithGamesave_Header header; // ~1.7 KB; keep it off the boot stack
    int bRead = stdConffile_Read(&header, sizeof(sithGamesave_Header));
    stdConffile_Close();

    if (!bRead
        || (header.version != 6 && !(Main_bMotsCompat && header.version == 0x7D6)))
    {
        stdPlatform_Printf("jkSession: session save %s is unreadable (version %d) - pose resume\n",
                           fpath, bRead ? header.version : -1);
        return 0;
    }

    // The pose record is ground truth for WHERE the user last played. A
    // session save for some other map is stale -- it predates a later exit
    // that had no valid pose to co-write it (e.g. a fresh playthrough that
    // ended in a death on an earlier level) -- so don't yank the player back.
    if (__strcmpi(header.jklName, Main_strMap))
    {
        stdPlatform_Printf("jkSession: session save is for map '%s' but the last session ended on '%s' - pose resume\n",
                           header.jklName, Main_strMap);
        return 0;
    }

    // Queue the engine's own "load a save with no world" flow -- identical to
    // the Load Game menu's no-world branch: JK_GAMEMODE_UNK mounts the
    // header's episode GOB, then gameMode 1 has jkMain_GameplayShow run
    // sithGamesave_Restore, which loads the map and replays the world state.
    // The save's own position is the resume position: the pose teleport must
    // not fire on top of it.
    jkSession_bPendingPosition = 0;
    stdPlatform_Printf("jkSession: full-state resume from %s (episode '%s', map '%s')\n",
                       fpath, header.episodeName, header.jklName);
    return jkMain_sub_4034D0(header.episodeName, saveFname, header.jklName, header.saveName);
}
