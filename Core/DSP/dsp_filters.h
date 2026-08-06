/*
 * dsp_filters.h
 *
 *  Created on: 6 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DSP_DSP_FILTERS_H_
#define DSP_DSP_FILTERS_H_

#define NOTCH_FREQ 50.0f
#define DC_BLOCK_R 32750

void notch_filter_init(void);
float NotchFilter(float input);
int32_t DCBlock_Process(int32_t input);


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

void Biquad_InitLowPass(
        Biquad_t *filter,
        float cutoff,
        float sampleRate);

float Biquad_Process(
        Biquad_t *filter,
        float input);


#endif /* DSP_DSP_FILTERS_H_ */
