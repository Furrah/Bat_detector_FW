/*
 * dsp_agc.c
 *
 *  Created on: 6 Aug 2026
 *      Author: Joe Spencer
 */


#include "dsp.h"
#include "dsp_agc.h"
#include "math.h"

float agcGain = 1.0f;
float agcEnvelope = 0.0f;


float AGC_Process(float input)
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
