/*
 * sd_bench.h
 *
 * Sustained SD write test under recording load (diary 38). Copies the
 * PSRAM ring's live audio into SBnnX.RAW (nn sectors per write, X = P for
 * polled or D for DMA, e.g. SB08P.RAW), N 512-byte blocks per f_write
 * (N = 1, 2, 4, 8 or 16), while the audio, the ring, the FFT and the
 * waterfall all keep running. With N > 1 FatFS hands the driver N sectors
 * at once, which it sends as one multi-block write (CMD25). It starts a pre-trigger's length behind the ring, like
 * a recording would, so the first phase measures the fastest rate the
 * writer manages under load and the second how busy it is once caught up.
 *
 * Started and stopped over the debug link (command 'W', then 'S'); the
 * result goes back as a 'W' frame. Tools/capture.py sdtest runs it.
 *
 * The file is raw little-endian uint16 ADC codes at AUDIO_SAMPLE_RATE_HZ,
 * with no header.
 */

#ifndef SD_BENCH_H_
#define SD_BENCH_H_

#include <stdint.h>


typedef enum
{
    SD_BENCH_DONE = 0,          /* stopped by the PC or the time limit */
    SD_BENCH_ERR_BUFFER,        /* the buffer lent to it is too small */
    SD_BENCH_ERR_PLAYBACK,      /* playback has the ring */
    SD_BENCH_ERR_RING,          /* the PSRAM ring isn't recording */
    SD_BENCH_ERR_OPEN,          /* f_open failed: no card, or not FAT */
    SD_BENCH_ERR_WRITE,         /* f_write failed, or the card is full */
    SD_BENCH_ERR_CLOSE,         /* f_close failed */
    SD_BENCH_ERR_SECTORS        /* sectors per write isn't 1, 2, 4, 8 or 16 */
} SdBench_Status_t;

/* Most blocks per f_write: the slots the lent buffer holds */
#define SD_BENCH_MAX_SECTORS        16U

/* SdBench_Result_t.flags */
#define SD_BENCH_FLAG_RING_PAUSED   0x01U   /* the ring paused during the test (PLAYBACK pressed) */
#define SD_BENCH_FLAG_TIME_LIMIT    0x02U   /* stopped by SD_BENCH_MAX_SECONDS, not the PC */
#define SD_BENCH_FLAG_NEVER_CAUGHT  0x04U   /* never cleared the starting backlog */

/* f_write time histogram: < 1, 1-2, 2-5, 5-10, 10-50, 50-100, 100-250, >= 250 ms */
#define SD_BENCH_HISTOGRAM_BINS     8U

/* Sent as the 'W' frame's payload: must match Tools/capture.py */
typedef struct
{
    uint32_t status;                /* SdBench_Status_t */
    uint32_t fresult;               /* FatFS result of the call that failed, else 0 */
    uint32_t flags;                 /* SD_BENCH_FLAG_... */
    uint32_t elapsedMs;             /* first write to stop */
    uint32_t blocksWritten;         /* 512-byte blocks saved to the file */
    uint32_t blocksLost;            /* overwritten in the ring before they were saved */
    uint32_t readFailures;          /* ring reads that failed (skipped, not saved) */
    uint32_t startBacklog;          /* blocks behind the ring at the start: the pre-trigger */
    uint32_t maxBacklog;            /* most blocks waiting to be saved at any time */
    uint32_t catchUpMs;             /* time to clear the starting backlog */
    uint32_t catchUpBlocks;         /* blocks saved meanwhile */
    uint32_t catchUpAudioBlocks;    /* audio blocks recorded meanwhile */
    uint32_t catchUpFftDropped;     /* of those, blocks the FFT dropped */
    uint32_t steadyMs;              /* time after catching up */
    uint32_t steadyBusyUs;          /* of that, time spent inside f_write */
    uint32_t steadyAudioBlocks;     /* audio blocks recorded after catching up */
    uint32_t steadyFftDropped;      /* of those, blocks the FFT dropped */
    uint32_t maxWriteUs;            /* slowest f_write */
    uint32_t sectorsPerWrite;       /* blocks per f_write: N */
    uint32_t clusterSectors;        /* the card's cluster size; FatFS splits a write that crosses one */
    uint32_t catchUpBusyUs;         /* time inside f_write while catching up */
    uint32_t usesDma;               /* 1: data blocks by DMA, 0: polled */
    uint32_t sdError;               /* sdLastError at the end (sd_spi.h), 0 if none during the test */
    uint32_t sdCardStatus;          /* sdLastCardStatus at the end */
    uint32_t sdBusyMaxUs;           /* longest busy wait during the test */
    uint32_t histogram[SD_BENCH_HISTOGRAM_BINS];    /* per f_write call */
} SdBench_Result_t;

_Static_assert(sizeof(SdBench_Result_t) == 132U, "SD bench result must match Tools/capture.py");


/*
 * Start the test, using buffer (at least SdBench_BufferBytes()) for the
 * blocks read back from the ring. The buffer stays in use until the result
 * has been released. Starts about pretriggerBlocks behind the ring's newest
 * block and writes sectorsPerWrite blocks per f_write, with the SD driver's
 * data blocks by DMA or polled (useDma) for the test's duration. Clears the
 * driver's error record and busy maximum first. A refusal is reported as a
 * result with an error status.
 */
void SdBench_Start(void *buffer, uint32_t bufferBytes, uint32_t pretriggerBlocks, uint32_t sectorsPerWrite,
        uint8_t useDma);

/* Ask the test to stop; the result follows from SdBench_Poll() */
void SdBench_Stop(void);

uint32_t SdBench_BufferBytes(void);

/* Main loop, before FFT_Process: does the work, for up to ~20 ms per call */
void SdBench_Poll(void);

/* The finished test's result, or NULL while it's running or idle */
const SdBench_Result_t *SdBench_GetResult(void);

/* The result has been sent: the buffer is free again */
void SdBench_ReleaseResult(void);


#endif /* SD_BENCH_H_ */
