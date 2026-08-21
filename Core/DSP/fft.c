/*
 * fft.c
 *
 *  Created on: 20 Aug 2026
 *      Author: Joe Spencer
 */


/*
 * fft.c
 *
 * Self-contained real FFT, no CMSIS-DSP dependency.
 *
 * Method: pack 1024 real samples as 512 complex pairs
 * (z[n] = x[2n] + j*x[2n+1]), run a 512-point radix-2 complex FFT
 * on that, then unpack the true 1024-point real spectrum from the
 * 512-point complex result using the standard half-length real-FFT
 * identity. This is the same trick CMSIS's arm_rfft_fast_f32 uses
 * internally - we're just doing it by hand.
 */

#include "fft.h"
#include <math.h>

#define CFFT_LEN    512U     // = FFT_LENGTH / 2
#define CFFT_LOG2   9U       // log2(CFFT_LEN)

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* In-place complex FFT working buffers. Also used directly as the
 * real-sample accumulation target - re[n]/im[n] hold x[2n]/x[2n+1]
 * until FFT_Process() overwrites them in place with the transform. */
static float cfftRe[CFFT_LEN];
static float cfftIm[CFFT_LEN];

/* Twiddle factors for the 512-point CFFT: W_512^k, k = 0..255 */
static float twiddleRe[CFFT_LEN / 2U];
static float twiddleIm[CFFT_LEN / 2U];

static volatile uint32_t fftFillIndex  = 0;
static volatile uint8_t  fftBufferFull = 0;

float fftMagnitude[FFT_NUM_BINS];
volatile uint8_t  fftMagnitudeReady = 0;
volatile uint32_t fftDroppedBlocks  = 0;


static uint32_t BitReverse(uint32_t x, uint32_t bits)
{
    uint32_t result = 0;

    for(uint32_t i = 0; i < bits; i++)
    {
        result = (result << 1) | (x & 1U);
        x >>= 1;
    }

    return result;
}


static void GenerateTwiddles(void)
{
    for(uint32_t k = 0; k < CFFT_LEN / 2U; k++)
    {
        float angle = -2.0f * M_PI * (float)k / (float)CFFT_LEN;
        twiddleRe[k] = cosf(angle);
        twiddleIm[k] = sinf(angle);
    }
}


/*
 * In-place radix-2 decimation-in-time complex FFT, length CFFT_LEN.
 * Standard textbook form: bit-reversal permutation, then log2(N)
 * butterfly stages. Input and output both in natural order.
 */
static void CFFT_Radix2(float *re, float *im)
{
    // Bit-reversal permutation
    for(uint32_t i = 0; i < CFFT_LEN; i++)
    {
        uint32_t j = BitReverse(i, CFFT_LOG2);

        if(j > i)
        {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    // Butterfly stages
    for(uint32_t stage = 1; stage <= CFFT_LOG2; stage++)
    {
        uint32_t butterflySize = 1U << stage;          // 2, 4, 8, ... 512
        uint32_t half          = butterflySize >> 1U;
        uint32_t twiddleStep   = CFFT_LEN / butterflySize;

        for(uint32_t base = 0; base < CFFT_LEN; base += butterflySize)
        {
            for(uint32_t j = 0; j < half; j++)
            {
                uint32_t twIdx = j * twiddleStep;
                float wr = twiddleRe[twIdx];
                float wi = twiddleIm[twIdx];

                uint32_t idxA = base + j;
                uint32_t idxB = idxA + half;

                float br = re[idxB];
                float bi = im[idxB];

                // t = W * B
                float tr = wr * br - wi * bi;
                float ti = wr * bi + wi * br;

                float ar = re[idxA];
                float ai = im[idxA];

                re[idxA] = ar + tr;
                im[idxA] = ai + ti;
                re[idxB] = ar - tr;
                im[idxB] = ai - ti;
            }
        }
    }
}


void FFT_Init(void)
{
    GenerateTwiddles();

    fftFillIndex      = 0;
    fftBufferFull     = 0;
    fftMagnitudeReady = 0;
}


void FFT_AddSamples(const uint16_t *input, uint32_t length)
{
    if(fftBufferFull)
    {
        // FFT_Process() hasn't drained the last capture yet.
        // Drop this block rather than tear the buffer mid-fill.
        fftDroppedBlocks++;
        return;
    }

    for(uint32_t i = 0; i < length; i++)
    {
        if(fftFillIndex >= FFT_LENGTH)
        {
            break;

        }

        // 12-bit ADC code (0..4095, midpoint ~2048) -> roughly +/-1.0f
        // could probably make this better by taking the average over the window. the real offset is around 2079
        float sample = ((float)input[i] - 2048.0f) * (1.0f / 2048.0f);

        // Pack alternating samples into the complex pair z[n] = x[2n] + j*x[2n+1]
        uint32_t n = fftFillIndex >> 1U;

        if((fftFillIndex & 1U) == 0U)
        {
            cfftRe[n] = sample;
        }
        else
        {
            cfftIm[n] = sample;
        }

        fftFillIndex++;
    }

    if(fftFillIndex >= FFT_LENGTH)
    {
        fftBufferFull = 1;
    }
}


void FFT_Process(void)
{
    if(!fftBufferFull)
    {
        return;
    }

    CFFT_Radix2(cfftRe, cfftIm);   // cfftRe/cfftIm now hold Z[k], k = 0..511

    const float N = (float)FFT_LENGTH;

    for(uint32_t k = 0; k < CFFT_LEN; k++)
    {
        uint32_t j = (k == 0) ? 0 : (CFFT_LEN - k);

        float Zr_k = cfftRe[k];
        float Zi_k = cfftIm[k];
        float Zr_j = cfftRe[j];
        float Zi_j = cfftIm[j];

        // Even/odd parts of the original real sequence's spectrum
        float Are = 0.5f * (Zr_k + Zr_j);
        float Aim = 0.5f * (Zi_k - Zi_j);
        float Bre = 0.5f * (Zi_k + Zi_j);
        float Bim = 0.5f * (Zr_j - Zr_k);

        if(k == 0)
        {
            // Both DC and Nyquist fall out of this special case
            fftMagnitude[0]        = fabsf(Are + Bre);   // X[0]
            fftMagnitude[CFFT_LEN] = fabsf(Are - Bre);   // X[N/2]
        }
        else
        {
            float angle = 2.0f * M_PI * (float)k / N;
            float c = cosf(angle);
            float s = sinf(angle);

            float Xre = Are + c * Bre + s * Bim;
            float Xim = Aim + c * Bim - s * Bre;

            fftMagnitude[k] = sqrtf(Xre * Xre + Xim * Xim);
        }
    }

    fftMagnitudeReady = 1;

    fftFillIndex  = 0;
    fftBufferFull = 0;   // safe to resume filling now
}
