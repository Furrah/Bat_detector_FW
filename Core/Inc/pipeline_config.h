/*
 * pipeline_config.h
 *
 * Single source for the audio pipeline's timing and block sizes. The ADC,
 * DAC, DSP, FFT/waterfall and (later) PSRAM/SD recording all derive their
 * rates and buffer sizes from here. Never hard-code these elsewhere.
 */

#ifndef PIPELINE_CONFIG_H_
#define PIPELINE_CONFIG_H_


/*
 * TIM2 update events trigger both the ADC and the DAC. Its kernel clock is
 * the APB1 timer clock: 170 MHz (APB1 runs at /2, and timers get 2 x PCLK1).
 * The CubeMX TIM2 Counter Period must be AUDIO_TIMER_PERIOD - 1; main.c
 * checks this at startup.
 */
#define AUDIO_TIMER_CLOCK_HZ    170000000UL
#define AUDIO_TIMER_PERIOD      680UL

#define AUDIO_SAMPLE_RATE_HZ    (AUDIO_TIMER_CLOCK_HZ / AUDIO_TIMER_PERIOD)    /* 250 000 */
#define AUDIO_NYQUIST_HZ        (AUDIO_SAMPLE_RATE_HZ / 2UL)

_Static_assert((AUDIO_TIMER_CLOCK_HZ % AUDIO_TIMER_PERIOD) == 0UL,
        "sample rate must be a whole number of Hz for the WAV header");


/*
 * Samples per DMA half-buffer, i.e. per DSP_ProcessBlock call: 1.024 ms
 * at 250 kHz. The ADC and DAC circular buffers each hold two blocks.
 */
#define AUDIO_BLOCK_SIZE        256U
#define AUDIO_BUFFER_SIZE       (2U * AUDIO_BLOCK_SIZE)

/* Duration of one block, used to turn millisecond timings into block counts */
#define AUDIO_BLOCK_US          ((AUDIO_BLOCK_SIZE * 1000000UL) / AUDIO_SAMPLE_RATE_HZ)   /* 1024 */

_Static_assert(((AUDIO_BLOCK_SIZE * 1000000UL) % AUDIO_SAMPLE_RATE_HZ) == 0UL,
        "block duration must be a whole number of microseconds");


/* Real samples per FFT frame (bin width = sample rate / this) */
#define AUDIO_FFT_LENGTH        1024U


/* Highest heterodyne LO setting, kept just below Nyquist */
#define AUDIO_LO_MAX_HZ         120000UL

_Static_assert(AUDIO_LO_MAX_HZ < AUDIO_NYQUIST_HZ, "LO must stay below Nyquist");


/*
 * Time-expansion playback replays the recording this many times slower,
 * dividing every frequency by it: a 45 kHz call plays at 4.5 kHz.
 */
#define PLAYBACK_EXPANSION      10U

_Static_assert(PLAYBACK_EXPANSION >= 2U, "playback must be slower than real time");


#endif /* PIPELINE_CONFIG_H_ */
