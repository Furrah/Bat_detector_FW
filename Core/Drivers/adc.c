/*
 * adc.c
 *
 * See adc.h.
 */

#include "adc.h"

#include "main.h"
#include "psram_ring.h"

extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

static uint16_t adcBuffer[AUDIO_BUFFER_SIZE];   /* two blocks: the DMA fills one while the other is processed */
static ADC_Callback_t blockCallback = 0;

static uint16_t adc2Buffer[ADC2_NUM_CHANNELS];
static volatile uint8_t adc2ScanReady = 0;


void ADC_Init(void)
{
    HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
    HAL_ADCEx_Calibration_Start(&hadc2, ADC_SINGLE_ENDED);

    /* ADC2_Channel_t must match the CubeMX rank list */
    if(hadc2.Init.NbrOfConversion != ADC2_NUM_CHANNELS)
    {
        Error_Handler();
    }
}


void ADC_Start(void)
{
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcBuffer, AUDIO_BUFFER_SIZE);
}


void ADC_SetBlockCallback(ADC_Callback_t callback)
{
    blockCallback = callback;
}


/* A block is ready: into the PSRAM ring first, then the DSP */
static void ADC_BlockReady(uint16_t *block)
{
    PsramRing_PushBlock(block);

    if(blockCallback != 0)
    {
        blockCallback(block, AUDIO_BLOCK_SIZE);
    }
}


void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if(hadc == &hadc1)
    {
        ADC_BlockReady(&adcBuffer[0]);
    }
}


void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if(hadc == &hadc1)
    {
        ADC_BlockReady(&adcBuffer[AUDIO_BLOCK_SIZE]);
    }
    else if(hadc == &hadc2)
    {
        adc2ScanReady = 1;
    }
}


void ADC2_StartScan(void)
{
    adc2ScanReady = 0;
    HAL_ADC_Start_DMA(&hadc2, (uint32_t *)adc2Buffer, ADC2_NUM_CHANNELS);
}


uint8_t ADC2_ScanReady(void)
{
    return adc2ScanReady;
}


uint16_t ADC2_Get(ADC2_Channel_t channel)
{
    return adc2Buffer[channel];
}
