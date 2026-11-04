/*
 * dac.c
 *
 * See dac.h.
 */

#include "dac.h"

#include "main.h"

extern DAC_HandleTypeDef hdac1;

static uint16_t dacBuffer[AUDIO_BUFFER_SIZE];


void DAC_Init(void)
{
    for(uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        dacBuffer[i] = 2048;        /* mid-scale: silence */
    }
}


void DAC_Start(void)
{
    if(HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, (uint32_t *)dacBuffer, AUDIO_BUFFER_SIZE,
            DAC_ALIGN_12B_R) != HAL_OK)
    {
        Error_Handler();
    }
}


/*
 * Copy the block into the half of the buffer the DMA isn't reading. The
 * DMA's remaining count says where it is: more than a block left means
 * it's still in the first half.
 */
void DAC_WriteBlock(const uint16_t *input, uint32_t length)
{
    if(length != AUDIO_BLOCK_SIZE)
    {
        return;
    }

    const uint8_t dmaInFirstHalf = (__HAL_DMA_GET_COUNTER(hdac1.DMA_Handle1) > AUDIO_BLOCK_SIZE);
    uint16_t *half = dmaInFirstHalf ? &dacBuffer[AUDIO_BLOCK_SIZE] : &dacBuffer[0];

    for(uint32_t i = 0; i < AUDIO_BLOCK_SIZE; i++)
    {
        half[i] = input[i];
    }
}
