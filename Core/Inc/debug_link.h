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
 *   'F'  capture the next FFT frame's raw input and FFT_Process's output for
 *        it, so the PC can check the FFT against numpy
 *   'N'  names and types of the timing and health counters (stats_table.c)
 *   'T'  their current values
 *   'Z'  restart the worst-case timings (DSP, FFT, SD, PSRAM ring)
 *
 * The frame format and 'N', 'T', 'Z' are Core/Diag/diag_link.h; the other
 * frames are described at the top of debug_link.c.
 *
 * Set DEBUG_CAPTURE to 0 to remove the module. Its 8 KB buffer stays,
 * because the WAV recorder borrows it (below).
 */

#ifndef DEBUG_LINK_H_
#define DEBUG_LINK_H_

#include <stdint.h>


#ifndef DEBUG_CAPTURE
#define DEBUG_CAPTURE   1
#endif


/* Audio blocks since reset (blockCount in the stats) */
extern volatile uint32_t debugBlockCount;

/*
 * The 8 KB buffer behind the snapshot, the energy FIFO and the FFT capture
 * is lent to the WAV recorder (recorder.c) while it records: there's no RAM
 * for a second one. While it's lent, 'R', 'E', 'M' and 'F' are ignored.
 * With DEBUG_CAPTURE 0 the buffer is the recorder's alone.
 */
#define DEBUG_LINK_BUFFER_BYTES     8192U

/* Main loop: borrow the buffer (4-byte aligned), or NULL while the debug link is using it */
void *DebugLink_LendBuffer(void);

/* Main loop: give it back, once nothing (e.g. a PSRAM ring read) still writes into it */
void DebugLink_ReturnBuffer(void);


#if DEBUG_CAPTURE

/* Main loop, once, before the first DebugLink_Poll */
void DebugLink_Init(void);

/* Audio interrupt, once per block, after the detector has run */
void DebugLink_AddBlock(const uint16_t *input, uint32_t length, float energy);

/* USB interrupt: command bytes received from the PC */
void DebugLink_OnReceive(const uint8_t *data, uint32_t length);

/* Main loop: handles commands and sends data. Never waits. */
void DebugLink_Poll(void);

#else

static inline void DebugLink_Init(void)
{
}

static inline void DebugLink_AddBlock(const uint16_t *input, uint32_t length, float energy)
{
    (void)length;
    (void)energy;
    (void)input;
    debugBlockCount++;
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
