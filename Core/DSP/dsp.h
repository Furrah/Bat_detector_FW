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
#define DSP_GAIN 8
#define DC_BLOCK_R 32750   // ~0.996 coefficient
#define NOTCH_FREQ 50.0f

#define AGC_TARGET      800.0f
#define AGC_MAX_GAIN    8.0f
#define AGC_MIN_GAIN    0.25f

#define AGC_ATTACK      0.05f
#define AGC_RELEASE     0.001f


typedef void (*DSP_Callback_t)(const uint16_t *buffer, uint32_t length);

void DSP_SetOutputCallback(DSP_Callback_t callback);

typedef struct
{
    int32_t x1;
    int32_t y1;

} DCBlocker_t;

typedef struct
{
    float b0;
    float b1;
    float b2;

    float a1;
    float a2;

    float x1;
    float x2;

    float y1;
    float y2;

} Biquad_t;

static void Biquad_InitLowPass(
        Biquad_t *filter,
        float cutoff,
        float sampleRate);

static float Biquad_Process(
        Biquad_t *filter,
        float input);

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

static int32_t DCBlock_Process(int32_t input);
static float NotchFilter(float input);

static float AGC_Process(float input);



static int32_t DSP_Mixer(int32_t sample);



#endif /* DSP_DSP_H_ */
