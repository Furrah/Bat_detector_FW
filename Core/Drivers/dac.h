/*
 * dac.h
 *
 * DAC1 to the amplifier: a circular DMA buffer of two blocks, clocked by
 * TIM2 like the ADC, so one block in gives one block out.
 */

#ifndef DRIVERS_DAC_H_
#define DRIVERS_DAC_H_

#include <stdint.h>

#include "pipeline_config.h"

void DAC_Init(void);
void DAC_Start(void);

/* The DSP's output callback: a block of DAC codes (0..4095) */
void DAC_WriteBlock(const uint16_t *input, uint32_t length);

#endif /* DRIVERS_DAC_H_ */
