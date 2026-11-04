/*
 * diag_spi.h
 *
 * Checks for STM32 SPI and interrupt set-ups that cost the bat detector
 * days. Each check reads the hardware registers, so it catches what CubeMX
 * actually generated, not what it was asked for. Call them once at start-up,
 * before the bus is used in earnest, and stop or report on a failure.
 *
 *   - NSS pulse mode (CR2.NSSP), CubeMX's G4 default: the SPI idles SCK for
 *     2 clocks after every byte, even with software NSS. That's 10 clocks a
 *     byte instead of 8, and it pushed the PSRAM's bursts past their limit.
 *   - Frame size other than 8 bits: CubeMX's G4 default is 4.
 *   - SPI pins left at Low speed, CubeMX's default: at 42.5 MHz a Low-speed
 *     SCK pad can't toggle fast enough, and the SPI hung waiting for RXNE.
 *   - An interrupt that must preempt everything is only safe alone at
 *     priority 0. CubeMX gives every newly enabled interrupt priority 0.
 *
 * Diag_SpiDrainRx() is for drivers that mix their own register access with
 * HAL calls. A byte left in the receive FIFO makes every later exchange read
 * the byte before its own.
 */

#ifndef DIAG_SPI_H_
#define DIAG_SPI_H_

#include <stdint.h>

#include "diag_port.h"


/* Diag_SpiCheck flags */
#define DIAG_SPI_NOT_8BIT       (1U << 0)   /* frames aren't 8 bits */
#define DIAG_SPI_NSS_PULSE      (1U << 1)   /* NSS pulse mode on: 2 idle clocks per byte */

/* 0 if the SPI is set up for 8-bit frames without NSS pulses, else DIAG_SPI_ flags */
uint32_t Diag_SpiCheck(const SPI_TypeDef *spi);

/* Which of the given pins are slower than minSpeed (a GPIO_SPEED_FREQ_
 * value, e.g. GPIO_SPEED_FREQ_HIGH); 0 if none */
uint32_t Diag_GpioSlowPins(const GPIO_TypeDef *port, uint32_t pins, uint32_t minSpeed);

/* 1 if irq is enabled at priority 0 and no other enabled interrupt from 0
 * to lastIrq (the device's last vector) shares that priority */
uint8_t Diag_IrqAloneAtTop(IRQn_Type irq, IRQn_Type lastIrq);

/* Empty the receive FIFO and clear an overrun. Returns 0 if nothing was
 * left, else the status register as it was found (SR), for the record. */
uint32_t Diag_SpiDrainRx(SPI_TypeDef *spi);

#endif /* DIAG_SPI_H_ */
