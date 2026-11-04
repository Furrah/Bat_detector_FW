/*
 * classifier_calls.c
 *
 * Stages 1-3 of the species classifier; see classifier_calls.h. Each step
 * follows Tools/classify.py, where the reasons for it are given; comments
 * here say what, the model says why.
 */

#include "classifier_calls.h"

#include <math.h>
#include <string.h>

#include "Tables/dsp_tables.h"

/* 4 frames per audio block, all in the main loop: built at -O2, like fft.c */
#pragma GCC optimize ("O2")

_Static_assert((DSP_TABLES_CLS_FFT_LENGTH == CLS_FFT_LENGTH) && (DSP_TABLES_CLS_CFFT_LENGTH == CLS_CFFT_LENGTH),
        "classifier FFT tables out of date: run python Tools/gen_dsp_tables.py");
_Static_assert(CLS_FFT_LENGTH == 2U * CLS_HOP, "a frame is the previous hop and this one");

#define CLS_NUM_BINS        (CLS_FFT_LENGTH / 2U + 1U)      /* 65: DC to Nyquist */
#define CLS_ADC_MIDSCALE    2048
#define CLS_ADC_MAX         4095U
#define CLS_TINY            1e-30f

/* Point flags */
#define PT_CLIP             2U
#define PT_RAISED           4U

typedef enum { SEG_IDLE = 0, SEG_ARMING, SEG_IN_CALL } SegState_t;


/* ---- stage 1 state */
static int32_t  dcTracker;                  /* slow DC estimate, ADC codes */
static int16_t  prevHop[CLS_HOP];           /* the last hop, DC removed: the first half of the next frame */
static float    cfftRe[CLS_CFFT_LENGTH];    /* the packed frame, transformed in place; then the whitened bins */
static float    cfftIm[CLS_CFFT_LENGTH];
static float    power[CLS_NUM_BINS];        /* |X[k]|^2 */
static float    noiseFloor[CLS_BINS];       /* per analysed bin */
static uint8_t  floorStarted;
static uint32_t frame;                      /* frames processed */

/* ---- stage 2 state: the segmenter and the open call's track */
static SegState_t segState;
static uint32_t segCount;                   /* frames above t_on while arming */
static uint32_t segOff;                     /* weak frames in a row */

static uint32_t trackFrame0;
static uint32_t trackLength;
static int16_t  trackF[CLS_TRACK_MAX];      /* 10 Hz units */
static int8_t   trackSnr[CLS_TRACK_MAX];    /* dB, rounded */
static uint8_t  trackFlags[CLS_TRACK_MAX];  /* PT_... */
static float    trackMaxSnr;
static uint32_t trackMaxIndex;
static uint8_t  trackHarmAtMax;
static uint32_t trackStrong;
static float    trackLastF;                 /* kHz, the last strong-enough point */
static uint8_t  trackCallFlags;


/* ------------------------------------------------------------------ stage 1 */

/* 64-point radix-2 complex FFT, in place, natural order in and out */
static void Cls_Cfft(float *re, float *im)
{
    for(uint32_t i = 0; i < CLS_CFFT_LENGTH; i++)
    {
        const uint32_t j = clsBitReverse[i];

        if(j > i)
        {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for(uint32_t size = 2U; size <= CLS_CFFT_LENGTH; size <<= 1)
    {
        const uint32_t half = size >> 1;
        const uint32_t step = CLS_CFFT_LENGTH / size;

        for(uint32_t base = 0; base < CLS_CFFT_LENGTH; base += size)
        {
            for(uint32_t j = 0; j < half; j++)
            {
                const float wr = clsTwiddleRe[j * step];
                const float wi = clsTwiddleIm[j * step];
                const uint32_t a = base + j;
                const uint32_t b = a + half;
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


/* The frame (previous hop, this hop), Hann-windowed, to its power spectrum */
static void Cls_PowerSpectrum(const int16_t *hop)
{
    for(uint32_t i = 0; i < CLS_CFFT_LENGTH; i++)
    {
        const uint32_t n = 2U * i;
        const float even = (float)((n < CLS_HOP) ? prevHop[n] : hop[n - CLS_HOP]);
        const float odd = (float)((n + 1U < CLS_HOP) ? prevHop[n + 1U] : hop[n + 1U - CLS_HOP]);

        cfftRe[i] = even * clsHannWindow[n];
        cfftIm[i] = odd * clsHannWindow[n + 1U];
    }

    Cls_Cfft(cfftRe, cfftIm);

    /* unpack the half-length FFT into the real spectrum, as fft.c */
    for(uint32_t k = 0; k < CLS_CFFT_LENGTH; k++)
    {
        const uint32_t j = (k == 0U) ? 0U : (CLS_CFFT_LENGTH - k);
        const float are = 0.5f * (cfftRe[k] + cfftRe[j]);
        const float aim = 0.5f * (cfftIm[k] - cfftIm[j]);
        const float bre = 0.5f * (cfftIm[k] + cfftIm[j]);
        const float bim = 0.5f * (cfftRe[j] - cfftRe[k]);

        if(k == 0U)
        {
            power[0] = (are + bre) * (are + bre);
            power[CLS_CFFT_LENGTH] = (are - bre) * (are - bre);
        }
        else
        {
            const float c = clsUnpackCos[k];
            const float s = clsUnpackSin[k];
            const float xre = are + c * bre + s * bim;
            const float xim = aim + c * bim - s * bre;

            power[k] = xre * xre + xim * xim;
        }
    }
}


/* Between calls: the floor rises slowly towards each bin's power and falls fast */
static void Cls_LearnFloor(void)
{
    for(uint32_t b = 0; b < CLS_BINS; b++)
    {
        const float pb = power[CLS_K_LO + b];
        const float rate = (pb > noiseFloor[b]) ? CLS_NF_UP : CLS_NF_DOWN;

        noiseFloor[b] += rate * (pb - noiseFloor[b]);
    }
}


/* ------------------------------------------------------------------ stage 2 */

static void Track_Reset(uint32_t frame0)
{
    trackFrame0 = frame0;
    trackLength = 0U;
    trackMaxSnr = -INFINITY;
    trackMaxIndex = 0U;
    trackHarmAtMax = 0U;
    trackStrong = 0U;
    trackLastF = 0.0f;
    trackCallFlags = 0U;
}


static void Track_Push(float f, float snr, uint8_t harm, uint8_t flags, uint8_t strong, uint8_t weak)
{
    if(trackLength < CLS_TRACK_MAX)
    {
        const long s = lrintf(snr);

        trackF[trackLength] = (int16_t)lrintf(f * 100.0f);
        trackSnr[trackLength] = (int8_t)((s < -128) ? -128 : ((s > 127) ? 127 : s));
        trackFlags[trackLength] = flags;

        if(snr > trackMaxSnr)
        {
            trackMaxIndex = trackLength;
            trackHarmAtMax = harm;
        }
        trackLength++;
    }
    else
    {
        trackCallFlags |= CLS_CALL_TRUNCATED;
    }

    if(snr > trackMaxSnr)
    {
        trackMaxSnr = snr;
    }
    if(strong)
    {
        trackStrong++;
    }
    if(!weak)
    {
        trackLastF = f;
    }
    if(flags & PT_CLIP)
    {
        trackCallFlags |= CLS_CALL_CLIPPED;
    }
}


/* 10 Hz units: the 3-point median of the track at i, its neighbours clamped to i0..i1 */
static int32_t Track_F10(uint32_t i, uint32_t i0, uint32_t i1)
{
    int32_t a = trackF[(i > i0) ? (i - 1U) : i0];
    int32_t b = trackF[i];
    int32_t c = trackF[(i < i1) ? (i + 1U) : i1];
    int32_t t;

    if(a > b) { t = a; a = b; b = t; }
    if(b > c) { t = b; b = c; c = t; }
    if(a > b) { b = a; }

    return b;
}

/* kHz */
static float Track_F(uint32_t i, uint32_t i0, uint32_t i1)
{
    return (float)Track_F10(i, i0, i1) * 0.01f;
}


/* kHz/ms: least-squares slope of the track over count points from start */
static float Track_Slope(uint32_t start, uint32_t count, uint32_t i0, uint32_t i1)
{
    const float xMean = 0.5f * (float)(count - 1U) * CLS_HOP_MS;
    float yMean = 0.0f;
    float sxy = 0.0f;
    float sxx = 0.0f;

    for(uint32_t k = 0; k < count; k++)
    {
        yMean += Track_F(start + k, i0, i1);
    }
    yMean /= (float)count;

    for(uint32_t k = 0; k < count; k++)
    {
        const float x = (float)k * CLS_HOP_MS - xMean;

        sxy += x * (Track_F(start + k, i0, i1) - yMean);
        sxx += x * x;
    }

    return sxy / sxx;
}


/* ------------------------------------------------------------------ stage 3 */

static uint8_t Cls_Features(ClsCall_t *c)
{
    const float threshold = fmaxf(CLS_T_ON_DB, trackMaxSnr - CLS_END_DROP_DB);
    uint32_t i0 = 0U;
    uint32_t i1 = 0U;
    uint8_t any = 0U;

    /* measured over the frames at or above max(t_on, peak - end_drop) */
    for(uint32_t i = 0; i < trackLength; i++)
    {
        if((float)trackSnr[i] >= threshold)
        {
            if(!any)
            {
                i0 = i;
                any = 1U;
            }
            i1 = i;
        }
    }

    if(!any)
    {
        return 0U;
    }

    const uint32_t m = i1 - i0 + 1U;

    memset(c, 0, sizeof(*c));
    c->frame0 = trackFrame0 + i0;
    c->t_s = (float)((double)c->frame0 * CLS_HOP / AUDIO_SAMPLE_RATE_HZ);
    c->dur_ms = (float)m * CLS_HOP_MS;
    c->f_start = Track_F(i0, i0, i1);
    c->f_end = Track_F(i1, i0, i1);
    c->f_peak = ((trackMaxIndex >= i0) && (trackMaxIndex <= i1)) ? Track_F(trackMaxIndex, i0, i1) : c->f_start;
    c->snr_db = trackMaxSnr;
    c->harmonic = trackHarmAtMax;
    c->flags = trackCallFlags;
    c->bw = c->f_start - c->f_end;
    c->label = CLS_NO_LABEL;

    if(m >= 6U)
    {
        const uint32_t s = m / 3U;

        c->slope_start = -Track_Slope(i0, s, i0, i1);
        c->slope_mid = -Track_Slope(i0 + s, s, i0, i1);
        c->slope_end = -Track_Slope(i1 - s + 1U, s, i0, i1);
    }
    else
    {
        c->slope_start = c->bw / c->dur_ms;
        c->slope_mid = c->slope_start;
        c->slope_end = c->slope_start;
    }

    uint32_t flat = 0U;

    for(uint32_t i = i0 + 1U; i <= i1; i++)
    {
        if((fabsf(Track_F(i - 1U, i0, i1) - Track_F(i, i0, i1)) / CLS_HOP_MS) < CLS_FLAT_SLOPE)
        {
            flat++;
        }
    }
    c->flat_frac = (m > 1U) ? ((float)flat / (float)(m - 1U)) : 0.0f;

    /* the characteristic frequency: the end of the flattest 4-frame stretch in
     * the last 40%. The slope's fit is in whole 10 Hz units (x2), so equal
     * slopes tie exactly, here and in the model: the latest one wins. */
    const uint32_t from = (i0 + 3U > i0 + (6U * m) / 10U) ? (i0 + 3U) : (i0 + (6U * m) / 10U);
    int32_t best = INT32_MAX;

    c->f_char = c->f_end;
    for(uint32_t i = from; i <= i1; i++)
    {
        int32_t slope = -3 * Track_F10(i - 3U, i0, i1) - Track_F10(i - 2U, i0, i1)
                        + Track_F10(i - 1U, i0, i1) + 3 * Track_F10(i, i0, i1);

        slope = (slope < 0) ? -slope : slope;
        if(slope <= best)
        {
            best = slope;
            c->f_char = Track_F(i, i0, i1);
        }
    }

    return 1U;
}


/* The track is complete: a call, unless it was a click or broadband */
static uint8_t Seg_EndCall(ClsCall_t *call)
{
    uint32_t raised = 0U;

    if(trackStrong < CLS_MIN_CALL_FRAMES)
    {
        return 0U;      /* a click */
    }

    for(uint32_t i = 0; i < trackLength; i++)
    {
        raised += (trackFlags[i] & PT_RAISED) ? 1U : 0U;
    }
    if((trackLength > 0U) && ((raised * 2U) >= trackLength))
    {
        return 0U;      /* mostly raised frames: a rub */
    }

    return Cls_Features(call);
}


static void Seg_Start(uint32_t at, float f, float snr, uint8_t harm, uint8_t flags)
{
    segState = SEG_ARMING;
    segCount = 1U;
    Track_Reset(at);
    Track_Push(f, snr, harm, flags, snr >= CLS_T_ON_DB, 0U);
}


static uint8_t Seg_Step(uint32_t at, float f, float snr, uint8_t harm, uint8_t flags, ClsCall_t *call)
{
    uint8_t ended = 0U;

    if(segState == SEG_IDLE)
    {
        if(snr >= CLS_T_ON_DB)
        {
            Seg_Start(at, f, snr, harm, flags);
        }
    }
    else if(segState == SEG_ARMING)
    {
        if(snr < CLS_T_ON_DB)
        {
            segState = SEG_IDLE;
        }
        else if((f - trackLastF) > CLS_RISE_MAX_KHZ)
        {
            Seg_Start(at, f, snr, harm, flags);         /* started on something lower: start again here */
        }
        else
        {
            Track_Push(f, snr, harm, flags, 1U, 0U);
            segCount++;
            if(segCount >= CLS_N_ON)
            {
                segState = SEG_IN_CALL;
                segOff = 0U;
            }
        }
    }
    else
    {
        const uint8_t weak = (snr < CLS_T_OFF_DB) || (snr < (trackMaxSnr - CLS_END_DROP_DB));

        if(!weak && ((f - trackLastF) > CLS_RISE_MAX_KHZ))
        {
            ended = Seg_EndCall(call);                  /* another bat, or the next call */
            Seg_Start(at, f, snr, harm, flags);
            return ended;
        }

        Track_Push(f, snr, harm, flags, snr >= CLS_T_ON_DB, weak);
        segOff = weak ? (segOff + 1U) : 0U;

        if(segOff >= CLS_N_OFF)
        {
            ended = Seg_EndCall(call);
            segState = SEG_IDLE;
        }
    }

    return ended;
}


/* ------------------------------------------------------------------ the frame */

static uint8_t Cls_Frame(uint8_t clipped, ClsCall_t *call)
{
    const float *pb = &power[CLS_K_LO];
    float *ratio = cfftRe;              /* free once the spectrum is out */

    if(!floorStarted)
    {
        for(uint32_t b = 0; b < CLS_BINS; b++)
        {
            noiseFloor[b] = fmaxf(pb[b], CLS_TINY);
        }
        floorStarted = 1U;
    }

    if(frame < CLS_SETTLE_FRAMES)
    {
        Cls_LearnFloor();
        return 0U;
    }

    /* whitened bins, and the peak over the whole band */
    uint32_t kg = 0U;

    for(uint32_t b = 0; b < CLS_BINS; b++)
    {
        ratio[b] = pb[b] / (noiseFloor[b] * CLS_NF_BIAS);
        if(ratio[b] > ratio[kg])
        {
            kg = b;
        }
    }

    /* in a call, no lower than drop_max below its last point */
    uint32_t kr = kg;

    if(segState != SEG_IDLE)
    {
        int32_t lo = (int32_t)ceilf((trackLastF - CLS_DROP_MAX_KHZ) / CLS_BIN_KHZ) - (int32_t)CLS_K_LO;

        lo = (lo < 0) ? 0 : ((lo > (int32_t)CLS_BINS - 1) ? ((int32_t)CLS_BINS - 1) : lo);
        if(kg < (uint32_t)lo)
        {
            kr = (uint32_t)lo;
            for(uint32_t b = (uint32_t)lo + 1U; b < CLS_BINS; b++)
            {
                if(ratio[b] > ratio[kr])
                {
                    kr = b;
                }
            }
        }
    }

    float snr = (ratio[kr] > 0.0f) ? (10.0f * log10f(ratio[kr])) : -99.0f;

    /* broadband tests: counts over the bins */
    const float near = ratio[kg] * CLS_NEAR_RATIO;
    uint32_t nBroad = 0U;
    uint32_t nNear = 0U;
    uint32_t nRaised = 0U;

    for(uint32_t b = 0; b < CLS_BINS; b++)
    {
        nBroad += (ratio[b] > CLS_BROAD_RATIO) ? 1U : 0U;
        nNear += (ratio[b] > near) ? 1U : 0U;
        nRaised += (ratio[b] > CLS_RAISED_RATIO) ? 1U : 0U;
    }

    const uint8_t raised = (nRaised * 2U) > CLS_BINS;

    if(((nBroad * 2U) > CLS_BINS) || ((nNear * 2U) > CLS_BINS))
    {
        snr = fminf(snr, CLS_T_OFF_DB - 0.01f);    /* broadband: can't start or carry a call */
    }

    /* the frequency: the louder neighbour if there is one, then a parabola on log power */
    uint32_t kb = kr + CLS_K_LO;

    if((kb < CLS_K_HI) && (power[kb + 1U] > power[kb]))
    {
        kb++;
    }
    else if((kb > CLS_K_LO) && (power[kb - 1U] > power[kb]))
    {
        kb--;
    }

    const float a = logf(fmaxf(power[kb - 1U], CLS_TINY));
    const float b = logf(fmaxf(power[kb], CLS_TINY));
    const float c = logf(fmaxf(power[kb + 1U], CLS_TINY));
    const float curve = a - 2.0f * b + c;
    float offset = 0.0f;

    if(curve < 0.0f)
    {
        offset = fminf(fmaxf(0.5f * (a - c) / curve, -0.5f), 0.5f);
    }

    const float f = ((float)kb + offset) * CLS_BIN_KHZ;
    const uint8_t harm = ((2U * kb) <= CLS_K_HI)
            && (power[2U * kb] > (CLS_HARM_RATIO * CLS_NF_BIAS * noiseFloor[2U * kb - CLS_K_LO]));
    const uint8_t flags = (clipped ? PT_CLIP : 0U) | (raised ? PT_RAISED : 0U);

    const uint8_t ended = Seg_Step(frame, f, snr, harm, flags, call);

    if(segState == SEG_IDLE)
    {
        Cls_LearnFloor();
    }

    return ended;
}


/* ------------------------------------------------------------------ API */

void ClsCalls_Reset(void)
{
    dcTracker = CLS_ADC_MIDSCALE;
    memset(prevHop, 0, sizeof(prevHop));
    floorStarted = 0U;
    frame = 0U;
    segState = SEG_IDLE;
    segCount = 0U;
    segOff = 0U;
    Track_Reset(0U);
}


uint8_t ClsCalls_ProcessHop(const uint16_t *codes, ClsCall_t *call)
{
    int16_t hop[CLS_HOP];
    uint32_t sum = 0U;
    uint8_t clipped = 0U;

    /* the DC tracker's value before this hop is subtracted from it; then it
     * moves 1/64 of the way to the hop's mean (integers, as the model) */
    for(uint32_t i = 0; i < CLS_HOP; i++)
    {
        sum += codes[i];
        clipped |= (codes[i] == 0U) || (codes[i] == CLS_ADC_MAX);
        hop[i] = (int16_t)((int32_t)codes[i] - dcTracker);
    }
    dcTracker += ((int32_t)(sum / CLS_HOP) - dcTracker) >> 6;

    Cls_PowerSpectrum(hop);
    memcpy(prevHop, hop, sizeof(prevHop));

    const uint8_t ended = Cls_Frame(clipped, call);

    frame++;
    return ended;
}


uint8_t ClsCalls_Flush(ClsCall_t *call)
{
    uint8_t ended = 0U;

    if(segState == SEG_IN_CALL)
    {
        ended = Seg_EndCall(call);
    }
    segState = SEG_IDLE;

    return ended;
}


uint32_t ClsCalls_GetFrames(void)
{
    return frame;
}
