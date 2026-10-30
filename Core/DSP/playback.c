/*
 * playback.c
 *
 * Time-expansion playback; see playback.h.
 *
 * The output runs at the full DAC rate (250 kHz). Each output sample steps
 * 1/PLAYBACK_EXPANSION of a sample through the recording, interpolating
 * linearly between recorded samples. Upsampling like this, rather than
 * slowing the DAC clock, leaves the unwanted images around multiples of
 * 25 kHz (at 10x): ultrasonic, and above the amplifier's ~6-10 kHz input
 * filter (diary P3).
 *
 * Recorded samples go through the same 10 kHz high-pass as the heterodyne
 * path first. It removes the ADC's mid-rail bias and the audible room
 * noise, which would otherwise play as rumble below 1 kHz.
 *
 * The ring is read one block at a time into two slots: one plays (10 ms
 * at 10x) while the next loads (~0.25 ms).
 */


#include "playback.h"

#include "main.h"
#include "dsp.h"
#include "dsp_filters.h"
#include "../Drivers/psram_ring.h"


/* Same loudness as the heterodyne for the same input: its mixer halves the amplitude */
#define PLAYBACK_GAIN           ((float)DSP_GAIN / 2.0f)

/* Same corner as the heterodyne path's high-pass (dsp.c) */
#define PLAYBACK_HIGH_PASS_HZ   10000.0f

#define PLAYBACK_BLOCKS         ((PLAYBACK_SECONDS * 1000000UL) / AUDIO_BLOCK_US)     /* 1953 */

/* Recording position advanced per output sample, as a 32-bit fraction */
#define PLAYBACK_STEP           ((uint32_t)(0x100000000ULL / PLAYBACK_EXPANSION))

/* How long Start waits for the previous playback's last reads to land */
#define PLAYBACK_START_WAIT_MS  5U

#define PLAYBACK_SILENCE        2048U       /* DAC mid-scale */
#define PLAYBACK_NO_BLOCK       UINT32_MAX

_Static_assert(PLAYBACK_BLOCKS <= (PSRAM_RING_BLOCKS / 2U), "playback must fit well inside the ring");


volatile uint32_t playbackUnderruns = 0;

static volatile uint8_t active = 0;
static uint8_t primed;                      /* the first two samples are loaded */

static uint32_t firstBlock;                 /* ring number of the first block played */
static uint32_t blockCount;                 /* blocks played */
static volatile uint32_t sampleIndex;       /* recorded sample being played, from 0 */
static uint32_t fraction;                   /* position from it towards the next, in 2^-32 */
static float current;                       /* high-passed recorded samples at sampleIndex... */
static float next;                          /* ...and sampleIndex + 1 */

static Biquad_t highPass;

/* Playback block n is read into slot n % 2 */
static uint16_t slots[2][AUDIO_BLOCK_SIZE];
static volatile uint8_t slotDone[2] = { PSRAM_RING_DONE, PSRAM_RING_DONE };
static uint32_t slotBlock[2];               /* playback block each slot holds or is loading */
static uint32_t requestBlock;               /* next playback block to ask the ring for */



/*
 * Ask the ring for the blocks needed next: up to the one after the block
 * being fetched from. A slot is free once fetching has moved past the
 * block it held, because the samples in use are copies.
 */
static void Playback_RequestBlocks(uint32_t fetchIndex)
{
    const uint32_t fetchBlock = fetchIndex / AUDIO_BLOCK_SIZE;

    while((requestBlock < blockCount) && (requestBlock <= (fetchBlock + 1U)))
    {
        const uint32_t slot = requestBlock & 1U;
        const PsramRing_ReadResult_t result = PsramRing_ReadBlock(firstBlock + requestBlock,
                slots[slot], &slotDone[slot]);

        if(result == PSRAM_RING_READ_BUSY)
        {
            return;     /* queue full (e.g. the debug link reading): try next block period */
        }

        if(result == PSRAM_RING_READ_INVALID)
        {
            slotDone[slot] = PSRAM_RING_FAILED;     /* plays as silence */
        }

        slotBlock[slot] = requestBlock;
        requestBlock++;
    }
}


/* Recorded sample number index, high-passed; 0 (silence) if its block isn't loaded */
static float Playback_Fetch(uint32_t index)
{
    const uint32_t block = index / AUDIO_BLOCK_SIZE;
    const uint32_t offset = index % AUDIO_BLOCK_SIZE;
    const uint32_t slot = block & 1U;

    if((slotBlock[slot] != block) || (slotDone[slot] != PSRAM_RING_DONE))
    {
        if(offset == 0U)
        {
            playbackUnderruns++;
        }

        return 0.0f;
    }

    return Biquad_Process(&highPass, (float)((int32_t)slots[slot][offset] - 2048));
}


static void Playback_Silence(uint16_t *output, uint32_t from, uint32_t length)
{
    for(uint32_t i = from; i < length; i++)
    {
        output[i] = PLAYBACK_SILENCE;
    }
}



Playback_Status_t Playback_Start(void)
{
    const uint32_t startTick = HAL_GetTick();
    uint32_t written;

    if(active)
    {
        return PLAYBACK_OK;
    }

    if(!PsramRing_IsRunning())
    {
        return PLAYBACK_ERR_NO_RECORDING;
    }

    /* The last playback's final reads may still be landing in the slots */
    while((slotDone[0] == PSRAM_RING_PENDING) || (slotDone[1] == PSRAM_RING_PENDING))
    {
        if((HAL_GetTick() - startTick) > PLAYBACK_START_WAIT_MS)
        {
            return PLAYBACK_ERR_BUSY;
        }
    }

    /* Stop recording first, so the blocks to be played can't be overwritten */
    PsramRing_SetPaused(1U);
    written = PsramRing_GetWrittenBlocks();

    if(written < 2U)
    {
        PsramRing_SetPaused(0U);
        return PLAYBACK_ERR_NO_RECORDING;
    }

    blockCount = (written < PLAYBACK_BLOCKS) ? written : PLAYBACK_BLOCKS;
    firstBlock = written - blockCount;

    Biquad_InitHighPass(&highPass, PLAYBACK_HIGH_PASS_HZ, SAMPLE_RATE);
    slotBlock[0] = PLAYBACK_NO_BLOCK;
    slotBlock[1] = PLAYBACK_NO_BLOCK;
    requestBlock = 0U;
    sampleIndex = 0U;
    fraction = 0U;
    primed = 0U;

    Playback_RequestBlocks(0U);

    /* The first block must be on its way: Render waits for it */
    if(requestBlock == 0U)
    {
        PsramRing_SetPaused(0U);
        return PLAYBACK_ERR_BUSY;
    }

    __DMB();        /* all of the above is set before the audio interrupt sees active */
    active = 1U;

    return PLAYBACK_OK;
}


void Playback_Stop(void)
{
    active = 0U;
    PsramRing_SetPaused(0U);
}


uint8_t Playback_IsActive(void)
{
    return active;
}


uint32_t Playback_GetRemainingMs(void)
{
    uint32_t left;

    if(!active)
    {
        return 0U;
    }

    left = (blockCount * AUDIO_BLOCK_SIZE) - sampleIndex;      /* recorded samples */

    return (left * PLAYBACK_EXPANSION) / (AUDIO_SAMPLE_RATE_HZ / 1000UL);
}


void Playback_Render(uint16_t *output, uint32_t length, float gain)
{
    const uint32_t lastIndex = (blockCount * AUDIO_BLOCK_SIZE) - 1U;
    const float scale = PLAYBACK_GAIN * gain;
    uint32_t index = sampleIndex;
    uint32_t i = 0U;

    Playback_RequestBlocks(index + 1U);

    /* Silence until the first block arrives (~0.25 ms after Start) */
    if(!primed)
    {
        if(slotDone[0] == PSRAM_RING_PENDING)
        {
            Playback_Silence(output, 0U, length);
            return;
        }

        current = Playback_Fetch(0U);
        next = Playback_Fetch(1U);
        primed = 1U;
    }

    while((i < length) && (index < lastIndex))
    {
        const float weight = (float)fraction * (1.0f / 4294967296.0f);
        const float value = current + ((next - current) * weight);

        output[i] = (uint16_t)DSP_clamp_output((int32_t)(value * scale));
        i++;

        fraction += PLAYBACK_STEP;

        if(fraction < PLAYBACK_STEP)        /* wrapped: on to the next recorded sample */
        {
            index++;
            current = next;
            next = (index < lastIndex) ? Playback_Fetch(index + 1U) : current;
        }
    }

    sampleIndex = index;

    /* The recording has run out: silence for the rest of the block, then live again */
    if(i < length)
    {
        Playback_Silence(output, i, length);
        active = 0U;
        PsramRing_SetPaused(0U);
    }
}
