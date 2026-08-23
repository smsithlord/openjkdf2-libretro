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

/* Put the local player somewhere, at runtime, from the frontend.
 *
 * Automated tests need to reach a specific spot without walking there: walking
 * is slow, non-deterministic (game time is wall-clock derived) and blocked by
 * whatever the test is trying to exercise. It is also the only way to get a
 * camera vantage point in a small room.
 *
 * "x y z" (optionally "x y z yaw"), parsed from the openjkdf2_cf_warp core
 * option -- a string the frontend can change at any time, so no new ABI is
 * needed. Refuses positions with no sector rather than dropping the player
 * into the void. Returns 0 and logs if it could not warp. */
int jkCogFactory_Warp(const char* pSpec);

/* Free camera: pin the view to an explicit pose without moving the player.
 *
 * Screenshots of generated content are the point. A player-follow camera sits
 * at eye height inside whatever you are testing, so a 0.3-unit pillar fills
 * the frame and a room reads as a grey wall; there is often no position the
 * PLAYER can occupy that shows the thing you built.
 *
 * "x y z pitch yaw roll", from openjkdf2_cf_cam; empty string releases it.
 * The pose is re-applied from sithCamera_Update, AFTER the engine has computed
 * whatever the camera type wanted -- the only ordering that holds. The sector
 * is resolved from the position every time, because the renderer culls and
 * lights from cam->sector and a stale one produces a black or clipped frame
 * (this is what the sister project's fly-cam gets right and is easy to miss).
 */
int  jkCogFactory_SetCam(const char* pSpec);
void jkCogFactory_CameraOverride(SithCamera* pCamera);

#else

#define JKCF_ON() (0)
#define jkCogFactory_SetEnabled(x)      do {} while (0)
#define jkCogFactory_DumpWorld(x)       do {} while (0)
#define jkCogFactory_CogLoadFailed(x,y) do {} while (0)

#endif /* LIBRETRO_BUILD */

#endif /* _JKCOGFACTORY_H */
