/*
 * psram_ring.h
 *
 * Continuous recording of the raw ADC samples into the 8 MB PSRAM, so the
 * last 16.8 s at 250 kHz are always available: the pre-trigger for the
 * record button and the source for time-expansion playback.
 *
 * Once started, an interrupt-driven engine owns SPI1. DMA2 moves each
 * burst, and the burst-complete interrupt raises CE# and starts the next
 * burst, so the PSRAM is never selected for longer than tCEM (8 us). That
 * interrupt (DMA2 Channel 1) must be the only one at priority 0: any other
 * interrupt that could delay it would hold CE# low past tCEM, and the
 * PSRAM would lose data while it can't refresh. PsramRing_Start() checks.
 *
 * Writes come from the audio interrupt and never wait on the main loop.
 * Reads are queued and run between writes.
 */

#ifndef DRIVERS_PSRAM_RING_H_
#define DRIVERS_PSRAM_RING_H_

#include <stdint.h>

#include "pipeline_config.h"
#include "psram.h"


/* One audio block of raw 12-bit ADC codes, stored as uint16 */
#define PSRAM_RING_BLOCK_BYTES  (AUDIO_BLOCK_SIZE * 2UL)                       /* 512 */
#define PSRAM_RING_BLOCKS       (PSRAM_SIZE_BYTES / PSRAM_RING_BLOCK_BYTES)    /* 16384 = 16.8 s */


typedef enum
{
    PSRAM_RING_OK = 0,
    PSRAM_RING_ERR_PRIORITY,    /* the burst interrupt doesn't have priority 0 to itself */
    PSRAM_RING_ERR_CONFIG       /* SPI1 DMA isn't on DMA2 Ch1/Ch2, or SPI1 isn't 8-bit at /4 with NSSP off
                                 * and High-speed pins */
} PsramRing_Status_t;

typedef enum
{
    PSRAM_RING_READ_QUEUED = 0,
    PSRAM_RING_READ_BUSY,       /* queue full: try again later */
    PSRAM_RING_READ_INVALID     /* not written yet, or already overwritten */
} PsramRing_ReadResult_t;

/* Values of a read's done flag */
#define PSRAM_RING_PENDING      0U
#define PSRAM_RING_DONE         1U
#define PSRAM_RING_FAILED       2U


/*
 * Take over SPI1 for the ring. Call after the boot tests, which use the
 * polled driver in psram.c (it must not be used once the ring runs), and
 * before the ADC starts, so ring block 0 is the first audio block.
 */
PsramRing_Status_t PsramRing_Start(void);

uint8_t PsramRing_IsRunning(void);

/*
 * Stop or restart recording, e.g. while playback reads the last seconds
 * back (writing on would overwrite them). Blocks already written stay
 * readable; blocks arriving while paused are not recorded.
 */
void PsramRing_SetPaused(uint8_t pause);

/* Running and not paused: the block being processed now is being recorded */
uint8_t PsramRing_IsRecording(void);

/*
 * The first block recorded since the last pause ended (0 if never paused).
 * Block numbers carry straight on across a pause, so the blocks before this
 * one are from before the pause: a recording's pre-trigger mustn't reach
 * back past it, or it would join audio from either side of the gap.
 */
uint32_t PsramRing_GetResumeBlock(void);

/* Audio interrupt, once per block, before the DSP: queue the block for writing */
void PsramRing_PushBlock(const uint16_t *block);

/* Blocks handed over so far; the block being processed now is this minus 1 */
uint32_t PsramRing_GetPushedBlocks(void);

/* Blocks completely written: the newest readable block is this minus 1 */
uint32_t PsramRing_GetWrittenBlocks(void);

/*
 * Queue a read of one whole block into dest (PSRAM_RING_BLOCK_BYTES).
 * *done goes from PSRAM_RING_PENDING to PSRAM_RING_DONE (or _FAILED after
 * a fault). The block must be completely written and still in the ring.
 */
PsramRing_ReadResult_t PsramRing_ReadBlock(uint32_t block, void *dest, volatile uint8_t *done);

/* From DMA2_Channel1_IRQHandler: a burst has finished */
void PsramRing_IRQHandler(void);


/* Health and timing counters (psram_ring.c describes each); the debug link
 * reports them. Write 1 to psramRingResetStats to restart the timings. */
extern volatile uint32_t psramRingBlocks;
extern volatile uint32_t psramRingMaxSelectNs;
extern volatile uint32_t psramRingLateBlocks;
extern volatile uint32_t psramRingFaults;
extern volatile uint32_t psramRingReads;
extern volatile uint32_t psramRingMinSelectNs;
extern volatile uint32_t psramRingMaxEntryNs;
extern volatile uint32_t psramRingMaxIsrNs;
extern volatile uint32_t psramRingOverTcem;
extern volatile uint32_t psramRingBursts;
extern volatile uint32_t psramRingResetStats;


#endif /* DRIVERS_PSRAM_RING_H_ */
