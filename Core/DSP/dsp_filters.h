/*
 * dsp_filters.h
 *
 * Second-order (biquad) Butterworth filters, direct form I, in float.
 */

#ifndef DSP_DSP_FILTERS_H_
#define DSP_DSP_FILTERS_H_

typedef struct
{
    float b0, b1, b2;       /* feed-forward coefficients */
    float a1, a2;           /* feedback coefficients */
    float x1, x2;           /* the last two inputs */
    float y1, y2;           /* the last two outputs */
} Biquad_t;

void Biquad_InitLowPass(Biquad_t *filter, float cutoff, float sampleRate);
void Biquad_InitHighPass(Biquad_t *filter, float cutoff, float sampleRate);

/* One sample through the filter */
float Biquad_Process(Biquad_t *filter, float input);

#endif /* DSP_DSP_FILTERS_H_ */
