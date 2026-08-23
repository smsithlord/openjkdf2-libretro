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
#include "World/sithActor.h"
#include "Engine/sithPhysics.h"
#include "Engine/sithCamera.h"
#include "Engine/rdCamera.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "Cog/sithCog.h"
#include "Gameplay/sithTime.h"
#include "Gameplay/sithInventory.h"
#include "World/sithWeapon.h"
#include "Devices/sithControl.h"
#include "types_enums.h"
#include <math.h>

int jkCogFactory_bEnabled = 0;

/* A stock level has thousands of surfaces/things; a generated test level has
 * hundreds. Dump everything when the level is small enough to be ours, and only
 * the interesting rows otherwise, so turning the gate on against stock content
 * stays usable.
 *
 * This was 256, and 256 was too small twice over. p09-modular-maze generated a
 * 366-surface level -- modest for a generated level, tiny for a stock one --
 * and got ZERO surface lines, because the >limit fallback filtered on
 * SITH_SURFACE_COG_LINKED and this dump runs BEFORE the cog link sets it. Not
 * truncated, no ellipsis: just "surfaces 366 (cog-linked only)" and straight on
 * to things. So the diagnostic died silently at exactly the size where it
 * starts being needed.
 *
 * Both halves are fixed: the limit is large enough for any plausible generated
 * level, and the fallback filter is now a predicate that is actually TRUE of
 * something at dump time. */
#define JKCF_DUMP_ALL_LIMIT 4096

/* Hard cap on printed rows, so a stock level cannot flood the log even when a
 * filter matches broadly. Truncation is always announced. */
#define JKCF_DUMP_ROW_CAP 4096

/* math.h only defines M_PI when _USE_MATH_DEFINES is set, which it is not on
 * every toolchain this builds under. */
#define JKCF_RAD2DEG (57.29577951308232)

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

    /* Empty string, or the bare sentinel "-" that `cam off` sends
     * (tools/harness/cmd.c:834), releases the camera back to the engine.
     *
     * The test MUST be for the whole string being "-", not just its first
     * character: a pose whose x is negative -- "-3.0 -3.3 0.3 ..." -- also
     * starts with '-', and the looser test silently released the camera
     * instead of pinning it. That is invisible in a test run, because
     * "cam: released" is exactly what a correct `cam off` prints, and the
     * screenshot that follows is a valid picture of the wrong viewpoint.
     * Found by p09-modular-maze, whose level is centred on the origin, so
     * every camera west of centre had a negative x and five shots came back
     * byte-identical to the player's own view. */
    if (!pSpec || !pSpec[0] || (pSpec[0] == '-' && !pSpec[1]))
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

    /* lookPos/lookPYR are a SECOND copy of the same pose, written at
     * sithCamera.c:370-371 -- i.e. before this override runs -- and the
     * renderer reads THEM, not orient, for every visibility decision:
     *
     *   sithRender.c:785/1117/1357  the adjoin back-face test
     *       rdMath_DistancePointToPlane(&pCurCamera->lookPos,
     *                                   &adjoinSurface->...face.normal, v20)
     *   sithRender.c:502            the per-sector distance reject
     *   sithRender.c:1797           the surface back-face test
     *
     * Leave them stale and sector traversal is done from the PLAYER's position
     * while the image is drawn from the pinned one. Every adjoin the player is
     * behind is culled as back-facing, so the sector on the far side of it is
     * never visited and renders as a flat black void with hard edges -- which
     * is indistinguishable from a missing surface or a broken adjoin, and is
     * exactly the symptom p09-modular-maze recorded and worked around with
     * `warp` before `cam`. It goes away when the player happens to be in the
     * same sector because then the two positions agree well enough.
     *
     * sithSoundMixer also pans 3D sound off lookPos, so this makes the pinned
     * camera hear from where it looks, too. */
    pCamera->lookPos = s_cam_pos;
    rdMatrix_ExtractAngles34(&pCamera->orient, &pCamera->lookPYR);

    /* Resolve the sector EVERY frame from the pinned position. The renderer
     * culls and lights from cam->sector, so keeping the old one (the player's)
     * renders the world as seen from the player's room -- typically a black or
     * half-clipped frame. Keep the previous sector if the pose is outside the
     * world rather than nulling it, which would be worse. */
    pSector = sithSector_FindSectorAtPos(sithWorld_g_pCurrentWorld, &s_cam_pos);
    if (pSector)
        pCamera->sector = pSector;
}

/* ------------------------------------------------------------------ look */
/* Aim the player at a world point, exactly.
 *
 * The player's VIEW direction is not one value. Body yaw lives in
 * thing->orient; head pitch lives in actorParams.headPYR and is applied on top
 * of it -- sithCogFunctionAI_ThingViewDot (sithCogFunctionAI.c:371-373) is the
 * canonical reader, and it PreRotates a copy of orient by headPYR whenever the
 * thing is an ACTOR or a PLAYER. So `warp x y z yaw` can only set half of it,
 * and the other half was reachable only by feeding synthetic mouse deltas and
 * hoping: a test that wants "look 12 degrees below the horizon at that panel"
 * had to discover the pixels-per-degree of the mouse binding first.
 *
 * This solves for both angles from a target position and applies them, which
 * makes any test about where the player is LOOKING as exact and as cheap as
 * `warp` made tests about where the player is STANDING.
 *
 * Two details are load-bearing, and both were found by having the command
 * report the dot it achieved rather than trusting the maths:
 *
 *   - It aims from the thing POSITION, not from the eye. ThingViewDot's
 *     direction vector is (target - thing->position), and walkplayer's
 *     eyeoffset is (0/0/0.037), so aiming from the rendered eye would produce
 *     a dot that is very close to 1.0 and not equal to it. The crosshair
 *     therefore sits a little above the aimed point; the COG-visible dot is
 *     exactly 1.
 *   - It clears SITH_AF_VIEWCENTRING. If that flag is up, sithControl's
 *     centring branch (sithControl.c:1294-1313) walks head pitch back to zero
 *     at 180 deg/sec on every frame with no pitch input -- so the pose would
 *     be correct for one frame and then silently drift out from under the
 *     test. */

/* The look vector ThingViewDot will use for this thing: orient, pre-rotated by
 * headPYR for actors and players. One function so `look` and `probe player`
 * cannot disagree with the verb or with each other. */
static void jkCogFactory_ViewLook(SithThing* pThing, rdVector3* pOut)
{
    rdMatrix34 m;

    stdPlatform_Memcpy32(&m, &pThing->orient, sizeof(m));
    if (pThing->type == SITH_THING_ACTOR || pThing->type == SITH_THING_PLAYER)
        rdMatrix_PreRotate34(&m, &pThing->actorParams.headPYR);
    *pOut = m.lvec;
    rdVector_Normalize3Acc(pOut);
}

int jkCogFactory_Look(const char* pSpec)
{
    SithThing* pLocal;
    SithThing* pTargetThing = NULL;
    rdVector3 target, dir, pyr, lvec;
    double horiz, yaw, pitch, wanted, dot;
    int idx, n, bClamped = 0;

    if (!jkCogFactory_bEnabled || !pSpec || !pSpec[0])
        return 0;

    pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!pLocal)
    {
        jkCogFactory_Printf("look: no local player yet");
        return 0;
    }

    if (sscanf(pSpec, "thing %d", &idx) == 1)
    {
        SithWorld* pWorld = sithWorld_g_pCurrentWorld;
        if (!pWorld || idx < 0 || idx >= (int)pWorld->numThingsLoaded
            || pWorld->aThings[idx].type == SITH_THING_FREE)
        {
            jkCogFactory_Printf("look: thing %d does not exist", idx);
            return 0;
        }
        pTargetThing = &pWorld->aThings[idx];
        target = pTargetThing->position;
    }
    else
    {
        n = sscanf(pSpec, "%f %f %f", &target.x, &target.y, &target.z);
        if (n < 3)
        {
            jkCogFactory_Printf("look: cannot parse '%s' (want \"x y z\" or \"thing <n>\")",
                                pSpec);
            return 0;
        }
    }

    rdVector_Sub3(&dir, &target, &pLocal->position);
    horiz = sqrt((double)dir.x * dir.x + (double)dir.y * dir.y);
    if (horiz == 0.0 && dir.z == 0.0)
    {
        jkCogFactory_Printf("look: target is the player's own position; refusing");
        return 0;
    }

    /* rdMatrix_Build34 (rdMatrix.c:30-32) defines the convention:
     *     lvec = (-sin(yaw)cos(pitch), cos(yaw)cos(pitch), sin(pitch))
     * so yaw 0 faces +y, yaw 90 faces -x, and POSITIVE pitch looks UP. */
    yaw   = atan2(-(double)dir.x, (double)dir.y) * JKCF_RAD2DEG;
    pitch = atan2((double)dir.z, horiz) * JKCF_RAD2DEG;

    wanted = pitch;
    if (pitch < (double)pLocal->actorParams.minHeadPitch)
    {
        pitch = (double)pLocal->actorParams.minHeadPitch;
        bClamped = 1;
    }
    if (pitch > (double)pLocal->actorParams.maxHeadPitch)
    {
        pitch = (double)pLocal->actorParams.maxHeadPitch;
        bClamped = 1;
    }

    pyr.x = 0.0f;
    pyr.y = (float)yaw;
    pyr.z = 0.0f;
    rdMatrix_BuildRotate34(&pLocal->orient, &pyr);

    pyr.x = (float)pitch;
    pyr.y = 0.0f;
    pyr.z = 0.0f;
    sithActor_SetHeadPYR(pLocal, &pyr);
    pLocal->actorParams.flags &= ~SITH_AF_VIEWCENTRING;

    sithCamera_Update(sithCamera_g_pCurCamera);

    /* Report the dot actually achieved, computed the way the verb computes it.
     * A command that says "I aimed there" is a claim; a command that says
     * "dot=1.0000" is a measurement, and it is what caught the sign of pitch
     * and the eye-vs-position offset without a single extra run. */
    jkCogFactory_ViewLook(pLocal, &lvec);
    rdVector_Normalize3Acc(&dir);
    dot = (double)lvec.x * dir.x + (double)lvec.y * dir.y + (double)lvec.z * dir.z;

    /* Target and dot ADJACENT, angles after. A test wants to assert two things
     * -- which point, and that the aim landed on it -- and `expect` is a
     * single substring match, so they have to be neighbours on the line. The
     * solved angles are for a human: they are float32 trigonometry and the
     * last decimal moves with the player's settled z, so a test that pinned
     * them would fail on a rounding difference and look like a real defect.
     * (It did, on this project's first control run.) */
    jkCogFactory_Printf("look: at (%.4f %.4f %.4f) dot=%.4f%s yaw=%.4f pitch=%.4f",
                        (double)target.x, (double)target.y, (double)target.z,
                        dot, bClamped ? " CLAMPED" : "", yaw, pitch);
    if (bClamped)
        jkCogFactory_Printf("look: pitch %.4f is outside the player's head range "
                            "[%.2f %.2f]; aimed as close as the engine allows",
                            wanted, (double)pLocal->actorParams.minHeadPitch,
                            (double)pLocal->actorParams.maxHeadPitch);
    return 1;
}

/* ------------------------------------------------------------- autopilot */
/* Walk the player somewhere by INPUT rather than by teleporting.
 *
 * `warp` skips physics, collision and adjoin traversal -- which is right for a
 * screenshot and wrong for any test about movement. Holding `w` for N frames is
 * reproducible only in order, not in distance, because game time is wall-clock
 * derived; and it only goes straight.
 *
 * This synthesises the same axis values the keyboard produces and lets the
 * whole movement pipeline run untouched. It is not a pathfinder: it points at
 * the target and walks, and a wall in between produces `stuck` with a position,
 * which is a useful answer rather than a failure. */

#define JKCF_GOTO_TURN_GAIN   (1.0f / 40.0f)  /* axis per degree of yaw error */
#define JKCF_GOTO_FACE_DEG    50.0f           /* walk only when roughly facing */
#define JKCF_GOTO_EASE        0.60f           /* ease off inside this radius */
#define JKCF_GOTO_MIN_FWD     0.25f           /* ...but never below this */
#define JKCF_GOTO_STILL_D     0.0025f         /* per-tick movement that counts */
#define JKCF_GOTO_STILL_TICKS 120             /* ...for this long is "stuck" */
#define JKCF_GOTO_DEFAULT_TOL 0.15f
#define JKCF_GOTO_Z_TOL       0.60f           /* z is advisory: ramps handle it */
#define JKCF_GOTO_LIMIT       3000            /* give up after this many ticks */

static int       s_go_active;
static int       s_go_thing = -1;      /* >= 0: chase a thing, else s_go_pos */
static rdVector3 s_go_pos;
static float     s_go_tol;
static int       s_go_ticks;
static int       s_go_still;
static int       s_go_said;            /* bit per event, so each logs once */
static rdVector3 s_go_last;
static float     s_go_turn, s_go_fwd;
static uint32_t  s_go_tick_stamp;
static int       s_go_ticked;
/* Every goto gets a number, and every line it prints carries it.
 *
 * `expect` matches any line logged at ANY point in the session, so three legs
 * that all report "goto: arrived" are indistinguishable: the second and third
 * assertions match the FIRST leg's line and pass instantly whatever happened.
 * This test did exactly that on its first green run -- legs 2 and 3 "passed"
 * while the player was still walking leg 2. p12 hit the same trap through a
 * prefix collision; this is the same hazard wearing different clothes. */
static int       s_go_seq;

#define JKCF_SAID_STUCK  0x1
#define JKCF_SAID_VOID   0x2

static void jkCogFactory_GotoStop(const char* pWhy, SithThing* pLocal)
{
    if (pWhy && pLocal)
        jkCogFactory_Printf("goto #%d: %s at (%.4f %.4f %.4f) after %d ticks",
                            s_go_seq, pWhy, (double)pLocal->position.x,
                            (double)pLocal->position.y,
                            (double)pLocal->position.z, s_go_ticks);
    s_go_active = 0;
    s_go_turn = s_go_fwd = 0.0f;
}

int jkCogFactory_SetGoto(const char* pSpec)
{
    float tol = JKCF_GOTO_DEFAULT_TOL;
    int idx;

    if (!jkCogFactory_bEnabled)
        return 0;

    /* Same sentinel discipline as `cam`: test the WHOLE string, never just its
     * first character, or every target with a negative x turns the thing off
     * (jkCogFactory_SetCam's own bug, found by p09). */
    if (!pSpec || !pSpec[0] || (pSpec[0] == '-' && !pSpec[1])
        || !strcmp(pSpec, "off"))
    {
        if (s_go_active)
            jkCogFactory_Printf("goto #%d: cancelled", s_go_seq);
        s_go_active = 0;
        s_go_thing = -1;
        s_go_turn = s_go_fwd = 0.0f;
        return 1;
    }

    if (sscanf(pSpec, "thing %d %f", &idx, &tol) >= 1)
    {
        s_go_thing = idx;
        rdVector_Zero3(&s_go_pos);
    }
    else if (sscanf(pSpec, "%f %f %f %f", &s_go_pos.x, &s_go_pos.y, &s_go_pos.z,
                    &tol) >= 3)
    {
        s_go_thing = -1;
    }
    else
    {
        jkCogFactory_Printf("goto: cannot parse '%s' (want \"x y z [tol]\", "
                            "\"thing <index> [tol]\" or \"off\")", pSpec);
        return 0;
    }

    s_go_tol = (tol > 0.0f) ? tol : JKCF_GOTO_DEFAULT_TOL;
    s_go_seq++;
    s_go_active = 1;
    s_go_ticks = 0;
    s_go_still = 0;
    s_go_said = 0;
    s_go_turn = s_go_fwd = 0.0f;
    s_go_ticked = 0;
    rdVector_Zero3(&s_go_last);

    if (s_go_thing >= 0)
        jkCogFactory_Printf("goto #%d: chasing thing %d, tol %.3f", s_go_seq, s_go_thing,
                            (double)s_go_tol);
    else
        jkCogFactory_Printf("goto #%d: heading for (%.4f %.4f %.4f), tol %.3f", s_go_seq,
                            (double)s_go_pos.x, (double)s_go_pos.y,
                            (double)s_go_pos.z, (double)s_go_tol);
    return 1;
}

/* Recompute the axes. Runs once per rendered frame -- the accessors below are
 * called several times each and must not each advance the state machine. */
static void jkCogFactory_GotoTick(SithThing* pLocal)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    rdVector3 target = s_go_pos;
    rdVector3 pyr;
    float dx, dy, dz, dist2d, moved, want, err, fwd;

    s_go_ticks++;

    if (s_go_thing >= 0)
    {
        if (!pWorld || s_go_thing >= pWorld->numThingsLoaded
            || pWorld->aThings[s_go_thing].type == SITH_THING_FREE)
        {
            jkCogFactory_Printf("goto #%d: thing %d does not exist", s_go_seq, s_go_thing);
            jkCogFactory_GotoStop(NULL, NULL);
            return;
        }
        target = pWorld->aThings[s_go_thing].position;
    }

    dx = target.x - pLocal->position.x;
    dy = target.y - pLocal->position.y;
    dz = target.z - pLocal->position.z;
    dist2d = (float)sqrt((double)(dx * dx + dy * dy));

    if (dist2d <= s_go_tol && (dz < JKCF_GOTO_Z_TOL && dz > -JKCF_GOTO_Z_TOL))
    {
        jkCogFactory_GotoStop("arrived", pLocal);
        return;
    }

    /* Is the player still in the world at all? This is the check that exists
     * because a human walked p11-terrace and fell through a floor: the level's
     * own cog could not see it, because every assertion it made fired on a
     * SECTOR CHANGE and a player in the void stops changing sector. */
    if (pWorld && !sithSector_FindSectorAtPos(pWorld, &pLocal->position)
        && !(s_go_said & JKCF_SAID_VOID))
    {
        s_go_said |= JKCF_SAID_VOID;
        jkCogFactory_Printf("goto #%d: VOIDED -- no sector at (%.4f %.4f %.4f), "
                            "%.3f from target", s_go_seq,
                            (double)pLocal->position.x,
                            (double)pLocal->position.y,
                            (double)pLocal->position.z, (double)dist2d);
    }

    /* Not moving while being told to move. Reported once, and re-armed as soon
     * as the player moves again, so a genuine pause does not flood the log. */
    moved = (float)sqrt(
        (double)((pLocal->position.x - s_go_last.x) * (pLocal->position.x - s_go_last.x)
               + (pLocal->position.y - s_go_last.y) * (pLocal->position.y - s_go_last.y)
               + (pLocal->position.z - s_go_last.z) * (pLocal->position.z - s_go_last.z)));
    s_go_last = pLocal->position;
    if (s_go_ticks > 2 && moved < JKCF_GOTO_STILL_D && s_go_fwd != 0.0f)
    {
        if (++s_go_still == JKCF_GOTO_STILL_TICKS && !(s_go_said & JKCF_SAID_STUCK))
        {
            SithSector* pSec = pWorld
                ? sithSector_FindSectorAtPos(pWorld, &pLocal->position) : NULL;
            s_go_said |= JKCF_SAID_STUCK;
            jkCogFactory_Printf("goto #%d: STUCK at (%.4f %.4f %.4f) sector %d, "
                                "%.3f from target after %d ticks", s_go_seq,
                                (double)pLocal->position.x,
                                (double)pLocal->position.y,
                                (double)pLocal->position.z,
                                pSec ? (int)pSec->id : -1,
                                (double)dist2d, s_go_ticks);
        }
    }
    else if (moved >= JKCF_GOTO_STILL_D)
    {
        s_go_still = 0;
        s_go_said &= ~JKCF_SAID_STUCK;
    }

    if (s_go_ticks > JKCF_GOTO_LIMIT)
    {
        jkCogFactory_GotoStop("gave up", pLocal);
        return;
    }

    /* Steering. Yaw 0 is +y and yaw 90 is -x -- counter-clockwise seen from
     * above -- measured by p09 and confirmed four ways by p14, so the heading
     * for yaw t is (-sin t, cos t) and the yaw that points at (dx, dy) is
     * atan2(-dx, dy). */
    want = (float)(atan2((double)(-dx), (double)dy) * (180.0 / 3.14159265358979323846));
    rdMatrix_ExtractAngles34(&pLocal->orient, &pyr);
    err = want - pyr.y;
    while (err > 180.0f) err -= 360.0f;
    while (err < -180.0f) err += 360.0f;

    s_go_turn = err * JKCF_GOTO_TURN_GAIN;
    if (s_go_turn > 1.0f) s_go_turn = 1.0f;
    if (s_go_turn < -1.0f) s_go_turn = -1.0f;

    /* Do not walk while pointing the wrong way, or the path is an arc that
     * misses. Ease off near the target so the tolerance is reachable instead
     * of being overshot every tick. */
    if (err < JKCF_GOTO_FACE_DEG && err > -JKCF_GOTO_FACE_DEG)
    {
        fwd = dist2d / JKCF_GOTO_EASE;
        if (fwd > 1.0f) fwd = 1.0f;
        if (fwd < JKCF_GOTO_MIN_FWD) fwd = JKCF_GOTO_MIN_FWD;
    }
    else
    {
        fwd = 0.0f;
    }
    s_go_fwd = fwd;
}

int jkCogFactory_AutopilotAxis(int axisId, flex_t* pOut)
{
    SithThing* pLocal;

    if (!jkCogFactory_bEnabled || !s_go_active || !pOut)
        return 0;

    pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!pLocal || !pLocal->sector)
        return 0;

    /* One state advance per frame, however many times the accessors are hit --
     * sithControl calls each of them more than once per frame. There is no
     * frame counter in sithTime, so the game clock is the stamp. */
    if (!s_go_ticked || sithTime_g_msecGameTime != s_go_tick_stamp)
    {
        s_go_tick_stamp = sithTime_g_msecGameTime;
        s_go_ticked = 1;
        jkCogFactory_GotoTick(pLocal);
        if (!s_go_active)
            return 0;
    }

    if (axisId == INPUT_FUNC_TURN)
    {
        *pOut = s_go_turn;
        return 1;
    }
    if (axisId == INPUT_FUNC_FORWARD)
    {
        /* MEASURED, not reasoned. The comment here first said the axis had to
         * be negated -- read off sithControl's `-GetKeyAsAxisNormalized(...)`
         * in a nearby function -- and asserted it was verified. It was not:
         * the first autowalk run drove the player 0.54 units SOUTH of a target
         * to the north and reported STUCK. Positive is forward. */
        *pOut = s_go_fwd;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ fixed step */
/* Make game time advance by an exact amount per frame. See jkCogFactory.h for
 * why, and sithTime_Advance/core_advance_time for the two places it lands. */

static double s_ts_secs;   /* 0.0 == not engaged */

int jkCogFactory_SetTimestep(const char* pSpec)
{
    double value = 0.0;
    double ms;
    double lo, hi;
    char unit[16];
    int n;

    if (!jkCogFactory_bEnabled)
        return 0;

    /* Same sentinel discipline as `cam` and `goto`: compare the WHOLE string.
     * A bare "-" clears; a leading '-' on a number does not (there is no such
     * thing as a negative step, but the habit is what stops the p09 bug where
     * every negative x silently released the camera). */
    if (!pSpec || !pSpec[0] || (pSpec[0] == '-' && !pSpec[1])
        || !strcmp(pSpec, "off"))
    {
        if (s_ts_secs > 0.0)
            jkCogFactory_Printf("timestep: cleared -- game time follows the wall clock again");
        s_ts_secs = 0.0;
        return 1;
    }

    unit[0] = '\0';
    n = sscanf(pSpec, "%lf %15s", &value, unit);
    if (n < 1 || !(value > 0.0))
    {
        jkCogFactory_Printf("timestep: cannot parse '%s' (want \"<ms>\", \"<n>fps\" or \"off\")",
                            pSpec);
        return 0;
    }

    if (!unit[0] || !strcmp(unit, "ms") || !strcmp(unit, "msec"))
    {
        ms = value;
    }
    else if (!strcmp(unit, "fps") || !strcmp(unit, "hz") || !strcmp(unit, "Hz"))
    {
        ms = 1000.0 / value;
    }
    else
    {
        jkCogFactory_Printf("timestep: unknown unit '%s' in '%s' (want ms, fps or hz)",
                            unit, pSpec);
        return 0;
    }

    /* Refuse, do not clamp. sithTime_SetFrameTime clamps a wall-clock delta
     * into SITHTIME_MINDELTA_US..MAXDELTA_US silently, which is right for a
     * measurement and wrong for a REQUEST: a test that asks for a 0.5 ms step
     * and is quietly given the floor would report frame counts for a step it
     * never ran at. The extra 1 ms floor is because sithTime_g_frameTime is
     * integer milliseconds -- below it the millisecond game clock stops
     * advancing on most frames, which also stalls anything keyed on it (the
     * autopilot's own per-frame stamp among them). */
    lo = (double)SITHTIME_MINDELTA_US / 1000.0;
    if (lo < 1.0)
        lo = 1.0;
    hi = (double)SITHTIME_MAXDELTA_US / 1000.0;
    if (ms < lo || ms > hi)
    {
        jkCogFactory_Printf("timestep: REFUSED %.4f ms -- outside the engine's own delta "
                            "clamp of %.3f..%.1f ms, which would silently clamp it to "
                            "something other than what was asked for",
                            ms, lo, hi);
        return 0;
    }

    s_ts_secs = ms * 0.001;
    jkCogFactory_Printf("timestep: FIXED at %.4f ms/frame (%.3f fps) -- game time no longer "
                        "follows the wall clock", ms, 1000.0 / ms);
    return 1;
}

double jkCogFactory_FixedStepSecs(void)
{
    if (!jkCogFactory_bEnabled)
        return 0.0;
    return s_ts_secs;
}

static void jkCogFactory_DumpSurfaces(SithWorld* pWorld)
{
    int bAll = (pWorld->numSurfaces <= JKCF_DUMP_ALL_LIMIT);
    int shown = 0;
    int i;

    /* The >limit filter is ADJOINS, not COG_LINKED. pAdjoin is populated by
     * sithWorld's surface parser and is live by the time this runs; COG_LINKED
     * is set by sithCog_Open, which runs later, so filtering on it printed
     * nothing at all. Adjoins are also the right rows to keep: on a level too
     * big to list, "where does this opening lead" is the question that survives. */
    jkCogFactory_Printf("surfaces %d (%s)", pWorld->numSurfaces,
                        bAll ? "all" : "adjoins only");

    for (i = 0; i < pWorld->numSurfaces; i++)
    {
        SithSurface* pSurf = &pWorld->surfaces[i];
        int bLinked = (pSurf->flags & SITH_SURFACE_COG_LINKED) != 0;

        if (!bAll && !pSurf->pAdjoin)
            continue;
        if (shown++ >= JKCF_DUMP_ROW_CAP)
        {
            jkCogFactory_Printf("surfaces ... (truncated at %d rows of %d)",
                                JKCF_DUMP_ROW_CAP, pWorld->numSurfaces);
            break;
        }

        /* flags is the field a generator most often gets wrong: a walkable
         * floor needs FLOOR|HAS_COLLISION (0x5), and COG_LINKED (0x2) is set
         * by the cog link, not by the JKL.
         *
         * adjoin= is the DESTINATION SECTOR, not a bare yes/no. sithWorld.c:322
         * computes it as `adjoin->sector = adjoin->mirror->surface->pSector`,
         * so it is the resolved answer to "where does this opening lead" --
         * the one adjoin fact a generator can get wrong while producing a level
         * that loads, renders and tests green. A bare 1 said nothing. */
        jkCogFactory_Printf("surface %d flags=0x%x sector=%d adjoin=%d nverts=%d%s%s%s",
                            i,
                            pSurf->flags,
                            pSurf->pSector ? (int)pSurf->pSector->id : -1,
                            (pSurf->pAdjoin && pSurf->pAdjoin->sector)
                                ? (int)pSurf->pAdjoin->sector->id : -1,
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

/* ----------------------------------------------------------------- probe */
/* On-demand introspection. See jkCogFactory.h for why this is not just the
 * load-time dump again. */

static void jkCogFactory_ProbePlayer(void)
{
    SithThing* p = sithPlayer_g_pLocalPlayerThing;
    rdVector3 pyr;

    if (!p)
    {
        jkCogFactory_Printf("probe player: no local player yet");
        return;
    }
    rdMatrix_ExtractAngles34(&p->orient, &pyr);
    jkCogFactory_Printf("probe player: pos=(%.4f %.4f %.4f) pyr=(%.2f %.2f %.2f) "
                        "sector=%d type=%d move=%d flags=0x%x attach=0x%x",
                        (double)p->position.x, (double)p->position.y,
                        (double)p->position.z,
                        (double)pyr.x, (double)pyr.y, (double)pyr.z,
                        p->sector ? (int)p->sector->id : -1,
                        p->type, p->moveType, p->flags, p->attach_flags);
    /* The pyr above is BODY orientation only -- it is extracted from ->orient,
     * and head pitch is a separate field that ->orient never carries. Anything
     * asking "where is the player looking" reads the composite
     * (sithCogFunctionAI_ThingViewDot pre-rotates orient by headPYR), so a
     * probe that printed only the body was quietly answering a question nobody
     * asked: a test could pitch the view 40 degrees down and see no change at
     * all in the probe. lvec is that composite, normalized -- the exact vector
     * ThingViewDot dots against, so a dot can be recomputed from the log. */
    if (p->type == SITH_THING_ACTOR || p->type == SITH_THING_PLAYER)
    {
        rdVector3 lvec;
        jkCogFactory_ViewLook(p, &lvec);
        jkCogFactory_Printf("probe player: headpyr=(%.4f %.4f %.4f) "
                            "pitchrange=[%.2f %.2f] lvec=(%.4f %.4f %.4f) "
                            "eyeoffset=(%.4f %.4f %.4f)",
                            (double)p->actorParams.headPYR.x,
                            (double)p->actorParams.headPYR.y,
                            (double)p->actorParams.headPYR.z,
                            (double)p->actorParams.minHeadPitch,
                            (double)p->actorParams.maxHeadPitch,
                            (double)lvec.x, (double)lvec.y, (double)lvec.z,
                            (double)p->actorParams.eyeOffset.x,
                            (double)p->actorParams.eyeOffset.y,
                            (double)p->actorParams.eyeOffset.z);
    }
    if (p->moveType == SITH_MT_PHYSICS)
    {
        jkCogFactory_Printf("probe player: vel=(%.4f %.4f %.4f) physflags=0x%x",
                            (double)p->physicsParams.vel.x,
                            (double)p->physicsParams.vel.y,
                            (double)p->physicsParams.vel.z,
                            p->physicsParams.flags);
    }
}

/* `probe input` -- the player's INPUT STATE, which is a different question from
 * `probe player`'s POSE.
 *
 * Added for p18-remotecontrol, the way `look` was added for p15. That project
 * asks how much of the player's input a LEVEL COG can read, and every answer it
 * finds is a COG reading a thing field: GetThingThrust is
 * physicsParams.acceleration (sithCogFunctionThing.c:1669), GetThingRotVel is
 * physicsParams.angularVelocity (:1915), the duck key is SITH_PF_CROUCHING in
 * physicsParams.flags (sithControl.c:1546-1559), and primary/secondary fire is
 * the file-static sithWeapon_CurWeaponMode.
 *
 * Printing those is only half of it. The second line is what the ENGINE'S INPUT
 * LAYER saw -- sithControl_GetKey per input function, straight out of
 * stdControl's key table -- so a test can assert that the cog's reading and the
 * key that caused it agree WITHOUT deriving one from the other. Without it the
 * only oracle for "the cog saw W" is the cog saying it saw W.
 *
 * sithControl_GetKey is a pure read: stdControl_ReadKey adds into *pOut from
 * aKeyPressed and returns aKeyInfo (stdControl.c:442-464), and neither array is
 * cleared by reading. Its one side effect is clearing stdControl_bControlsIdle,
 * which only suppresses the 30-second idle camera. */
static void jkCogFactory_ProbeInput(void)
{
    static const struct { const char* pName; int func; } aFuncs[] = {
        { "fwd",  INPUT_FUNC_FORWARD  },
        { "turn", INPUT_FUNC_TURN     },
        { "side", INPUT_FUNC_SLIDE    },
        { "jump", INPUT_FUNC_JUMP     },
        { "duck", INPUT_FUNC_DUCK     },
        { "fire1", INPUT_FUNC_FIRE1   },
        { "fire2", INPUT_FUNC_FIRE2   },
        { "use",  INPUT_FUNC_ACTIVATE },
    };
    SithThing* p = sithPlayer_g_pLocalPlayerThing;
    char aKeys[192];
    int n = 0;
    size_t i;

    if (!p)
    {
        jkCogFactory_Printf("probe input: no local player yet");
        return;
    }

    if (p->moveType == SITH_MT_PHYSICS)
    {
        /* thrust IS what GetThingThrust returns, and rotvel IS what
         * GetThingRotVel returns -- the same two fields, unscaled. */
        jkCogFactory_Printf("probe input: thrust=(%.4f %.4f %.4f) "
                            "rotvel=(%.4f %.4f %.4f) crouch=%d attach=0x%x "
                            "physflags=0x%x maxthrust=%.4f",
                            (double)p->physicsParams.acceleration.x,
                            (double)p->physicsParams.acceleration.y,
                            (double)p->physicsParams.acceleration.z,
                            (double)p->physicsParams.angularVelocity.x,
                            (double)p->physicsParams.angularVelocity.y,
                            (double)p->physicsParams.angularVelocity.z,
                            (p->physicsParams.flags & SITH_PF_CROUCHING) ? 1 : 0,
                            p->attach_flags, p->physicsParams.flags,
                            (double)(p->actorParams.maxThrust
                                     + p->actorParams.extraSpeed));
    }
    else
    {
        /* GetThingThrust pushes NOTHING for a non-physics thing
         * (sithCogFunctionThing.c:1669-1677) -- not zero, nothing -- so a cog
         * reading it underflows its own stack. Worth seeing in the probe. */
        jkCogFactory_Printf("probe input: move=%d -- NOT physics, so "
                            "GetThingThrust pushes nothing at all", p->moveType);
    }

    jkCogFactory_Printf("probe input: weapon=%d mode=%d",
                        sithInventory_GetCurrentWeapon(p),
                        sithWeapon_GetCurWeaponMode());

    /* Held/pressed per input function. `held` is aKeyInfo (down right now),
     * `hits` is the accumulated press count for this frame. */
    aKeys[0] = 0;
    for (i = 0; i < sizeof(aFuncs) / sizeof(aFuncs[0]); i++)
    {
        int hits = 0;
        int held = sithControl_IsOpen()
                 ? sithControl_GetKey(aFuncs[i].func, &hits) : 0;
        n += snprintf(aKeys + n, sizeof(aKeys) - (size_t)n, "%s%s=%d/%d",
                      i ? " " : "", aFuncs[i].pName, held ? 1 : 0, hits);
        if (n >= (int)sizeof(aKeys) - 1)
            break;
    }
    jkCogFactory_Printf("probe input: keys %s", aKeys);
}

static void jkCogFactory_ProbeTime(void)
{
    /* The one probe that answers a question about the HARNESS's own contract:
     * is the fixed step actually engaged, and is frameTimeFlex -- the delta the
     * physics integrates -- really the value that was asked for, rather than a
     * wall-clock measurement that merely looks close to it? */
    jkCogFactory_Printf("probe time: frameTime=%u ms frameTimeFlex=%.6f s fps=%.3f "
                        "msecGameTime=%u secGameTime=%.3f",
                        (unsigned)sithTime_g_frameTime,
                        (double)sithTime_g_frameTimeFlex,
                        (double)sithTime_g_fps,
                        (unsigned)sithTime_g_msecGameTime,
                        (double)sithTime_g_secGameTime);
    jkCogFactory_Printf("probe time: fixed step %s (%.4f ms requested)",
                        s_ts_secs > 0.0 ? "ENGAGED" : "off",
                        s_ts_secs * 1000.0);
}

static void jkCogFactory_ProbeThing(SithWorld* pWorld, int idx)
{
    SithThing* pThing;

    if (!pWorld || idx < 0 || idx >= (int)pWorld->numThingsLoaded)
    {
        jkCogFactory_Printf("probe thing %d: out of range (capacity %d)", idx,
                            pWorld ? (int)pWorld->numThingsLoaded : -1);
        return;
    }
    pThing = &pWorld->aThings[idx];
    if (pThing->type == SITH_THING_FREE)
    {
        jkCogFactory_Printf("probe thing %d: FREE slot", idx);
        return;
    }
    jkCogFactory_Printf("probe thing %d: type=%d move=%d frames=%d tpl='%s' sector=%d "
                        "pos=(%.4f %.4f %.4f) flags=0x%x",
                        idx, pThing->type, pThing->moveType,
                        pThing->moveType == SITH_MT_PATH
                            ? pThing->trackParams.loadedFrames : 0,
#ifdef SITH_DEBUG_STRUCT_NAMES
                        pThing->pTemplate ? pThing->pTemplate->aName : "?",
#else
                        "?",
#endif
                        pThing->sector ? (int)pThing->sector->id : -1,
                        (double)pThing->position.x, (double)pThing->position.y,
                        (double)pThing->position.z, pThing->flags);
}

static void jkCogFactory_ProbeSector(SithWorld* pWorld, int idx)
{
    SithSector* pSec;
    int nThings = 0;
    SithThing* pT;

    if (!pWorld || idx < 0 || idx >= (int)pWorld->numSectors)
    {
        jkCogFactory_Printf("probe sector %d: out of range (%d sectors)", idx,
                            pWorld ? (int)pWorld->numSectors : -1);
        return;
    }
    pSec = &pWorld->aSectors[idx];
    for (pT = pSec->pFirstThingInSector; pT; pT = pT->pNextThingInSector)
        nThings++;

    /* ambientLight and extraLight are the two terms p07 had to separate by
     * hand: ambient lights THINGS only and moves no surface pixel, extra is
     * the one that reaches a surface. Printing both is the whole point. */
    jkCogFactory_Printf("probe sector %d: ambient=%.4f extra=%.4f flags=0x%x "
                        "surfaces=%d vertices=%d things=%d tint=(%.2f %.2f %.2f)",
                        idx, (double)pSec->ambientLight, (double)pSec->extraLight,
                        pSec->flags, (int)pSec->numSurfaces, (int)pSec->numVertices,
                        nThings, (double)pSec->tint.x, (double)pSec->tint.y,
                        (double)pSec->tint.z);
}

static void jkCogFactory_ProbeSurface(SithWorld* pWorld, int idx)
{
    SithSurface* pSurf;

    if (!pWorld || idx < 0 || idx >= (int)pWorld->numSurfaces)
    {
        jkCogFactory_Printf("probe surface %d: out of range (%d surfaces)", idx,
                            pWorld ? (int)pWorld->numSurfaces : -1);
        return;
    }
    pSurf = &pWorld->surfaces[idx];
    /* Unlike the load-time dump, COG_LINKED is meaningful here: the link has
     * long since happened by the time anyone probes. */
    jkCogFactory_Printf("probe surface %d: flags=0x%x sector=%d adjoin=%d nverts=%d%s%s%s",
                        idx, pSurf->flags,
                        pSurf->pSector ? (int)pSurf->pSector->id : -1,
                        (pSurf->pAdjoin && pSurf->pAdjoin->sector)
                            ? (int)pSurf->pAdjoin->sector->id : -1,
                        pSurf->surfaceInfo.face.numVertices,
                        (pSurf->flags & SITH_SURFACE_FLOOR) ? " FLOOR" : "",
                        (pSurf->flags & SITH_SURFACE_HAS_COLLISION) ? " COLLIDE" : "",
                        (pSurf->flags & SITH_SURFACE_COG_LINKED) ? " COGLINKED" : "");
}

int jkCogFactory_Probe(const char* pSpec)
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    int idx;

    if (!jkCogFactory_bEnabled || !pSpec || !pSpec[0])
        return 0;

    if (!strncmp(pSpec, "world", 5))
    {
        if (!pWorld)
            jkCogFactory_Printf("probe world: no world loaded");
        else
            jkCogFactory_DumpWorld(pWorld);
        return 1;
    }
    if (!strncmp(pSpec, "player", 6))
    {
        jkCogFactory_ProbePlayer();
        return 1;
    }
    if (!strncmp(pSpec, "input", 5))
    {
        jkCogFactory_ProbeInput();
        return 1;
    }
    if (!strncmp(pSpec, "time", 4))
    {
        jkCogFactory_ProbeTime();
        return 1;
    }
    if (sscanf(pSpec, "thing %d", &idx) == 1)
    {
        jkCogFactory_ProbeThing(pWorld, idx);
        return 1;
    }
    if (sscanf(pSpec, "sector %d", &idx) == 1)
    {
        jkCogFactory_ProbeSector(pWorld, idx);
        return 1;
    }
    if (sscanf(pSpec, "surface %d", &idx) == 1)
    {
        jkCogFactory_ProbeSurface(pWorld, idx);
        return 1;
    }

    jkCogFactory_Printf("probe: cannot parse '%s' (want world | player | input | "
                        "time | thing <n> | sector <n> | surface <n>)", pSpec);
    return 0;
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
