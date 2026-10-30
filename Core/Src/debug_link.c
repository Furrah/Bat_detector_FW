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
 *
 * One 8 KB buffer holds either the snapshot or the energy FIFO, so a
 * snapshot request stops the energy stream.
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
#include "usbd_cdc_if.h"
#include "../DSP/bat_detect.h"
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
} DebugCaptureBuffer_t;

typedef enum
{
    DEBUG_MODE_IDLE = 0,
    DEBUG_MODE_SNAPSHOT,        /* the interrupt is filling the snapshot */
    DEBUG_MODE_SNAPSHOT_READY,  /* full; the main loop sends it */
    DEBUG_MODE_ENERGY,          /* the interrupt adds an energy record per block */
    DEBUG_MODE_READBACK         /* the PSRAM ring is reading the snapshot back */
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


static void DebugLink_HandleCommand(uint8_t command)
{
    uint32_t primask;

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
        primask = __get_PRIMASK();
        __disable_irq();

        energyWrite = 0U;
        energyRead = 0U;
        energyDropped = 0U;
        mode = DEBUG_MODE_ENERGY;

        __set_PRIMASK(primask);
        break;

    case 'S':
        mode = DEBUG_MODE_IDLE;
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
    mode = DEBUG_MODE_IDLE;
    txStage = TX_IDLE;
    commandTail = commandHead;
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
        }
    }

    DebugLink_ContinueFrame();
}


#endif /* DEBUG_CAPTURE */
