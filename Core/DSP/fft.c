/*
 * fft.c
 *
 * A 1024-point real FFT without CMSIS-DSP. The 1024 real samples are packed
 * as 512 complex pairs (z[n] = x[2n] + j*x[2n+1]), transformed by a 512-point
 * radix-2 complex FFT, then unpacked into the 513 bins of the real spectrum
 * with the half-length real-FFT identity. The window, twiddle and unpack
 * tables are in flash (Core/DSP/Tables, made by Tools/gen_dsp_tables.py).
 */

#include "fft.h"

#include <math.h>

#include "main.h"
#include "Tables/dsp_tables.h"

/* Built at -O2 whatever the project setting: at -Og a frame took 2.3 ms and
 * the FFT dropped a third of the audio blocks. */
#pragma GCC optimize ("O2")

#define CFFT_LEN    512U        /* FFT_LENGTH / 2 */
#define CFFT_LOG2   9U

_Static_assert((DSP_TABLES_FFT_LENGTH == FFT_LENGTH) && (DSP_TABLES_CFFT_LENGTH == CFFT_LEN),
        "FFT tables out of date: run python Tools/gen_dsp_tables.py");

/* The frame being filled (re[n] = x[2n], im[n] = x[2n+1]), transformed in place */
static float cfftRe[CFFT_LEN];
static float cfftIm[CFFT_LEN];

static volatile uint32_t fillIndex = 0;
static volatile uint8_t  frameFull = 0;

float fftMagnitude[FFT_NUM_BINS];
volatile uint8_t fftMagnitudeReady = 0;


/* In-place radix-2 decimation-in-time complex FFT, natural order in and out */
static void CFFT_Radix2(float *re, float *im)
{
    /* bit-reversal permutation (RBIT reverses all 32 bits; keep the top 9) */
    for(uint32_t i = 0; i < CFFT_LEN; i++)
    {
        const uint32_t j = __RBIT(i) >> (32U - CFFT_LOG2);

        if(j > i)
        {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    /* butterflies */
    for(uint32_t size = 2; size <= CFFT_LEN; size <<= 1)
    {
        const uint32_t half = size >> 1;
        const uint32_t twiddleStep = CFFT_LEN / size;

        for(uint32_t base = 0; base < CFFT_LEN; base += size)
        {
            for(uint32_t j = 0; j < half; j++)
            {
                const float wr = fftTwiddleRe[j * twiddleStep];
                const float wi = fftTwiddleIm[j * twiddleStep];
                const uint32_t a = base + j;
                const uint32_t b = a + half;

                /* t = W * B */
                const float tr = wr * re[b] - wi * im[b];
                const float ti = wr * im[b] + wi * re[b];

                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}


void FFT_Init(void)
{
    fillIndex = 0;
    frameFull = 0;
    fftMagnitudeReady = 0;
}


void FFT_AddSamples(const uint16_t *input, uint32_t length)
{
    if(frameFull)
    {
        return;     /* the last frame hasn't been transformed yet: drop this block */
    }

    for(uint32_t i = 0; (i < length) && (fillIndex < FFT_LENGTH); i++)
    {
        /* 12-bit code -> about +-1, windowed by the sample's place in the frame */
        const float sample = ((float)input[i] - 2048.0f) * (1.0f / 2048.0f) * fftHannWindow[fillIndex];

        if((fillIndex & 1U) == 0U)
        {
            cfftRe[fillIndex >> 1] = sample;
        }
        else
        {
            cfftIm[fillIndex >> 1] = sample;
        }

        fillIndex++;
    }

    if(fillIndex >= FFT_LENGTH)
    {
        frameFull = 1;
    }
}


void FFT_Process(void)
{
    if(!frameFull)
    {
        return;
    }

    CFFT_Radix2(cfftRe, cfftIm);    /* now Z[k], k = 0..511 */

    /* unpack: X[k] = A[k] + W^k B[k], from the even (A) and odd (B) parts */
    for(uint32_t k = 0; k < CFFT_LEN; k++)
    {
        const uint32_t j = (k == 0) ? 0 : (CFFT_LEN - k);

        const float Are = 0.5f * (cfftRe[k] + cfftRe[j]);
        const float Aim = 0.5f * (cfftIm[k] - cfftIm[j]);
        const float Bre = 0.5f * (cfftIm[k] + cfftIm[j]);
        const float Bim = 0.5f * (cfftRe[j] - cfftRe[k]);

        if(k == 0)
        {
            fftMagnitude[0]        = fabsf(Are + Bre);      /* DC */
            fftMagnitude[CFFT_LEN] = fabsf(Are - Bre);      /* Nyquist */
        }
        else
        {
            const float c = fftUnpackCos[k];
            const float s = fftUnpackSin[k];
            const float Xre = Are + c * Bre + s * Bim;
            const float Xim = Aim + c * Bim - s * Bre;

            fftMagnitude[k] = sqrtf(Xre * Xre + Xim * Xim);
        }
    }

    /* DC and its leakage would swamp the waterfall's scale */
    fftMagnitude[0] = 0.0f;
    fftMagnitude[1] = 0.0f;

    fftMagnitudeReady = 1;
    fillIndex = 0;
    frameFull = 0;      /* last: the interrupt may start filling again */
}
