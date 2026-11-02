/*
 * fft.h
 *
 *  Created on: 20 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DSP_FFT_H_
#define DSP_FFT_H_

#include <stdint.h>

#include "pipeline_config.h"

#define FFT_LENGTH    AUDIO_FFT_LENGTH         // real samples per capture
#define FFT_NUM_BINS  (FFT_LENGTH / 2U + 1U)    // DC ... Nyquist inclusive (513)

void FFT_Init(void);

/*
 * Feed one processed block into the accumulation buffer.
 * Call this from DSP_ProcessBlock (ISR context) - it only ever
 * writes floats, never runs the transform.
 */
void FFT_AddSamples(const uint16_t *input, uint32_t length);

/*
 * Call from the main() while(1) loop, every iteration. No-op unless
 * a full 1024-sample capture is ready; runs the transform and
 * updates fftMagnitude[] when it is.
 */
void FFT_Process(void);

/*
 * Debug capture, for checking the FFT against numpy: the next whole frame's
 * raw ADC codes (FFT_LENGTH, in order) go into raw, and FFT_Process's
 * magnitudes for that frame (FFT_NUM_BINS, before bins 0-1 are blanked)
 * into magnitude. Main loop only.
 */
void FFT_RequestCapture(uint16_t *raw, float *magnitude);
uint8_t FFT_CaptureDone(void);
void FFT_CancelCapture(void);

extern volatile uint8_t fftMagnitudeReady;
extern volatile uint32_t fftDroppedBlocks;
extern volatile uint32_t fftProcessCyclesAvg;
extern volatile uint32_t fftProcessCyclesMax;
extern float fftMagnitude[FFT_NUM_BINS];

#endif /* DSP_FFT_H_ */
