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

/* Autopilot: walk the player to a place, by INPUT.
 *
 * `warp` teleports, which is exactly what you want for a screenshot and
 * exactly what you do not want for a test about MOVEMENT -- it skips the
 * physics, the collision and the adjoin traversal that are usually the thing
 * under test. Holding `w` for N frames is the alternative and it is worse: it
 * only goes in a straight line, and unless jkCogFactory_SetTimestep is engaged
 * the distance is wall-clock derived and so not reproducible.
 *
 * This drives the same control axes the keyboard drives, so everything
 * downstream -- acceleration, drag, floor stick, slope handling, adjoin
 * crossing -- happens exactly as it does for a human.
 *
 * "x y z [tol]", or "thing <index> [tol]" to chase a thing (drop a marker
 * template at a waypoint and aim at that), or "off". From openjkdf2_cf_goto.
 *
 * It reports, once per event, on the [CF] channel: `arrived`, `stuck`,
 * `voided` (no sector under the player -- which is the failure a human found
 * in p11-terrace by falling through the floor), and `gave up`. Those four
 * lines are the point: a test asserts on them rather than on a frame count.
 *
 * NOT a pathfinder. It steers straight at the target and walks. A wall between
 * the two produces `stuck`, with the position, which is a useful answer. */
int  jkCogFactory_SetGoto(const char* pSpec);

/* Returns 1 and writes *pOut if the autopilot is driving this input function
 * this frame; 0 to let the real device through. Called from sithControl's axis
 * accessors, which is the narrowest possible injection point: the whole
 * movement pipeline below it is untouched and cannot tell the difference. */
int  jkCogFactory_AutopilotAxis(int axisId, flex_t* pOut);

/* Ask the running engine a question, right now, without reloading anything.
 *
 * The level inventory dump (jkCogFactory_DumpWorld) is fired once, at load,
 * which answers "what did the JKL contain" and nothing about what the world
 * has become since. Every other diagnostic in this file is a one-way report on
 * a schedule the engine chooses. When a test stops at a breakpoint and the
 * question is "where is the player actually standing" or "did that sector's
 * light really change", the only options were to add a Print() to a cog and
 * rerun, or to read it out of a screenshot.
 *
 * "world", "player", "time", "thing <n>", "sector <n>", "surface <n>", from
 * the openjkdf2_cf_probe core option. Answers on the [CF] channel like
 * everything else, so `expect` can assert on a probe.
 *
 * NOTE the frontend appends a "#<seq>" the parser ignores. The option channel
 * only fires on CHANGE, so without it, asking the same question twice would
 * silently answer once -- which for an interactive debugging aid is the worst
 * possible failure. */
int jkCogFactory_Probe(const char* pSpec);

/* Fixed timestep: make game time advance by an exact amount per frame, so a
 * test is reproducible in DISTANCE and not merely in order.
 *
 * The engine's clock is wall-clock derived -- sithTime_Advance() is
 * `sithTime_SetFrameTime(stdPlatform_GetTimeMsec() - sithTime_g_clockTime)`
 * (sithTime.c:24) -- so `key hold w 300` and `goto` walk a distance that
 * depends on how fast the machine happened to be. Every settle and timeout
 * budget in the test suite is therefore a guess, and the same script measured
 * 590/584/551 autopilot ticks on three runs of p11-terrace.
 *
 * With this engaged, one frame is exactly one step of game time regardless of
 * how long it took to compute, so `--pace turbo` and `--pace realtime` produce
 * IDENTICAL results -- same frame counts, same final position, same
 * screenshots -- while differing in wall-clock by a large factor.
 *
 * Accepted spellings, from the openjkdf2_cf_timestep core option:
 *
 *     "41.667"     milliseconds per frame
 *     "41.667ms"   the same, spelled out
 *     "24fps"      1/24 s per frame; "24 fps" and "24hz" also work
 *     "off"        release it; "" and "-" do the same
 *
 * Bounded by the engine's own delta clamps (SITHTIME_MINDELTA_US ..
 * SITHTIME_MAXDELTA_US, engine_config.h), with an extra 1 ms floor because
 * sithTime_g_frameTime is integer milliseconds and would otherwise floor to
 * zero. Out-of-range steps are REFUSED with a reason rather than clamped, so a
 * test can never silently run at a step other than the one it asked for.
 *
 * Off unless set, and unset unless the gate is on: normal play never reaches
 * any of this. */
int jkCogFactory_SetTimestep(const char* pSpec);

/* The engaged step in SECONDS, or 0.0 when not engaged (which is the answer
 * whenever the gate is off). Two callers, and both are required:
 *
 *   - sithTime_Advance() substitutes it for the wall-clock delta. Note it must
 *     bypass sithTime_SetFrameTime entirely: under MICROSECOND_TIME that
 *     function recomputes sithTime_g_frameTimeFlex -- the delta the physics
 *     actually integrates -- from Linux_TimeUs() and ignores its own argument,
 *     so passing a fixed millisecond delta fixes the millisecond clock and
 *     leaves the physics exactly as non-deterministic as before.
 *
 *   - retro_run's virtual clock advances by this instead of by real dt. That
 *     is not redundant: jkMain's tick gate is `GetTimeMsec() > lastTick +
 *     TICKRATE_MS` (jkMain.c:239/364/742), so whether a frame ticks the
 *     simulation AT ALL is a wall-clock decision. Under turbo a retro_run can
 *     take under a millisecond and skip the tick, which no amount of fixing
 *     the delta would repair. */
double jkCogFactory_FixedStepSecs(void);

#else

#define JKCF_ON() (0)
#define jkCogFactory_SetEnabled(x)      do {} while (0)
#define jkCogFactory_DumpWorld(x)       do {} while (0)
#define jkCogFactory_CogLoadFailed(x,y) do {} while (0)
#define jkCogFactory_FixedStepSecs()    (0.0)

#endif /* LIBRETRO_BUILD */

#endif /* _JKCOGFACTORY_H */
