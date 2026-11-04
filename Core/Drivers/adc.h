/*
 * adc.h
 *
 * ADC1: the microphone, 250 kHz, triggered by TIM2 and written by circular
 * DMA into a two-block buffer. Each time a half fills, that block goes to
 * the PSRAM ring and then to the block callback (the DSP), in the DMA
 * interrupt.
 *
 * ADC2: the slow inputs (LO knob, volume knob, battery), one scan on demand.
 */

#ifndef DRIVERS_ADC_H_
#define DRIVERS_ADC_H_

#include <stdint.h>

#include "pipeline_config.h"

typedef void (*ADC_Callback_t)(uint16_t *block, uint32_t length);

void ADC_Init(void);
void ADC_Start(void);

/* Called with every finished block of AUDIO_BLOCK_SIZE samples */
void ADC_SetBlockCallback(ADC_Callback_t callback);

/* ADC2 ranks, in the order CubeMX scans them */
typedef enum
{
    ADC2_LO_POT = 0,    /* PA1, ADC2_IN2  */
    ADC2_VOLUME,        /* PA5, ADC2_IN13 */
    ADC2_VBAT,          /* PA6, ADC2_IN3: battery / 2 */
    ADC2_NUM_CHANNELS
} ADC2_Channel_t;

void ADC2_StartScan(void);              /* ~18 us, by DMA */
uint8_t ADC2_ScanReady(void);
uint16_t ADC2_Get(ADC2_Channel_t channel);

#endif /* DRIVERS_ADC_H_ */
