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
    int bAll = (pWorld->numThings <= JKCF_DUMP_ALL_LIMIT);
    int i;

    jkCogFactory_Printf("things %d/%d (%s)", pWorld->numThings, pWorld->numThingsLoaded,
                        bAll ? "all" : "players only");

    for (i = 0; i < pWorld->numThings; i++)
    {
        SithThing* pThing = &pWorld->aThings[i];

        if (pThing->type == SITH_THING_FREE)
            continue;
        if (!bAll && pThing->type != SITH_THING_PLAYER)
            continue;

        jkCogFactory_Printf("thing %d type=%d tpl='%s' sector=%d pos=(%.4f/%.4f/%.4f) flags=0x%x",
                            i,
                            pThing->type,
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
