/*
 * tim3_lo.c
 *
 *  Created on: 6 Aug 2026
 *      Author: Joe Spencer
 */


#include "tim3_lo.h"
#include "main.h"

extern TIM_HandleTypeDef htim3;


void LO_SetFrequency(uint32_t frequency)
{
    uint32_t arr;


    if(frequency < 1000)
        frequency = 1000;

    if(frequency > 90000)
        frequency = 90000;


    arr = (170000000UL / frequency) - 1;


    __HAL_TIM_DISABLE(&htim3);


    __HAL_TIM_SET_AUTORELOAD(
            &htim3,
            arr);


    /*
     * Keep PWM roughly 50%
     */
    __HAL_TIM_SET_COMPARE(
            &htim3,
            TIM_CHANNEL_1,
            arr/2);


    __HAL_TIM_ENABLE(&htim3);
}
