/*
 * waterfall.h
 *
 * The scrolling spectrogram on the ILI9341, in portrait. Frequency runs
 * across (240 columns, 0 to 125 kHz, ~0.52 kHz each) and time runs down:
 * each new row enters at the top of the scroll area and older rows move
 * down. The scrolling is done by the display itself (VSCRDEF/VSCRSADD), so
 * a row costs one 240-pixel write.
 *
 * The FFT (fft.h) runs faster than the rows, so each column keeps the peak
 * of every FFT frame since the last row: a call that lasts one frame still
 * shows. The FFT is of the raw ADC, so the axis is absolute frequency
 * whatever the LO is.
 *
 * Screen layout, top to bottom: a 34-pixel header (ui.c writes the LO,
 * messages and battery there), the scroll area, a 14-pixel axis strip.
 */

#ifndef DISPLAY_WATERFALL_H_
#define DISPLAY_WATERFALL_H_

#include <stdint.h>

#define WF_SCREEN_W     240     /* columns: the frequency axis */
#define WF_SCREEN_H     320
#define WF_TFA          34      /* fixed header, pixels */
#define WF_BFA          14      /* fixed axis strip at the bottom */
#define WF_VSA          (WF_SCREEN_H - WF_TFA - WF_BFA)     /* scroll area */

/* After ILI9341_Init: clears the screen and sets up the scroll area */
void Waterfall_Init(void);

/* The dB range the palette spans: floor and below is black, ceiling and above red */
void Waterfall_SetRange(float lowDb, float highDb);

/* Draw the kHz ticks in the bottom strip (once, after Waterfall_Init) */
void Waterfall_DrawAxis(void);

/* Each FFT frame: peak-hold fftMagnitude[] (linear) into the row being built */
void Waterfall_Accumulate(const float *magnitude, uint16_t bins);

/* The row being built: each column's peak in dB, and in *frames how many
 * FFT frames went into it. Read it before Waterfall_FlushRow clears it. */
const float *Waterfall_GetRow(uint16_t *frames);

/* The frequency (Hz) at a column; a fractional column interpolates */
float Waterfall_ColumnToHz(float column);

/* Every 40 ms: draw the row at the top of the scroll area and start a new one */
void Waterfall_FlushRow(void);

#endif /* DISPLAY_WATERFALL_H_ */
