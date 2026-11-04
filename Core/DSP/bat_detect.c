/*
 * bat_detect.c
 *
 * 1. Noise floor: follows the block energy down quickly and up slowly, and
 *    is frozen during a pulse, so short calls barely move it.
 * 2. Pulse: a run of blocks more than 12 dB above the floor, 1-100 ms long.
 *    A run over 150 ms is continuous noise (insects, interference), and the
 *    floor then learns it.
 * 3. Pass: 3 pulses in a row, each within 600 ms of the one before, then
 *    held for 2 s after the last pulse.
 *
 * Everything counts blocks (1.024 ms each), so nothing here needs the clock.
 * The numbers are explained in the engineering diary, entries 30 and 44.
 */

#include "bat_detect.h"

#include <math.h>

#include "pipeline_config.h"

#define MS_TO_BLOCKS(ms)        ((((ms) * 1000UL) + AUDIO_BLOCK_US - 1UL) / AUDIO_BLOCK_US)

#define THRESHOLD_DB            12.0f
#define PULSE_MAX_BLOCKS        MS_TO_BLOCKS(100UL)
#define NOISE_RUN_BLOCKS        MS_TO_BLOCKS(150UL)
#define TRIGGER_PULSES          3U
#define TRIGGER_WINDOW_BLOCKS   MS_TO_BLOCKS(600UL)     /* the slowest caller, the greater noctule, calls every ~0.45 s */
#define HOLD_BLOCKS             MS_TO_BLOCKS(2000UL)
#define WARMUP_BLOCKS           MS_TO_BLOCKS(500UL)     /* learn the floor before detecting */

/* Floor smoothing per block, for a time constant in ms */
#define ALPHA(tauMs)            ((float)AUDIO_BLOCK_US / ((tauMs) * 1000.0f))
#define FLOOR_FALL              ALPHA(250.0f)
#define FLOOR_RISE              ALPHA(4000.0f)
#define FLOOR_WARMUP            ALPHA(16.0f)

static float    thresholdFactor;            /* 12 dB as an energy ratio */
static float    floorEnergy;
static uint32_t warmupBlocks;
static uint32_t runBlocks;                  /* length of the current loud run */
static uint32_t blocksSincePulse;
static uint8_t  chainPulses;                /* pulses in a row, each within the window */
static uint32_t holdBlocks;                 /* time left in the pass */
static volatile uint32_t pulses;
static volatile uint8_t  present;


/* A loud run has ended: count it if it was bat-like, and see whether it starts or extends a pass */
static void BatDetect_EndOfRun(uint32_t lengthBlocks)
{
    if(lengthBlocks > PULSE_MAX_BLOCKS)
    {
        return;
    }

    pulses++;

    if((chainPulses > 0U) && (blocksSincePulse <= TRIGGER_WINDOW_BLOCKS))
    {
        if(chainPulses < TRIGGER_PULSES)
        {
            chainPulses++;
        }
    }
    else
    {
        chainPulses = 1U;       /* the first, or too long since the last */
    }

    if(present || (chainPulses >= TRIGGER_PULSES))
    {
        present = 1U;
        holdBlocks = HOLD_BLOCKS;
    }

    blocksSincePulse = 0U;
}


void BatDetect_Init(void)
{
    thresholdFactor = powf(10.0f, THRESHOLD_DB / 10.0f);
    floorEnergy = 0.0f;
    warmupBlocks = WARMUP_BLOCKS;
    runBlocks = 0U;
    blocksSincePulse = 0U;
    chainPulses = 0U;
    holdBlocks = 0U;
    present = 0U;
}


void BatDetect_ProcessBlock(float energy)
{
    if(warmupBlocks > 0U)
    {
        floorEnergy += (energy - floorEnergy) * FLOOR_WARMUP;
        warmupBlocks--;
    }
    else if(energy > floorEnergy * thresholdFactor)
    {
        runBlocks++;

        /* frozen during a pulse, but a continuous noise must be learnt,
         * or the detector would stay "loud" forever */
        if(runBlocks > NOISE_RUN_BLOCKS)
        {
            floorEnergy += (energy - floorEnergy) * FLOOR_RISE;
        }
    }
    else
    {
        if(runBlocks > 0U)
        {
            BatDetect_EndOfRun(runBlocks);
            runBlocks = 0U;
        }

        floorEnergy += (energy - floorEnergy) * ((energy < floorEnergy) ? FLOOR_FALL : FLOOR_RISE);
    }

    if(blocksSincePulse <= TRIGGER_WINDOW_BLOCKS)
    {
        blocksSincePulse++;     /* stops just past the window */
    }

    if(present && (holdBlocks > 0U))
    {
        holdBlocks--;

        if(holdBlocks == 0U)
        {
            present = 0U;
        }
    }
}


uint8_t BatDetect_IsBatPresent(void)
{
    return present;
}


uint32_t BatDetect_GetPulseCount(void)
{
    return pulses;
}
