/*
 * dac.c
 *
 *  Created on: 5 Aug 2026
 *      Author: Joe Spencer
 */

#include "dac.h"
#include "main.h"


extern DAC_HandleTypeDef hdac1;

static volatile uint8_t blocksAvailable = 0;
static uint16_t dacBuffer[DAC_BUFFER_SIZE];
static uint16_t dacQueue[DAC_BUFFER_SIZE];

static volatile uint8_t dacBlockReady = 0;


void HAL_DAC_ConvHalfCpltCallbackCh1(
        DAC_HandleTypeDef *hdac)
{
    if(dacBlockReady)
    {
        for(uint32_t i=0;i<DAC_BLOCK_SIZE;i++)
        {
            dacBuffer[i]=dacQueue[i];
        }

        dacBlockReady=0;
    }
}


void HAL_DAC_ConvCpltCallbackCh1(
        DAC_HandleTypeDef *hdac)
{
    if(dacBlockReady)
    {
        for(uint32_t i=0;i<DAC_BLOCK_SIZE;i++)
        {
            dacBuffer[DAC_BLOCK_SIZE+i]=dacQueue[i];
        }

        dacBlockReady=0;
    }
}


void DAC_Init(void)
{
    for(uint32_t i = 0; i < DAC_BUFFER_SIZE; i++)
    {
        dacBuffer[i] = 2048;
    }
}

void DAC_Start(void)
{


    HAL_StatusTypeDef status;

    status = HAL_DAC_Start_DMA(
            &hdac1,
            DAC_CHANNEL_1,
            (uint32_t*)dacBuffer,
            DAC_BUFFER_SIZE,
            DAC_ALIGN_12B_R);

}

void DAC_WriteBlock(
        const uint16_t *input,
        uint32_t length)
{
    if(length != DAC_BLOCK_SIZE)
        return;

    for(uint32_t i=0;i<DAC_BLOCK_SIZE;i++)
    {
        dacQueue[i]=input[i];
    }

    dacBlockReady=1;
}



void DAC_Stop(void)
{

    HAL_DAC_Stop_DMA(
            &hdac1,
            DAC_CHANNEL_1);

}

