/*
 * dac.h
 *
 *  Created on: 5 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DRIVERS_DAC_H_
#define DRIVERS_DAC_H_

#include <stdint.h>

#include "pipeline_config.h"


#define DAC_BUFFER_SIZE AUDIO_BUFFER_SIZE
#define DAC_BLOCK_SIZE AUDIO_BLOCK_SIZE


void DAC_Init(void);

void DAC_Start(void);

void DAC_Stop(void);


/*
 * Receive a block of processed samples
 * from DSP.
 */
void DAC_WriteBlock(
        const uint16_t *input,
        uint32_t length);


/* Blocks that collided with the DMA; should stay 0 */
extern volatile uint32_t dacLateBlocks;


#endif /* DRIVERS_DAC_H_ */
