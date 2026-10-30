/*
 * dsp.h
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DSP_DSP_H_
#define DSP_DSP_H_

#include <stdint.h>

#define DSP_BLOCK_SIZE 256
#define LUT_SIZE 512

#define SAMPLE_RATE 192060.0f
#define FILTER_CUTOFF 20000.0f
#define DSP_GAIN 8


typedef void (*DSP_Callback_t)(const uint16_t *buffer, uint32_t length);

void DSP_SetOutputCallback(DSP_Callback_t callback);


/*
 * Initialise DSP modules.
 * Called once during sta  rtup.
 */
void DSP_Init(void);



/*
 * Process a block of ADC samples.
 *
 * input:
 *     Pointer to ADC samples from DMA
 *
 * length:
 *     Number of samples in the block
 */
void DSP_ProcessBlock(
        uint16_t *input,
        uint32_t length);

void DSP_SetMixerFrequency(float frequency);

/*
 * Output volume from the volume knob position, 0.0 (min) to 1.0 (max).
 * A square-law taper is applied so the knob sounds roughly even.
 */
void DSP_SetVolume(float position);


int32_t DSP_Mixer(int32_t sample);

int32_t DSP_clamp_output(int32_t sample);

uint32_t DSP_calc_phase_increment(float frequency, float sample_rate);

void DSP_ProcessFFT(void);


#endif /* DSP_DSP_H_ */
