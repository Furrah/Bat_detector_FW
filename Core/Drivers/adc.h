/*
 * adc.h
 *
 *  Created on: 4 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DRIVERS_ADC_H_
#define DRIVERS_ADC_H_

#include <stdint.h>


#define ADC_BUFFER_SIZE 512


typedef void (*ADC_Callback_t)(
        uint16_t *input,
        uint32_t length
);


void ADC_Init(void);
void ADC_Start(void);
void ADC_Stop(void);


void ADC_SetHalfBufferCallback(ADC_Callback_t callback);
void ADC_SetFullBufferCallback(ADC_Callback_t callback);
uint16_t ADC2_Read(void);


#endif /* DRIVERS_ADC_H_ */
