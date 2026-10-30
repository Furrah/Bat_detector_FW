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


/*
 * ADC2 scans the slow analogue inputs, one DMA transfer per scan.
 * The order must match the CubeMX ADC2 regular ranks.
 */
typedef enum
{
    ADC2_LO_POT = 0,    /* rank 1: PA1, ADC2_IN2  */
    ADC2_VOLUME,        /* rank 2: PA5, ADC2_IN13 */
    ADC2_VBAT,          /* rank 3: PA6, ADC2_IN3  */
    ADC2_NUM_CHANNELS
} ADC2_Channel_t;

void ADC2_StartScan(void);
uint8_t ADC2_ScanReady(void);
uint16_t ADC2_Get(ADC2_Channel_t channel);


#endif /* DRIVERS_ADC_H_ */
