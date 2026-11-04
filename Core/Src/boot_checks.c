/*
 * boot_checks.c
 *
 * The power-up checks; see boot_checks.h. A failure is shown for 5 s and
 * the board carries on without that part: no PSRAM means no recording or
 * playback, no card means no recording.
 */

#include "boot_checks.h"

#include <stdio.h>

#include "main.h"
#include "app_fatfs.h"
#include "fonts.h"
#include "ili9341.h"
#include "sd_spi.h"
#include "../Drivers/psram.h"
#include "../Drivers/psram_ring.h"

extern SPI_HandleTypeDef hspi2;

/* OSPEEDR has 2 bits per pin; High (2) and Very High (3) both set the upper one */
#define SPI2_PINS_FAST  ((1UL << 27) | (1UL << 29) | (1UL << 31))   /* PB13, PB14, PB15 */


static void BootChecks_Show(uint16_t y, const char *text, uint16_t colour)
{
    ILI9341_WriteString(5, y, text, Font_11x18, colour, ILI9341_BLACK);
}


/*
 * SPI2 (display and SD card): the CubeMX defaults that have caught this
 * project before. A 4-bit frame size, NSS pulse mode (2 idle clocks after
 * every byte) and Low-speed pins.
 */
void BootChecks_Spi2(void)
{
    const SPI_TypeDef *spi = hspi2.Instance;

    if(((spi->CR2 & SPI_CR2_DS) != SPI_DATASIZE_8BIT)
            || ((spi->CR2 & SPI_CR2_NSSP) != 0U)
            || ((GPIOB->OSPEEDR & SPI2_PINS_FAST) != SPI2_PINS_FAST))
    {
        BootChecks_Show(200, "SPI2: config", ILI9341_RED);
        HAL_Delay(5000);
    }
}


uint8_t BootChecks_Psram(void)
{
    const PSRAM_Status_t status = PSRAM_Init();

    if(status != PSRAM_OK)
    {
        BootChecks_Show(40, (status == PSRAM_ERR_ID) ? "PSRAM: no reply" : "PSRAM: SPI1 stuck", ILI9341_RED);
        HAL_Delay(5000);
        return 0U;
    }

    /* From here SPI1 belongs to the ring, which keeps the last 16.8 s of raw
     * audio. Started before the ADC, so ring block 0 is the first block. */
    const PsramRing_Status_t ringStatus = PsramRing_Start();

    if(ringStatus != PSRAM_RING_OK)
    {
        BootChecks_Show(40, (ringStatus == PSRAM_RING_ERR_PRIORITY) ? "Ring: NVIC prio" : "Ring: SPI1 config",
                ILI9341_RED);
        HAL_Delay(5000);
        return 0U;
    }

    BootChecks_Show(40, "PSRAM OK", ILI9341_GREEN);
    return 1U;
}


void BootChecks_Sd(void)
{
    char text[24];

    /* 1 = mount now, so a missing card shows up here rather than on first use */
    const FRESULT result = f_mount(&USERFatFs, USERPath, 1);

    if(result == FR_OK)
    {
        snprintf(text, sizeof(text), "SD %s %luMB",
                (SD_GetCardType() == SD_CARD_V2_HC) ? "SDHC" : "SDSC",
                SD_GetSectorCount() / 2048UL);
        BootChecks_Show(65, text, ILI9341_GREEN);
    }
    else if((result == FR_NOT_READY) && (SD_GetInitStatus() == SD_ERR_NO_CARD))
    {
        BootChecks_Show(65, "SD: no card", ILI9341_RED);
    }
    else if(result == FR_NO_FILESYSTEM)
    {
        /* exFAT cards (64 GB+) land here: FatFS is built without exFAT */
        BootChecks_Show(65, "SD: not FAT32", ILI9341_RED);
    }
    else
    {
        snprintf(text, sizeof(text), "SD: error %d/%d", (int)result, (int)SD_GetInitStatus());
        BootChecks_Show(65, text, ILI9341_RED);
    }

    HAL_Delay(1500U);
}
