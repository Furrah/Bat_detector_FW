/*
 * DSP_filters.c
 *
 *  Created on: 6 Aug 2026
 *      Author: Joe Spencer
 */

#include "dsp.h"
#include "dsp_filters.h"
#include "math.h"


// notch filter coefficieints
static float notch_b0;
static float notch_b1;
static float notch_b2;
static float notch_a1;
static float notch_a2;

static float notch_x1 = 0;
static float notch_x2 = 0;
static float notch_y1 = 0;
static float notch_y2 = 0;

//// low pass filters
//static Biquad_t lowPass1;
//static Biquad_t lowPass2;
//

// dc blocking
static DCBlocker_t dcBlocker =
{
    .x1 = 0,
    .y1 = 0
};



void notch_filter_init(void){


    float r = 0.9999f;

    float omega = 2.0f * M_PI * NOTCH_FREQ / SAMPLE_RATE;

    float c = cosf(omega);

    notch_b0 = 1.0f;
    notch_b1 = -2.0f * c;
    notch_b2 = 1.0f;

    notch_a1 = -2.0f * r * c;
    notch_a2 = r * r;
}


float NotchFilter(float input)
{
    float output;

    output =
        notch_b0 * input
      + notch_b1 * notch_x1
      + notch_b2 * notch_x2
      - notch_a1 * notch_y1
      - notch_a2 * notch_y2;


    notch_x2 = notch_x1;
    notch_x1 = input;

    notch_y2 = notch_y1;
    notch_y1 = output;


    return output;
}

int32_t DCBlock_Process(int32_t input)
{
    int32_t output;


    output =
        input
        - dcBlocker.x1
        + ((dcBlocker.y1 * DC_BLOCK_R) >> 15);


    dcBlocker.x1 = input;
    dcBlocker.y1 = output;


    return output;
}



void Biquad_InitLowPass(
        Biquad_t *filter,
        float cutoff,
        float sampleRate)
{

    float K = tanf(M_PI * cutoff / sampleRate);
    float norm =1.0f /(1.0f + sqrtf(2.0f)*K + K*K);

    filter->b0 = K*K*norm;
    filter->b1 = 2.0f * filter->b0;
    filter->b2 =filter->b0;
    filter->a1 =2.0f *(K*K - 1.0f) * norm;
    filter->a2 =(1.0f -sqrtf(2.0f)*K +K*K)* norm;


    /*
     * Clear delay registers
     */
    filter->x1 = 0.0f;
    filter->x2 = 0.0f;

    filter->y1 = 0.0f;
    filter->y2 = 0.0f;

}

float Biquad_Process(
        Biquad_t *filter,
        float input)
{

    float output;

    output =filter->b0 * input + filter->b1 * filter->x1 + filter->b2 * filter->x2 -
        filter->a1 * filter->y1-filter->a2 * filter->y2;


    /*
     * Shift history
     */

    filter->x2 =filter->x1;
    filter->x1 =input;
    filter->y2 =filter->y1;
    filter->y1 =output;



    return output;
}


