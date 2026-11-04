/*
 * dsp.h
 *
 * The audio chain, run once per block in the ADC's DMA interrupt: a digital
 * heterodyne (high-pass, gain, mixer, low-pass, volume) to the DAC, plus the
 * feeds to the bat detector and the FFT. During time-expansion playback the
 * heterodyne output is replaced by the playback (playback.h).
 */

#ifndef DSP_DSP_H_
#define DSP_DSP_H_

#include <stdint.h>

#include "pipeline_config.h"

#define SAMPLE_RATE     ((float)AUDIO_SAMPLE_RATE_HZ)
#define DSP_GAIN        8           /* after the high-pass, before the mixer */

/* NCO sine table entries; Tools/gen_dsp_tables.py reads this line */
#define LUT_SIZE 512

/* Called with each finished output block (the DAC) */
typedef void (*DSP_Callback_t)(const uint16_t *buffer, uint32_t length);

void DSP_Init(void);
void DSP_SetOutputCallback(DSP_Callback_t callback);

/* ADC DMA interrupt: process one block of raw 12-bit ADC codes */
void DSP_ProcessBlock(uint16_t *input, uint32_t length);

/* Main loop: the heterodyne LO, 0 to AUDIO_LO_MAX_HZ */
void DSP_SetMixerFrequency(float frequency);

/* Main loop: volume knob position, 0.0 to 1.0 (square-law taper) */
void DSP_SetVolume(float position);

/* A signed sample about 0 -> a DAC code, clamped to 0..4095 */
int32_t DSP_ClampOutput(int32_t sample);

#endif /* DSP_DSP_H_ */
