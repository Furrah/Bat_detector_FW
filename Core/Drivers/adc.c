/*
 * adc.c
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */


#include "adc.h"

#include "main.h"


extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;

uint16_t adcBuffer[ADC_BUFFER_SIZE];


static ADC_Callback_t halfCallback = 0;
static ADC_Callback_t fullCallback = 0;


volatile uint32_t adcConversionCount = 0;


static uint16_t adc2Buffer[ADC2_NUM_CHANNELS];
static volatile uint8_t adc2ScanReady = 0;


/*
 * Worst-case time spent in the block callback (DSP_ProcessBlock), in CPU
 * cycles and as a % of one block period. Watch these in Live Expressions:
 * at 100% the DMA overwrites the half being processed, and well before
 * that the main loop (FFT, display) starves.
 */
volatile uint32_t dspCyclesMax = 0;
volatile uint32_t dspLoadMaxPercent = 0;

static uint32_t blockCycles = 1;



/*
 * Run one block callback and record how long it took, using the DWT
 * cycle counter.
 */
static void ADC_RunBlockCallback(ADC_Callback_t callback, uint16_t *block)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles;

    callback(block, AUDIO_BLOCK_SIZE);

    cycles = DWT->CYCCNT - start;

    if(cycles > dspCyclesMax)
    {
        dspCyclesMax = cycles;
        dspLoadMaxPercent = (cycles * 100U) / blockCycles;
    }
}



void ADC_Init(void)
{
    /* DWT cycle counter for the DSP load measurement */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* CPU cycles in one block period: 680 x 256 = 174 080 at 250 kHz */
    blockCycles = (SystemCoreClock / AUDIO_SAMPLE_RATE_HZ) * AUDIO_BLOCK_SIZE;

    HAL_ADCEx_Calibration_Start(
            &hadc1,
            ADC_SINGLE_ENDED);

    HAL_ADCEx_Calibration_Start(
            &hadc2,
            ADC_SINGLE_ENDED);

    /* ADC2_Channel_t must mirror the CubeMX ADC2 rank list */
    if(hadc2.Init.NbrOfConversion != ADC2_NUM_CHANNELS)
    {
        Error_Handler();
    }
}


void ADC_Start(void)
{
    HAL_ADC_Start_DMA(
            &hadc1,
            (uint32_t*)adcBuffer,
            ADC_BUFFER_SIZE);
}


void ADC_Stop(void)
{
    HAL_ADC_Stop_DMA(&hadc1);
}


void ADC_SetHalfBufferCallback(ADC_Callback_t callback)
{
    halfCallback = callback;
}


void ADC_SetFullBufferCallback(ADC_Callback_t callback)
{
    fullCallback = callback;
}


void HAL_ADC_ConvHalfCpltCallback(
        ADC_HandleTypeDef *hadc)
{
    if(hadc == &hadc1)
    {
//    	HAL_GPIO_TogglePin(GPIOB,LED1_Pin);

        if(halfCallback != 0)
        {
            ADC_RunBlockCallback(halfCallback, &adcBuffer[0]);
        }
    }
}


void HAL_ADC_ConvCpltCallback(
        ADC_HandleTypeDef *hadc)
{
    if(hadc == &hadc1)
    {

        adcConversionCount++;
//    	HAL_GPIO_TogglePin(GPIOB,LED1_Pin);

        if(fullCallback != 0)
        {
            ADC_RunBlockCallback(fullCallback, &adcBuffer[AUDIO_BLOCK_SIZE]);
        }
    }
    else if(hadc == &hadc2)
    {
        adc2ScanReady = 1;
    }
}


/*
 * Start one scan of all ADC2 ranks into adc2Buffer via DMA1 Ch3.
 * Non-blocking; three 247.5-cycle conversions take ~18 us.
 */
void ADC2_StartScan(void)
{
    adc2ScanReady = 0;

    HAL_ADC_Start_DMA(
            &hadc2,
            (uint32_t*)adc2Buffer,
            ADC2_NUM_CHANNELS);
}


uint8_t ADC2_ScanReady(void)
{
    return adc2ScanReady;
}


uint16_t ADC2_Get(ADC2_Channel_t channel)
{
    return adc2Buffer[channel];
}

