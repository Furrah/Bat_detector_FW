/*
 * diag_spi.c
 *
 * SPI and interrupt set-up checks; see diag_spi.h.
 */

#include "diag_spi.h"


uint32_t Diag_SpiCheck(const SPI_TypeDef *spi)
{
    uint32_t problems = 0U;

    /* CR2.DS holds the frame size minus one: 0111 is 8 bits */
    if((spi->CR2 & SPI_CR2_DS) != (7U << SPI_CR2_DS_Pos))
    {
        problems |= DIAG_SPI_NOT_8BIT;
    }

    if((spi->CR2 & SPI_CR2_NSSP) != 0U)
    {
        problems |= DIAG_SPI_NSS_PULSE;
    }

    return problems;
}


uint32_t Diag_GpioSlowPins(const GPIO_TypeDef *port, uint32_t pins, uint32_t minSpeed)
{
    uint32_t slow = 0U;

    for(uint32_t pin = 0; pin < 16U; pin++)
    {
        /* OSPEEDR: two bits per pin, 0 = low ... 3 = very high */
        if(((pins & (1UL << pin)) != 0U) && (((port->OSPEEDR >> (pin * 2U)) & 3U) < minSpeed))
        {
            slow |= 1UL << pin;
        }
    }

    return slow;
}


uint8_t Diag_IrqAloneAtTop(IRQn_Type irq, IRQn_Type lastIrq)
{
    if((NVIC_GetEnableIRQ(irq) == 0U) || (NVIC_GetPriority(irq) != 0U))
    {
        return 0U;
    }

    for(int32_t other = 0; other <= (int32_t)lastIrq; other++)
    {
        if((other != (int32_t)irq)
                && (NVIC_GetEnableIRQ((IRQn_Type)other) != 0U)
                && (NVIC_GetPriority((IRQn_Type)other) == 0U))
        {
            return 0U;
        }
    }

    return 1U;
}


uint32_t Diag_SpiDrainRx(SPI_TypeDef *spi)
{
    const uint32_t sr = spi->SR;

    if((sr & (SPI_SR_FRLVL | SPI_SR_OVR)) == 0U)
    {
        return 0U;
    }

    /* 8-bit reads pop one byte each (a wider read would pop two). The FIFO
     * holds at most 4 bytes; 8 reads is a safe bound. */
    for(uint32_t i = 0; (i < 8U) && ((spi->SR & SPI_SR_FRLVL) != 0U); i++)
    {
        (void)*(volatile uint8_t *)&spi->DR;
    }

    /* reading DR then SR clears an overrun */
    if((spi->SR & SPI_SR_OVR) != 0U)
    {
        (void)*(volatile uint8_t *)&spi->DR;
        (void)spi->SR;
    }

    return sr;
}
