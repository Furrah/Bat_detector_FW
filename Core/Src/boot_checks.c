/*
 * boot_checks.c
 *
 * The power-up checks; see boot_checks.h.
 */

#include "boot_checks.h"

#include <stdio.h>

#include "main.h"
#include "app_fatfs.h"
#include "diag_spi.h"
#include "fonts.h"
#include "ili9341.h"
#include "sd_spi.h"
#include "../Drivers/psram.h"
#include "../Drivers/psram_ring.h"


#define SD_TEST_FILE_BYTES  (256UL * 1024UL)

extern SPI_HandleTypeDef hspi2;


/*
 * SPI2 (display and SD card): the set-up traps CubeMX has fallen into
 * before, NSS pulse mode, frames other than 8 bits and Low-speed pins
 * (diag_spi.h).
 */
void BootChecks_Spi2(void)
{
    if((Diag_SpiCheck(hspi2.Instance) != 0U)
            || (Diag_GpioSlowPins(GPIOB, GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15, GPIO_SPEED_FREQ_HIGH) != 0U))
    {
        ILI9341_WriteString(5, 200, "SPI2: config", Font_11x18, ILI9341_RED, ILI9341_BLACK);
        HAL_Delay(5000);
    }
}


/*
 * PSRAM bring-up check. The address-line test always runs (a few ms). The
 * full 8 MB pattern test runs only if RECORD is held at power-up, because
 * the polled boot-time driver takes ~35 s for it.
 */
static PSRAM_Status_t BootChecks_PsramTest(void)
{
    char text[24];
    uint8_t mfid;
    uint8_t kgd;
    uint32_t failAddress = 0;
    uint32_t selectNs;
    const uint8_t fullTest = (HAL_GPIO_ReadPin(RECORD_BUTTON_GPIO_Port, RECORD_BUTTON_Pin) == GPIO_PIN_RESET);

    PSRAM_Status_t status = PSRAM_Init();

    PSRAM_GetId(&mfid, &kgd);
    snprintf(text, sizeof(text), "PSRAM ID %02X %02X", mfid, kgd);
    ILI9341_WriteString(5, 40, text, Font_11x18, ILI9341_WHITE, ILI9341_BLACK);

    if(status == PSRAM_OK)
    {
        if(fullTest)
        {
            ILI9341_WriteString(5, 65, "Full test ~35s...", Font_11x18, ILI9341_WHITE, ILI9341_BLACK);
            status = PSRAM_TestFull(&failAddress);
        }
        else
        {
            status = PSRAM_TestAddressLines(&failAddress);
        }
    }

    if(status == PSRAM_OK)
    {
        ILI9341_WriteString(5, 90, fullTest ? "8MB test OK" : "Address test OK",
                Font_11x18, ILI9341_GREEN, ILI9341_BLACK);
    }
    else if(status == PSRAM_ERR_ID)
    {
        ILI9341_WriteString(5, 90, "FAIL: bad ID", Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(status == PSRAM_ERR_TIMEOUT)
    {
        /* SPI1 SR decodes which flag never arrived (TXE bit 1, RXNE bit 0, BSY bit 7) */
        snprintf(text, sizeof(text), "SPI timeout SR=%04lX", PSRAM_GetTimeoutStatus());
        ILI9341_WriteString(5, 90, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        snprintf(text, sizeof(text), "FAIL at 0x%06lX", failAddress);
        ILI9341_WriteString(5, 90, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }

    /* CE# low time must stay within tCEM, or the PSRAM can't refresh */
    selectNs = PSRAM_GetMaxSelectNs();
    snprintf(text, sizeof(text), "CE max %lu.%lu us", selectNs / 1000U, (selectNs % 1000U) / 100U);
    ILI9341_WriteString(5, 115, text, Font_11x18,
            (selectNs > PSRAM_TCEM_NS) ? ILI9341_RED : ILI9341_WHITE, ILI9341_BLACK);

    HAL_Delay((status == PSRAM_OK) ? 1500U : 5000U);

    return status;
}


uint8_t BootChecks_Psram(void)
{
    if(BootChecks_PsramTest() != PSRAM_OK)
    {
        return 0U;
    }

    /* From here SPI1 belongs to the ring, which keeps the last 16.8 s of raw
     * audio. Started before the ADC, so ring block 0 is the first block. */
    const PsramRing_Status_t ringStatus = PsramRing_Start();

    if(ringStatus != PSRAM_RING_OK)
    {
        ILI9341_WriteString(5, 140, (ringStatus == PSRAM_RING_ERR_PRIORITY) ? "Ring: NVIC prio" : "Ring: SPI1 config",
                Font_11x18, ILI9341_RED, ILI9341_BLACK);
        HAL_Delay(5000);
        return 0U;
    }

    return 1U;
}


static uint8_t BootChecks_SdPattern(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16));
}


/*
 * Write, read back and verify a 256 KB file and show the speeds. Uses
 * 64-byte f_write/f_read calls: FatFS gathers them in its 512-byte sector
 * buffer, so the card only sees single-sector writes. The recorder writes
 * 8 sectors at a time, so this figure is pessimistic.
 */
static void BootChecks_SdSpeed(void)
{
    char text[24];
    uint8_t buffer[64];
    UINT done;
    FRESULT result;
    FRESULT closeResult;
    uint32_t start;
    uint32_t writeMs = 1U;
    uint32_t readMs = 1U;
    uint32_t badOffset = 0xFFFFFFFFU;

    ILI9341_WriteString(5, 175, "SD speed test...", Font_11x18, ILI9341_WHITE, ILI9341_BLACK);

    result = f_open(&USERFile, "SDTEST.BIN", FA_CREATE_ALWAYS | FA_WRITE);

    if(result == FR_OK)
    {
        start = HAL_GetTick();

        for(uint32_t offset = 0U; (offset < SD_TEST_FILE_BYTES) && (result == FR_OK); offset += sizeof(buffer))
        {
            for(uint32_t i = 0U; i < sizeof(buffer); i++)
            {
                buffer[i] = BootChecks_SdPattern(offset + i);
            }

            result = f_write(&USERFile, buffer, sizeof(buffer), &done);

            if((result == FR_OK) && (done != sizeof(buffer)))
            {
                result = FR_DENIED;     /* card full */
            }
        }

        /* closing flushes the last sector, so it counts towards the write time */
        closeResult = f_close(&USERFile);
        writeMs = HAL_GetTick() - start + 1U;

        if(result == FR_OK)
        {
            result = closeResult;
        }
    }

    if(result == FR_OK)
    {
        result = f_open(&USERFile, "SDTEST.BIN", FA_READ);
    }

    if(result == FR_OK)
    {
        start = HAL_GetTick();

        for(uint32_t offset = 0U; (offset < SD_TEST_FILE_BYTES) && (result == FR_OK)
                && (badOffset == 0xFFFFFFFFU); offset += sizeof(buffer))
        {
            result = f_read(&USERFile, buffer, sizeof(buffer), &done);

            if((result == FR_OK) && (done != sizeof(buffer)))
            {
                result = FR_INT_ERR;    /* file shorter than written */
            }

            for(uint32_t i = 0U; (i < sizeof(buffer)) && (result == FR_OK); i++)
            {
                if((buffer[i] != BootChecks_SdPattern(offset + i)) && (badOffset == 0xFFFFFFFFU))
                {
                    badOffset = offset + i;
                }
            }
        }

        readMs = HAL_GetTick() - start + 1U;
        (void)f_close(&USERFile);
    }

    if(result != FR_OK)
    {
        snprintf(text, sizeof(text), "SD test err %d", (int)result);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(badOffset != 0xFFFFFFFFU)
    {
        snprintf(text, sizeof(text), "SD bad at 0x%05lX", badOffset);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        /* kB/s = 256 KB / seconds */
        snprintf(text, sizeof(text), "W%lu R%lu kB/s",
                (SD_TEST_FILE_BYTES / 1024UL) * 1000UL / writeMs,
                (SD_TEST_FILE_BYTES / 1024UL) * 1000UL / readMs);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_GREEN, ILI9341_BLACK);
    }
}


void BootChecks_Sd(void)
{
    char text[24];
    FRESULT result;
    const uint8_t speedTest = (HAL_GPIO_ReadPin(PLAYBACK_BUTTON_GPIO_Port, PLAYBACK_BUTTON_Pin) == GPIO_PIN_RESET);

    /* 1 = mount now, so a missing card shows up here rather than on first use */
    result = f_mount(&USERFatFs, USERPath, 1);

    if(result == FR_NOT_READY)
    {
        if(SD_GetInitStatus() == SD_ERR_NO_CARD)
        {
            ILI9341_WriteString(5, 150, "SD: no card", Font_11x18, ILI9341_RED, ILI9341_BLACK);
        }
        else
        {
            snprintf(text, sizeof(text), "SD: init fail %d", (int)SD_GetInitStatus());
            ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
        }
    }
    else if(result == FR_NO_FILESYSTEM)
    {
        /* exFAT cards (64 GB+) land here: FatFS is built without exFAT */
        ILI9341_WriteString(5, 150, "SD: not FAT32", Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(result != FR_OK)
    {
        snprintf(text, sizeof(text), "SD: mount err %d", (int)result);
        ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        snprintf(text, sizeof(text), "SD %s %luMB",
                (SD_GetCardType() == SD_CARD_V2_HC) ? "SDHC" : "SDSC",
                SD_GetSectorCount() / 2048UL);
        ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_GREEN, ILI9341_BLACK);

        if(speedTest)
        {
            BootChecks_SdSpeed();
        }
    }

    HAL_Delay(1500U);
}
