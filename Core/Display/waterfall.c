/* vim: set ai et ts=4 sw=4: */
#include "waterfall.h"
#include "ili9341.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t s_scroll_pos  = 0;             // 0..WF_VSA-1, position within scroll area
static uint16_t s_row_buf[WF_SCREEN_W];        // one scanline, ready for ILI9341_DrawImage
static float    s_peak_db[WF_SCREEN_W];        // running peak-hold accumulator, per column
static float    s_floor_db = -60.0f;
static float    s_ceil_db  =   0.0f;
static uint32_t s_lo_freq_hz = 0;

void Waterfall_SetRange(float floor_db, float ceil_db) {
    s_floor_db = floor_db;
    s_ceil_db  = ceil_db;
}

static void ResetAccumulator(void) {
    for (uint16_t i = 0; i < WF_SCREEN_W; i++) {
        s_peak_db[i] = s_floor_db;
    }
}

void Waterfall_Init(void) {
    ILI9341_FillScreen(ILI9341_BLACK);
    ILI9341_SetScrollArea(WF_TFA, WF_VSA, WF_BFA);
    s_scroll_pos = 0;
    ILI9341_SetScrollStart(WF_TFA);   // absolute GRAM row = TFA + 0
    ResetAccumulator();

    // Header/footer stay blank until Waterfall_SetLOFrequency() is called --
    // do that once right after Init() with your startup LO setting, then
    // again wherever you currently retune it.
}

// 6-stop thermal palette: black -> blue -> cyan -> green -> yellow -> red.
// Built on ILI9341_COLOR565 rather than the named constants so the
// gradient is smooth instead of jumping between 8 fixed colours.
static uint16_t ColorFromDb(float mag_db) {
    if (mag_db < s_floor_db) mag_db = s_floor_db;
    if (mag_db > s_ceil_db)  mag_db = s_ceil_db;
    float t = (mag_db - s_floor_db) / (s_ceil_db - s_floor_db); // 0..1

    static const uint8_t stops[5][3] = {
        {   0,   0,   0 },  // black
        {   0,   0, 255 },  // blue
        {   0, 255, 255 },  // cyan
        {   0, 255,   0 },  // green
        { 255,   0,   0 },  // red
    };
    const int n_stops = 5;

    float pos = t * (n_stops - 1);
    int idx = (int)pos;
    if (idx >= n_stops - 1) idx = n_stops - 2;
    float frac = pos - idx;

    uint8_t r = (uint8_t)(stops[idx][0] + frac * (stops[idx + 1][0] - stops[idx][0]));
    uint8_t g = (uint8_t)(stops[idx][1] + frac * (stops[idx + 1][1] - stops[idx][1]));
    uint8_t b = (uint8_t)(stops[idx][2] + frac * (stops[idx + 1][2] - stops[idx][2]));

    return ILI9341_COLOR565(r, g, b);
}

// Maps a screen column (0..WF_SCREEN_W-1) to the real-world displayed
// frequency in Hz. Column 0 corresponds to the first bin kept after
// WF_SKIP_BINS is excluded; the last column corresponds to ~Nyquist.
// ASSUMES upper-sideband mixing -- change the '+' to a '-' below if your
// front end is lower-sideband or I/Q instead.
static uint32_t BinToDisplayFreq(uint16_t col) {
    const float bin_width_hz    = (float)WF_SAMPLE_RATE_HZ / (float)WF_FFT_LENGTH;
    const float min_baseband_hz = (float)WF_SKIP_BINS * bin_width_hz;
    const float max_baseband_hz = (float)WF_SAMPLE_RATE_HZ / 2.0f;

    float t = (float)col / (float)(WF_SCREEN_W - 1);
    float baseband_hz = min_baseband_hz + t * (max_baseband_hz - min_baseband_hz);

    return s_lo_freq_hz + (uint32_t)baseband_hz;
}

// Inverse of BinToDisplayFreq: given a real-world frequency, returns which
// screen column it falls on. Must be kept consistent with BinToDisplayFreq
// above -- same sideband assumption, same WF_SKIP_BINS offset.
static uint16_t DisplayFreqToColumn(uint32_t freq_hz) {
    const float bin_width_hz    = (float)WF_SAMPLE_RATE_HZ / (float)WF_FFT_LENGTH;
    const float min_baseband_hz = (float)WF_SKIP_BINS * bin_width_hz;
    const float max_baseband_hz = (float)WF_SAMPLE_RATE_HZ / 2.0f;

    float baseband_hz = (float)freq_hz - (float)s_lo_freq_hz;
    float t = (baseband_hz - min_baseband_hz) / (max_baseband_hz - min_baseband_hz);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    return (uint16_t)(t * (float)(WF_SCREEN_W - 1) + 0.5f);
}

// Spacing between axis ticks, in Hz. Ticks land on round multiples of this
// (20, 40, 60, 80 kHz ...) rather than whatever frequency a fixed column
// happens to land on.
#define WF_TICK_INTERVAL_HZ   20000UL
#define WF_MAX_TICKS          20   // safety bound, not a normal-use limit

// Redraws the bottom fixed strip with round-number kHz tick labels.
static void DrawFreqAxis(void) {
    ILI9341_FillRectangle(0, WF_SCREEN_H - WF_BFA, WF_SCREEN_W, WF_BFA, ILI9341_BLACK);

    uint32_t freq_min = BinToDisplayFreq(0);
    uint32_t freq_max = BinToDisplayFreq(WF_SCREEN_W - 1);

    // first round tick at or above the left edge of the visible range
    uint32_t first_tick = ((freq_min + WF_TICK_INTERVAL_HZ - 1) / WF_TICK_INTERVAL_HZ)
                           * WF_TICK_INTERVAL_HZ;

    char text[8];
    uint32_t freq = first_tick;
    for (int i = 0; i < WF_MAX_TICKS && freq <= freq_max; i++, freq += WF_TICK_INTERVAL_HZ) {
        uint16_t col = DisplayFreqToColumn(freq);

        snprintf(text, sizeof(text), "%lu", (unsigned long)(freq / 1000)); // whole kHz

        uint16_t text_w = (uint16_t)(strlen(text) * Font_7x10.width);
        int16_t  x = (int16_t)col - (int16_t)(text_w / 2);
        if (x < 0) x = 0;
        if (x + text_w > WF_SCREEN_W) x = WF_SCREEN_W - text_w;

        ILI9341_WriteString((uint16_t)x, WF_SCREEN_H - WF_BFA + 2, text,
                             Font_7x10, ILI9341_WHITE, ILI9341_BLACK);
    }
}

// Redraws the top fixed strip with the LO readout, in your existing format.
static void DrawLOLabel(void) {
    ILI9341_FillRectangle(0, 0, WF_SCREEN_W, WF_TFA, ILI9341_BLACK);

    char text[24];
    // same fixed 13-char format as main.c, clear of the battery readout
    snprintf(text, sizeof(text), "LO: %3lu.%lu kHz",
             (unsigned long)(s_lo_freq_hz / 1000),
             (unsigned long)((s_lo_freq_hz % 1000) / 100));

    ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}

void Waterfall_SetLOFrequency(uint32_t lo_freq_hz) {
    s_lo_freq_hz = lo_freq_hz;
    DrawLOLabel();
    DrawFreqAxis();
}


void Waterfall_Accumulate(const float *mag_linear, uint16_t n_bins) {
    if (n_bins <= WF_SKIP_BINS) return;

    uint16_t usable_bins = n_bins - WF_SKIP_BINS;

    for (uint16_t col = 0; col < WF_SCREEN_W; col++) {
        // Bin indices start at WF_SKIP_BINS so DC/leakage never enters any
        // column's peak-hold; the remaining bins are spread across
        // WF_SCREEN_W columns without dropping any (mirrors BinToDisplayFreq's
        // mapping above, so the plot and its axis labels stay consistent).
        uint32_t start = WF_SKIP_BINS + (uint32_t)col * usable_bins / WF_SCREEN_W;
        uint32_t end   = WF_SKIP_BINS + (uint32_t)(col + 1) * usable_bins / WF_SCREEN_W;
        if (end <= start) end = start + 1;
        if (end > n_bins) end = n_bins;

        float group_peak = mag_linear[start];
        for (uint32_t i = start + 1; i < end; i++) {
            if (mag_linear[i] > group_peak) group_peak = mag_linear[i];
        }

        // fft.c's output is unnormalised linear magnitude, not dB -
        // convert once here so SetRange/ColorFromDb only ever deal in dB.
        // +1e-6f avoids log10(0) when a bin is exactly zero.
        float group_peak_db = 20.0f * log10f(group_peak + 1e-6f);

        // Peak-hold: keep the loudest value seen since the last flush,
        // so a call that only shows up in one skipped FFT frame survives.
        if (group_peak_db > s_peak_db[col]) {
            s_peak_db[col] = group_peak_db;
        }
    }
}

void Waterfall_FlushRow(void) {
    for (uint16_t col = 0; col < WF_SCREEN_W; col++) {
        s_row_buf[col] = ColorFromDb(s_peak_db[col]);
    }

    // New row enters at the TOP of the scrolling area; older rows shift
    // down and eventually wrap back round to be overwritten. Walking the
    // pointer backwards each flush achieves this (see writeup for the
    // line-by-line trace of why).
    s_scroll_pos = (s_scroll_pos == 0) ? (WF_VSA - 1) : (s_scroll_pos - 1);
    uint16_t physical_row = WF_TFA + s_scroll_pos;

    ILI9341_DrawImage(0, physical_row, WF_SCREEN_W, 1, s_row_buf);
    ILI9341_SetScrollStart(physical_row);

    ResetAccumulator();
}
