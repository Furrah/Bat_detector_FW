/*
 * waterfall.c
 *
 * The scrolling spectrogram; see waterfall.h.
 *
 * Bins to columns: the FFT gives 513 bins of 244 Hz (0 to 125 kHz). Bins 0
 * and 1 (DC and its leakage) are left out, because they'd set the scale
 * and flatten everything else. The other 511 are shared out over the 240
 * columns, about 2.1 bins each, and each column shows the loudest of its
 * bins.
 */

#include "waterfall.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ili9341.h"
#include "pipeline_config.h"

#define SKIP_BINS       2U                                  /* DC and its leakage */
#define FFT_BINS        ((AUDIO_FFT_LENGTH / 2U) + 1U)
#define USABLE_BINS     (FFT_BINS - SKIP_BINS)
#define BIN_HZ          ((float)AUDIO_SAMPLE_RATE_HZ / (float)AUDIO_FFT_LENGTH)
#define TICK_HZ         20000U                              /* an axis label every 20 kHz */

static float    peakDb[WF_SCREEN_W];        /* the row being built */
static uint16_t rowFrames = 0;              /* FFT frames in it so far */
static uint16_t rowPixels[WF_SCREEN_W];     /* the row as RGB565, for the display */
static uint16_t scrollRow = 0;              /* 0 .. WF_VSA-1: where the newest row is */
static float    floorDb = -60.0f;
static float    ceilDb = 0.0f;


static void Waterfall_ClearRow(void)
{
    for(uint16_t i = 0; i < WF_SCREEN_W; i++)
    {
        peakDb[i] = floorDb;
    }
    rowFrames = 0;
}


/* Thermal palette: black -> blue -> cyan -> green -> red */
static uint16_t Waterfall_Colour(float db)
{
    static const uint8_t stops[5][3] =
    {
        {   0,   0,   0 },
        {   0,   0, 255 },
        {   0, 255, 255 },
        {   0, 255,   0 },
        { 255,   0,   0 },
    };

    if(db < floorDb) db = floorDb;
    if(db > ceilDb) db = ceilDb;

    const float position = ((db - floorDb) / (ceilDb - floorDb)) * 4.0f;   /* 0..4 along the stops */
    int stop = (int)position;

    if(stop > 3)
    {
        stop = 3;
    }

    const float frac = position - (float)stop;
    const uint8_t r = (uint8_t)(stops[stop][0] + frac * (stops[stop + 1][0] - stops[stop][0]));
    const uint8_t g = (uint8_t)(stops[stop][1] + frac * (stops[stop + 1][1] - stops[stop][1]));
    const uint8_t b = (uint8_t)(stops[stop][2] + frac * (stops[stop + 1][2] - stops[stop][2]));

    return ILI9341_COLOR565(r, g, b);
}


void Waterfall_Init(void)
{
    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_SetScrollArea(WF_TFA, WF_VSA, WF_BFA);
    scrollRow = 0;
    ILI9341_SetScrollStart(WF_TFA);
    Waterfall_ClearRow();
}


void Waterfall_SetRange(float lowDb, float highDb)
{
    floorDb = lowDb;
    ceilDb = highDb;
}


float Waterfall_ColumnToHz(float column)
{
    /* column c holds bins SKIP + c x 511/240 up to the next column's first;
     * this is the middle of that range */
    const float bin = (float)SKIP_BINS + ((column + 0.5f) * (float)USABLE_BINS / (float)WF_SCREEN_W) - 0.5f;

    return bin * BIN_HZ;
}


void Waterfall_DrawAxis(void)
{
    char text[8];

    ILI9341_FillRectangle(0, WF_SCREEN_H - WF_BFA, WF_SCREEN_W, WF_BFA, ILI9341_BLACK);

    for(uint32_t hz = TICK_HZ; hz <= (AUDIO_SAMPLE_RATE_HZ / 2U); hz += TICK_HZ)
    {
        /* the inverse of Waterfall_ColumnToHz */
        const float column = ((((float)hz / BIN_HZ) + 0.5f - (float)SKIP_BINS) * (float)WF_SCREEN_W / (float)USABLE_BINS) - 0.5f;

        snprintf(text, sizeof(text), "%lu", (unsigned long)(hz / 1000U));

        const int16_t width = (int16_t)(strlen(text) * Font_7x10.width);
        int16_t x = (int16_t)(column + 0.5f) - (width / 2);     /* centred on its column */

        if(x < 0) x = 0;
        if((x + width) > WF_SCREEN_W) x = WF_SCREEN_W - width;

        ILI9341_WriteString((uint16_t)x, WF_SCREEN_H - WF_BFA + 2, text, Font_7x10, ILI9341_WHITE, ILI9341_BLACK);
    }
}


void Waterfall_Accumulate(const float *magnitude, uint16_t bins)
{
    if(bins < FFT_BINS)
    {
        return;
    }

    for(uint16_t column = 0; column < WF_SCREEN_W; column++)
    {
        const uint32_t first = SKIP_BINS + ((uint32_t)column * USABLE_BINS) / WF_SCREEN_W;
        const uint32_t end = SKIP_BINS + ((uint32_t)(column + 1U) * USABLE_BINS) / WF_SCREEN_W;
        float peak = magnitude[first];

        for(uint32_t i = first + 1U; i < end; i++)
        {
            if(magnitude[i] > peak)
            {
                peak = magnitude[i];
            }
        }

        /* the FFT gives linear magnitude; the 1e-6 keeps log10 off 0 */
        const float db = 20.0f * log10f(peak + 1e-6f);

        if(db > peakDb[column])
        {
            peakDb[column] = db;
        }
    }

    rowFrames++;
}


const float *Waterfall_GetRow(uint16_t *frames)
{
    *frames = rowFrames;
    return peakDb;
}


void Waterfall_FlushRow(void)
{
    for(uint16_t column = 0; column < WF_SCREEN_W; column++)
    {
        rowPixels[column] = Waterfall_Colour(peakDb[column]);
    }

    /* Step the scroll start back one line and draw there: the display shows
     * from the scroll start downwards, so the new row is at the top and the
     * rest move down one, with the oldest falling off the bottom. */
    scrollRow = (scrollRow == 0U) ? (WF_VSA - 1U) : (scrollRow - 1U);

    const uint16_t line = WF_TFA + scrollRow;

    ILI9341_DrawImage(0, line, WF_SCREEN_W, 1, rowPixels);
    ILI9341_SetScrollStart(line);

    Waterfall_ClearRow();
}
