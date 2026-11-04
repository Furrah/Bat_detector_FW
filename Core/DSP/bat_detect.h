/*
 * bat_detect.h
 *
 * Bat-call detector: looks for short ultrasonic pulses repeating in a
 * rhythm, using the energy of each audio block above 10 kHz. Runs in the
 * audio interrupt, once per block (1.024 ms). Drives the LEDs (ui.c) and
 * tells auto-tune when a bat pass is on.
 */

#ifndef DSP_BAT_DETECT_H_
#define DSP_BAT_DETECT_H_

#include <stdint.h>

void BatDetect_Init(void);

/* Audio interrupt: one block's energy, the sum of its squared high-passed samples */
void BatDetect_ProcessBlock(float energy);

/* 1 from the third pulse in a row until 2 s after the last */
uint8_t BatDetect_IsBatPresent(void);

/* Bat-like pulses so far: changes each time one ends */
uint32_t BatDetect_GetPulseCount(void);

#endif /* DSP_BAT_DETECT_H_ */
