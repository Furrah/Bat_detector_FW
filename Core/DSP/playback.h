/*
 * playback.h
 *
 * Time-expansion playback. The PLAYBACK button replays the last
 * PLAYBACK_SECONDS of raw audio from the PSRAM ring PLAYBACK_EXPANSION
 * times slower (pipeline_config.h). Every frequency is divided by the
 * factor, so a 45 kHz pipistrelle call plays at 4.5 kHz with its shape,
 * harmonics and rhythm intact. The heterodyne only hears a narrow band
 * around the LO, folded about it.
 *
 * The ring is paused while playback runs: 2 s played at 10x takes 20 s,
 * longer than the 16.8 s the ring holds, so writing on would overwrite the
 * recording being played. It resumes when playback ends.
 */

#ifndef DSP_PLAYBACK_H_
#define DSP_PLAYBACK_H_

#include <stdint.h>

#include "pipeline_config.h"


/* Seconds of recording replayed per press */
#define PLAYBACK_SECONDS        2U


typedef enum
{
    PLAYBACK_OK = 0,
    PLAYBACK_ERR_NO_RECORDING,  /* the PSRAM ring isn't running (the boot screen says why) */
    PLAYBACK_ERR_BUSY           /* the last playback's reads are still landing: try again */
} Playback_Status_t;


/* Main loop: start replaying the last PLAYBACK_SECONDS, or stop early */
Playback_Status_t Playback_Start(void);
void Playback_Stop(void);

uint8_t Playback_IsActive(void);

/* Main loop: playing time left, for the display */
uint32_t Playback_GetRemainingMs(void);

/*
 * Audio interrupt, once per block while active, in place of the heterodyne:
 * fill output with the next DAC codes (0-4095), scaled by gain (the volume).
 * Ends the playback, and resumes recording, when the recording runs out.
 */
void Playback_Render(uint16_t *output, uint32_t length, float gain);



#endif /* DSP_PLAYBACK_H_ */
