/*
 * debug_link.c
 *
 * Debug data capture over the USB virtual COM port. The commands are
 * listed in debug_link.h.
 *
 * Every reply is one frame, all fields little-endian:
 *
 *   header   20 bytes  DebugFrameHeader_t: "BATD", type, version,
 *                      sequence, payload length, arg0, arg1
 *   payload  length bytes
 *   crc      4 bytes   CRC-32 of header + payload (the same as Python's
 *                      zlib.crc32)
 *
 *   'I'  payload: DebugInfo_t
 *   'R'  payload: DEBUG_SNAPSHOT_SAMPLES raw 12-bit ADC codes (uint16)
 *        arg0 = block number of the first sample, arg1 = sample rate
 *   'E'  payload: arg1 x DebugEnergyRecord_t
 *        arg0 = records dropped so far because the PC fell behind
 *   'M'  payload: the last snapshot's blocks as read back from the PSRAM
 *        ring, arg0 = ring block number of the first, arg1 = sample rate.
 *        Empty, with arg0 = 0xFFFFFFFF, if they can't be read.
 *   'W'  payload: SdBench_Result_t (sd_bench.h), sent when the SD write
 *        test ends
 *   'N'  payload: the stats table, arg0 entries of arg1 (32) bytes: name
 *        (27 chars, NUL-padded), type ('u' uint32, 'b' uint8, 'f' float),
 *        then 4 bytes the PC ignores
 *   'T'  payload: arg0 stat values in table order, 4 bytes each (uint8s
 *        widened, floats as their bits), arg1 = uptime in ms
 *   'F'  payload: one FFT frame's arg0 raw ADC codes (uint16), then
 *        FFT_Process's arg1 magnitudes for it (float), bins 0-1 included
 *
 * One 8 KB buffer holds the snapshot, the energy FIFO or the SD write
 * test's blocks, so starting one stops the energy stream. It's lent to the
 * WAV recorder while that records (DebugLink_LendBuffer).
 *
 * The audio interrupt fills the buffer and the main loop sends it with
 * CDC_Transmit_FS, which reports busy until the previous transfer has
 * finished. A frame goes out as three transfers (header, payload, CRC),
 * so once the CRC transfer is accepted the payload has been sent and that
 * part of the buffer can be reused. Commands are only acted on between
 * frames, so a command never changes the buffer while it is being sent.
 */


#include "debug_link.h"

#if DEBUG_CAPTURE

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "pipeline_config.h"
#include "recorder.h"
#include "sd_bench.h"
#include "sd_spi.h"
#include "usbd_cdc_if.h"
#include "../DSP/autotune.h"
#include "../DSP/bat_detect.h"
#include "../DSP/fft.h"
#include "../DSP/playback.h"
#include "../Drivers/adc.h"
#include "../Drivers/dac.h"
#include "../Drivers/psram_ring.h"


/* Raw snapshot: 16 blocks = 4096 samples = 16.4 ms at 250 kHz */
#define DEBUG_SNAPSHOT_BLOCKS       16U
#define DEBUG_SNAPSHOT_SAMPLES      (DEBUG_SNAPSHOT_BLOCKS * AUDIO_BLOCK_SIZE)

/* Energy records go out in batches of at least 32 blocks (~33 ms) */
#define DEBUG_ENERGY_BATCH          32U
#define DEBUG_ENERGY_FRAME_MAX      64U

#define DEBUG_PROTOCOL_VERSION      1U
#define DEBUG_COMMAND_QUEUE         16U

/* 'M' frame when the snapshot can't be read back */
#define DEBUG_READBACK_FAILED       0xFFFFFFFFU

/* 12-bit ADC mid-scale, as in DSP_ProcessBlock */
#define ADC_MIDSCALE                2048

/* 'W' is followed by three bytes: the SD test's pre-trigger in seconds, its
 * sectors per write, and 1/0 for DMA or polled. A larger byte isn't a
 * parameter. */
#define DEBUG_SD_BENCH_MAX_PRETRIGGER_S     15U
#define DEBUG_SD_BENCH_PARAMS               3U
#define DEBUG_SD_BENCH_PARAM_MAX            16U


typedef struct
{
    char     magic[4];          /* "BATD" */
    uint8_t  type;              /* 'I', 'R' or 'E' */
    uint8_t  version;           /* DEBUG_PROTOCOL_VERSION */
    uint16_t sequence;          /* +1 per frame, so the PC can spot lost frames */
    uint32_t length;            /* payload bytes */
    uint32_t arg0;              /* per frame type, see above */
    uint32_t arg1;
} DebugFrameHeader_t;

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

_Static_assert(sizeof(DebugFrameHeader_t) == 20U, "frame header must match Tools/capture.py");
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
    DEBUG_MODE_SD_BENCH,        /* the SD write test has the buffer */
    DEBUG_MODE_FFT_CAPTURE,     /* the FFT is capturing a frame and its result */
    DEBUG_MODE_LENT             /* lent to the WAV recorder */
} DebugMode_t;

typedef enum
{
    TX_IDLE = 0,
    TX_HEADER,
    TX_PAYLOAD,
    TX_CRC
} DebugTxStage_t;


extern USBD_HandleTypeDef hUsbDeviceFS;

static DebugCaptureBuffer_t captureBuffer;

/* Shared with the audio interrupt */
static volatile uint8_t  mode = DEBUG_MODE_IDLE;
static volatile uint32_t blockCount = 0;
static volatile uint32_t snapshotBlocks = 0;
static volatile uint32_t snapshotStartBlock = 0;
static volatile uint32_t snapshotRingBlock = 0;     /* PSRAM ring number of its first block */
static volatile uint8_t  snapshotInRing = 0;
static volatile uint32_t energyWrite = 0;       /* the interrupt adds records here */
static volatile uint32_t energyRead = 0;        /* the main loop sends from here */
static volatile uint32_t energyDropped = 0;

/* Shared with the USB interrupt */
static volatile uint8_t commandQueue[DEBUG_COMMAND_QUEUE];
static volatile uint8_t commandHead = 0;        /* the USB interrupt writes here */
static volatile uint8_t commandTail = 0;        /* the main loop reads from here */

/* Main loop only */
static DebugFrameHeader_t txHeader;
static DebugInfo_t txInfo;
static const void *txPayload;
static uint32_t txCrc;
static uint8_t txCrcBytes[4];
static uint8_t txStage = TX_IDLE;
static uint16_t txSequence = 0;
static uint32_t txRecords = 0;                  /* energy records in the frame being sent */
static uint32_t readbackNext = 0;               /* next snapshot block to ask the ring for */
static volatile uint8_t readbackDone[DEBUG_SNAPSHOT_BLOCKS];
static uint8_t pendingCommand = 0;              /* a command collecting its parameter bytes */
static uint8_t pendingCount = 0;
static uint8_t pendingParams[DEBUG_SD_BENCH_PARAMS];


/*
 * Timing and health counters, sent by 'N' (this table, from flash) and 'T'
 * (the values). The names match the variables, as Live Expressions shows
 * them. Add an entry here and Tools/capture.py stats picks it up.
 */
typedef struct
{
    char name[27];
    char type;                      /* 'u' uint32_t, 'b' uint8_t, 'f' float */
    const volatile void *value;
} DebugStat_t;

_Static_assert(sizeof(DebugStat_t) == 32U, "stat entry must match Tools/capture.py");

static const DebugStat_t debugStats[] =
{
    { "SystemCoreClock",        'u', &SystemCoreClock },
    { "blockCount",             'u', &blockCount },             /* audio blocks since reset */

    /* Audio interrupt (adc.c): worst-case DSP_ProcessBlock */
    { "dspCyclesMax",           'u', &dspCyclesMax },
    { "dspLoadMaxPercent",      'u', &dspLoadMaxPercent },
    { "dacLateBlocks",          'u', &dacLateBlocks },

    /* Main loop: FFT_Process */
    { "fftProcessCyclesAvg",    'u', &fftProcessCyclesAvg },
    { "fftProcessCyclesMax",    'u', &fftProcessCyclesMax },
    { "fftDroppedBlocks",       'u', &fftDroppedBlocks },

    /* PSRAM ring */
    { "psramRingBlocks",        'u', &psramRingBlocks },
    { "psramRingReads",         'u', &psramRingReads },
    { "psramRingBursts",        'u', &psramRingBursts },
    { "psramRingMaxSelectNs",   'u', &psramRingMaxSelectNs },
    { "psramRingMinSelectNs",   'u', &psramRingMinSelectNs },
    { "psramRingMaxEntryNs",    'u', &psramRingMaxEntryNs },
    { "psramRingMaxIsrNs",      'u', &psramRingMaxIsrNs },
    { "psramRingOverTcem",      'u', &psramRingOverTcem },
    { "psramRingLateBlocks",    'u', &psramRingLateBlocks },
    { "psramRingFaults",        'u', &psramRingFaults },

    /* Playback */
    { "playbackUnderruns",      'u', &playbackUnderruns },

    /* SD card driver (sd_spi.h) */
    { "sdErrorCount",           'u', &sdErrorCount },
    { "sdLastError",            'u', &sdLastError },
    { "sdLastCardStatus",       'u', &sdLastCardStatus },
    { "sdLateResponse",         'u', &sdLateResponse },
    { "sdBusyMaxUs",            'u', &sdBusyMaxUs },
    { "sdHalFailures",          'u', &sdHalFailures },
    { "sdHalFailure",           'u', &sdHalFailure },
    { "sdStaleRx",              'u', &sdStaleRx },
    { "sdStaleRxFirst",         'u', &sdStaleRxFirst },
    { "sdUseDma",               'b', &sdUseDma },
    { "sdRecoveries",           'u', &sdRecoveries },
    { "sdRecoveryFailures",     'u', &sdRecoveryFailures },

    /* WAV recorder (recorder.h); all but recFile are for the current or last recording */
    { "recFile",                'u', &recFile },
    { "recBlocks",              'u', &recBlocks },
    { "recMaxBacklog",          'u', &recMaxBacklog },
    { "recMaxWriteUs",          'u', &recMaxWriteUs },
    { "recMaxSyncUs",           'u', &recMaxSyncUs },
    { "recReadFailures",        'u', &recReadFailures },
    { "recLastEnd",             'u', &recLastEnd },

    /* Auto-tune (autotune.h) */
    { "autoTuneCalls",          'u', &autoTuneCalls },
    { "autoTuneRetunes",        'u', &autoTuneRetunes },
    { "autoTuneLastHz",         'u', &autoTuneLastHz },

    /* Bat detector */
    { "batEnergyDb",            'f', &batEnergyDb },
    { "batFloorDb",             'f', &batFloorDb },
    { "batPulses",              'u', &batPulses },
    { "batTriggers",            'u', &batTriggers },
    { "batNoiseRuns",           'u', &batNoiseRuns },
    { "batPresent",             'b', &batPresent },
};

#define DEBUG_STAT_COUNT    (sizeof(debugStats) / sizeof(debugStats[0]))

static uint32_t statValues[DEBUG_STAT_COUNT];  /* the 'T' payload */



/* CRC-32 as zlib computes it (reflected polynomial 0xEDB88320), 4 bits at a time */
static uint32_t DebugLink_Crc32(uint32_t crc, const void *data, uint32_t length)
{
    static const uint32_t table[16] =
    {
        0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
        0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
        0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
        0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
    };
    const uint8_t *bytes = data;

    crc = ~crc;

    while(length > 0U)
    {
        crc ^= *bytes++;
        crc = (crc >> 4) ^ table[crc & 0x0FU];
        crc = (crc >> 4) ^ table[crc & 0x0FU];
        length--;
    }

    return ~crc;
}


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
    const uint32_t block = blockCount++;

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
    for(uint32_t i = 0; i < length; i++)
    {
        const uint8_t next = (uint8_t)((commandHead + 1U) % DEBUG_COMMAND_QUEUE);

        if(next == commandTail)
        {
            break;      /* queue full: drop the rest */
        }

        commandQueue[commandHead] = data[i];
        commandHead = next;
    }
}



static void DebugLink_StartFrame(uint8_t type, const void *payload, uint32_t length,
        uint32_t arg0, uint32_t arg1)
{
    memcpy(txHeader.magic, "BATD", sizeof(txHeader.magic));
    txHeader.type = type;
    txHeader.version = DEBUG_PROTOCOL_VERSION;
    txHeader.sequence = txSequence++;
    txHeader.length = length;
    txHeader.arg0 = arg0;
    txHeader.arg1 = arg1;

    txPayload = payload;
    txCrc = DebugLink_Crc32(0U, &txHeader, sizeof(txHeader));
    txCrc = DebugLink_Crc32(txCrc, payload, length);
    txStage = TX_HEADER;
}


/* Start one USB transfer. Fails while the previous one is still going. */
static uint8_t DebugLink_Send(const void *data, uint32_t length)
{
    return CDC_Transmit_FS((uint8_t *)data, (uint16_t)length) == USBD_OK;
}


/* The frame's payload has been sent, so release its part of the buffer */
static void DebugLink_FrameSent(void)
{
    if((txHeader.type == 'R') || ((txHeader.type == 'M') && (mode == DEBUG_MODE_READBACK)))
    {
        mode = DEBUG_MODE_IDLE;
    }
    else if(txHeader.type == 'E')
    {
        uint32_t read = energyRead + txRecords;

        if(read >= DEBUG_ENERGY_RECORDS)
        {
            read -= DEBUG_ENERGY_RECORDS;
        }

        energyRead = read;
    }
    else if(txHeader.type == 'W')
    {
        SdBench_ReleaseResult();        /* the SD test hands the buffer back */
        mode = DEBUG_MODE_IDLE;
    }
    else if((txHeader.type == 'F') && (mode == DEBUG_MODE_FFT_CAPTURE))
    {
        mode = DEBUG_MODE_IDLE;
    }
}


static void DebugLink_ContinueFrame(void)
{
    switch(txStage)
    {
    case TX_HEADER:
        if(DebugLink_Send(&txHeader, sizeof(txHeader)))
        {
            txStage = (txHeader.length > 0U) ? TX_PAYLOAD : TX_CRC;
        }
        break;

    case TX_PAYLOAD:
        if(DebugLink_Send(txPayload, txHeader.length))
        {
            txStage = TX_CRC;
        }
        break;

    case TX_CRC:
        /* safe to write: the transfer in flight, if any, is this frame's payload */
        memcpy(txCrcBytes, &txCrc, sizeof(txCrcBytes));

        if(DebugLink_Send(txCrcBytes, sizeof(txCrcBytes)))
        {
            DebugLink_FrameSent();
            txStage = TX_IDLE;
        }
        break;

    default:
        break;
    }
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

    DebugLink_StartFrame('E', &captureBuffer.records[read],
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
            DebugLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
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
            DebugLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
            return;
        }
    }

    DebugLink_StartFrame('M', captureBuffer.samples, sizeof(captureBuffer.samples),
            snapshotRingBlock, AUDIO_SAMPLE_RATE_HZ);
}


/* Copy the stats' current values into the 'T' payload */
static void DebugLink_SnapshotStats(void)
{
    for(uint32_t i = 0; i < DEBUG_STAT_COUNT; i++)
    {
        const volatile void *value = debugStats[i].value;

        if(debugStats[i].type == 'b')
        {
            statValues[i] = *(const volatile uint8_t *)value;
        }
        else if(debugStats[i].type == 'f')
        {
            const float number = *(const volatile float *)value;

            memcpy(&statValues[i], &number, sizeof(statValues[i]));
        }
        else
        {
            statValues[i] = *(const volatile uint32_t *)value;
        }
    }
}


/* Restart the worst-case timings, so the next reading covers only what follows */
static void DebugLink_ResetTimings(void)
{
    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    /* the audio interrupt updates these two together */
    dspCyclesMax = 0U;
    dspLoadMaxPercent = 0U;

    __set_PRIMASK(primask);

    fftProcessCyclesMax = 0U;       /* main loop, like this */
    sdBusyMaxUs = 0U;
    psramRingResetStats = 1U;       /* the ring's interrupt clears its own on the next burst */
}


static void DebugLink_StartSdBench(uint8_t pretriggerSeconds, uint8_t sectorsPerWrite, uint8_t useDma)
{
    /* takes the buffer, so it stops an energy stream; not while a snapshot or read-back has it */
    if((mode != DEBUG_MODE_IDLE) && (mode != DEBUG_MODE_ENERGY))
    {
        return;
    }

    if(pretriggerSeconds > DEBUG_SD_BENCH_MAX_PRETRIGGER_S)
    {
        pretriggerSeconds = DEBUG_SD_BENCH_MAX_PRETRIGGER_S;
    }

    mode = DEBUG_MODE_SD_BENCH;

    /* an invalid sectors-per-write comes back as an error result */
    SdBench_Start(&captureBuffer, sizeof(captureBuffer),
            ((uint32_t)pretriggerSeconds * AUDIO_SAMPLE_RATE_HZ) / AUDIO_BLOCK_SIZE, sectorsPerWrite,
            useDma != 0U);
}


static void DebugLink_HandleCommand(uint8_t command)
{
    uint32_t primask;

    if(pendingCommand == 'W')
    {
        if(command <= DEBUG_SD_BENCH_PARAM_MAX)
        {
            pendingParams[pendingCount++] = command;

            if(pendingCount == DEBUG_SD_BENCH_PARAMS)
            {
                pendingCommand = 0U;
                DebugLink_StartSdBench(pendingParams[0], pendingParams[1], pendingParams[2]);
            }
            return;
        }

        /* not a parameter (e.g. an 'S' after a lone 'W'): a command of its own */
        pendingCommand = 0U;
    }

    switch(command)
    {
    case 'I':
        txInfo.sampleRateHz = AUDIO_SAMPLE_RATE_HZ;
        txInfo.blockSize = AUDIO_BLOCK_SIZE;
        txInfo.snapshotBlocks = DEBUG_SNAPSHOT_BLOCKS;
        txInfo.energyRecordSize = sizeof(DebugEnergyRecord_t);
        txInfo.energyRecords = DEBUG_ENERGY_RECORDS;
        txInfo.blocks = blockCount;
        snprintf(txInfo.build, sizeof(txInfo.build), "%s %s", __DATE__, __TIME__);

        DebugLink_StartFrame('I', &txInfo, sizeof(txInfo), 0U, 0U);
        break;

    case 'R':
        /* takes over the energy stream's buffer; ignored if a snapshot is under way */
        if((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY))
        {
            snapshotBlocks = 0U;
            mode = DEBUG_MODE_SNAPSHOT;
        }
        break;

    case 'E':
        if((mode == DEBUG_MODE_SD_BENCH) || (mode == DEBUG_MODE_FFT_CAPTURE) || (mode == DEBUG_MODE_LENT))
        {
            break;      /* the buffer is taken */
        }

        primask = __get_PRIMASK();
        __disable_irq();

        energyWrite = 0U;
        energyRead = 0U;
        energyDropped = 0U;
        mode = DEBUG_MODE_ENERGY;

        __set_PRIMASK(primask);
        break;

    case 'S':
        if(mode == DEBUG_MODE_SD_BENCH)
        {
            SdBench_Stop();     /* keeps the buffer until its result has been sent */
        }
        else if(mode != DEBUG_MODE_LENT)    /* the recorder keeps a lent buffer */
        {
            FFT_CancelCapture();
            mode = DEBUG_MODE_IDLE;
        }
        break;

    case 'F':
        /* takes the buffer, so it stops an energy stream like 'R' does */
        if((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY))
        {
            mode = DEBUG_MODE_FFT_CAPTURE;
            FFT_RequestCapture(captureBuffer.fft.raw, captureBuffer.fft.magnitude);
        }
        break;

    case 'W':
        pendingCommand = 'W';   /* the next two bytes are its parameters */
        pendingCount = 0U;
        break;

    case 'N':
        DebugLink_StartFrame('N', debugStats, sizeof(debugStats), DEBUG_STAT_COUNT, sizeof(DebugStat_t));
        break;

    case 'T':
        DebugLink_SnapshotStats();
        DebugLink_StartFrame('T', statValues, sizeof(statValues), DEBUG_STAT_COUNT, HAL_GetTick());
        break;

    case 'Z':
        DebugLink_ResetTimings();
        break;

    case 'M':
        /* reuses the buffer, so it stops an energy stream like 'R' does */
        if(snapshotInRing && ((mode == DEBUG_MODE_IDLE) || (mode == DEBUG_MODE_ENERGY)))
        {
            readbackNext = 0U;
            mode = DEBUG_MODE_READBACK;
        }
        else
        {
            DebugLink_StartFrame('M', NULL, 0U, DEBUG_READBACK_FAILED, 0U);
        }
        break;

    default:
        break;      /* ignore anything else, e.g. line endings from a terminal */
    }
}


static void DebugLink_Reset(void)
{
    if(mode == DEBUG_MODE_SD_BENCH)
    {
        /* the SD test keeps the buffer until it has closed its file */
        SdBench_Stop();

        if(SdBench_GetResult() != NULL)
        {
            SdBench_ReleaseResult();
            mode = DEBUG_MODE_IDLE;
        }
    }
    else if(mode != DEBUG_MODE_LENT)    /* runs on every pass with USB unplugged: the recorder keeps the buffer */
    {
        FFT_CancelCapture();
        mode = DEBUG_MODE_IDLE;
    }

    txStage = TX_IDLE;
    commandTail = commandHead;
    pendingCommand = 0U;
}



void DebugLink_Poll(void)
{
    /* Unplugged, suspended or not yet enumerated: drop whatever was going on */
    if(hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
    {
        DebugLink_Reset();
        return;
    }

    if(txStage == TX_IDLE)
    {
        if(commandTail != commandHead)
        {
            DebugLink_HandleCommand(commandQueue[commandTail]);
            commandTail = (uint8_t)((commandTail + 1U) % DEBUG_COMMAND_QUEUE);
        }

        /* the command may have started an info frame */
        if(txStage == TX_IDLE)
        {
            if(mode == DEBUG_MODE_SNAPSHOT_READY)
            {
                DebugLink_StartFrame('R', captureBuffer.samples, sizeof(captureBuffer.samples),
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
            else if((mode == DEBUG_MODE_SD_BENCH) && (SdBench_GetResult() != NULL))
            {
                DebugLink_StartFrame('W', SdBench_GetResult(), sizeof(SdBench_Result_t), 0U, 0U);
            }
            else if((mode == DEBUG_MODE_FFT_CAPTURE) && FFT_CaptureDone())
            {
                DebugLink_StartFrame('F', &captureBuffer.fft, sizeof(captureBuffer.fft), FFT_LENGTH, FFT_NUM_BINS);
            }
        }
    }

    DebugLink_ContinueFrame();
}


void *DebugLink_LendBuffer(void)
{
    /* an energy frame on its way out is still being read from the buffer */
    const uint8_t sendingEnergy = (txStage != TX_IDLE) && (txHeader.type == 'E');

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
