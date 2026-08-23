#include "sithTime.h"

#include "stdPlatform.h"
#include "Main/jkCogFactory.h"

#ifdef MICROSECOND_TIME
static int64_t sithTime_deltaUs;
static uint64_t sithTime_curUsAbsolute;
static int64_t sithTime_pauseTimeUs;
#endif

//#define TIME_PROFILING
#ifdef TIME_PROFILING
static uint64_t sithTime_deltaUs_history[1000];
static size_t sithTime_deltaUs_history_idx = 0;
static size_t sithTime_deltaUs_history_collected_entries = 0;
#endif

// Added
flex_d_t sithTime_physicsRolloverFrames = 0.0;

#ifdef LIBRETRO_BUILD
/* Fractional millisecond carried between fixed-step frames. sithTime_g_frameTime
 * is integer ms, so a 41.667 ms step would otherwise advance the millisecond
 * game clock by 41 -- 1.6% slow, forever, which is deterministic but would make
 * a COG's Sleep(1.0) cover 1.016 s of physics. With the carry, 24 frames add
 * exactly 1000 ms and frameTime alternates 41/42 in a fixed, repeatable
 * pattern. Reset wherever the clock itself is re-based. */
static flex_d_t sithTime_fixedMsCarry = 0.0;

/* The fixed-step substitute for sithTime_SetFrameTime.
 *
 * It deliberately does NOT call sithTime_SetFrameTime. Under MICROSECOND_TIME
 * that function recomputes sithTime_g_frameTimeFlex -- the delta the physics
 * integrates and the one sithMain divides into physics ticks -- from
 * Linux_TimeUs(), ignoring the frameTime argument entirely. Handing it a fixed
 * millisecond delta therefore fixes sithTime_g_frameTime and
 * sithTime_g_msecGameTime and leaves the simulation exactly as wall-clock
 * dependent as it was. Both paths have to be written here.
 *
 * DEBUGFLAG_SLOWMO is intentionally not applied: this exists to make a run
 * reproducible, and the caller asked for a specific step. */
static void sithTime_SetFixedFrameTime(flex_d_t stepSecs)
{
    int wholeMs;

    /* Re-base both clocks every frame. Nothing here reads them, but CLEARING
     * the fixed step must not hand the next real Advance() a delta the size of
     * the entire fixed-step run -- that would clamp to SITHTIME_MAXDELTA and
     * teleport every moving thing in the level on one frame. */
    sithTime_g_clockTime = stdPlatform_GetTimeMsec();

    sithTime_fixedMsCarry += stepSecs * 1000.0;
    wholeMs = (int)sithTime_fixedMsCarry;
    sithTime_fixedMsCarry -= (flex_d_t)wholeMs;

    sithTime_g_frameTime = wholeMs;
    sithTime_g_msecGameTime += wholeMs;
    sithTime_g_frameTimeFlex = stepSecs;
#ifdef MICROSECOND_TIME
    sithTime_deltaUs = (int64_t)(stepSecs * 1000000.0);
    sithTime_curUsAbsolute = Linux_TimeUs();
#endif
    sithTime_g_fps = 1.0 / sithTime_g_frameTimeFlex;
    sithTime_g_secGameTime = (flex32_t)sithTime_g_msecGameTime * 0.001;
}
#endif // LIBRETRO_BUILD

// MOTS altered
void sithTime_Advance()
{
#ifdef LIBRETRO_BUILD
    /* Gated behind the COG Factory debug mode and unset by default, so this is
     * 0.0 -- and the line below is the stock one-liner -- for every normal
     * session. See jkCogFactory_SetTimestep. */
    {
        flex_d_t fixedSecs = (flex_d_t)jkCogFactory_FixedStepSecs();
        if (fixedSecs > 0.0)
        {
            sithTime_SetFixedFrameTime(fixedSecs);
            return;
        }
    }
#endif
    sithTime_SetFrameTime(stdPlatform_GetTimeMsec() - sithTime_g_clockTime);
}

void sithTime_Pause()
{
    if ( !sithTime_g_bPaused )
    {
        sithTime_msecPauseStartTime = stdPlatform_GetTimeMsec();
        sithTime_g_bPaused = 1;
#ifdef MICROSECOND_TIME
        sithTime_pauseTimeUs = Linux_TimeUs();
#endif

        sithTime_physicsRolloverFrames = 0.0; // Added
    }
}

void sithTime_Resume()
{
    if ( sithTime_g_bPaused )
    {
        sithTime_g_bPaused = 0;
        sithTime_g_clockTime += stdPlatform_GetTimeMsec() - sithTime_msecPauseStartTime;
#ifdef MICROSECOND_TIME
        sithTime_curUsAbsolute += Linux_TimeUs() - sithTime_pauseTimeUs;
#endif

        sithTime_physicsRolloverFrames = 0.0; // Added
    }
}

// MOTS altered: min/max are variables
void sithTime_SetFrameTime(int frameTime)
{
    sithTime_g_frameTime = frameTime;
    sithTime_g_clockTime = stdPlatform_GetTimeMsec();
    if ( sithTime_g_frameTime < SITHTIME_MINDELTA )
    {
        sithTime_g_frameTime = SITHTIME_MINDELTA;
    }
    if ( sithTime_g_frameTime > SITHTIME_MAXDELTA )
    {
        sithTime_g_frameTime = SITHTIME_MAXDELTA;
    }
    if (g_debugmodeFlags & DEBUGFLAG_SLOWMO) {
        sithTime_g_frameTime = (uint32_t)((flex_d_t)sithTime_g_frameTime * 0.2);
    }
    sithTime_g_msecGameTime += sithTime_g_frameTime;
#ifdef MICROSECOND_TIME
    sithTime_deltaUs = Linux_TimeUs() - sithTime_curUsAbsolute;
    if ( sithTime_deltaUs < SITHTIME_MINDELTA_US )
    {
        sithTime_deltaUs = SITHTIME_MINDELTA_US;
    }
    if ( sithTime_deltaUs > SITHTIME_MAXDELTA_US)
    {
        sithTime_deltaUs = SITHTIME_MAXDELTA_US;
    }
    if (g_debugmodeFlags & DEBUGFLAG_SLOWMO) {
        sithTime_deltaUs = (uint64_t)((flex_d_t)sithTime_deltaUs * 0.2);
    }
    sithTime_curUsAbsolute = Linux_TimeUs();
    sithTime_g_frameTimeFlex = (flex_d_t)sithTime_deltaUs * 0.001 * 0.001;

#ifdef TIME_PROFILING
    sithTime_deltaUs_history[sithTime_deltaUs_history_idx++] = sithTime_deltaUs;
    if (sithTime_deltaUs_history_idx >= 1000) {
        sithTime_deltaUs_history_idx = 0;
    }
    sithTime_deltaUs_history_collected_entries++;
    if (sithTime_deltaUs_history_collected_entries >= 1000) {
        sithTime_deltaUs_history_collected_entries = 1000;
    } 
    uint64_t avg_us = 0;
    uint64_t largest_us = 0;
    uint64_t smallest_us = 0xFFFFFFFF;
    for (int i = 0; i < sithTime_deltaUs_history_collected_entries; i++) {
        uint64_t val = sithTime_deltaUs_history[i];
        if (val > largest_us) 
            largest_us = val;
        if (val < smallest_us)
            smallest_us = val;
        avg_us += val;
    }
    avg_us /= sithTime_deltaUs_history_collected_entries;

    printf("%u %u %f %llu %llu %llu\n", sithTime_g_frameTime, sithTime_deltaUs, sithTime_g_frameTimeFlex, avg_us, largest_us, smallest_us);
#endif // TIME_PROFILING
#else
    sithTime_g_frameTimeFlex = (flex_d_t)sithTime_g_frameTime * 0.001;
#endif
    sithTime_g_fps = 1.0 / sithTime_g_frameTimeFlex;
    sithTime_g_secGameTime = (flex32_t)sithTime_g_msecGameTime * 0.001;
}

void sithTime_Startup()
{
#ifdef MICROSECOND_TIME
    sithTime_curUsAbsolute = Linux_TimeUs();
    sithTime_pauseTimeUs = 0;
    sithTime_deltaUs = 0;
#endif
    sithTime_g_msecGameTime = 0;
    sithTime_g_secGameTime = 0.0;
    sithTime_g_frameTime = 0;
    sithTime_g_frameTimeFlex = 0.0;
    sithTime_g_fps = 0.0;
    sithTime_g_clockTime = stdPlatform_GetTimeMsec();

    sithTime_physicsRolloverFrames = 0.0; // Added
#ifdef LIBRETRO_BUILD
    sithTime_fixedMsCarry = 0.0; // Added
#endif
}

void sithTime_SetGameTime(uint32_t msecTime)
{
#ifdef MICROSECOND_TIME
    sithTime_curUsAbsolute = Linux_TimeUs();
    sithTime_pauseTimeUs = 0;
    sithTime_deltaUs = 0;
#endif
    sithTime_g_frameTimeFlex = 0.0;
    sithTime_g_fps = 0.0;
    sithTime_g_msecGameTime = msecTime;
    sithTime_g_frameTime = 0;
    sithTime_g_secGameTime = (flex32_t)msecTime * 0.001;
    sithTime_g_clockTime = stdPlatform_GetTimeMsec();
#ifdef LIBRETRO_BUILD
    sithTime_fixedMsCarry = 0.0; // Added
#endif
}
