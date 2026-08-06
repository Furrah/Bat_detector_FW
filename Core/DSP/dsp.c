/*
 * dsp.c
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */


#include "dsp.h"

#include <math.h>

static int16_t sineTable[LUT_SIZE];
static uint32_t phase = 0;
static uint32_t phaseIncrement;
static volatile float loFrequency = 38000.0f;

static DSP_Callback_t outputCallback = 0;

static Biquad_t lowPass1;
static Biquad_t lowPass2;


static DCBlocker_t dcBlocker =
{
    .x1 = 0,
    .y1 = 0
};



static float notch_b0;
static float notch_b1;
static float notch_b2;
static float notch_a1;
static float notch_a2;

static float notch_x1 = 0;
static float notch_x2 = 0;
static float notch_y1 = 0;
static float notch_y2 = 0;

static float agcGain = 1.0f;
static float agcEnvelope = 0.0f;



void DSP_Init(void)
{
    for(int i=0;i<LUT_SIZE;i++)
    {
        sineTable[i] =(int16_t)(32767 *sinf(2.0f*M_PI*i/LUT_SIZE));
    }

    DSP_SetMixerFrequency(38000.0f);


    phaseIncrement =(uint32_t)((loFrequency *4294967296.0f)/SAMPLE_RATE);

    /*20kHz Butterworth LPF*/
    Biquad_InitLowPass(&lowPass1,8000.0f,SAMPLE_RATE);
    Biquad_InitLowPass(&lowPass2,8000.0f,SAMPLE_RATE);


    float r = 0.9999f;

    float omega = 2.0f * M_PI * NOTCH_FREQ / SAMPLE_RATE;

    float c = cosf(omega);

    notch_b0 = 1.0f;
    notch_b1 = -2.0f * c;
    notch_b2 = 1.0f;

    notch_a1 = -2.0f * r * c;
    notch_a2 = r * r;


}


void DSP_SetOutputCallback(
        DSP_Callback_t callback)
{
    outputCallback = callback;
}


void DSP_ProcessBlock(
        uint16_t *input,
        uint32_t length)
{
    static uint16_t output[DSP_BLOCK_SIZE];

    for(uint32_t i=0;i<length;i++)
    {

        int32_t sample = (int32_t)input[i] - 2048;

        sample = DCBlock_Process(sample); //
        sample = NotchFilter(sample); // 50 Hz notch

        sample *= DSP_GAIN;

        sample = DSP_Mixer(sample);

        sample =Biquad_Process(&lowPass1,(float)sample);
        sample =Biquad_Process(&lowPass2,(float)sample);


        if(sample > 2047)
            sample = 2047;

        if(sample < -2048)
            sample = -2048;

//        filtered = AGC_Process(filtered);

        sample += 2048;


        if(sample < 0)
        {
        	sample = 0;
        }

        if(sample > 4095)
        {
        	sample = 4095;
        }





        output[i]=(uint16_t)sample;


    }



    if(outputCallback)
    {
        outputCallback(output,length);
    }
}


static void Biquad_InitLowPass(
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

static float Biquad_Process(
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


void DSP_SetMixerFrequency(float frequency)
{
    /* Clamp to valid range */
    if(frequency < 0.0f)
        frequency = 0.0f;

    if(frequency > 90000.0f)
        frequency = 90000.0f;

    loFrequency = frequency;

    phaseIncrement =
        (uint32_t)(
            (frequency * 4294967296.0f)
            / SAMPLE_RATE);
}



static int32_t DCBlock_Process(int32_t input)
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


static float NotchFilter(float input)
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


static float AGC_Process(float input)
{
    float absSample;

    absSample = fabsf(input);


    /*
     * Envelope follower
     */
    if(absSample > agcEnvelope)
    {
        agcEnvelope +=
            (absSample - agcEnvelope) * AGC_ATTACK;
    }
    else
    {
        agcEnvelope +=
            (absSample - agcEnvelope) * AGC_RELEASE;
    }


    /*
     * Adjust gain
     */
    if(agcEnvelope > AGC_TARGET)
    {
        agcGain -= 0.001f;
    }
    else
    {
        agcGain += 0.001f;
    }


    /*
     * Clamp gain
     */
    if(agcGain > AGC_MAX_GAIN)
        agcGain = AGC_MAX_GAIN;

    if(agcGain < AGC_MIN_GAIN)
        agcGain = AGC_MIN_GAIN;


    return input * agcGain;
}


static int32_t DSP_Mixer(int32_t sample)
{
    uint32_t index;
    int32_t oscillator;

    index = phase >> 24;

    oscillator = sineTable[index];

    int32_t mixed = sample * oscillator;

    mixed >>= 15;

    phase += phaseIncrement;


    return mixed;
}



