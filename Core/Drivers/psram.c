/*
 * psram.c
 *
 * Boot-time reset and ID check of the PSRAM; see psram.h. Each command is
 * one CE# cycle of a few bytes, polled at register level with interrupts
 * masked, so CE# stays far below the chip's 8 us limit.
 */

#include "psram.h"

#include "main.h"

extern SPI_HandleTypeDef hspi1;

#define CMD_RESET_ENABLE    0x66U
#define CMD_RESET           0x99U
#define CMD_READ_ID         0x9FU

#define ID_MANUFACTURER     0x0DU       /* AP Memory */
#define ID_KNOWN_GOOD_DIE   0x5DU

#define WAIT_LOOPS          10000U      /* far longer than any byte takes */


/* Wait for an SPI status flag; 0 if it never comes */
static uint8_t PSRAM_WaitFlag(SPI_TypeDef *spi, uint32_t flag)
{
    for(uint32_t i = 0; i < WAIT_LOOPS; i++)
    {
        if((spi->SR & flag) != 0U)
        {
            return 1U;
        }
    }
    return 0U;
}


/* One CE# cycle: send tx, keeping what comes back in rx (if not NULL) */
static PSRAM_Status_t PSRAM_Transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    SPI_TypeDef *spi = hspi1.Instance;
    PSRAM_Status_t status = PSRAM_OK;
    const uint32_t primask = __get_PRIMASK();

    __disable_irq();
    SPI1_PSRAM_CS_GPIO_Port->BRR = SPI1_PSRAM_CS_Pin;

    for(uint32_t i = 0; (i < length) && (status == PSRAM_OK); i++)
    {
        if(!PSRAM_WaitFlag(spi, SPI_SR_TXE))
        {
            status = PSRAM_ERR_SPI;
            break;
        }

        *(__IO uint8_t *)&spi->DR = tx[i];      /* 8-bit access: a 16-bit write sends two bytes */

        if(!PSRAM_WaitFlag(spi, SPI_SR_RXNE))
        {
            status = PSRAM_ERR_SPI;
            break;
        }

        const uint8_t in = *(__IO uint8_t *)&spi->DR;

        if(rx != NULL)
        {
            rx[i] = in;
        }
    }

    for(uint32_t i = 0; (i < WAIT_LOOPS) && ((spi->SR & SPI_SR_BSY) != 0U); i++)
    {
    }

    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;
    __set_PRIMASK(primask);

    return status;
}


PSRAM_Status_t PSRAM_Init(void)
{
    SPI_TypeDef *spi = hspi1.Instance;
    const uint8_t resetEnable = CMD_RESET_ENABLE;
    const uint8_t reset = CMD_RESET;
    const uint8_t readId[6] = { CMD_READ_ID, 0, 0, 0, 0, 0 };     /* command, 3 address bytes, 2 ID bytes */
    uint8_t reply[6];
    PSRAM_Status_t status;

    HAL_GPIO_WritePin(SPI1_PSRAM_CS_GPIO_Port, SPI1_PSRAM_CS_Pin, GPIO_PIN_SET);

    /* Run these at 21 MHz (/8), well inside every command's limit, and
     * enable the SPI (HAL only does that on its first transfer) */
    CLEAR_BIT(spi->CR1, SPI_CR1_SPE);
    MODIFY_REG(spi->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_8);
    SET_BIT(spi->CR1, SPI_CR1_SPE);

    while((spi->SR & SPI_SR_RXNE) != 0U)
    {
        (void)*(__IO uint8_t *)&spi->DR;
    }

    status = PSRAM_Transfer(&resetEnable, NULL, 1U);

    if(status == PSRAM_OK)
    {
        status = PSRAM_Transfer(&reset, NULL, 1U);
    }

    if(status == PSRAM_OK)
    {
        HAL_Delay(1);       /* reset recovery */
        status = PSRAM_Transfer(readId, reply, sizeof(readId));
    }

    /* back to the CubeMX rate (42.5 MHz) for the ring */
    CLEAR_BIT(spi->CR1, SPI_CR1_SPE);
    MODIFY_REG(spi->CR1, SPI_CR1_BR, hspi1.Init.BaudRatePrescaler);
    SET_BIT(spi->CR1, SPI_CR1_SPE);

    if((status == PSRAM_OK) && ((reply[4] != ID_MANUFACTURER) || (reply[5] != ID_KNOWN_GOOD_DIE)))
    {
        status = PSRAM_ERR_ID;
    }

    return status;
}
