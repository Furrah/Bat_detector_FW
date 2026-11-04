/*
 * diag_link.h
 *
 * A framed command link between the board and a PC over any byte transport
 * (here USB CDC). The PC sends single command bytes. The board answers with
 * frames, all fields little-endian:
 *
 *   header   20 bytes  magic (4 chars), type, version, sequence (uint16,
 *                      +1 per frame so the PC can spot lost ones), payload
 *                      length, arg0, arg1 (uint32 each, meaning per type)
 *   payload  length bytes
 *   crc      4 bytes   CRC-32 of header and payload (Python's zlib.crc32)
 *
 * Built in when a stats table is given (diag_stats.h):
 *   'N'  the table: arg0 entries of arg1 (32) bytes
 *   'T'  its values: arg0 words in table order, arg1 = uptime in ms
 *   'Z'  restart the worst-case timings (the project's resetTimings)
 * The project adds its own commands and frame types. The PC side is
 * Tools/diaglink.py.
 *
 * A frame goes out as three transfers (header, payload, CRC). The payload is
 * sent from where it lies, so it must stay put until the sent() hook says
 * the frame has gone. Commands are acted on only between frames, so a
 * command never changes a buffer that's being sent.
 */

#ifndef DIAG_LINK_H_
#define DIAG_LINK_H_

#include <stdint.h>

#include "diag_stats.h"


#define DIAG_LINK_HEADER_BYTES      20U
#define DIAG_LINK_COMMAND_QUEUE     16U


typedef struct
{
    uint8_t command;
    void (*handler)(void);          /* main loop, between frames */
} DiagLinkCommand_t;

typedef struct
{
    char magic[4];                  /* the first 4 bytes of every frame */
    uint8_t version;                /* protocol version, in every frame */

    /* transport */
    uint8_t (*send)(const void *data, uint32_t length);    /* start a transfer; 0 while the last is still going */
    uint8_t (*connected)(void);     /* 1 while the PC is there */
    uint32_t (*uptimeMs)(void);     /* for 'T' */

    /* the project's commands and frames */
    const DiagLinkCommand_t *commands;
    uint32_t commandCount;
    void (*idle)(void);             /* between frames: start any frame that's ready (streams); may be NULL */
    void (*sent)(uint8_t type);     /* a frame of this type has gone: release its payload; may be NULL */
    void (*disconnected)(void);     /* the PC has gone: drop what was going on; may be NULL */

    /* the stats table, for 'N', 'T' and 'Z'; NULL to leave them out */
    const DiagStatTable_t *stats;
} DiagLinkConfig_t;


/* Main loop, before the first DiagLink_Poll. The config must stay put. */
void DiagLink_Init(const DiagLinkConfig_t *config);

/* The transport's receive callback (an interrupt): queue command bytes.
 * Bytes beyond the queue's 16 are dropped. */
void DiagLink_OnReceive(const uint8_t *data, uint32_t length);

/* Main loop: acts on a command, sends what's ready. Never waits. */
void DiagLink_Poll(void);

/* Main loop: start sending a frame. Returns 0, and does nothing, if a
 * frame is already on its way. */
uint8_t DiagLink_StartFrame(uint8_t type, const void *payload, uint32_t length, uint32_t arg0, uint32_t arg1);

/* The type of the frame being sent, 0 if none */
uint8_t DiagLink_SendingType(void);

/* CRC-32 as zlib computes it. Start with crc = 0; feed the result back to continue. */
uint32_t Diag_Crc32(uint32_t crc, const void *data, uint32_t length);

#endif /* DIAG_LINK_H_ */
