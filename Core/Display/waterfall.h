/*
 * display.h
 *
 *  Created on: 24 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DISPLAY_WATERFALL_H_
#define DISPLAY_WATERFALL_H_



#include <stdint.h>

#include "pipeline_config.h"

// Native portrait orientation matches ILI9341_ROTATION already in use
// (MADCTL_MX | BGR) -- no MV bit, so hardware scroll behaves correctly.
#define WF_SCREEN_W   240   // frequency axis (columns)
#define WF_SCREEN_H   320   // full panel height

// Split the panel height into a fixed header (LO readout, Font_16x26),
// the scrolling waterfall, and a fixed footer (frequency-axis ticks,
// Font_7x10). Must sum to WF_SCREEN_H.
#define WF_TFA        34
#define WF_BFA        14
#define WF_VSA        (WF_SCREEN_H - WF_TFA - WF_BFA)

// Sample rate and FFT length come from pipeline_config.h, so they always
// match fft.c -- used to convert a screen column back to a baseband
// frequency for the axis labels.
#define WF_SAMPLE_RATE_HZ   AUDIO_SAMPLE_RATE_HZ
#define WF_FFT_LENGTH       AUDIO_FFT_LENGTH

// Number of leading FFT bins to exclude from the plot: DC (bin 0) and its
// local leakage into neighbouring bins. These dominate the magnitude scale
// without carrying signal, so leaving them in flattens everything else
// toward the palette floor. Start at 2 (bins 0 and 1); drop to 1 if bin 1
// turns out to carry real signal on your setup.
#define WF_SKIP_BINS   2

// Call once at startup, after ILI9341_Init().
void Waterfall_Init(void);

// Sets the local-oscillator frequency and redraws both the top LO
// readout and the bottom frequency-axis ticks. Call once at startup with
// your initial LO setting, and again wherever you currently retune it --
// NOT every flush, since WriteString is slow and the axis doesn't need
// to change at 25Hz.
//
// ASSUMES upper-sideband mixing (displayed_freq = lo_freq_hz +
// baseband_freq). If your front end is lower-sideband or I/Q, flip the
// '+' to a '-' in BinToDisplayFreq() inside waterfall.c.
void Waterfall_SetLOFrequency(uint32_t lo_freq_hz);

// Sets the dB range the colour palette maps across. Anything at/below
// floor_db is black, anything at/above ceil_db is white. Tune these once
// you know your mic's actual noise floor and typical call levels.
void Waterfall_SetRange(float floor_db, float ceil_db);

// Call this every time a new FFT frame is ready (i.e. at your ~192kHz/1024
// FFT cadence). Takes LINEAR magnitude straight from fftMagnitude[] -- the
// dB conversion happens internally. Just peak-holds into an accumulator --
// cheap, no SPI traffic -- so short bat calls aren't lost between display
// rows. n_bins is whatever your real-unpacking stage produces (check
// FFT_NUM_BINS in fft.h); it's re-binned onto WF_SCREEN_W columns internally.
void Waterfall_Accumulate(const float *mag_linear, uint16_t n_bins);

// Call this at your target display refresh rate (~20-30Hz is plenty for a
// human to read -- do NOT call it once per FFT frame, and don't call it
// from inside your ADC/DAC DMA callback since it blocks on SPI). Pushes
// the accumulated peak row to the display and resets the accumulator.
void Waterfall_FlushRow(void);

// The row being accumulated: each column's peak level in dB (columns with
// nothing louder sit at the palette floor), and in *frames how many FFT
// frames went into it (0 when the main loop had no time for the FFT).
// Read it just before Waterfall_FlushRow, which clears it. Used by
// auto-tune (autotune.h).
const float *Waterfall_GetRow(uint16_t *frames);

// The absolute frequency (Hz) of a screen column; a fractional column
// interpolates. Matches Waterfall_Accumulate's mapping of bins to columns.
float Waterfall_ColumnToHz(float column);

#endif /* DISPLAY_WATERFALL_H_ */
