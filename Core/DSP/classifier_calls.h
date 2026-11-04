/*
 * classifier_calls.h
 *
 * Stages 1-3 of the species classifier: audio in, measured calls out.
 *   1. Frames: a 128-point FFT every 64 samples, each bin compared with its
 *      own noise floor, and the frame's peak found.
 *   2. The segmenter: frames into calls.
 *   3. Features: each call's duration, frequencies, slopes and flatness.
 * A port of Tools/classify.py (analyse, Segmenter, extract_features). It
 * uses no HAL, so Tools/check_classifier_c.py can build it on the PC and
 * check it against the model on the same audio.
 *
 * One instance (static state). Feed hops of raw 12-bit ADC codes in order;
 * a call is returned on the hop where it ends.
 */

#ifndef DSP_CLASSIFIER_CALLS_H_
#define DSP_CLASSIFIER_CALLS_H_

#include <stdint.h>

#include "classifier_config.h"


/* Call flags */
#define CLS_CALL_TRUNCATED      1U          /* longer than CLS_TRACK_MAX frames */
#define CLS_CALL_GAPPED         2U          /* (not set yet: for gaps in the audio) */
#define CLS_CALL_CLIPPED        4U          /* the ADC clipped during it */

#define CLS_NO_LABEL            0xFFU


/* One call. Stage 3 fills the first part; classifier_species.c the rest. */
typedef struct
{
    uint32_t frame0;        /* its first measured frame, counted from ClsCalls_Reset */
    float t_s;              /* frame0 in seconds */
    float dur_ms;
    float f_start;          /* kHz */
    float f_end;
    float f_peak;           /* at the strongest frame */
    float f_char;           /* characteristic: the end of the flattest part */
    float bw;               /* f_start - f_end */
    float slope_start;      /* kHz/ms over each third, positive = falling */
    float slope_mid;
    float slope_end;
    float flat_frac;        /* share of frame steps flatter than CLS_FLAT_SLOPE */
    float snr_db;           /* the strongest frame, re the mean noise */
    uint8_t harmonic;       /* 2nd harmonic present at the strongest frame */
    uint8_t flags;          /* CLS_CALL_... */

    /* stages 4-5 */
    uint8_t group;          /* ClsGroup_t */
    uint8_t label;          /* taxon (classifier_species.h), or CLS_NO_LABEL */
    uint8_t echo;
    float f_start_x;        /* the sweep's ends, extrapolated */
    float f_end_x;
    float bw_x;
    float flat_ms;
    float score;
    float weight;
} ClsCall_t;


/* Start afresh: noise floor unlearnt (the next CLS_SETTLE_FRAMES learn it),
 * no call open, frame count 0 */
void ClsCalls_Reset(void);

/* One hop of CLS_HOP raw ADC codes. Returns 1 and fills *call if a call
 * ended on this frame. */
uint8_t ClsCalls_ProcessHop(const uint16_t *codes, ClsCall_t *call);

/* End of the audio: close a call still open. Returns 1 and fills *call if
 * that made one. */
uint8_t ClsCalls_Flush(ClsCall_t *call);

/* Frames processed since the reset */
uint32_t ClsCalls_GetFrames(void);

#endif /* DSP_CLASSIFIER_CALLS_H_ */
