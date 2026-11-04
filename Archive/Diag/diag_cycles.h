/*
 * diag_cycles.h
 *
 * Timing with the Cortex-M3/M4/M7 DWT cycle counter: one CPU clock per
 * count, read in one instruction, no timer needed. Every function is
 * inline, so it can be used in interrupts, including code that runs from
 * RAM, without a call into flash.
 *
 *     const uint32_t start = Diag_Cycles();
 *     DoSomething();
 *     Diag_KeepMax(&worstUs, Diag_CyclesToUs(Diag_CyclesSince(start)));
 *
 * The counter wraps every 2^32 cycles (25 s at 170 MHz). Differences of
 * unsigned counts stay right across one wrap, so time anything shorter.
 */

#ifndef DIAG_CYCLES_H_
#define DIAG_CYCLES_H_

#include <stdint.h>

#include "diag_port.h"


/* Start the counter. Harmless to call more than once (every user can). */
DIAG_INLINE void Diag_CyclesInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

DIAG_INLINE uint32_t Diag_Cycles(void)
{
    return DWT->CYCCNT;
}

DIAG_INLINE uint32_t Diag_CyclesSince(uint32_t start)
{
    return DWT->CYCCNT - start;
}

DIAG_INLINE uint32_t Diag_CyclesToUs(uint32_t cycles)
{
    return cycles / (SystemCoreClock / 1000000U);
}

/* Up to 4.29 million cycles (25 ms at 170 MHz) before the product overflows */
DIAG_INLINE uint32_t Diag_CyclesToNs(uint32_t cycles)
{
    return (cycles * 1000U) / (SystemCoreClock / 1000000U);
}


/* Keep the largest value seen */
DIAG_INLINE void Diag_KeepMax(volatile uint32_t *max, uint32_t value)
{
    if(value > *max)
    {
        *max = value;
    }
}

/* Keep the smallest value seen; 0 means none yet */
DIAG_INLINE void Diag_KeepMin(volatile uint32_t *min, uint32_t value)
{
    if((*min == 0U) || (value < *min))
    {
        *min = value;
    }
}

/* Running average over about the last 16 values (signed, so it can fall) */
DIAG_INLINE void Diag_Average16(volatile uint32_t *average, uint32_t value)
{
    *average = (uint32_t)((int32_t)*average + ((int32_t)value - (int32_t)*average) / 16);
}

#endif /* DIAG_CYCLES_H_ */
