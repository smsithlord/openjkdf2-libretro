#ifndef _JKCOGFACTORY_H
#define _JKCOGFACTORY_H

#include "types.h"

/*
 * COG Factory debug mode.
 *
 * A gated set of debug signals for the external content-generation tooling
 * (tools/cogfactory). The engine knows the factory exists; it never depends on
 * it. Everything here is off unless the frontend turns it on, and nothing
 * outside the gate changes behaviour.
 *
 * The signals exist because generating content fails SILENTLY in this engine
 * in several places, each of which leaves a level that loads, renders, and
 * does nothing, with a clean log:
 *
 *   - COG Print()/PrintInt() go to jkDev, which writes with raw printf and so
 *     never reaches the libretro log. Without this, an automated test cannot
 *     assert on anything a COG does.
 *   - A COG that fails to parse, or that doesn't fit the level's declared
 *     `World scripts N` pool, is skipped without failing the level load.
 *   - Which JKL section the engine rejected only goes to the console channel.
 *
 * Output is prefixed "[CF] " so a test harness can match it unambiguously, and
 * routed through stdPlatform_Printf, which is the chokepoint the libretro core
 * mirrors to the frontend log (see devdocs/07 and devdocs/14).
 */

#ifdef LIBRETRO_BUILD

extern int jkCogFactory_bEnabled;

#define JKCF_ON() (jkCogFactory_bEnabled != 0)

void jkCogFactory_SetEnabled(int bEnabled);

/* Emits one "[CF] ..." line. Callers should check JKCF_ON() first when
   building the arguments is not free. */
void jkCogFactory_Printf(const char* fmt, ...);

/* Level inventory: the index tables a generator must diff its own output
   against. Called once, after a world loads successfully. */
void jkCogFactory_DumpWorld(SithWorld* pWorld);

/* Why a cog did not load. bPoolFull distinguishes "the level's `World scripts`
   capacity is exhausted" (a JKL authoring bug) from "the script failed to
   parse" (a COG authoring bug) -- both are silent in the stock engine. */
void jkCogFactory_CogLoadFailed(const char* pName, int bPoolFull);

#else

#define JKCF_ON() (0)
#define jkCogFactory_SetEnabled(x)      do {} while (0)
#define jkCogFactory_DumpWorld(x)       do {} while (0)
#define jkCogFactory_CogLoadFailed(x,y) do {} while (0)

#endif /* LIBRETRO_BUILD */

#endif /* _JKCOGFACTORY_H */
