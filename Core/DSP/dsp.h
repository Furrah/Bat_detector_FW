/*
 * dsp.h
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DSP_DSP_H_
#define DSP_DSP_H_

#include <stdint.h>

#define DSP_BLOCK_SIZE 128
#define LUT_SIZE 256

#define SAMPLE_RATE 192060.0f
#define FILTER_CUTOFF 20000.0f
#define DSP_GAIN 12


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


static int32_t DSP_Mixer(int32_t sample);
int32_t DSP_clamp_output(int32_t sample);


#endif /* DSP_DSP_H_ */
