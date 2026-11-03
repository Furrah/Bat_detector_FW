/*
 * dac.c
 *
 *  Created on: 5 Aug 2026
 *      Author: Joe Spencer
 */

#include "dac.h"
#include "main.h"


extern DAC_HandleTypeDef hdac1;

static uint16_t dacBuffer[DAC_BUFFER_SIZE];

/*
 * Blocks whose copy overlapped the DMA crossing into the half being
 * written (an audible glitch). Should stay 0; watch in Live Expressions.
 */
volatile uint32_t dacLateBlocks = 0;


void DAC_Init(void)
{
    for(uint32_t i = 0; i < DAC_BUFFER_SIZE; i++)
    {
        dacBuffer[i] = 2048;
    }
}

void DAC_Start(void)
{
    if(HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, (uint32_t *)dacBuffer, DAC_BUFFER_SIZE,
            DAC_ALIGN_12B_R) != HAL_OK)
    {
        Error_Handler();
    }
}

/*
 * Copy a processed block straight into the half of the circular DAC
 * buffer that the DMA is not reading. The DMA's remaining count (CNDTR)
 * says where it is: more than one block left means it is still in the
 * first half. This works whichever of ADC and DAC was started first.
 */
void DAC_WriteBlock(
        const uint16_t *input,
        uint32_t length)
{
    uint32_t inFirstHalf;
    uint16_t *half;

    if(length != DAC_BLOCK_SIZE)
        return;

    inFirstHalf = (__HAL_DMA_GET_COUNTER(hdac1.DMA_Handle1) > DAC_BLOCK_SIZE);
    half = inFirstHalf ? &dacBuffer[DAC_BLOCK_SIZE] : &dacBuffer[0];

    for(uint32_t i=0;i<DAC_BLOCK_SIZE;i++)
    {
        half[i]=input[i];
    }

    /* if the DMA changed half during the copy, it reached the block being written */
    if((__HAL_DMA_GET_COUNTER(hdac1.DMA_Handle1) > DAC_BLOCK_SIZE) != inFirstHalf)
    {
        dacLateBlocks++;
    }
}



void DAC_Stop(void)
{

    HAL_DAC_Stop_DMA(
            &hdac1,
            DAC_CHANNEL_1);

}

