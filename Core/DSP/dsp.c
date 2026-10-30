/*
 * dsp.c
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */


#include "dsp.h"
#include "dsp_filters.h"
#include "fft.h"
#include <math.h>


static int16_t sineTable[LUT_SIZE];
static uint32_t phase = 0;
static uint32_t phaseIncrement;

static float loFrequency = 38000.0f;

static volatile float volumeGain = 0.0f;    // silent until the knob is first read

static DSP_Callback_t outputCallback = 0;

static Biquad_t lowPass1;
static Biquad_t lowPass2;
static Biquad_t high_pass;




void DSP_Init(void)
{
    for(int i=0;i<LUT_SIZE;i++)
    {
        sineTable[i] =(int16_t)(32767 *sinf(2.0f*M_PI*i/LUT_SIZE));
    }

    DSP_SetMixerFrequency(38000.0f);

    phaseIncrement = DSP_calc_phase_increment(loFrequency,SAMPLE_RATE );

    /*20kHz Butterworth LPF*/
    Biquad_InitLowPass(&lowPass1,8000.0f,SAMPLE_RATE);
    Biquad_InitLowPass(&lowPass2,8000.0f,SAMPLE_RATE);

    Biquad_InitHighPass(&high_pass, 10000.0f, SAMPLE_RATE);

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

    // one gain per block; written from the main loop
    const float gain = volumeGain;

    for(uint32_t i=0;i<length;i++)
    {

        int32_t sample = (int32_t)input[i] - 2048;



        sample =Biquad_Process(&high_pass,(float)sample);
        sample *= DSP_GAIN;

        sample = DSP_Mixer(sample);

        sample =Biquad_Process(&lowPass1,(float)sample);
        // volume applied before the float -> int truncation
        sample =(int32_t)(Biquad_Process(&lowPass2,(float)sample) * gain);

        sample = DSP_clamp_output(sample);

    	output[i]=(uint16_t)sample;



    }

    FFT_AddSamples(input, length);

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

    if(frequency > (float)AUDIO_LO_MAX_HZ)
        frequency = (float)AUDIO_LO_MAX_HZ;

    loFrequency = frequency;

    phaseIncrement = DSP_calc_phase_increment(frequency,SAMPLE_RATE );
}


void DSP_SetVolume(float position)
{
    if(position < 0.0f)
        position = 0.0f;

    if(position > 1.0f)
        position = 1.0f;

    volumeGain = position * position;
}

uint32_t DSP_calc_phase_increment(float frequency, float sample_rate){

	 const float TWO_POW_32 = 4294967296.0f;

	 float increment = (frequency * TWO_POW_32) / sample_rate;

	 return increment;
}



int32_t DSP_Mixer(int32_t sample)
{
    uint32_t index;
    int32_t oscillator;

    index = phase >> 23;

    oscillator = sineTable[index];

    int32_t mixed = sample * oscillator;

    mixed >>= 15;

    phase += phaseIncrement;


    return mixed;
}



