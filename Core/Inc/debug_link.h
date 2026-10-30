/*
 * debug_link.h
 *
 * Debug data capture over the USB virtual COM port (CDC). The PC sends
 * single command bytes and the board answers with framed binary data,
 * which Tools/capture.py records and Tools/analyse_capture.py analyses.
 *
 *   'I'  info: sample rate, block size, build time
 *   'R'  raw snapshot: the next DEBUG_SNAPSHOT_BLOCKS ADC blocks, unprocessed
 *   'E'  start streaming per-block energies and the detector state
 *   'S'  stop streaming
 *   'M'  read the last raw snapshot's blocks back from the PSRAM ring, so
 *        the PC can check them against the snapshot bit for bit
 *
 * The frame format is described at the top of debug_link.c.
 *
 * Set DEBUG_CAPTURE to 0 to remove the module and free its 8 KB buffer,
 * which PSRAM/SD recording will need.
 */

#ifndef DEBUG_LINK_H_
#define DEBUG_LINK_H_

#include <stdint.h>


#ifndef DEBUG_CAPTURE
#define DEBUG_CAPTURE   1
#endif


#if DEBUG_CAPTURE

/* Audio interrupt, once per block, after the detector has run */
void DebugLink_AddBlock(const uint16_t *input, uint32_t length, float energy);

/* USB interrupt: command bytes received from the PC */
void DebugLink_OnReceive(const uint8_t *data, uint32_t length);

/* Main loop: handles commands and sends data. Never waits. */
void DebugLink_Poll(void);

#else

static inline void DebugLink_AddBlock(const uint16_t *input, uint32_t length, float energy)
{
    (void)input;
    (void)length;
    (void)energy;
}

static inline void DebugLink_OnReceive(const uint8_t *data, uint32_t length)
{
    (void)data;
    (void)length;
}

static inline void DebugLink_Poll(void)
{
}

#endif /* DEBUG_CAPTURE */


#endif /* DEBUG_LINK_H_ */
