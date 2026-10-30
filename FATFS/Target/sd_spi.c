/*
 * sd_spi.c
 *
 * SD card driver, SPI mode, on SPI2 (shared with the display; SD CS on PB2).
 *
 * The card initialises at <= 400 kHz (SPI2 /256 with APB1 = 85 MHz gives
 * 332 kHz), then runs at the CubeMX rate (21.25 MHz). Every wait is bounded
 * by a timeout, so a missing or stuck card returns an error instead of
 * hanging.
 *
 * Uses HAL SPI calls rather than registers: unlike the PSRAM there is no
 * limit on how long CS may stay low, and HAL keeps SPI2 in the state the
 * display driver (also HAL) expects.
 */


#include "sd_spi.h"

#include <string.h>

#include "main.h"


#define SD_SPI_PORT             hspi2
extern SPI_HandleTypeDef SD_SPI_PORT;

/* Command indexes; bit 7 marks an application command (sent after CMD55) */
#define SD_CMD0                 0U              /* GO_IDLE_STATE */
#define SD_CMD8                 8U              /* SEND_IF_COND */
#define SD_CMD9                 9U              /* SEND_CSD */
#define SD_CMD12                12U             /* STOP_TRANSMISSION */
#define SD_CMD16                16U             /* SET_BLOCKLEN */
#define SD_CMD17                17U             /* READ_SINGLE_BLOCK */
#define SD_CMD18                18U             /* READ_MULTIPLE_BLOCK */
#define SD_CMD24                24U             /* WRITE_BLOCK */
#define SD_CMD25                25U             /* WRITE_MULTIPLE_BLOCK */
#define SD_CMD55                55U             /* APP_CMD */
#define SD_CMD58                58U             /* READ_OCR */
#define SD_ACMD23               (0x80U | 23U)   /* SET_WR_BLK_ERASE_COUNT */
#define SD_ACMD41               (0x80U | 41U)   /* SD_SEND_OP_COND */

#define SD_R1_READY             0x00U
#define SD_R1_IDLE              0x01U
#define SD_R1_NO_RESPONSE       0xFFU

#define SD_TOKEN_START_BLOCK    0xFEU   /* single read/write, multi read */
#define SD_TOKEN_START_MULTI    0xFCU   /* each block of a multi write */
#define SD_TOKEN_STOP_TRAN      0xFDU   /* end of a multi write */
#define SD_DATA_ACCEPTED        0x05U

#define SD_INIT_TIMEOUT_MS      1000U   /* ACMD41 until the card leaves idle */
#define SD_READY_TIMEOUT_MS     500U    /* card busy, e.g. programming a write */
#define SD_READ_TIMEOUT_MS      200U    /* wait for a data token */
#define SD_SPI_TIMEOUT_MS       100U    /* one HAL transfer */


static SD_CardType_t cardType = SD_CARD_NONE;
static uint32_t sectorCount = 0;
static SD_Status_t initStatus = SD_ERR_INIT;



/*
 * Exchange one byte. A HAL failure returns 0xFF, which callers see as
 * "no response".
 */
static uint8_t SD_Exchange(uint8_t out)
{
    uint8_t in = 0xFFU;

    if(HAL_SPI_TransmitReceive(&SD_SPI_PORT, &out, &in, 1U, SD_SPI_TIMEOUT_MS) != HAL_OK)
    {
        return 0xFFU;
    }

    return in;
}


/*
 * Change the SPI2 clock between transfers (HAL leaves the bus idle after
 * each call). The display shares SPI2 at the CubeMX rate, so the slow
 * clock is only used during SD_Init().
 */
static void SD_SetPrescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(&SD_SPI_PORT);
    MODIFY_REG(SD_SPI_PORT.Instance->CR1, SPI_CR1_BR, prescaler);
    __HAL_SPI_ENABLE(&SD_SPI_PORT);
}


static void SD_Deselect(void)
{
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_SET);

    /* the card only releases MISO on the next clock after CS goes high */
    (void)SD_Exchange(0xFFU);
}


/*
 * The card holds MISO low while busy and returns 0xFF once ready.
 */
static SD_Status_t SD_WaitReady(uint32_t timeoutMs)
{
    uint32_t start = HAL_GetTick();

    do
    {
        if(SD_Exchange(0xFFU) == 0xFFU)
        {
            return SD_OK;
        }
    } while((HAL_GetTick() - start) < timeoutMs);

    return SD_ERR_TIMEOUT;
}


/*
 * Select the card and wait until it isn't busy. The display driver always
 * deselects after each call, so only the SD CS is low here.
 */
static SD_Status_t SD_Select(void)
{
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_RESET);

    (void)SD_Exchange(0xFFU);   /* dummy clock so the card drives MISO */

    if(SD_WaitReady(SD_READY_TIMEOUT_MS) != SD_OK)
    {
        SD_Deselect();
        return SD_ERR_TIMEOUT;
    }

    return SD_OK;
}


/*
 * Send a command and return its R1 response (bit 7 set = no response).
 * Leaves the card selected: the caller deselects once the whole
 * transaction, including any data, is done.
 */
static uint8_t SD_SendCommand(uint8_t command, uint32_t argument)
{
    uint8_t frame[6];
    uint8_t r1 = SD_R1_NO_RESPONSE;

    if((command & 0x80U) != 0U)
    {
        /* application command: CMD55 first */
        command &= 0x7FU;
        r1 = SD_SendCommand(SD_CMD55, 0U);

        if(r1 > SD_R1_IDLE)
        {
            return r1;
        }
    }

    /* CMD12 is sent mid-read, while the card is still streaming data */
    if(command != SD_CMD12)
    {
        SD_Deselect();

        if(SD_Select() != SD_OK)
        {
            return SD_R1_NO_RESPONSE;
        }
    }

    frame[0] = (uint8_t)(0x40U | command);
    frame[1] = (uint8_t)(argument >> 24);
    frame[2] = (uint8_t)(argument >> 16);
    frame[3] = (uint8_t)(argument >> 8);
    frame[4] = (uint8_t)argument;

    /* in SPI mode the CRC is only checked for CMD0 and CMD8 */
    if(command == SD_CMD0)
    {
        frame[5] = 0x95U;
    }
    else if(command == SD_CMD8)
    {
        frame[5] = 0x87U;
    }
    else
    {
        frame[5] = 0x01U;
    }

    for(uint32_t i = 0; i < sizeof(frame); i++)
    {
        (void)SD_Exchange(frame[i]);
    }

    if(command == SD_CMD12)
    {
        (void)SD_Exchange(0xFFU);   /* skip the stuff byte */
    }

    /* R1 arrives within 8 bytes */
    for(uint32_t i = 0; i < 10U; i++)
    {
        r1 = SD_Exchange(0xFFU);

        if((r1 & 0x80U) == 0U)
        {
            break;
        }
    }

    return r1;
}


/*
 * Receive one data block after a read command: wait for the start token,
 * then the data and its (unchecked) CRC.
 */
static SD_Status_t SD_ReceiveData(uint8_t *buffer, uint32_t length)
{
    uint32_t start = HAL_GetTick();
    uint8_t token;

    do
    {
        token = SD_Exchange(0xFFU);
    } while((token == 0xFFU) && ((HAL_GetTick() - start) < SD_READ_TIMEOUT_MS));

    if(token != SD_TOKEN_START_BLOCK)
    {
        return (token == 0xFFU) ? SD_ERR_TIMEOUT : SD_ERR_DATA;
    }

    /* MOSI must stay high while the card sends, so transmit 0xFF in place */
    memset(buffer, 0xFF, length);

    if(HAL_SPI_TransmitReceive(&SD_SPI_PORT, buffer, buffer, (uint16_t)length, SD_SPI_TIMEOUT_MS) != HAL_OK)
    {
        return SD_ERR_SPI;
    }

    (void)SD_Exchange(0xFFU);
    (void)SD_Exchange(0xFFU);

    return SD_OK;
}


/*
 * Send one 512-byte block (or, with SD_TOKEN_STOP_TRAN, just end a multi
 * write) and check the card accepted it.
 */
static SD_Status_t SD_TransmitData(const uint8_t *buffer, uint8_t token)
{
    uint8_t response;

    if(SD_WaitReady(SD_READY_TIMEOUT_MS) != SD_OK)
    {
        return SD_ERR_TIMEOUT;
    }

    (void)SD_Exchange(token);

    if(token == SD_TOKEN_STOP_TRAN)
    {
        return SD_OK;
    }

    if(HAL_SPI_Transmit(&SD_SPI_PORT, (uint8_t *)buffer, SD_SECTOR_SIZE, SD_SPI_TIMEOUT_MS) != HAL_OK)
    {
        return SD_ERR_SPI;
    }

    /* dummy CRC */
    (void)SD_Exchange(0xFFU);
    (void)SD_Exchange(0xFFU);

    response = SD_Exchange(0xFFU);

    if((response & 0x1FU) != SD_DATA_ACCEPTED)
    {
        return SD_ERR_DATA;
    }

    return SD_OK;
}


/*
 * Card size from the CSD register, in 512-byte sectors.
 */
static SD_Status_t SD_ReadSectorCount(void)
{
    uint8_t csd[16];
    SD_Status_t status = SD_ERR_CMD;

    if(SD_SendCommand(SD_CMD9, 0U) == SD_R1_READY)
    {
        status = SD_ReceiveData(csd, sizeof(csd));
    }

    SD_Deselect();

    if(status != SD_OK)
    {
        return status;
    }

    if((csd[0] >> 6) == 1U)
    {
        /* CSD v2.0 (SDHC/SDXC): C_SIZE counts 512 KB units */
        uint32_t cSize = ((uint32_t)(csd[7] & 0x3FU) << 16) | ((uint32_t)csd[8] << 8) | csd[9];

        sectorCount = (cSize + 1UL) << 10;
    }
    else
    {
        /* CSD v1.0: (C_SIZE + 1) x 2^(C_SIZE_MULT + 2) x 2^READ_BL_LEN bytes */
        uint32_t readBlLen = csd[5] & 0x0FU;
        uint32_t cSize = ((uint32_t)(csd[6] & 0x03U) << 10) | ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
        uint32_t cSizeMult = ((uint32_t)(csd[9] & 0x03U) << 1) | ((uint32_t)csd[10] >> 7);

        sectorCount = (cSize + 1UL) << (cSizeMult + readBlLen + 2UL - 9UL);
    }

    return SD_OK;
}



SD_Status_t SD_Init(void)
{
    SD_CardType_t type = SD_CARD_NONE;
    SD_Status_t status = SD_OK;
    uint8_t r1;
    uint8_t ocr[4];
    uint32_t start;

    cardType = SD_CARD_NONE;
    sectorCount = 0U;

    /* initialisation must run at <= 400 kHz: 85 MHz / 256 = 332 kHz */
    SD_SetPrescaler(SPI_BAUDRATEPRESCALER_256);

    /* >= 74 clocks with CS high before the first command */
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_SET);

    for(uint32_t i = 0; i < 10U; i++)
    {
        (void)SD_Exchange(0xFFU);
    }

    r1 = SD_SendCommand(SD_CMD0, 0U);

    if(r1 != SD_R1_IDLE)
    {
        status = (r1 == SD_R1_NO_RESPONSE) ? SD_ERR_NO_CARD : SD_ERR_INIT;
    }
    else
    {
        start = HAL_GetTick();

        if(SD_SendCommand(SD_CMD8, 0x1AAU) == SD_R1_IDLE)
        {
            /* SD v2: R7 echoes the accepted voltage range and check pattern */
            for(uint32_t i = 0; i < sizeof(ocr); i++)
            {
                ocr[i] = SD_Exchange(0xFFU);
            }

            if((ocr[2] == 0x01U) && (ocr[3] == 0xAAU))
            {
                /* HCS set: high-capacity cards are supported */
                do
                {
                    r1 = SD_SendCommand(SD_ACMD41, 1UL << 30);
                } while((r1 != SD_R1_READY) && ((HAL_GetTick() - start) < SD_INIT_TIMEOUT_MS));

                if((r1 == SD_R1_READY) && (SD_SendCommand(SD_CMD58, 0U) == SD_R1_READY))
                {
                    for(uint32_t i = 0; i < sizeof(ocr); i++)
                    {
                        ocr[i] = SD_Exchange(0xFFU);
                    }

                    /* CCS bit: block-addressed SDHC/SDXC */
                    type = ((ocr[0] & 0x40U) != 0U) ? SD_CARD_V2_HC : SD_CARD_V2_SC;
                }
            }
        }
        else
        {
            /* SD v1 (MMC is not supported) */
            do
            {
                r1 = SD_SendCommand(SD_ACMD41, 0U);
            } while((r1 != SD_R1_READY) && ((HAL_GetTick() - start) < SD_INIT_TIMEOUT_MS));

            if(r1 == SD_R1_READY)
            {
                type = SD_CARD_V1;
            }
        }

        /* byte-addressed cards: fix the block length at 512 */
        if(((type == SD_CARD_V1) || (type == SD_CARD_V2_SC))
                && (SD_SendCommand(SD_CMD16, SD_SECTOR_SIZE) != SD_R1_READY))
        {
            type = SD_CARD_NONE;
        }

        if(type == SD_CARD_NONE)
        {
            status = SD_ERR_INIT;
        }
    }

    SD_Deselect();

    /* back to full speed, which the display also uses */
    SD_SetPrescaler(SD_SPI_PORT.Init.BaudRatePrescaler);

    if(status == SD_OK)
    {
        cardType = type;
        status = SD_ReadSectorCount();

        if(status != SD_OK)
        {
            cardType = SD_CARD_NONE;
        }
    }

    initStatus = status;

    return status;
}


SD_Status_t SD_ReadBlocks(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_ERR_CMD;

    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    /* only SDHC/SDXC are block addressed */
    if(cardType != SD_CARD_V2_HC)
    {
        sector *= SD_SECTOR_SIZE;
    }

    if(count == 1U)
    {
        if(SD_SendCommand(SD_CMD17, sector) == SD_R1_READY)
        {
            status = SD_ReceiveData(buffer, SD_SECTOR_SIZE);
        }
    }
    else if(SD_SendCommand(SD_CMD18, sector) == SD_R1_READY)
    {
        do
        {
            status = SD_ReceiveData(buffer, SD_SECTOR_SIZE);
            buffer += SD_SECTOR_SIZE;
        } while((--count > 0U) && (status == SD_OK));

        (void)SD_SendCommand(SD_CMD12, 0U);
    }

    SD_Deselect();

    return status;
}


SD_Status_t SD_WriteBlocks(const uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_ERR_CMD;

    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    if(cardType != SD_CARD_V2_HC)
    {
        sector *= SD_SECTOR_SIZE;
    }

    if(count == 1U)
    {
        if(SD_SendCommand(SD_CMD24, sector) == SD_R1_READY)
        {
            status = SD_TransmitData(buffer, SD_TOKEN_START_BLOCK);
        }
    }
    else
    {
        /* pre-erase hint: lets the card prepare, which speeds up multi writes */
        if(cardType != SD_CARD_V1)
        {
            (void)SD_SendCommand(SD_ACMD23, count);
        }

        if(SD_SendCommand(SD_CMD25, sector) == SD_R1_READY)
        {
            do
            {
                status = SD_TransmitData(buffer, SD_TOKEN_START_MULTI);
                buffer += SD_SECTOR_SIZE;
            } while((--count > 0U) && (status == SD_OK));

            /* always end the multi write, even after an error */
            if((SD_TransmitData(NULL, SD_TOKEN_STOP_TRAN) != SD_OK) && (status == SD_OK))
            {
                status = SD_ERR_TIMEOUT;
            }
        }
    }

    SD_Deselect();

    return status;
}


SD_Status_t SD_Sync(void)
{
    SD_Status_t status;

    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    status = SD_Select();       /* waits until the card has finished programming */
    SD_Deselect();

    return status;
}


SD_CardType_t SD_GetCardType(void)
{
    return cardType;
}


uint32_t SD_GetSectorCount(void)
{
    return sectorCount;
}


SD_Status_t SD_GetInitStatus(void)
{
    return initStatus;
}
