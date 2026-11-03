/*
 * debug_link.c
 *
 * USB debug capture for the PC tools (Tools/capture.py). The framing, the
 * command queue and the stats commands ('N', 'T', 'Z', with the table in
 * stats_table.c) are Core/Diag/diag_link.c. This file adds the project's
 * captures, listed in debug_link.h, and their frames:
 *
 *   'I'  payload: DebugInfo_t
 *   'R'  payload: DEBUG_SNAPSHOT_SAMPLES raw 12-bit ADC codes (uint16)
 *        arg0 = block number of the first sample, arg1 = sample rate
 *   'E'  payload: arg1 x DebugEnergyRecord_t
 *        arg0 = records dropped so far because the PC fell behind
 *   'M'  payload: the last snapshot's blocks as read back from the PSRAM
 *        ring, arg0 = ring block number of the first, arg1 = sample rate.
 *        Empty, with arg0 = 0xFFFFFFFF, if they can't be read.
 *   'F'  payload: one FFT frame's arg0 raw ADC codes (uint16), then
 *        FFT_Process's arg1 magnitudes for it (float), bins 0-1 included
 *
 * One 8 KB buffer holds the snapshot, the energy FIFO or an FFT frame, so
 * starting one stops the energy stream. It's lent to the WAV recorder
 * while that records (DebugLink_LendBuffer). The audio interrupt fills the
 * buffer; the link sends a frame's payload from where it lies, and says
 * when it has gone (DebugLink_Sent), so that part of the buffer can be
 * reused.
 */


#include "debug_link.h"


/* Audio blocks since reset (the stats table's blockCount), even without the capture */
volatile uint32_t debugBlockCount = 0;

#if DEBUG_CAPTURE

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "diag_link.h"
#include "pipeline_config.h"
#include "stats_table.h"
#include "usbd_cdc_if.h"
#include "../DSP/bat_detect.h"
#include "../DSP/fft.h"
#include "../Drivers/psram_ring.h"


/* Raw snapshot: 16 blocks = 4096 samples = 16.4 ms at 250 kHz */
#define DEBUG_SNAPSHOT_BLOCKS       16U
#define DEBUG_SNAPSHOT_SAMPLES      (DEBUG_SNAPSHOT_BLOCKS * AUDIO_BLOCK_SIZE)

/* Energy records go out in batches of at least 32 blocks (~33 ms) */
#define DEBUG_ENERGY_BATCH          32U
#define DEBUG_ENERGY_FRAME_MAX      64U

#define DEBUG_PROTOCOL_VERSION      1U

/* 'M' frame when the snapshot can't be read back */
#define DEBUG_READBACK_FAILED       0xFFFFFFFFU

/* 12-bit ADC mid-scale, as in DSP_ProcessBlock */
#define ADC_MIDSCALE                2048


typedef struct
{
    uint32_t sampleRateHz;      /* AUDIO_SAMPLE_RATE_HZ */
    uint16_t blockSize;         /* AUDIO_BLOCK_SIZE */
    uint16_t snapshotBlocks;    /* DEBUG_SNAPSHOT_BLOCKS */
    uint16_t energyRecordSize;  /* sizeof(DebugEnergyRecord_t) */
    uint16_t energyRecords;     /* FIFO size: how far the PC may fall behind */
    uint32_t blocks;            /* blocks processed since reset */
    char     build[24];         /* __DATE__ " " __TIME__ */
} DebugInfo_t;

/* One per audio block while streaming */
typedef struct
{
    uint32_t block;             /* block number since reset */
    float    energy;            /* 10 kHz high-pass energy, as fed to the detector */
    float    total;             /* full-band energy with the block mean removed */
    float    floorDb;           /* detector noise floor after this block */
    uint16_t pulses;            /* batPulses, low 16 bits */
    uint8_t  noiseRuns;         /* batNoiseRuns, low 8 bits */
    uint8_t  flags;             /* bit 0: bat present */
} DebugEnergyRecord_t;

_Static_assert(sizeof(DebugInfo_t) == 40U, "info payload must match Tools/capture.py");
_Static_assert(sizeof(DebugEnergyRecord_t) == 20U, "energy record must match Tools/capture.py");

/* sumSquares in DebugLink_BlockEnergy holds up to AUDIO_BLOCK_SIZE x 2048^2 */
_Static_assert(AUDIO_BLOCK_SIZE <= 512U, "block energy sum would overflow 32 bits");

/* The energy FIFO shares the snapshot's buffer: 409 records, ~0.4 s */
#define DEBUG_ENERGY_RECORDS \
    ((DEBUG_SNAPSHOT_SAMPLES * sizeof(uint16_t)) / sizeof(DebugEnergyRecord_t))

typedef union
{
    uint16_t            samples[DEBUG_SNAPSHOT_SAMPLES];
    DebugEnergyRecord_t records[DEBUG_ENERGY_RECORDS];
    struct
    {
        uint16_t raw[FFT_LENGTH];           /* one FFT frame's ADC codes, in order */
        float    magnitude[FFT_NUM_BINS];   /* FFT_Process's result for it, bins 0-1 included */
    } fft;
} DebugCaptureBuffer_t;

/* the FFT capture mustn't make the buffer any bigger */
_Static_assert(sizeof(((DebugCaptureBuffer_t *)0)->fft) <= sizeof(((DebugCaptureBuffer_t *)0)->samples),
        "FFT capture doesn't fit the snapshot buffer");

_Static_assert(sizeof(DebugCaptureBuffer_t) == DEBUG_LINK_BUFFER_BYTES, "the lent buffer's size is in debug_link.h");

typedef enum
{
    DEBUG_MODE_IDLE = 0,
    DEBUG_MODE_SNAPSHOT,        /* the interrupt is filling the snapshot */
    DEBUG_MODE_SNAPSHOT_READY,  /* full; the main loop sends it */
    DEBUG_MODE_ENERGY,          /* the interrupt adds an energy record per block */
    DEBUG_MODE_READBACK,        /* the PSRAM ring is reading the snapshot back */
    DEBUG_MODE_FFT_CAPTURE,     /* the FFT is capturing a frame and its result */
    DEBUG_MODE_LENT             /* lent to the WAV recorder */
} DebugMode_t;


extern USBD_HandleTypeDef hUsbDeviceFS;

static DebugCaptureBuffer_t captureBuffer;

/* Shared with the audio interrupt */
static volatile uint8_t  mode = DEBUG_MODE_IDLE;
static volatile uint32_t snapshotBlocks = 0;
static volatile uint32_t snapshotStartBlock = 0;
static volatile uint32_t snapshotRingBlock = 0;     /* PSRAM ring number of its first block */
static volatile uint8_t  snapshotInRing = 0;
static volatile uint32_t energyWrite = 0;           /* the interrupt adds records here */
static volatile uint32_t energyRead = 0;            /* the main loop sends from here */
static volatile uint32_t energyDropped = 0;

/* Main loop only */
static DebugInfo_t txInfo;
static uint32_t txRecords = 0;                      /* energy records in the frame being sent */
static uint32_t readbackNext = 0;                   /* next snapshot block to ask the ring for */
static volatile uint8_t readbackDone[DEBUG_SNAPSHOT_BLOCKS];


/*
 * Full-band energy of one raw block with its mean removed. It's on the same
 * scale as the high-pass energy (both are ADC codes about mid-scale), so
 * energy / total is the fraction of the block's energy above 10 kHz.
 */
static float DebugLink_BlockEnergy(const uint16_t *input)
{
    int32_t sum = 0;
    uint32_t sumSquares = 0;

    for(uint32_t i = 0; i < AUDIO_BLOCK_SIZE; i++)
    {
        const int32_t sample = (int32_t)input[i] - ADC_MIDSCALE;

        sum += sample;
        sumSquares += (uint32_t)(sample * sample);
    }

    return (float)sumSquares - ((float)sum * (float)sum) / (float)AUDIO_BLOCK_SIZE;
}


void DebugLink_AddBlock(const uint16_t *input, uint32_t length, float energy)
{
    const uint32_t block = debugBlockCount++;

    if(length != AUDIO_BLOCK_SIZE)
    {
        return;
    }

    if(mode == DEBUG_MODE_SNAPSHOT)
    {
        const uint32_t filled = snapshotBlocks;

        if(filled == 0U)
        {
            snapshotStartBlock = block;

            /* the ring took this block just before the DSP ran */
            snapshotRingBlock = PsramRing_GetPushedBlocks() - 1U;
            snapshotInRing = PsramRing_IsRecording();     /* not while playback has it paused */
        }

        memcpy(&captureBuffer.samples[filled * AUDIO_BLOCK_SIZE], input,
                AUDIO_BLOCK_SIZE * sizeof(uint16_t));

        snapshotBlocks = filled + 1U;

        if(snapshotBlocks == DEBUG_SNAPSHOT_BLOCKS)
        {
            mode = DEBUG_MODE_SNAPSHOT_READY;
        }
    }
    else if(mode == DEBUG_MODE_ENERGY)
    {
        const uint32_t write = energyWrite;
        uint32_t next = write + 1U;

        if(next == DEBUG_ENERGY_RECORDS)
        {
            next = 0U;
        }

        if(next == energyRead)
        {
            energyDropped++;        /* FIFO full: the PC isn't keeping up */
            return;
        }

        DebugEnergyRecord_t *record = &captureBuffer.records[write];

        record->block = block;
        record->energy = energy;
        record->total = DebugLink_BlockEnergy(input);
        record->floorDb = batFloorDb;
        record->pulses = (uint16_t)batPulses;
        record->noiseRuns = (uint8_t)batNoiseRuns;
        record->flags = batPresent ? 1U : 0U;

        energyWrite = next;
    }
}


void DebugLink_OnReceive(const uint8_t *data, uint32_t length)
{
    DiagLink_OnReceive(data, length);
}


/* Send the oldest energy records once a batch has built up */
static void DebugLink_StartEnergyFrame(void)
{
    const uint32_t read = energyRead;
    const uint32_t write = energyWrite;
    uint32_t available;
    uint32_t count;

    if(write >= read)
    {
        available = write - read;
        count = available;
    }
    else
    {
        available = DEBUG_ENERGY_RECORDS - read + write;
        count = DEBUG_ENERGY_RECORDS - read;    /* up to the end of the buffer */
    }

    if(available < DEBUG_ENERGY_BATCH)
    {
        return;
    }

    if(count > DEBUG_ENERGY_FRAME_MAX)
    {
        count = DEBUG_ENERGY_FRAME_MAX;
    }

    txRecords = count;

    (void)DiagLink_StartFrame('E', &captureBuffer.records[read],
            count * sizeof(DebugEnergyRecord_t), energyDropped, count);
}


/*
 * Ask the ring for the snapshot's blocks as its queue allows, then send
 * them once all have arrived. Nothing else uses the buffer meanwhile.
 */
static void DebugLink_ContinueReadback(void)
{
    while(readbackNext < DEBUG_SNAPSHOT_BLOCKS)
    {
        const PsramRing_ReadResult_t result = PsramRing_ReadBlock(snapshotRingBlock + readbackNext,
                &captureBuffer.samples[readbackNext * AUDIO_BLOCK_SIZE], &readbackDone[readbackNext]);

        if(result == PSRAM_RING_READ_BUSY)
        {
            return;     /* queue full: carry on next time */
        }

        if(result == PSRAM_RING_READ_INVALID)
        {
            (void)DiagLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
            return;
        }

        readbackNext++;
    }

    for(uint32_t i = 0; i < DEBUG_SNAPSHOT_BLOCKS; i++)
    {
        if(readbackDone[i] == PSRAM_RING_PENDING)
        {
            return;
        }

        if(readbackDone[i] == PSRAM_RING_FAILED)
        {
            (void)DiagLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
            return;
        }
    }

    (void)DiagLink_StartFrame('M', captureBuffer.samples, sizeof(captureBuffer.samples),
            snapshotRingBlock, AUDIO_SAMPLE_RATE_HZ);
}


/* ---- Commands, acted on between frames ---- */

/* 'I': sample rate, block size, build time */
static void DebugLink_Info(void)
{
    txInfo.sampleRateHz = AUDIO_SAMPLE_RATE_HZ;
    txInfo.blockSize = AUDIO_BLOCK_SIZE;
    txInfo.snapshotBlocks = DEBUG_SNAPSHOT_BLOCKS;
    txInfo.energyRecordSize = sizeof(DebugEnergyRecord_t);
    txInfo.energyRecords = DEBUG_ENERGY_RECORDS;
    txInfo.blocks = debugBlockCount;
    snprintf(txInfo.build, sizeof(txInfo.build), "%s %s", __DATE__, __TIME__);

    (void)DiagLink_StartFrame('I', &txInfo, sizeof(txInfo), 0U, 0U);
}

/* 'R': a raw snapshot. Takes over the energy stream's buffer; ignored if a snapshot is under way. */
static void DebugLink_Snapshot(void)
{
    if((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY))
    {
        snapshotBlocks = 0U;
        mode = DEBUG_MODE_SNAPSHOT;
    }
}

/* 'E': stream one energy record per block */
static void DebugLink_Energy(void)
{
    if((mode == DEBUG_MODE_FFT_CAPTURE) || (mode == DEBUG_MODE_LENT))
    {
        return;     /* the buffer is taken */
    }

    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    energyWrite = 0U;
    energyRead = 0U;
    energyDropped = 0U;
    mode = DEBUG_MODE_ENERGY;

    __set_PRIMASK(primask);
}

/* 'S': stop a stream or a capture (the recorder keeps a lent buffer) */
static void DebugLink_Stop(void)
{
    if(mode != DEBUG_MODE_LENT)
    {
        FFT_CancelCapture();
        mode = DEBUG_MODE_IDLE;
    }
}

/* 'F': capture the next FFT frame and FFT_Process's result for it. Stops an energy stream, like 'R'. */
static void DebugLink_FftCapture(void)
{
    if((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY))
    {
        mode = DEBUG_MODE_FFT_CAPTURE;
        FFT_RequestCapture(captureBuffer.fft.raw, captureBuffer.fft.magnitude);
    }
}

/* 'M': read the last snapshot back from the PSRAM ring. Stops an energy stream, like 'R'. */
static void DebugLink_Readback(void)
{
    if(snapshotInRing && ((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY)))
    {
        readbackNext = 0U;
        mode = DEBUG_MODE_READBACK;
    }
    else
    {
        (void)DiagLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
    }
}

static const DiagLinkCommand_t commands[] =
{
    { 'I', DebugLink_Info },
    { 'R', DebugLink_Snapshot },
    { 'E', DebugLink_Energy },
    { 'S', DebugLink_Stop },
    { 'F', DebugLink_FftCapture },
    { 'M', DebugLink_Readback },
};


/* ---- Link hooks ---- */

/* Between frames: send whatever is ready */
static void DebugLink_Idle(void)
{
    if(mode == DEBUG_MODE_SNAPSHOT_READY)
    {
        (void)DiagLink_StartFrame('R', captureBuffer.samples, sizeof(captureBuffer.samples),
                snapshotStartBlock, AUDIO_SAMPLE_RATE_HZ);
    }
    else if(mode == DEBUG_MODE_ENERGY)
    {
        DebugLink_StartEnergyFrame();
    }
    else if(mode == DEBUG_MODE_READBACK)
    {
        DebugLink_ContinueReadback();
    }
    else if((mode == DEBUG_MODE_FFT_CAPTURE) && FFT_CaptureDone())
    {
        (void)DiagLink_StartFrame('F', &captureBuffer.fft, sizeof(captureBuffer.fft), FFT_LENGTH, FFT_NUM_BINS);
    }
}

/* A frame's payload has gone: release its part of the buffer */
static void DebugLink_Sent(uint8_t type)
{
    if((type == 'R') || ((type == 'M') && (mode == DEBUG_MODE_READBACK)))
    {
        mode = DEBUG_MODE_IDLE;
    }
    else if(type == 'E')
    {
        uint32_t read = energyRead + txRecords;

        if(read >= DEBUG_ENERGY_RECORDS)
        {
            read -= DEBUG_ENERGY_RECORDS;
        }

        energyRead = read;
    }
    else if((type == 'F') && (mode == DEBUG_MODE_FFT_CAPTURE))
    {
        mode = DEBUG_MODE_IDLE;
    }
}

/* Unplugged, suspended or not yet enumerated: drop whatever was going on.
 * Runs on every pass while USB is away, so the recorder keeps a lent buffer. */
static void DebugLink_Disconnected(void)
{
    if(mode != DEBUG_MODE_LENT)
    {
        FFT_CancelCapture();
        mode = DEBUG_MODE_IDLE;
    }
}

/* The transport: USB CDC, which reports busy until the last transfer has finished */
static uint8_t DebugLink_UsbSend(const void *data, uint32_t length)
{
    return CDC_Transmit_FS((uint8_t *)data, (uint16_t)length) == USBD_OK;
}

static uint8_t DebugLink_UsbConnected(void)
{
    return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED;
}

static const DiagLinkConfig_t linkConfig =
{
    .magic = { 'B', 'A', 'T', 'D' },
    .version = DEBUG_PROTOCOL_VERSION,
    .send = DebugLink_UsbSend,
    .connected = DebugLink_UsbConnected,
    .uptimeMs = HAL_GetTick,
    .commands = commands,
    .commandCount = sizeof(commands) / sizeof(commands[0]),
    .idle = DebugLink_Idle,
    .sent = DebugLink_Sent,
    .disconnected = DebugLink_Disconnected,
    .stats = &statsTable,
};


void DebugLink_Init(void)
{
    DiagLink_Init(&linkConfig);
}


void DebugLink_Poll(void)
{
    DiagLink_Poll();
}


void *DebugLink_LendBuffer(void)
{
    /* an energy frame on its way out is still being read from the buffer */
    const uint8_t sendingEnergy = (DiagLink_SendingType() == 'E');

    if((mode == DEBUG_MODE_IDLE) || ((mode == DEBUG_MODE_ENERGY) && !sendingEnergy))
    {
        /* stops an energy stream: the audio interrupt checks the mode before
         * touching the buffer, and can't be part-way through while this runs */
        mode = DEBUG_MODE_LENT;
        return &captureBuffer;
    }

    return NULL;
}


void DebugLink_ReturnBuffer(void)
{
    if(mode == DEBUG_MODE_LENT)
    {
        mode = DEBUG_MODE_IDLE;
    }
}


#else /* DEBUG_CAPTURE */

/* The debug link is left out, but the recorder still needs the buffer */
static uint32_t lendBuffer[DEBUG_LINK_BUFFER_BYTES / sizeof(uint32_t)];

void *DebugLink_LendBuffer(void)
{
    return lendBuffer;
}

void DebugLink_ReturnBuffer(void)
{
}

#endif /* DEBUG_CAPTURE */
