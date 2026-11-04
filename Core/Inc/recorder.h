/*
 * recorder.h
 *
 * WAV recording to the SD card. RECORD starts a recording that begins
 * RECORDER_PRETRIGGER_SECONDS before the press, taken from the PSRAM ring,
 * and carries on with the live audio until RECORD is pressed again or the
 * file reaches RECORDER_MAX_SECONDS. Each recording is a new file,
 * BATnnnnn.WAV in the card's root folder: mono 16-bit PCM at
 * AUDIO_SAMPLE_RATE_HZ, the raw ADC signal (no filtering).
 *
 * Samples are the 12-bit ADC codes about mid-scale, times 16: the ADC's
 * full scale is 16-bit full scale, and the low 4 bits are always 0.
 *
 * The audio is written 8 sectors at a time, polled, from the main loop
 * (diary 39-40). Until the pre-trigger has been caught up the file lags
 * the live audio; stopping writes out that backlog before the file is
 * closed (RECORDER_SAVING). The file is synced every 2 s, so a power cut
 * loses at most the last 2 s.
 */

#ifndef RECORDER_H_
#define RECORDER_H_

#include <stdint.h>


/* Audio kept from before the press. The ring holds 16.8 s; at the measured
 * 1.38x catch-up rate, 5 s takes about 13 s to catch up (diary 40). */
#define RECORDER_PRETRIGGER_SECONDS     5U

/* Longest file, pre-trigger included: 300 MB at 500 kB/s */
#define RECORDER_MAX_SECONDS            600U


typedef enum
{
    RECORDER_IDLE = 0,
    RECORDER_RECORDING,
    RECORDER_SAVING             /* stopped: writing out the backlog, then closing the file */
} Recorder_State_t;

typedef enum
{
    RECORDER_OK = 0,
    RECORDER_TIME_LIMIT,        /* ended at RECORDER_MAX_SECONDS */
    RECORDER_ERR_NO_RING,       /* the PSRAM ring isn't recording (boot screen says why) */
    RECORDER_ERR_BUSY,          /* the last recording is still saving */
    RECORDER_ERR_NO_CARD,       /* no card, or it didn't start */
    RECORDER_ERR_NOT_FAT,       /* no FAT file system (e.g. an exFAT card) */
    RECORDER_ERR_FILE,          /* couldn't create the file */
    RECORDER_ERR_WRITE,         /* a write failed */
    RECORDER_ERR_FULL,          /* the card is full */
    RECORDER_ERR_TOO_SLOW,      /* the ring overwrote audio before it was saved */
    RECORDER_ERR_RING_PAUSED    /* the ring stopped recording */
} Recorder_Status_t;


/* Start a recording. The pre-trigger is measured from this call. Mounts
 * the card afresh each time, so a card inserted after boot is picked up. */
Recorder_Status_t Recorder_Start(void);

/* Stop at the block being processed now. The backlog is still saved. */
void Recorder_Stop(void);

/* Main loop, ahead of FFT_Process: does the work, for up to ~20 ms per call */
void Recorder_Poll(void);

Recorder_State_t Recorder_GetState(void);

/* Time since RECORD was pressed, while recording; 0 otherwise */
uint32_t Recorder_GetElapsedMs(void);

/* How the last recording ended, once, after it has closed: an error or
 * RECORDER_TIME_LIMIT. RECORDER_OK if it was stopped normally, or if
 * there's nothing new. */
Recorder_Status_t Recorder_TakeResult(void);

/* A 13-character message for the display's header line */
const char *Recorder_StatusText(Recorder_Status_t status);

#endif /* RECORDER_H_ */
