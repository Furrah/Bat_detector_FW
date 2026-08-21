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



void ADC_Init(void)
{
    HAL_ADCEx_Calibration_Start(
            &hadc1,
            ADC_SINGLE_ENDED);
}


void ADC_Start(void)
{
    uint32_t state;

    state = HAL_ADC_GetState(&hadc1);


    HAL_StatusTypeDef status;

    status = HAL_ADC_Start_DMA(
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

        	halfCallback(
        	    &adcBuffer[0],
        	    ADC_BUFFER_SIZE/2);
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
        	fullCallback(
        	    &adcBuffer[ADC_BUFFER_SIZE/2],
        	    ADC_BUFFER_SIZE/2);
        }
    }
}


uint16_t ADC2_Read(void)
{
    HAL_ADC_Start(&hadc2);

    HAL_ADC_PollForConversion(&hadc2, HAL_MAX_DELAY);

    return HAL_ADC_GetValue(&hadc2);
}

