/*
 * diag_link.c
 *
 * The framed command link; see diag_link.h for the format.
 */

#include "diag_link.h"

#include <stddef.h>
#include <string.h>


typedef struct
{
    char     magic[4];
    uint8_t  type;
    uint8_t  version;
    uint16_t sequence;
    uint32_t length;
    uint32_t arg0;
    uint32_t arg1;
} DiagFrameHeader_t;

_Static_assert(sizeof(DiagFrameHeader_t) == DIAG_LINK_HEADER_BYTES, "frame header must match Tools/diaglink.py");

typedef enum
{
    TX_IDLE = 0,
    TX_HEADER,
    TX_PAYLOAD,
    TX_CRC
} TxStage_t;


static const DiagLinkConfig_t *cfg = NULL;

/* Shared with the transport's receive interrupt */
static volatile uint8_t commandQueue[DIAG_LINK_COMMAND_QUEUE];
static volatile uint8_t commandHead = 0;        /* the interrupt writes here */
static volatile uint8_t commandTail = 0;        /* the main loop reads from here */

/* Main loop only */
static DiagFrameHeader_t txHeader;
static const void *txPayload;
static uint32_t txCrc;
static uint8_t txCrcBytes[4];
static uint8_t txStage = TX_IDLE;
static uint16_t txSequence = 0;


/* zlib's CRC-32 (reflected polynomial 0xEDB88320), 4 bits at a time from a 64-byte table */
uint32_t Diag_Crc32(uint32_t crc, const void *data, uint32_t length)
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


void DiagLink_Init(const DiagLinkConfig_t *config)
{
    cfg = config;
    txStage = TX_IDLE;
}


void DiagLink_OnReceive(const uint8_t *data, uint32_t length)
{
    for(uint32_t i = 0; i < length; i++)
    {
        const uint8_t next = (uint8_t)((commandHead + 1U) % DIAG_LINK_COMMAND_QUEUE);

        if(next == commandTail)
        {
            break;      /* queue full: drop the rest */
        }

        commandQueue[commandHead] = data[i];
        commandHead = next;
    }
}


uint8_t DiagLink_StartFrame(uint8_t type, const void *payload, uint32_t length, uint32_t arg0, uint32_t arg1)
{
    if(txStage != TX_IDLE)
    {
        return 0U;
    }

    memcpy(txHeader.magic, cfg->magic, sizeof(txHeader.magic));
    txHeader.type = type;
    txHeader.version = cfg->version;
    txHeader.sequence = txSequence++;
    txHeader.length = length;
    txHeader.arg0 = arg0;
    txHeader.arg1 = arg1;

    txPayload = payload;
    txCrc = Diag_Crc32(0U, &txHeader, sizeof(txHeader));
    txCrc = Diag_Crc32(txCrc, payload, length);
    txStage = TX_HEADER;
    return 1U;
}


uint8_t DiagLink_SendingType(void)
{
    return (txStage != TX_IDLE) ? txHeader.type : 0U;
}


static void DiagLink_ContinueFrame(void)
{
    switch(txStage)
    {
    case TX_HEADER:
        if(cfg->send(&txHeader, sizeof(txHeader)))
        {
            txStage = (txHeader.length > 0U) ? TX_PAYLOAD : TX_CRC;
        }
        break;

    case TX_PAYLOAD:
        if(cfg->send(txPayload, txHeader.length))
        {
            txStage = TX_CRC;
        }
        break;

    case TX_CRC:
        /* safe to write: the transfer in flight, if any, is this frame's payload */
        memcpy(txCrcBytes, &txCrc, sizeof(txCrcBytes));

        if(cfg->send(txCrcBytes, sizeof(txCrcBytes)))
        {
            /* the CRC transfer was accepted, so the payload has gone */
            txStage = TX_IDLE;

            if(cfg->sent != NULL)
            {
                cfg->sent(txHeader.type);
            }
        }
        break;

    default:
        break;
    }
}


static void DiagLink_HandleCommand(uint8_t command)
{
    const DiagStatTable_t *stats = cfg->stats;

    if(stats != NULL)
    {
        if(command == 'N')
        {
            (void)DiagLink_StartFrame('N', stats->entries, stats->count * sizeof(DiagStat_t),
                    stats->count, sizeof(DiagStat_t));
            return;
        }

        if(command == 'T')
        {
            Diag_StatsSnapshot(stats->entries, stats->count, stats->values);
            (void)DiagLink_StartFrame('T', stats->values, stats->count * sizeof(uint32_t),
                    stats->count, cfg->uptimeMs());
            return;
        }

        if(command == 'Z')
        {
            if(stats->resetTimings != NULL)
            {
                stats->resetTimings();
            }
            return;
        }
    }

    for(uint32_t i = 0; i < cfg->commandCount; i++)
    {
        if(cfg->commands[i].command == command)
        {
            cfg->commands[i].handler();
            return;
        }
    }

    /* anything else is ignored, e.g. line endings from a terminal */
}


void DiagLink_Poll(void)
{
    if(cfg == NULL)
    {
        return;
    }

    /* unplugged, suspended or not yet enumerated: drop whatever was going on */
    if(!cfg->connected())
    {
        txStage = TX_IDLE;
        commandTail = commandHead;

        if(cfg->disconnected != NULL)
        {
            cfg->disconnected();
        }
        return;
    }

    if(txStage == TX_IDLE)
    {
        if(commandTail != commandHead)
        {
            DiagLink_HandleCommand(commandQueue[commandTail]);
            commandTail = (uint8_t)((commandTail + 1U) % DIAG_LINK_COMMAND_QUEUE);
        }

        /* the command may have started a frame */
        if((txStage == TX_IDLE) && (cfg->idle != NULL))
        {
            cfg->idle();
        }
    }

    DiagLink_ContinueFrame();
}
