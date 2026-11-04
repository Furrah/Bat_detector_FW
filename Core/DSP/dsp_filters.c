/*
 * dsp_filters.c
 *
 * Butterworth biquads from the bilinear transform: K = tan(pi * fc / fs).
 */

#include "dsp_filters.h"

#include <math.h>


static void Biquad_Clear(Biquad_t *filter)
{
    filter->x1 = 0.0f;
    filter->x2 = 0.0f;
    filter->y1 = 0.0f;
    filter->y2 = 0.0f;
}


void Biquad_InitLowPass(Biquad_t *filter, float cutoff, float sampleRate)
{
    const float K = tanf((float)M_PI * cutoff / sampleRate);
    const float norm = 1.0f / (1.0f + sqrtf(2.0f) * K + K * K);

    filter->b0 = K * K * norm;
    filter->b1 = 2.0f * filter->b0;
    filter->b2 = filter->b0;
    filter->a1 = 2.0f * (K * K - 1.0f) * norm;
    filter->a2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
    Biquad_Clear(filter);
}


void Biquad_InitHighPass(Biquad_t *filter, float cutoff, float sampleRate)
{
    const float K = tanf((float)M_PI * cutoff / sampleRate);
    const float norm = 1.0f / (1.0f + sqrtf(2.0f) * K + K * K);

    filter->b0 = norm;
    filter->b1 = -2.0f * norm;
    filter->b2 = norm;
    filter->a1 = 2.0f * (K * K - 1.0f) * norm;
    filter->a2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
    Biquad_Clear(filter);
}


float Biquad_Process(Biquad_t *filter, float input)
{
    const float output = filter->b0 * input + filter->b1 * filter->x1 + filter->b2 * filter->x2
            - filter->a1 * filter->y1 - filter->a2 * filter->y2;

    filter->x2 = filter->x1;
    filter->x1 = input;
    filter->y2 = filter->y1;
    filter->y1 = output;

    return output;
}
