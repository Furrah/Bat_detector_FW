/*
 * bat_detect.h
 *
 * Bat-call detector. Looks for short ultrasonic pulses repeating in a
 * rhythm, using the energy of each audio block above the 10 kHz high-pass.
 * Runs in the audio interrupt, once per block (1.024 ms at 250 kHz).
 *
 * Stage 1 (monitor only): drives the LEDs and debug counters; it does not
 * yet switch the amplifier.
 */

#ifndef DSP_BAT_DETECT_H_
#define DSP_BAT_DETECT_H_

#include <stdint.h>


void BatDetect_Init(void);

/*
 * Feed one block's energy: the sum of the squared high-passed samples.
 * Call from the audio interrupt, once per block.
 */
void BatDetect_ProcessBlock(float energy);

/* 1 from the trigger (2 pulses close together) until the hold time expires */
uint8_t BatDetect_IsBatPresent(void);

/* Number of bat-like pulses so far; changes each time one ends */
uint32_t BatDetect_GetPulseCount(void);


/* Debug view, for Live Expressions and the USB capture (debug_link.c) */
extern volatile float    batEnergyDb;
extern volatile float    batFloorDb;
extern volatile uint32_t batPulses;
extern volatile uint32_t batTriggers;
extern volatile uint32_t batNoiseRuns;
extern volatile uint8_t  batPresent;


#endif /* DSP_BAT_DETECT_H_ */
