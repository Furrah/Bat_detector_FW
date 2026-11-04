/*
 * dsp.c
 *
 * Per sample:  code - 2048 -> 10 kHz high-pass -> x DSP_GAIN -> x LO (mixer)
 *              -> 8 kHz low-pass x2 -> x volume -> + 2048 -> DAC
 *
 * The mixer multiplies by a sine at the LO frequency, which shifts a bat
 * call at f down to |f - LO|; the low-passes keep only what's audible.
 * The LO is a 32-bit phase accumulator indexing a 512-entry sine table.
 */

#include "dsp.h"

#include "bat_detect.h"
#include "dsp_filters.h"
#include "fft.h"
#include "playback.h"
#include "Tables/dsp_tables.h"

_Static_assert(DSP_TABLES_SINE_LENGTH == LUT_SIZE, "sine table out of date: run python Tools/gen_dsp_tables.py");

#define HIGH_PASS_HZ    10000.0f
#define LOW_PASS_HZ     8000.0f

static uint32_t phase = 0;
static volatile uint32_t phaseIncrement = 0;    /* LO frequency x 2^32 / sample rate */
static volatile float volumeGain = 0.0f;        /* silent until the knob is first read */

static DSP_Callback_t outputCallback = 0;

static Biquad_t highPass;
static Biquad_t lowPass1;
static Biquad_t lowPass2;


void DSP_Init(void)
{
    DSP_SetMixerFrequency(0.0f);

    Biquad_InitHighPass(&highPass, HIGH_PASS_HZ, SAMPLE_RATE);
    Biquad_InitLowPass(&lowPass1, LOW_PASS_HZ, SAMPLE_RATE);
    Biquad_InitLowPass(&lowPass2, LOW_PASS_HZ, SAMPLE_RATE);
}


void DSP_SetOutputCallback(DSP_Callback_t callback)
{
    outputCallback = callback;
}


/* Multiply by the LO: Q15 sine, top 9 bits of the phase index the table */
static int32_t DSP_Mixer(int32_t sample)
{
    const int32_t oscillator = dspSineTable[phase >> 23];

    phase += phaseIncrement;
    return (sample * oscillator) >> 15;
}


int32_t DSP_ClampOutput(int32_t sample)
{
    sample += 2048;

    if(sample < 0)
    {
        sample = 0;
    }
    else if(sample > 4095)
    {
        sample = 4095;
    }

    return sample;
}


void DSP_ProcessBlock(uint16_t *input, uint32_t length)
{
    static uint16_t output[AUDIO_BLOCK_SIZE];
    const float gain = volumeGain;                  /* one gain per block */
    const uint8_t playing = Playback_IsActive();    /* playback replaces the heterodyne */
    float energy = 0.0f;                            /* above 10 kHz, for the bat detector */

    for(uint32_t i = 0; i < length; i++)
    {
        const float highPassed = Biquad_Process(&highPass, (float)((int32_t)input[i] - 2048));

        energy += highPassed * highPassed;

        if(playing)
        {
            continue;
        }

        int32_t sample = DSP_Mixer((int32_t)highPassed * DSP_GAIN);

        sample = (int32_t)Biquad_Process(&lowPass1, (float)sample);
        sample = (int32_t)(Biquad_Process(&lowPass2, (float)sample) * gain);
        output[i] = (uint16_t)DSP_ClampOutput(sample);
    }

    if(playing)
    {
        Playback_Render(output, length, gain);
    }

    BatDetect_ProcessBlock(energy);
    FFT_AddSamples(input, length);

    if(outputCallback)
    {
        outputCallback(output, length);
    }
}


void DSP_SetMixerFrequency(float frequency)
{
    if(frequency < 0.0f)
    {
        frequency = 0.0f;
    }
    if(frequency > (float)AUDIO_LO_MAX_HZ)
    {
        frequency = (float)AUDIO_LO_MAX_HZ;
    }

    phaseIncrement = (uint32_t)((frequency * 4294967296.0f) / SAMPLE_RATE);
}


void DSP_SetVolume(float position)
{
    if(position < 0.0f)
    {
        position = 0.0f;
    }
    if(position > 1.0f)
    {
        position = 1.0f;
    }

    volumeGain = position * position;
}
