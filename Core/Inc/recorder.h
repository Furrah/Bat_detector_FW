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
 * The audio is written the way the SD write test found best (diary 39-40):
 * 8 sectors per write, polled, from the main loop ahead of the FFT. Until
 * the pre-trigger has been caught up, the file falls behind the live audio
 * by up to the pre-trigger's length; stopping writes out that backlog
 * before the file is closed (RECORDER_SAVING).
 *
 * The file is synced every 2 s, so a power cut loses at most the last 2 s.
 *
 * While recording, the 8 KB debug capture buffer is borrowed, so the
 * debug link's snapshot, energy stream, FFT capture and SD test are
 * refused.
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
    RECORDER_ERR_BUSY,          /* the debug link is using the buffer, or a recording is still saving */
    RECORDER_ERR_NO_CARD,       /* no card, or it didn't start */
    RECORDER_ERR_NOT_FAT,       /* no FAT file system (e.g. an exFAT card) */
    RECORDER_ERR_FILE,          /* couldn't create the file */
    RECORDER_ERR_WRITE,         /* a write failed */
    RECORDER_ERR_FULL,          /* the card is full */
    RECORDER_ERR_TOO_SLOW,      /* the ring overwrote audio before it was saved */
    RECORDER_ERR_RING_PAUSED    /* the ring stopped recording */
} Recorder_Status_t;


/*
 * Main loop: start a recording. The pre-trigger is measured from the call,
 * before the card is mounted and the next file number found. Mounts the
 * card afresh each time, so a card inserted after boot, or a different
 * one, is picked up.
 */
Recorder_Status_t Recorder_Start(void);

/* Main loop: stop at the block being processed now. The backlog is still saved. */
void Recorder_Stop(void);

/* Main loop, ahead of FFT_Process: does the work, for up to ~20 ms per call */
void Recorder_Poll(void);

Recorder_State_t Recorder_GetState(void);

/* Time since RECORD was pressed, while recording; 0 otherwise */
uint32_t Recorder_GetElapsedMs(void);

/*
 * Main loop: how the last recording ended, once, after it has closed: an
 * error or RECORDER_TIME_LIMIT. RECORDER_OK if it was stopped normally, or
 * there's nothing new.
 */
Recorder_Status_t Recorder_TakeResult(void);

/* A 13-character message for the display's header line */
const char *Recorder_StatusText(Recorder_Status_t status);


/* Debug view (capture.py stats). All but recFile are for the current or
 * last recording; they restart when one starts. */
extern volatile uint32_t recFile;           /* number of the current or last file, BATnnnnn.WAV */
extern volatile uint32_t recBlocks;         /* audio blocks saved */
extern volatile uint32_t recMaxBacklog;     /* most blocks waiting to be saved; audio is lost above ~16300 */
extern volatile uint32_t recMaxWriteUs;     /* slowest 8-sector f_write */
extern volatile uint32_t recMaxSyncUs;      /* slowest sync: f_sync plus the header update */
extern volatile uint32_t recReadFailures;   /* blocks the ring failed to read, saved as silence (should stay 0) */
extern volatile uint32_t recLastEnd;        /* Recorder_Status_t the last recording ended with */


#endif /* RECORDER_H_ */
