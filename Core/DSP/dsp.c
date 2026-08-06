/*
 * dsp.c
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */


#include "dsp.h"
#include "dsp_filters.h"

#include <math.h>

static int16_t sineTable[LUT_SIZE];
static uint32_t phase = 0;
static uint32_t phaseIncrement;

static volatile float loFrequency = 38000.0f;

static DSP_Callback_t outputCallback = 0;

static Biquad_t lowPass1;
static Biquad_t lowPass2;



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

    notch_filter_init();


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

//        sample = DCBlock_Process(sample); //
//        sample = NotchFilter(sample); // 50 Hz notch
//
        sample *= DSP_GAIN;
//
        sample = DSP_Mixer(sample);
//
        sample =Biquad_Process(&lowPass1,(float)sample);
        sample =Biquad_Process(&lowPass2,(float)sample);
//
//


        sample = DSP_clamp_output(sample);

        output[i]=(uint16_t)sample;


    }



    if(outputCallback)
    {
        outputCallback(output,length);
    }
}


int32_t DSP_clamp_output(int32_t sample){

    // convert to unsigned DAC range
    sample += 2048;


    // clamp to DAC output values (12 bit)
    if(sample < 0)
    	sample = 0;
    else if(sample > 4095)
    	sample = 4095;


    return sample;


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



