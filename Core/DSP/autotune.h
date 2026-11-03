/*
 * autotune.h
 *
 * Auto-tune: points the heterodyne LO at the bats being heard.
 *
 * It works from the waterfall's rows (one every 40 ms): each row holds the
 * peak level in each of its 240 frequency columns, about 0.52 kHz wide.
 *
 *   - A noise floor per column is learnt from rows with no bat, so a
 *     steady tone (e.g. the 24.95 kHz line) sits in its own floor and
 *     never wins.
 *   - During a bat pass (BatDetect_IsBatPresent), each row with a call
 *     gives the column furthest above its floor: the call's loudest
 *     frequency, refined between columns to ~0.1 kHz.
 *   - The LO goes AUTOTUNE_OFFSET_HZ below the median of the last calls,
 *     so a call's steady tail beats at an audible tone rather than near
 *     0 Hz. It only moves for a change of more than 2 kHz, at most once a
 *     second, and holds between passes.
 *
 * It learns in both tuning modes; main.c applies its result only in auto.
 * Diary entry 45.
 */

#ifndef DSP_AUTOTUNE_H_
#define DSP_AUTOTUNE_H_

#include <stdint.h>


/* The LO sits this far below the calls' loudest frequency */
#define AUTOTUNE_OFFSET_HZ      1500U


void AutoTune_Init(void);

/*
 * Once per waterfall row, before Waterfall_FlushRow clears it: the row's
 * per-column peak levels in dB and how many FFT frames went into it
 * (Waterfall_GetRow). Returns 1, with *loHz set, when the LO should move.
 */
uint8_t AutoTune_AddRow(const float *rowDb, uint16_t frames, uint32_t *loHz);

/* Entering auto mode: compare against this LO from now on, and allow a
 * retune straight away. Calls already heard in this pass still count. */
void AutoTune_Restart(uint32_t currentLoHz);

/* Debug view (capture.py stats) */
extern volatile uint32_t autoTuneCalls;     /* calls measured */
extern volatile uint32_t autoTuneRetunes;   /* LO moves asked for */
extern volatile uint32_t autoTuneLastHz;    /* the last call's loudest frequency */

#endif /* DSP_AUTOTUNE_H_ */
