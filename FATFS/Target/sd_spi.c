/*
 * sd_spi.c
 *
 * SD card driver, SPI mode; see sd_spi.h. Everything goes through HAL's
 * polled SPI calls, which keep SPI2 the way the display driver expects.
 *
 * A command is a 6-byte frame (0x40 | index, 32-bit argument, CRC); the
 * card answers with an R1 status byte within 8 bytes. A data block is a
 * start token, 512 bytes and a 2-byte CRC (not checked in SPI mode); after
 * a written block the card answers "accepted" and then holds MISO low while
 * it programs. Every wait has a timeout, so a missing or stuck card
 * returns an error instead of hanging.
 */

#include "sd_spi.h"

#include <string.h>

#include "main.h"

extern SPI_HandleTypeDef hspi2;

/* Command indexes; bit 7 marks an application command (sent after CMD55) */
#define CMD0                    0U              /* GO_IDLE_STATE */
#define CMD8                    8U              /* SEND_IF_COND */
#define CMD9                    9U              /* SEND_CSD */
#define CMD12                   12U             /* STOP_TRANSMISSION */
#define CMD16                   16U             /* SET_BLOCKLEN */
#define CMD17                   17U             /* READ_SINGLE_BLOCK */
#define CMD18                   18U             /* READ_MULTIPLE_BLOCK */
#define CMD24                   24U             /* WRITE_BLOCK */
#define CMD25                   25U             /* WRITE_MULTIPLE_BLOCK */
#define CMD55                   55U             /* APP_CMD */
#define CMD58                   58U             /* READ_OCR */
#define ACMD23                  (0x80U | 23U)   /* SET_WR_BLK_ERASE_COUNT */
#define ACMD41                  (0x80U | 41U)   /* SD_SEND_OP_COND */

#define R1_READY                0x00U
#define R1_IDLE                 0x01U
#define R1_NO_RESPONSE          0xFFU

#define TOKEN_START_BLOCK       0xFEU           /* single read/write, multi read */
#define TOKEN_START_MULTI       0xFCU           /* each block of a multi write */
#define TOKEN_STOP_TRAN         0xFDU           /* end of a multi write */
#define DATA_ACCEPTED           0x05U

#define INIT_TIMEOUT_MS         1000U           /* ACMD41 until the card leaves idle */
#define READY_TIMEOUT_MS        500U            /* card busy, e.g. programming a write */
#define READ_TIMEOUT_MS         200U            /* wait for a data token */
#define SPI_TIMEOUT_MS          100U            /* one HAL transfer */

static SD_CardType_t cardType = SD_CARD_NONE;
static uint32_t sectorCount = 0;
static SD_Status_t initStatus = SD_ERR_INIT;


/* Exchange one byte. A failed transfer reads as 0xFF: "no response". */
static uint8_t SD_Exchange(uint8_t out)
{
    uint8_t in = 0xFFU;

    if(HAL_SPI_TransmitReceive(&hspi2, &out, &in, 1U, SPI_TIMEOUT_MS) != HAL_OK)
    {
        return 0xFFU;
    }
    return in;
}


/* Change SPI2's clock (between transfers; the display shares the bus) */
static void SD_SetPrescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(&hspi2);
    MODIFY_REG(hspi2.Instance->CR1, SPI_CR1_BR, prescaler);
    __HAL_SPI_ENABLE(&hspi2);
}


static void SD_Deselect(void)
{
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_SET);
    (void)SD_Exchange(0xFFU);   /* the card releases MISO on the next clock */
}


/* The card holds MISO low while busy and returns 0xFF once ready */
static SD_Status_t SD_WaitReady(uint32_t timeoutMs)
{
    const uint32_t start = HAL_GetTick();

    do
    {
        if(SD_Exchange(0xFFU) == 0xFFU)
        {
            return SD_OK;
        }
    } while((HAL_GetTick() - start) < timeoutMs);

    return SD_ERR_TIMEOUT;
}


/* Select the card and wait until it isn't busy */
static SD_Status_t SD_Select(void)
{
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_RESET);
    (void)SD_Exchange(0xFFU);   /* a clock so the card drives MISO */

    if(SD_WaitReady(READY_TIMEOUT_MS) != SD_OK)
    {
        SD_Deselect();
        return SD_ERR_TIMEOUT;
    }

    return SD_OK;
}


/*
 * Send a command and return its R1 (bit 7 set = no response). Leaves the
 * card selected: the caller deselects once any data has been moved too.
 */
static uint8_t SD_SendCommand(uint8_t command, uint32_t argument)
{
    uint8_t r1 = R1_NO_RESPONSE;

    if((command & 0x80U) != 0U)
    {
        command &= 0x7FU;
        r1 = SD_SendCommand(CMD55, 0U);     /* application command: CMD55 first */

        if(r1 > R1_IDLE)
        {
            return r1;
        }
    }

    /* CMD12 is sent mid-read, while the card is still streaming data */
    if(command != CMD12)
    {
        SD_Deselect();

        if(SD_Select() != SD_OK)
        {
            return R1_NO_RESPONSE;
        }
    }

    /* the CRC is only checked for CMD0 and CMD8 in SPI mode */
    const uint8_t frame[6] =
    {
        (uint8_t)(0x40U | command),
        (uint8_t)(argument >> 24), (uint8_t)(argument >> 16), (uint8_t)(argument >> 8), (uint8_t)argument,
        (command == CMD0) ? 0x95U : ((command == CMD8) ? 0x87U : 0x01U)
    };

    for(uint32_t i = 0; i < sizeof(frame); i++)
    {
        (void)SD_Exchange(frame[i]);
    }

    if(command == CMD12)
    {
        (void)SD_Exchange(0xFFU);   /* skip the stuff byte */
    }

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


/* Receive one data block after a read command: start token, data, CRC */
static SD_Status_t SD_ReceiveData(uint8_t *buffer, uint32_t length)
{
    const uint32_t start = HAL_GetTick();
    uint8_t token;

    do
    {
        token = SD_Exchange(0xFFU);
    } while((token == 0xFFU) && ((HAL_GetTick() - start) < READ_TIMEOUT_MS));

    if(token != TOKEN_START_BLOCK)
    {
        return (token == 0xFFU) ? SD_ERR_TIMEOUT : SD_ERR_DATA;
    }

    /* MOSI must stay high while the card sends: 0xFF goes out from the
     * buffer in place (each byte is sent before the reply overwrites it) */
    memset(buffer, 0xFF, length);

    if(HAL_SPI_TransmitReceive(&hspi2, buffer, buffer, (uint16_t)length, SPI_TIMEOUT_MS) != HAL_OK)
    {
        return SD_ERR_SPI;
    }

    (void)SD_Exchange(0xFFU);       /* CRC */
    (void)SD_Exchange(0xFFU);

    return SD_OK;
}


/* Send one 512-byte block (or, with TOKEN_STOP_TRAN, end a multi write) */
static SD_Status_t SD_TransmitData(const uint8_t *buffer, uint8_t token)
{
    if(SD_WaitReady(READY_TIMEOUT_MS) != SD_OK)
    {
        return SD_ERR_TIMEOUT;
    }

    (void)SD_Exchange(token);

    if(token == TOKEN_STOP_TRAN)
    {
        return SD_OK;
    }

    if(HAL_SPI_Transmit(&hspi2, (uint8_t *)buffer, SD_SECTOR_SIZE, SPI_TIMEOUT_MS) != HAL_OK)
    {
        return SD_ERR_SPI;
    }

    (void)SD_Exchange(0xFFU);       /* dummy CRC */
    (void)SD_Exchange(0xFFU);

    return ((SD_Exchange(0xFFU) & 0x1FU) == DATA_ACCEPTED) ? SD_OK : SD_ERR_DATA;
}


/* Card size from the CSD register, in 512-byte sectors */
static SD_Status_t SD_ReadSectorCount(void)
{
    uint8_t csd[16];
    SD_Status_t status = SD_ERR_CMD;

    if(SD_SendCommand(CMD9, 0U) == R1_READY)
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
        const uint32_t cSize = ((uint32_t)(csd[7] & 0x3FU) << 16) | ((uint32_t)csd[8] << 8) | csd[9];

        sectorCount = (cSize + 1UL) << 10;
    }
    else
    {
        /* CSD v1.0: (C_SIZE + 1) x 2^(C_SIZE_MULT + 2) x 2^READ_BL_LEN bytes */
        const uint32_t readBlLen = csd[5] & 0x0FU;
        const uint32_t cSize = ((uint32_t)(csd[6] & 0x03U) << 10) | ((uint32_t)csd[7] << 2) | ((uint32_t)csd[8] >> 6);
        const uint32_t cSizeMult = ((uint32_t)(csd[9] & 0x03U) << 1) | ((uint32_t)csd[10] >> 7);

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

    cardType = SD_CARD_NONE;
    sectorCount = 0U;

    /* initialisation must run at <= 400 kHz: 85 MHz / 256 = 332 kHz */
    SD_SetPrescaler(SPI_BAUDRATEPRESCALER_256);

    /* at least 74 clocks with CS high before the first command */
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_SET);

    for(uint32_t i = 0; i < 10U; i++)
    {
        (void)SD_Exchange(0xFFU);
    }

    r1 = SD_SendCommand(CMD0, 0U);

    if(r1 != R1_IDLE)
    {
        status = (r1 == R1_NO_RESPONSE) ? SD_ERR_NO_CARD : SD_ERR_INIT;
    }
    else
    {
        const uint32_t start = HAL_GetTick();

        if(SD_SendCommand(CMD8, 0x1AAU) == R1_IDLE)
        {
            /* SD v2: R7 echoes the voltage range and check pattern */
            for(uint32_t i = 0; i < sizeof(ocr); i++)
            {
                ocr[i] = SD_Exchange(0xFFU);
            }

            if((ocr[2] == 0x01U) && (ocr[3] == 0xAAU))
            {
                /* HCS set: high-capacity cards are welcome */
                do
                {
                    r1 = SD_SendCommand(ACMD41, 1UL << 30);
                } while((r1 != R1_READY) && ((HAL_GetTick() - start) < INIT_TIMEOUT_MS));

                if((r1 == R1_READY) && (SD_SendCommand(CMD58, 0U) == R1_READY))
                {
                    for(uint32_t i = 0; i < sizeof(ocr); i++)
                    {
                        ocr[i] = SD_Exchange(0xFFU);
                    }

                    /* CCS: block-addressed SDHC/SDXC */
                    type = ((ocr[0] & 0x40U) != 0U) ? SD_CARD_V2_HC : SD_CARD_V2_SC;
                }
            }
        }
        else
        {
            /* SD v1 (MMC isn't supported) */
            do
            {
                r1 = SD_SendCommand(ACMD41, 0U);
            } while((r1 != R1_READY) && ((HAL_GetTick() - start) < INIT_TIMEOUT_MS));

            if(r1 == R1_READY)
            {
                type = SD_CARD_V1;
            }
        }

        /* byte-addressed cards: fix the block length at 512 */
        if(((type == SD_CARD_V1) || (type == SD_CARD_V2_SC))
                && (SD_SendCommand(CMD16, SD_SECTOR_SIZE) != R1_READY))
        {
            type = SD_CARD_NONE;
        }

        if(type == SD_CARD_NONE)
        {
            status = SD_ERR_INIT;
        }
    }

    SD_Deselect();

    /* back to the CubeMX speed, which the display also uses */
    SD_SetPrescaler(hspi2.Init.BaudRatePrescaler);

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


static SD_Status_t SD_ReadBlocksOnce(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_ERR_CMD;

    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    if(cardType != SD_CARD_V2_HC)
    {
        sector *= SD_SECTOR_SIZE;       /* only SDHC/SDXC are block addressed */
    }

    if(count == 1U)
    {
        if(SD_SendCommand(CMD17, sector) == R1_READY)
        {
            status = SD_ReceiveData(buffer, SD_SECTOR_SIZE);
        }
    }
    else if(SD_SendCommand(CMD18, sector) == R1_READY)
    {
        do
        {
            status = SD_ReceiveData(buffer, SD_SECTOR_SIZE);
            buffer += SD_SECTOR_SIZE;
        } while((--count > 0U) && (status == SD_OK));

        (void)SD_SendCommand(CMD12, 0U);    /* a multi read runs until stopped */
    }

    SD_Deselect();
    return status;
}


static SD_Status_t SD_WriteBlocksOnce(const uint8_t *buffer, uint32_t sector, uint32_t count)
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
        if(SD_SendCommand(CMD24, sector) == R1_READY)
        {
            status = SD_TransmitData(buffer, TOKEN_START_BLOCK);
        }
    }
    else
    {
        /* pre-erase hint: lets the card prepare, which speeds up multi writes */
        if(cardType != SD_CARD_V1)
        {
            (void)SD_SendCommand(ACMD23, count);
        }

        if(SD_SendCommand(CMD25, sector) == R1_READY)
        {
            do
            {
                status = SD_TransmitData(buffer, TOKEN_START_MULTI);
                buffer += SD_SECTOR_SIZE;
            } while((--count > 0U) && (status == SD_OK));

            /* always end the multi write, even after an error */
            if((SD_TransmitData(NULL, TOKEN_STOP_TRAN) != SD_OK) && (status == SD_OK))
            {
                status = SD_ERR_TIMEOUT;
            }
        }
    }

    SD_Deselect();
    return status;
}


/*
 * Recovery: after a failed transfer, re-initialise the card and try once
 * more. CMD0 puts the card back in a known state whatever it thought was
 * going on, and the retry rewrites any sector the failed attempt left
 * corrupt. A card that won't restart stays uninitialised, and FatFS starts
 * it again on the next mount (user_diskio.c).
 */
SD_Status_t SD_ReadBlocks(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_ReadBlocksOnce(buffer, sector, count);

    if((status != SD_OK) && (status != SD_ERR_INIT) && (SD_Init() == SD_OK))
    {
        status = SD_ReadBlocksOnce(buffer, sector, count);
    }

    return status;
}


SD_Status_t SD_WriteBlocks(const uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_WriteBlocksOnce(buffer, sector, count);

    if((status != SD_OK) && (status != SD_ERR_INIT) && (SD_Init() == SD_OK))
    {
        status = SD_WriteBlocksOnce(buffer, sector, count);
    }

    return status;
}


SD_Status_t SD_Sync(void)
{
    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    const SD_Status_t status = SD_Select();     /* waits until the card has finished programming */

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
