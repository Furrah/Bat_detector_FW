/*
 * diag_port.h
 *
 * The one file to change when Core/Diag moves to another STM32 project:
 * the device header, which brings in the CMSIS core registers (DWT, NVIC),
 * the peripheral definitions and SystemCoreClock.
 */

#ifndef DIAG_PORT_H_
#define DIAG_PORT_H_

#include "stm32g4xx.h"

/* Helpers that must compile into their caller, even one running from RAM
 * at any optimisation level (a call into flash would cost time there) */
#define DIAG_INLINE     static inline __attribute__((always_inline))

#endif /* DIAG_PORT_H_ */
