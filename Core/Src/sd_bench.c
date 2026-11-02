/*
 * sd_bench.c
 *
 * Sustained SD write test under recording load; see sd_bench.h.
 *
 * Blocks go ring -> slot -> file, in order. Block b uses slot
 * b % SD_BENCH_SLOTS, and blocks saveNext .. readNext-1 have their ring
 * reads queued or finished, waiting to be written. They're written N at a
 * time (sectorsPerWrite), as one f_write from N consecutive slots: saveNext
 * stays a multiple of N, and N divides the slot count, so a chunk never
 * wraps round the slots. While one chunk is being written, the ring reads
 * the next into the other slots in the background (up to N = 8; at N = 16
 * the whole buffer is one chunk, so its reads wait for the write).
 *
 * The file offset of each chunk is a multiple of N x 512, and N sectors
 * divide the 32 KB cluster, so FatFS never has to split a chunk at a
 * cluster boundary: each f_write reaches the driver as one N-sector write.
 *
 * The writer runs in the main loop ahead of the FFT and keeps going while
 * there's work, for up to SD_BENCH_POLL_MS per call: it gets priority, the
 * way a recorder would have to, and the FFT gets what's left.
 */

#include "sd_bench.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "app_fatfs.h"
#include "pipeline_config.h"
#include "sd_spi.h"
#include "../Drivers/psram_ring.h"
#include "../DSP/fft.h"
#include "../DSP/playback.h"


/* One file per setting, so several runs can be checked on the PC together:
 * SB08P.RAW = 8 sectors per write, polled; SB01D.RAW = 1 sector, DMA.
 * Tools/capture.py prints the same name. */
#define SD_BENCH_FILE_FORMAT    "SB%02lu%c.RAW"
#define SD_BENCH_SLOTS          SD_BENCH_MAX_SECTORS    /* 16 x 512 B: the debug link's 8 KB buffer */
#define SD_BENCH_POLL_MS        20U         /* longest one SdBench_Poll() keeps the main loop */
#define SD_BENCH_MAX_SECONDS    600U        /* stops itself if the PC never says stop */
#define SD_BENCH_DRAIN_MS       50U         /* wait for reads in flight before closing */

/* Longest pre-trigger accepted: keeps 1 s of the ring's 16.8 s spare */
#define SD_BENCH_MAX_PRETRIGGER (PSRAM_RING_BLOCKS - 1024U)

/* After the ring overwrites blocks not yet saved, carry on this far behind */
#define SD_BENCH_RESYNC_BACKLOG (PSRAM_RING_BLOCKS / 2U)


typedef enum
{
    BENCH_IDLE = 0,
    BENCH_RUNNING,
    BENCH_STOPPING,             /* SdBench_Poll() closes the file next */
    BENCH_RESULT                /* finished; the result waits to be sent */
} BenchState_t;


/* Upper edges of the f_write time histogram's bins; the last bin is open */
static const uint32_t histogramLimitUs[SD_BENCH_HISTOGRAM_BINS - 1U] =
{
    1000U, 2000U, 5000U, 10000U, 50000U, 100000U, 250000U
};

static uint8_t state = BENCH_IDLE;
static SdBench_Result_t result;

static uint8_t *slots;                          /* lent by the caller */
static volatile uint8_t slotDone[SD_BENCH_SLOTS];

static uint32_t saveNext;                       /* next block to write to the file */
static uint32_t readNext;                       /* next block to ask the ring for */
static uint32_t sectorsPerWrite;                /* N: blocks per f_write */
static uint8_t  savedUseDma;                    /* the driver's setting before the test */
static uint8_t  caughtUp;
static uint8_t  lostPending;                    /* the ring overwrote readNext: skip once the slots drain */
static uint32_t cyclesPerUs;

static uint32_t startTick;
static uint32_t startPushed;                    /* ring blocks recorded, for the audio block counts */
static uint32_t startFftDropped;
static uint32_t catchUpTick;
static uint32_t catchUpPushed;
static uint32_t catchUpFftDropped;



uint32_t SdBench_BufferBytes(void)
{
    return SD_BENCH_SLOTS * PSRAM_RING_BLOCK_BYTES;
}


static void SdBench_Fail(SdBench_Status_t status, FRESULT fresult)
{
    result.status = status;
    result.fresult = (uint32_t)fresult;
}


/* The SD driver's diagnostics for this test (sd_spi.h) */
static void SdBench_CopyDriverState(void)
{
    result.sdError = sdLastError;
    result.sdCardStatus = sdLastCardStatus;
    result.sdBusyMaxUs = sdBusyMaxUs;
}


void SdBench_Start(void *buffer, uint32_t bufferBytes, uint32_t pretriggerBlocks, uint32_t sectors,
        uint8_t useDma)
{
    FRESULT fresult;
    uint32_t written;

    if(state != BENCH_IDLE)
    {
        return;         /* one at a time: the last result is still waiting */
    }

    memset(&result, 0, sizeof(result));
    state = BENCH_RESULT;       /* a refusal below is reported as the result */
    result.sectorsPerWrite = sectors;
    result.usesDma = useDma;

    if((buffer == NULL) || (bufferBytes < SdBench_BufferBytes()))
    {
        result.status = SD_BENCH_ERR_BUFFER;
        return;
    }

    /* a power of two up to the slot count, so chunks tile the slots */
    if((sectors == 0U) || (sectors > SD_BENCH_MAX_SECTORS) || ((sectors & (sectors - 1U)) != 0U))
    {
        result.status = SD_BENCH_ERR_SECTORS;
        return;
    }

    if(Playback_IsActive())
    {
        result.status = SD_BENCH_ERR_PLAYBACK;
        return;
    }

    if(!PsramRing_IsRecording())
    {
        result.status = SD_BENCH_ERR_RING;
        return;
    }

    /* this test's driver settings and a clean error record */
    savedUseDma = sdUseDma;
    sdUseDma = useDma;
    sdLastError = 0U;
    sdLastCardStatus = 0U;
    sdLateResponse = 0U;
    sdBusyMaxUs = 0U;
    sdHalFailures = 0U;
    sdHalFailure = 0U;
    sdStaleRx = 0U;
    sdStaleRxFirst = 0U;
    sdRecoveries = 0U;
    sdRecoveryFailures = 0U;

    /* replaces the last run's file; freeing its clusters isn't timed */
    char name[13];

    snprintf(name, sizeof(name), SD_BENCH_FILE_FORMAT, (unsigned long)sectors, useDma ? 'D' : 'P');
    fresult = f_open(&USERFile, name, FA_CREATE_ALWAYS | FA_WRITE);

    if(fresult != FR_OK)
    {
        SdBench_Fail(SD_BENCH_ERR_OPEN, fresult);
        SdBench_CopyDriverState();
        sdUseDma = savedUseDma;
        return;
    }

    slots = buffer;
    sectorsPerWrite = sectors;
    cyclesPerUs = SystemCoreClock / 1000000U;
    result.clusterSectors = USERFatFs.csize;    /* mounted by now */

    written = PsramRing_GetWrittenBlocks();

    if(pretriggerBlocks > SD_BENCH_MAX_PRETRIGGER)
    {
        pretriggerBlocks = SD_BENCH_MAX_PRETRIGGER;
    }

    if(pretriggerBlocks > written)
    {
        pretriggerBlocks = written;     /* the ring hasn't recorded that much yet */
    }

    /* start on a chunk boundary: up to N - 1 blocks more pre-trigger */
    saveNext = (written - pretriggerBlocks) & ~(sectors - 1U);
    readNext = saveNext;
    caughtUp = 0U;
    lostPending = 0U;
    result.startBacklog = written - saveNext;

    startTick = HAL_GetTick();
    startPushed = PsramRing_GetPushedBlocks();
    startFftDropped = fftDroppedBlocks;

    state = BENCH_RUNNING;
}


void SdBench_Stop(void)
{
    if(state == BENCH_RUNNING)
    {
        state = BENCH_STOPPING;
    }
}


const SdBench_Result_t *SdBench_GetResult(void)
{
    return (state == BENCH_RESULT) ? &result : NULL;
}


void SdBench_ReleaseResult(void)
{
    if(state == BENCH_RESULT)
    {
        state = BENCH_IDLE;
        slots = NULL;
    }
}


/* Write `blocks` consecutive blocks to the file in one f_write, timing it.
 * 0 = failed, and the test stops. */
static uint8_t SdBench_Write(const uint8_t *data, uint32_t blocks)
{
    UINT done = 0U;
    uint32_t bin = 0U;
    const UINT bytes = blocks * PSRAM_RING_BLOCK_BYTES;
    const uint32_t start = DWT->CYCCNT;
    const FRESULT fresult = f_write(&USERFile, data, bytes, &done);
    const uint32_t us = (DWT->CYCCNT - start) / cyclesPerUs;

    if((fresult != FR_OK) || (done != bytes))
    {
        SdBench_Fail(SD_BENCH_ERR_WRITE, (fresult != FR_OK) ? fresult : FR_DENIED);    /* FR_DENIED: card full */
        return 0U;
    }

    while((bin < (SD_BENCH_HISTOGRAM_BINS - 1U)) && (us >= histogramLimitUs[bin]))
    {
        bin++;
    }

    result.histogram[bin]++;

    if(us > result.maxWriteUs)
    {
        result.maxWriteUs = us;
    }

    if(caughtUp)
    {
        result.steadyBusyUs += us;
    }
    else
    {
        result.catchUpBusyUs += us;
    }

    result.blocksWritten += blocks;

    return 1U;
}


/* All of blocks first .. first+count-1 have arrived from the ring (or failed) */
static uint8_t SdBench_Landed(uint32_t first, uint32_t count)
{
    for(uint32_t i = 0; i < count; i++)
    {
        if(slotDone[(first + i) % SD_BENCH_SLOTS] == PSRAM_RING_PENDING)
        {
            return 0U;
        }
    }

    return 1U;
}


static void SdBench_MarkCaughtUp(void)
{
    caughtUp = 1U;
    catchUpTick = HAL_GetTick();
    catchUpPushed = PsramRing_GetPushedBlocks();
    catchUpFftDropped = fftDroppedBlocks;

    result.catchUpMs = catchUpTick - startTick;
    result.catchUpBlocks = result.blocksWritten;
}


/* Let the reads in flight land (they write into the lent buffer), then close */
static void SdBench_Finish(void)
{
    const uint32_t drainStart = HAL_GetTick();
    uint32_t now;
    FRESULT fresult;

    /* a stuck read is failed by the ring within a block period, so this is short */
    while((saveNext != readNext) && ((HAL_GetTick() - drainStart) < SD_BENCH_DRAIN_MS))
    {
        if(slotDone[saveNext % SD_BENCH_SLOTS] != PSRAM_RING_PENDING)
        {
            saveNext++;     /* landed; not saved, the test is over */
        }
    }

    now = HAL_GetTick();
    result.elapsedMs = now - startTick;

    if(caughtUp)
    {
        result.catchUpAudioBlocks = catchUpPushed - startPushed;
        result.catchUpFftDropped = catchUpFftDropped - startFftDropped;
        result.steadyMs = now - catchUpTick;
        result.steadyAudioBlocks = PsramRing_GetPushedBlocks() - catchUpPushed;
        result.steadyFftDropped = fftDroppedBlocks - catchUpFftDropped;
    }
    else
    {
        /* the whole test was spent catching up */
        result.flags |= SD_BENCH_FLAG_NEVER_CAUGHT;
        result.catchUpMs = result.elapsedMs;
        result.catchUpBlocks = result.blocksWritten;
        result.catchUpAudioBlocks = PsramRing_GetPushedBlocks() - startPushed;
        result.catchUpFftDropped = fftDroppedBlocks - startFftDropped;
    }

    fresult = f_close(&USERFile);

    if((fresult != FR_OK) && (result.status == SD_BENCH_DONE))
    {
        SdBench_Fail(SD_BENCH_ERR_CLOSE, fresult);
    }

    SdBench_CopyDriverState();
    sdUseDma = savedUseDma;
    state = BENCH_RESULT;
}


void SdBench_Poll(void)
{
    uint32_t entry;

    if(state == BENCH_STOPPING)
    {
        SdBench_Finish();
        return;
    }

    if(state != BENCH_RUNNING)
    {
        return;
    }

    entry = HAL_GetTick();

    if((entry - startTick) >= (SD_BENCH_MAX_SECONDS * 1000U))
    {
        result.flags |= SD_BENCH_FLAG_TIME_LIMIT;
        SdBench_Finish();
        return;
    }

    do
    {
        const uint32_t written = PsramRing_GetWrittenBlocks();

        if((written - saveNext) > result.maxBacklog)
        {
            result.maxBacklog = written - saveNext;
        }

        if(!PsramRing_IsRecording())
        {
            result.flags |= SD_BENCH_FLAG_RING_PAUSED;
        }

        /* Queue reads into the free slots */
        while(!lostPending && ((readNext - saveNext) < SD_BENCH_SLOTS) && (readNext != written))
        {
            const uint32_t slot = readNext % SD_BENCH_SLOTS;
            const PsramRing_ReadResult_t read = PsramRing_ReadBlock(readNext,
                    &slots[slot * PSRAM_RING_BLOCK_BYTES], &slotDone[slot]);

            if(read == PSRAM_RING_READ_BUSY)
            {
                break;              /* the ring's queue is full: try again */
            }

            if(read == PSRAM_RING_READ_INVALID)
            {
                lostPending = 1U;   /* overwritten before the writer got to it */
                break;
            }

            readNext++;
        }

        if((readNext - saveNext) >= sectorsPerWrite)
        {
            /* A whole chunk is queued: write it once it has all arrived */
            const uint32_t slot = saveNext % SD_BENCH_SLOTS;

            if(!SdBench_Landed(saveNext, sectorsPerWrite))
            {
                continue;           /* still loading */
            }

            /* a failed read leaves that block's slot stale; it's written anyway, and counted */
            for(uint32_t i = 0; i < sectorsPerWrite; i++)
            {
                if(slotDone[slot + i] == PSRAM_RING_FAILED)
                {
                    result.readFailures++;
                }
            }

            if(!SdBench_Write(&slots[slot * PSRAM_RING_BLOCK_BYTES], sectorsPerWrite))
            {
                SdBench_Finish();   /* the error is in the result */
                return;
            }

            saveNext += sectorsPerWrite;
        }
        else if(lostPending)
        {
            /* Once the reads in flight have landed, skip what the ring has
             * overwritten, part chunk included, to a chunk boundary */
            if(!SdBench_Landed(saveNext, readNext - saveNext))
            {
                continue;
            }

            const uint32_t restart = (written - SD_BENCH_RESYNC_BACKLOG) & ~(sectorsPerWrite - 1U);

            result.blocksLost += restart - saveNext;
            saveNext = restart;
            readNext = restart;
            lostPending = 0U;
        }
        else if(readNext == written)
        {
            /* Less than a chunk waiting, and nothing more to read: caught up */
            if(!caughtUp)
            {
                SdBench_MarkCaughtUp();
            }

            break;
        }

        /* otherwise the ring's read queue was full: go round again */
    } while((HAL_GetTick() - entry) < SD_BENCH_POLL_MS);
}
