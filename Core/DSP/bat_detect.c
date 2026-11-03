/*
 * bat_detect.c
 *
 * Bat-call detector, run once per audio block from DSP_ProcessBlock.
 *
 * 1. Energy: the caller sums the squared 10 kHz high-pass output over the
 *    block.
 * 2. Noise floor: follows the energy down quickly and up slowly, and is
 *    frozen during a pulse, so short calls barely move it while steady
 *    noise is gradually absorbed.
 * 3. Pulses: a run of blocks more than BAT_THRESHOLD_DB above the floor.
 *    1-100 ms is bat-like; over 150 ms is continuous noise.
 * 4. Trigger: 3 bat-like pulses, each within 600 ms of the one before;
 *    hold for 2 s after the last.
 *
 * Timings come from Docs/European Bat Calls -- Species, Frequencies &
 * Acoustic ID.md. Everything counts blocks, so nothing here needs
 * HAL_GetTick() inside the interrupt.
 */


#include "bat_detect.h"

#include <math.h>

#include "pipeline_config.h"


/* Milliseconds to whole blocks, rounded up */
#define MS_TO_BLOCKS(ms)        ((((ms) * 1000UL) + AUDIO_BLOCK_US - 1UL) / AUDIO_BLOCK_US)

/* A block is loud when its energy is this far above the noise floor.
 * Replaying the Oct 30 quiet-room capture (diary 33, 44): at 10 dB the
 * room's own noise gave 57 false pulses and 6 false passes a minute, and
 * LED0 was lit 59% of the time; at 12 dB, 11 false pulses and no passes. */
#define BAT_THRESHOLD_DB        12.0f

/* Pulses in a row, each within BAT_TRIGGER_WINDOW_MS of the one before,
 * that make a pass. Each extra pulse multiplies the chance of random
 * pulses chaining into a pass by the chance of another within 600 ms:
 * ~0.1 at 11 a minute, so 3 instead of 2 makes false passes ~10x rarer.
 * A real pass costs one more call interval (0.1-0.2 s at 5-10 calls/s). */
#define BAT_TRIGGER_PULSES      3U

/* Shortest calls ~0.8 ms (long-eared bats) fit in one block. The longest
 * are greater noctules (~30 ms) and horseshoe CF tones (tens of ms). */
#define BAT_PULSE_MAX_MS        100UL

/* A loud run longer than this is continuous noise (insects, interference) */
#define BAT_NOISE_RUN_MS        150UL

/* Second pulse must follow within this. The slowest European caller,
 * the greater noctule, calls every 364-468 ms. */
#define BAT_TRIGGER_WINDOW_MS   600UL

/* Stay "present" this long after the last pulse: ~4x the longest call
 * interval, so the sound doesn't chop when a bat pauses or turns away. */
#define BAT_HOLD_MS             2000UL

/* Noise floor time constants: fall fast so sensitivity returns quickly,
 * rise slowly so a 10-30 ms call hardly moves it. */
#define BAT_FLOOR_FALL_MS       250.0f
#define BAT_FLOOR_RISE_MS       4000.0f

/* After reset the filters settle and the floor is learnt; no pulses yet */
#define BAT_WARMUP_MS           500UL
#define BAT_WARMUP_FLOOR_MS     16.0f

#define BAT_PULSE_MAX_BLOCKS        MS_TO_BLOCKS(BAT_PULSE_MAX_MS)
#define BAT_NOISE_RUN_BLOCKS        MS_TO_BLOCKS(BAT_NOISE_RUN_MS)
#define BAT_TRIGGER_WINDOW_BLOCKS   MS_TO_BLOCKS(BAT_TRIGGER_WINDOW_MS)
#define BAT_HOLD_BLOCKS             MS_TO_BLOCKS(BAT_HOLD_MS)
#define BAT_WARMUP_BLOCKS           MS_TO_BLOCKS(BAT_WARMUP_MS)

/* Floor smoothing factor for a time constant in ms: one block / tau */
#define BAT_ALPHA(tauMs)        ((float)AUDIO_BLOCK_US / ((tauMs) * 1000.0f))


/*
 * Debug view: watch these in Live Expressions while tuning. The dB values
 * are relative (10 * log10 of the block energy), so compare them with each
 * other rather than reading them as absolute levels.
 */
volatile float    batEnergyDb   = 0.0f;   /* last block */
volatile float    batFloorDb    = 0.0f;   /* noise floor */
volatile uint32_t batPulses     = 0;      /* bat-like pulses (1-100 ms) */
volatile uint32_t batTriggers   = 0;      /* times "present" switched on */
volatile uint32_t batNoiseRuns  = 0;      /* loud runs rejected as continuous noise */
volatile uint8_t  batPresent    = 0;

static float    thresholdFactor = 10.0f;
static float    floorEnergy = 0.0f;
static uint32_t warmupBlocks = 0;         /* blocks left in the warm-up */
static uint32_t runBlocks = 0;            /* length of the current loud run */
static uint32_t blocksSincePulse = 0;     /* since the last bat-like pulse ended */
static uint8_t  chainPulses = 0;          /* pulses in a row, each within the window (max BAT_TRIGGER_PULSES) */
static uint32_t holdBlocks = 0;           /* "present" time remaining */



static float BatDetect_ToDb(float energy)
{
    /* the tiny offset keeps log10f away from zero */
    return 10.0f * log10f(energy + 1.0e-6f);
}


/*
 * A loud run has just ended. If it was bat-like, count it and check
 * whether it completes a chain of pulses that triggers (or extends) a pass.
 */
static void BatDetect_EndOfRun(uint32_t lengthBlocks)
{
    if(lengthBlocks > BAT_PULSE_MAX_BLOCKS)
    {
        return;     /* too long for a call: ambiguous or continuous noise */
    }

    batPulses++;

    if((chainPulses > 0U) && (blocksSincePulse <= BAT_TRIGGER_WINDOW_BLOCKS))
    {
        if(chainPulses < BAT_TRIGGER_PULSES)
        {
            chainPulses++;
        }
    }
    else
    {
        chainPulses = 1U;       /* the first, or too long since the last */
    }

    if(batPresent)
    {
        holdBlocks = BAT_HOLD_BLOCKS;
    }
    else if(chainPulses >= BAT_TRIGGER_PULSES)
    {
        batPresent = 1;
        holdBlocks = BAT_HOLD_BLOCKS;
        batTriggers++;
    }

    blocksSincePulse = 0;
}



void BatDetect_Init(void)
{
    thresholdFactor = powf(10.0f, BAT_THRESHOLD_DB / 10.0f);
    floorEnergy = 0.0f;
    warmupBlocks = BAT_WARMUP_BLOCKS;
    runBlocks = 0;
    blocksSincePulse = 0;
    chainPulses = 0;
    holdBlocks = 0;
    batPresent = 0;
}


void BatDetect_ProcessBlock(float energy)
{
    batEnergyDb = BatDetect_ToDb(energy);

    if(warmupBlocks > 0U)
    {
        /* learn the floor quickly in both directions; no pulse detection */
        floorEnergy += (energy - floorEnergy) * BAT_ALPHA(BAT_WARMUP_FLOOR_MS);
        warmupBlocks--;
    }
    else if(energy > floorEnergy * thresholdFactor)
    {
        runBlocks++;

        if(runBlocks == (BAT_NOISE_RUN_BLOCKS + 1U))
        {
            batNoiseRuns++;
        }

        /* the floor stays frozen during a pulse, but a continuous noise
         * must be learnt, or the detector would stay "loud" forever */
        if(runBlocks > BAT_NOISE_RUN_BLOCKS)
        {
            floorEnergy += (energy - floorEnergy) * BAT_ALPHA(BAT_FLOOR_RISE_MS);
        }
    }
    else
    {
        if(runBlocks > 0U)
        {
            BatDetect_EndOfRun(runBlocks);
            runBlocks = 0;
        }

        if(energy < floorEnergy)
        {
            floorEnergy += (energy - floorEnergy) * BAT_ALPHA(BAT_FLOOR_FALL_MS);
        }
        else
        {
            floorEnergy += (energy - floorEnergy) * BAT_ALPHA(BAT_FLOOR_RISE_MS);
        }
    }

    batFloorDb = BatDetect_ToDb(floorEnergy);

    if(blocksSincePulse < BAT_TRIGGER_WINDOW_BLOCKS + 1U)
    {
        blocksSincePulse++;     /* saturates just past the window */
    }

    if(batPresent && (holdBlocks > 0U))
    {
        holdBlocks--;

        if(holdBlocks == 0U)
        {
            batPresent = 0;
        }
    }
}


uint8_t BatDetect_IsBatPresent(void)
{
    return batPresent;
}


uint32_t BatDetect_GetPulseCount(void)
{
    return batPulses;
}
