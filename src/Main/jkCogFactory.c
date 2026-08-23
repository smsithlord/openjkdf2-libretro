#include "jkCogFactory.h"

#ifdef LIBRETRO_BUILD

#include <stdarg.h>
#include <stdio.h>

#include "engine_config.h"
#include "stdPlatform.h"
#include "World/sithWorld.h"
#include "World/sithSector.h"
#include "World/sithSurface.h"
#include "World/sithThing.h"
#include "Gameplay/sithPlayer.h"
#include "Engine/sithPhysics.h"
#include "Engine/sithCamera.h"
#include "Engine/rdCamera.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "Cog/sithCog.h"

int jkCogFactory_bEnabled = 0;

/* A stock level has thousands of surfaces/things; a generated test level has
 * tens. Dump everything when the level is small enough to be ours, and only
 * the interesting rows otherwise, so turning the gate on against stock content
 * stays usable. */
#define JKCF_DUMP_ALL_LIMIT 256

void jkCogFactory_SetEnabled(int bEnabled)
{
    if (jkCogFactory_bEnabled == (bEnabled != 0))
        return;
    jkCogFactory_bEnabled = (bEnabled != 0);
    stdPlatform_Printf("[CF] cogfactory debug mode %s\n",
                       jkCogFactory_bEnabled ? "ENABLED" : "disabled");
}

void jkCogFactory_Printf(const char* fmt, ...)
{
    char tmp[512];
    va_list args;

    if (!jkCogFactory_bEnabled)
        return;

    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    stdPlatform_Printf("[CF] %s\n", tmp);
}

void jkCogFactory_CogLoadFailed(const char* pName, int bPoolFull)
{
    if (!jkCogFactory_bEnabled)
        return;

    if (bPoolFull)
    {
        jkCogFactory_Printf("cog FAILED '%s': the level's `World scripts` pool is full "
                            "(%d/%d used) -- raise the count in SECTION: COGSCRIPTS",
                            pName ? pName : "(null)",
                            sithWorld_g_pLastLoadedWorld ? sithWorld_g_pLastLoadedWorld->numCogScripts : -1,
                            sithWorld_g_pLastLoadedWorld ? sithWorld_g_pLastLoadedWorld->sizeCogScripts : -1);
    }
    else
    {
        jkCogFactory_Printf("cog FAILED '%s': script did not parse "
                            "(see the PARSER line above for the syntax error)",
                            pName ? pName : "(null)");
    }
}

int jkCogFactory_Warp(const char* pSpec)
{
    SithThing* pLocal;
    SithWorld* pWorld;
    SithSector* pSector;
    rdVector3 pos;
    float yaw = 0.0f;
    int n;

    if (!jkCogFactory_bEnabled || !pSpec || !pSpec[0])
        return 0;

    n = sscanf(pSpec, "%f %f %f %f", &pos.x, &pos.y, &pos.z, &yaw);
    if (n < 3)
    {
        jkCogFactory_Printf("warp: cannot parse '%s' (want \"x y z\" or \"x y z yaw\")", pSpec);
        return 0;
    }

    pWorld = sithWorld_g_pCurrentWorld;
    pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!pWorld || !pLocal)
    {
        jkCogFactory_Printf("warp: no world or no local player yet");
        return 0;
    }

    /* Never drop the player into the void -- the same guard jkSession's
     * position restore uses (jkSession.c:464). */
    pSector = sithSector_FindSectorAtPos(pWorld, &pos);
    if (!pSector)
    {
        jkCogFactory_Printf("warp: (%.4f %.4f %.4f) is in the void (no sector); refusing",
                            (double)pos.x, (double)pos.y, (double)pos.z);
        return 0;
    }

    /* The engine's canonical teleport sequence, as used by
     * sithCogFunctionThing_TeleportThing and jkSession's resume. */
    if (pLocal->attach_flags)
        sithThing_DetachThing(pLocal);
    if (n >= 4)
    {
        rdVector3 pyr;
        pyr.x = 0.0f;
        pyr.y = yaw;
        pyr.z = 0.0f;
        rdMatrix_BuildRotate34(&pLocal->orient, &pyr);
    }
    rdVector_Copy3(&pLocal->position, &pos);
    sithThing_SetSector(pLocal, pSector, 0);
    if (pLocal->moveType == SITH_MT_PHYSICS
        && (pLocal->physicsParams.flags & SITH_PF_FLOORSTICK))
    {
        /* Let gravity settle them onto whatever is below, so a caller can aim
         * roughly and still land on the floor. */
        sithPhysics_FindFloor(pLocal, 1);
    }
    sithCamera_Update(sithCamera_g_pCurCamera);

    jkCogFactory_Printf("warp: player -> (%.4f %.4f %.4f) sector %d",
                        (double)pos.x, (double)pos.y, (double)pos.z, (int)pSector->id);
    return 1;
}

/* Free-camera state. Pose is held here and re-applied every frame; the engine
 * would otherwise recompute the camera from its focus thing. */
static int       s_cam_active;
static rdVector3 s_cam_pos;
static rdVector3 s_cam_pyr;

int jkCogFactory_SetCam(const char* pSpec)
{
    int n;

    if (!jkCogFactory_bEnabled)
        return 0;

    /* Empty string releases the camera back to the engine. */
    if (!pSpec || !pSpec[0] || pSpec[0] == '-')
    {
        if (s_cam_active)
            jkCogFactory_Printf("cam: released");
        s_cam_active = 0;
        return 1;
    }

    n = sscanf(pSpec, "%f %f %f %f %f %f",
               &s_cam_pos.x, &s_cam_pos.y, &s_cam_pos.z,
               &s_cam_pyr.x, &s_cam_pyr.y, &s_cam_pyr.z);
    if (n < 3)
    {
        jkCogFactory_Printf("cam: cannot parse '%s' (want \"x y z [pitch yaw roll]\")", pSpec);
        return 0;
    }
    if (n < 6)
    {
        if (n < 4) s_cam_pyr.x = 0.0f;
        if (n < 5) s_cam_pyr.y = 0.0f;
        s_cam_pyr.z = 0.0f;
    }

    s_cam_active = 1;
    jkCogFactory_Printf("cam: pinned at (%.4f %.4f %.4f) pyr (%.2f %.2f %.2f)",
                        (double)s_cam_pos.x, (double)s_cam_pos.y, (double)s_cam_pos.z,
                        (double)s_cam_pyr.x, (double)s_cam_pyr.y, (double)s_cam_pyr.z);
    return 1;
}

void jkCogFactory_CameraOverride(SithCamera* pCamera)
{
    SithSector* pSector;

    if (!jkCogFactory_bEnabled || !s_cam_active || !pCamera)
        return;

    /* pCamera->orient IS the view matrix; its .scale member is the position. */
    rdMatrix_BuildRotate34(&pCamera->orient, &s_cam_pyr);
    pCamera->orient.scale = s_cam_pos;

    /* Resolve the sector EVERY frame from the pinned position. The renderer
     * culls and lights from cam->sector, so keeping the old one (the player's)
     * renders the world as seen from the player's room -- typically a black or
     * half-clipped frame. Keep the previous sector if the pose is outside the
     * world rather than nulling it, which would be worse. */
    pSector = sithSector_FindSectorAtPos(sithWorld_g_pCurrentWorld, &s_cam_pos);
    if (pSector)
        pCamera->sector = pSector;
}

static void jkCogFactory_DumpSurfaces(SithWorld* pWorld)
{
    int bAll = (pWorld->numSurfaces <= JKCF_DUMP_ALL_LIMIT);
    int shown = 0;
    int i;

    jkCogFactory_Printf("surfaces %d (%s)", pWorld->numSurfaces,
                        bAll ? "all" : "cog-linked only");

    for (i = 0; i < pWorld->numSurfaces; i++)
    {
        SithSurface* pSurf = &pWorld->surfaces[i];
        int bLinked = (pSurf->flags & SITH_SURFACE_COG_LINKED) != 0;

        if (!bAll && !bLinked)
            continue;
        if (shown++ >= JKCF_DUMP_ALL_LIMIT)
        {
            jkCogFactory_Printf("surfaces ... (truncated)");
            break;
        }

        /* flags is the field a generator most often gets wrong: a walkable
         * floor needs FLOOR|HAS_COLLISION (0x5), and COG_LINKED (0x2) is set
         * by the cog link, not by the JKL. */
        jkCogFactory_Printf("surface %d flags=0x%x sector=%d adjoin=%d nverts=%d%s%s%s",
                            i,
                            pSurf->flags,
                            pSurf->pSector ? (int)pSurf->pSector->id : -1,
                            pSurf->pAdjoin ? 1 : -1,
                            pSurf->surfaceInfo.face.numVertices,
                            (pSurf->flags & SITH_SURFACE_FLOOR) ? " FLOOR" : "",
                            (pSurf->flags & SITH_SURFACE_HAS_COLLISION) ? " COLLIDE" : "",
                            bLinked ? " COGLINKED" : "");
    }
}

static void jkCogFactory_DumpThings(SithWorld* pWorld)
{
    /* numThings is a HIGH-WATER INDEX, not a count: sithThing.c initialises it
     * to -1 and raises it to the largest live slot. numThingsLoaded is the
     * allocated capacity (the JKL's `World things N`). Iterate the capacity and
     * skip free slots -- iterating numThings misses the last thing, and misses
     * everything in a level whose only thing is index 0. */
    int bAll = (pWorld->numThingsLoaded <= JKCF_DUMP_ALL_LIMIT);
    int i;

    jkCogFactory_Printf("things high_index=%d capacity=%d (%s)",
                        pWorld->numThings, pWorld->numThingsLoaded,
                        bAll ? "all" : "players only");

    for (i = 0; i < pWorld->numThingsLoaded; i++)
    {
        SithThing* pThing = &pWorld->aThings[i];

        if (pThing->type == SITH_THING_FREE)
            continue;
        if (!bAll && pThing->type != SITH_THING_PLAYER)
            continue;

        /* moveType and the loaded frame count are the two things MoveToFrame
         * silently requires (sithCogFunctionThing.c:369): a path thing with
         * fewer loaded frames than the target index simply does not move, with
         * no diagnostic anywhere. Worth printing for every thing. */
        jkCogFactory_Printf("thing %d type=%d move=%d frames=%d tpl='%s' sector=%d "
                            "pos=(%.4f/%.4f/%.4f) flags=0x%x",
                            i,
                            pThing->type,
                            pThing->moveType,
                            pThing->moveType == SITH_MT_PATH
                                ? pThing->trackParams.loadedFrames : 0,
#ifdef SITH_DEBUG_STRUCT_NAMES
                            pThing->pTemplate ? pThing->pTemplate->aName : "?",
#else
                            "?",
#endif
                            pThing->sector ? (int)pThing->sector->id : -1,
                            (double)pThing->position.x,
                            (double)pThing->position.y,
                            (double)pThing->position.z,
                            pThing->flags);
    }
}

static void jkCogFactory_DumpCogs(SithWorld* pWorld)
{
    int i, j;

    jkCogFactory_Printf("cogs %d (scripts %d/%d)", pWorld->numCogs,
                        pWorld->numCogScripts, pWorld->sizeCogScripts);

    for (i = 0; i < pWorld->numCogs; i++)
    {
        sithCog* pCog = &pWorld->aCogs[i];
        SithCogScript* pScript = pCog->pScript;

        if (!pScript)
            continue;

        jkCogFactory_Printf("cog %d '%s' handlers=%d symrefs=%d flags=0x%x",
                            i,
#ifdef SITH_DEBUG_STRUCT_NAMES
                            pScript->aName,
#else
                            "?",
#endif
                            pScript->numHandlers,
                            pScript->numSymbolRefs,
                            (unsigned)pCog->flags);

        /* The JKL's COGS line maps positionally onto the non-`local` symbol
         * declarations. Getting that mapping wrong is the single most likely
         * wiring bug in a generated level, and it is otherwise invisible.
         *
         * Two different strings matter here and they are easy to confuse:
         * pScript->aSymRefs[].value is the SCRIPT's in-file default (shared by
         * every instance of that script), while pCog->aInitArgs holds THIS
         * instance's arguments from the JKL -- 32 bytes per ref, and the
         * cursor advances only for non-`local` refs, exactly as sithCog_Open
         * consumes them. We run before sithCog_Open, so aInitArgs is still
         * alive and nothing is linked yet; print the JKL's own words. */
        {
            /* Same conditional the linker uses (sithCog_Open): the init
             * strings are a heap block on RETRO builds and an inline 4 KB
             * array everywhere else. */
#ifdef COG_HEAP_INIT_ARGS
            const char* pArg = pCog->aInitArgs; /* may be NULL for an arg-less cog */
#else
            const char* pArg = pCog->field_4BC;
#endif

            for (j = 0; j < (int)pScript->numSymbolRefs; j++)
            {
                SithCogSymbolRef* pRef = &pScript->aSymRefs[j];
                int bLocal = (pRef->flags & 1) != 0;
                const char* pBound = bLocal ? NULL : pArg;

                jkCogFactory_Printf("cog %d   ref %d type=%d linkid=%d mask=0x%x%s jkl='%s' default='%s'",
                                    i, j,
                                    pRef->type,
                                    pRef->linkid,
                                    pRef->mask,
                                    bLocal ? " local" : "",
                                    (pBound && pBound[0]) ? pBound : "-",
                                    pRef->value);

                if (!bLocal && pArg)
                    pArg += 32;
            }
        }
    }
}

void jkCogFactory_DumpWorld(SithWorld* pWorld)
{
    if (!jkCogFactory_bEnabled || !pWorld)
        return;

    jkCogFactory_Printf("---- world '%s' (episode '%s') ----",
                        pWorld->map_jkl_fname, pWorld->episodeName);
    jkCogFactory_Printf("sectors %d vertices %d texverts %d materials %d/%d sounds %d/%d",
                        pWorld->numSectors, pWorld->numVertices, pWorld->numTexVertices,
                        pWorld->numMaterials, pWorld->sizeMaterials,
                        pWorld->numSounds, pWorld->numSoundsLoaded);

    jkCogFactory_DumpSurfaces(pWorld);
    jkCogFactory_DumpThings(pWorld);
    jkCogFactory_DumpCogs(pWorld);

    jkCogFactory_Printf("---- world inventory end ----");
}

#endif /* LIBRETRO_BUILD */
