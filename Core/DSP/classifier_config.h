/*
 * classifier_config.h
 *
 * The species classifier's constants. Each mirrors the PC model
 * (Tools/classify.py PARAMS, Tools/species.py), where the reasons for the
 * values are given; the C and the model must stay in step, and
 * Tools/check_classifier_c.py checks they agree on the same audio.
 */

#ifndef DSP_CLASSIFIER_CONFIG_H_
#define DSP_CLASSIFIER_CONFIG_H_

#include "pipeline_config.h"


/* ---- Stage 1: frames. A 128-point FFT every 64 samples: 0.512 ms windows,
 * 1.95 kHz bins, a frame every 0.256 ms. */
#define CLS_FFT_LENGTH          128U        /* Tools/gen_dsp_tables.py reads this */
#define CLS_CFFT_LENGTH         (CLS_FFT_LENGTH / 2U)
#define CLS_HOP                 64U
#define CLS_BIN_KHZ             ((float)AUDIO_SAMPLE_RATE_HZ / (float)CLS_FFT_LENGTH / 1000.0f)
#define CLS_HOP_MS              (1000.0f * (float)CLS_HOP / (float)AUDIO_SAMPLE_RATE_HZ)
#define CLS_K_LO                5U          /* 9.8 kHz: the lowest bin analysed */
#define CLS_K_HI                61U         /* 119.1 kHz */
#define CLS_BINS                (CLS_K_HI - CLS_K_LO + 1U)

_Static_assert((AUDIO_BLOCK_SIZE % CLS_HOP) == 0U, "a ring block must be whole hops");
_Static_assert((CLS_K_LO >= 1U) && (CLS_K_HI + 1U <= CLS_FFT_LENGTH / 2U), "the peak's neighbours must be bins");

/* The noise floor per bin: rises over ~0.5 s, falls over ~16 ms, learnt only
 * between calls. SNR is measured against the mean noise, 6 dB above the
 * tracked floor (classify.py "nf_bias_db"). */
#define CLS_NF_UP               (1.0f / 2048.0f)
#define CLS_NF_DOWN             (1.0f / 64.0f)
#define CLS_NF_BIAS             3.98107171f     /* 10^(6/10) */
#define CLS_SETTLE_FRAMES       2048U           /* 0.52 s learning the floor before calls count */

/* Broadband (finger rub) tests, as counts over the CLS_BINS whitened bins */
#define CLS_BROAD_RATIO         3.98107171f     /* 6 dB: a median bin this high is broadband */
#define CLS_NEAR_RATIO          0.1f            /* within 10 dB of the peak */
#define CLS_RAISED_RATIO        1.99526231f     /* 3 dB: the frame is "raised" */

#define CLS_HARM_RATIO          10.0f           /* 10 dB: the 2nd harmonic counts */


/* ---- Stage 2: the segmenter */
#define CLS_T_ON_DB             12.0f       /* SNR to start a call */
#define CLS_T_OFF_DB            9.0f        /* below this a frame is weak */
#define CLS_END_DROP_DB         24.0f       /* ... or this far below the call's peak */
#define CLS_N_ON                2U          /* frames above t_on to start */
#define CLS_N_OFF               3U          /* weak frames to end */
#define CLS_RISE_MAX_KHZ        8.0f        /* a jump up this big starts a new call */
#define CLS_DROP_MAX_KHZ        16.0f       /* in a call, the peak is searched no lower than this below the track */
#define CLS_MIN_CALL_FRAMES     3U          /* fewer strong frames: a click */
#define CLS_TRACK_MAX           320U        /* points kept per call: 82 ms */


/* ---- Stage 3: features */
#define CLS_FLAT_SLOPE          2.0f        /* kHz/ms: flatter counts as flat */


/* ---- Stage 4: shape groups (species.py) */
#define CLS_END_EXTRAP_MS       0.25f
#define CLS_EXTRAP_MIN_MS       1.5f
#define CLS_BAND_LO_KHZ         9.8f
#define CLS_BAND_HI_KHZ         119.1f
#define CLS_RISING_KHZ          5.0f
#define CLS_NARROW_KHZ          8.0f
#define CLS_BAND_EDGE_KHZ       (CLS_BAND_LO_KHZ + 1.0f)


/* ---- Stage 5: rules */
#define CLS_MIN_SCORE           0.3f
#define CLS_AMBIG               0.7f


/* ---- Stage 6: sequences */
#define CLS_SEQ_GAP_S           1.0f        /* silence that ends a sequence */
#define CLS_SEQ_MIN_CALLS       3U          /* fewer: not reported */
#define CLS_VOTE_N              10U         /* calls in the voting window */
#define CLS_LEAF_SHARE          0.6f
#define CLS_LEAF_MIN_CALLS      5U
#define CLS_ECHO_S              0.010f
#define CLS_ECHO_DB             10.0f
#define CLS_ECHO_KHZ            3.0f
#define CLS_ALT_STEP_KHZ        2.0f


#endif /* DSP_CLASSIFIER_CONFIG_H_ */
