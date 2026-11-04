/*
 * fft.h
 *
 * The spectrum for the waterfall: a 1024-point real FFT of the raw ADC
 * input, with a Hann window. The audio interrupt feeds samples in; the main
 * loop runs the transform when a frame is full.
 */

#ifndef DSP_FFT_H_
#define DSP_FFT_H_

#include <stdint.h>

#include "pipeline_config.h"

#define FFT_LENGTH    AUDIO_FFT_LENGTH          /* real samples per frame (1024) */
#define FFT_NUM_BINS  (FFT_LENGTH / 2U + 1U)    /* DC ... Nyquist (513), 244 Hz apart */

void FFT_Init(void);

/* Audio interrupt: add one block's raw ADC codes to the frame being filled.
 * Blocks arriving while a full frame waits for FFT_Process are dropped. */
void FFT_AddSamples(const uint16_t *input, uint32_t length);

/* Main loop: if a frame is full, transform it into fftMagnitude[] and set
 * fftMagnitudeReady */
void FFT_Process(void);

extern volatile uint8_t fftMagnitudeReady;
extern float fftMagnitude[FFT_NUM_BINS];

#endif /* DSP_FFT_H_ */
