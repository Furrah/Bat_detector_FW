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
 *
 * Everything is polled through HAL, the data blocks included. A
 * register-level DMA version of the block transfers was tried and dropped:
 * now and then the card missed a block's start token and stored a shifted
 * block, for reasons not found (diary 40). It is in v2.0.0.
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
#define SD_CMD13                13U             /* SEND_STATUS (R2) */

/* sdLastError's command field for a failure in SD_Sync (not a command) */
#define SD_OP_SYNC              0xFEU
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

/* Diagnostics (sd_spi.h) */
volatile uint32_t sdErrorCount = 0;
volatile uint32_t sdLastError = 0;
volatile uint32_t sdLastCardStatus = 0;
volatile uint32_t sdLateResponse = 0;
volatile uint32_t sdBusyMaxUs = 0;
volatile uint32_t sdRecoveries = 0;
volatile uint32_t sdRecoveryFailures = 0;
volatile uint32_t sdHalFailures = 0;
volatile uint32_t sdHalFailure = 0;
volatile uint32_t sdStaleRx = 0;
volatile uint32_t sdStaleRxFirst = 0;

static uint8_t opCommand;           /* the read/write command in progress */
static uint8_t opFailed;            /* its first failure has been recorded */
static uint8_t opPhase;             /* SD_Phase_t: where in the protocol the next byte is */


/* Count each failed read or write once, and keep the details of the first
 * failure since sdLastError was cleared: later ones are usually knock-on */
static void SD_RecordError(SD_Step_t step, SD_Status_t status, uint8_t reply)
{
    if(!opFailed)
    {
        opFailed = 1U;
        sdErrorCount++;

        if(sdLastError == 0U)
        {
            sdLastError = (uint32_t)step | ((uint32_t)status << 8) | ((uint32_t)reply << 16)
                    | ((uint32_t)opCommand << 24);
        }
    }
}


static void SD_StartOp(uint8_t command)
{
    opCommand = command;
    opFailed = 0U;
}



/*
 * Diagnostic: nothing should be in SPI2's receive FIFO, or overrun, between
 * transfers. A leftover byte would make every later exchange read the byte
 * before its own. Count it, note where it was found, and flush it.
 */
static void SD_CheckRxFifo(void)
{
    SPI_TypeDef *const spi = SD_SPI_PORT.Instance;
    const uint32_t sr = spi->SR;

    if((sr & (SPI_SR_FRLVL | SPI_SR_OVR)) != 0U)
    {
        sdStaleRx++;

        if(sdStaleRxFirst == 0U)
        {
            sdStaleRxFirst = (sr & 0xFFFFU) | ((uint32_t)opPhase << 16) | ((uint32_t)opCommand << 24);
        }

        /* 8-bit reads pop one byte each (a wider read would pop two) */
        for(uint32_t i = 0; (i < 8U) && ((spi->SR & SPI_SR_FRLVL) != 0U); i++)
        {
            (void)*(volatile uint8_t *)&spi->DR;
        }

        if((spi->SR & SPI_SR_OVR) != 0U)
        {
            (void)*(volatile uint8_t *)&spi->DR;    /* DR then SR clears OVR */
            (void)spi->SR;
        }
    }
}


/*
 * Exchange one byte. A HAL failure returns 0xFF, which callers see as
 * "no response", and is recorded: a failure here silently drops the byte,
 * e.g. a data token.
 */
static uint8_t SD_Exchange(uint8_t out)
{
    uint8_t in = 0xFFU;
    HAL_StatusTypeDef hal;

    SD_CheckRxFifo();

    hal = HAL_SPI_TransmitReceive(&SD_SPI_PORT, &out, &in, 1U, SD_SPI_TIMEOUT_MS);

    if(hal != HAL_OK)
    {
        sdHalFailures++;

        if(sdHalFailure == 0U)
        {
            sdHalFailure = (uint32_t)hal | ((uint32_t)SD_SPI_PORT.State << 4)
                    | ((SD_SPI_PORT.ErrorCode & 0xFFU) << 8) | ((uint32_t)SD_SPI_PORT.Lock << 16)
                    | ((uint32_t)opPhase << 17) | ((uint32_t)opCommand << 24);
        }

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
    opPhase = SD_PHASE_DESELECT;
    (void)SD_Exchange(0xFFU);
}


/*
 * The card holds MISO low while busy and returns 0xFF once ready.
 */
static SD_Status_t SD_WaitReady(uint32_t timeoutMs)
{
    const uint32_t start = HAL_GetTick();
    const uint32_t startCycles = DWT->CYCCNT;
    SD_Status_t status = SD_ERR_TIMEOUT;
    uint32_t us;

    do
    {
        if(SD_Exchange(0xFFU) == 0xFFU)
        {
            status = SD_OK;
            break;
        }
    } while((HAL_GetTick() - start) < timeoutMs);

    /* the cycle counter wraps every 25 s, far beyond any timeout here */
    us = (DWT->CYCCNT - startCycles) / (SystemCoreClock / 1000000U);

    if(us > sdBusyMaxUs)
    {
        sdBusyMaxUs = us;
    }

    return status;
}


/*
 * Select the card and wait until it isn't busy. The display driver always
 * deselects after each call, so only the SD CS is low here.
 */
static SD_Status_t SD_Select(void)
{
    HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_RESET);

    opPhase = SD_PHASE_SELECT;
    (void)SD_Exchange(0xFFU);   /* dummy clock so the card drives MISO */

    if(SD_WaitReady(SD_READY_TIMEOUT_MS) != SD_OK)
    {
        SD_RecordError(SD_STEP_SELECT, SD_ERR_TIMEOUT, 0x00U);
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

    opPhase = SD_PHASE_COMMAND;

    for(uint32_t i = 0; i < sizeof(frame); i++)
    {
        (void)SD_Exchange(frame[i]);
    }

    if(command == SD_CMD12)
    {
        (void)SD_Exchange(0xFFU);   /* skip the stuff byte */
    }

    /* R1 arrives within 8 bytes */
    opPhase = SD_PHASE_R1;

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

    opPhase = SD_PHASE_READ_TOKEN;

    do
    {
        token = SD_Exchange(0xFFU);
    } while((token == 0xFFU) && ((HAL_GetTick() - start) < SD_READ_TIMEOUT_MS));

    if(token != SD_TOKEN_START_BLOCK)
    {
        const SD_Status_t status = (token == 0xFFU) ? SD_ERR_TIMEOUT : SD_ERR_DATA;

        SD_RecordError(SD_STEP_READ_TOKEN, status, token);
        return status;
    }

    /* MOSI must stay high while the card sends: 0xFF goes out from the buffer
     * in place (each byte is sent before the reply overwrites it) */
    memset(buffer, 0xFF, length);

    if(HAL_SPI_TransmitReceive(&SD_SPI_PORT, buffer, buffer, (uint16_t)length, SD_SPI_TIMEOUT_MS) != HAL_OK)
    {
        SD_RecordError(SD_STEP_READ_DATA, SD_ERR_SPI, (uint8_t)HAL_SPI_GetError(&SD_SPI_PORT));
        return SD_ERR_SPI;
    }

    opPhase = SD_PHASE_READ_END;
    (void)SD_Exchange(0xFFU);
    (void)SD_Exchange(0xFFU);

    return SD_OK;
}


/*
 * Diagnostic, after a write's data response read as 0xFF: the card sent
 * nothing, as if it hadn't received the whole block. Keep clocking until it
 * answers, to count how many bytes it was short. A few bytes means bytes
 * were lost; hundreds, that it missed the start token and took an 0xFE in
 * the data for one. It also gets the card back in step.
 */
#define SD_PROBE_BYTES          1024U

static void SD_ProbeLateResponse(void)
{
    opPhase = SD_PHASE_PROBE;

    for(uint32_t i = 1U; i <= SD_PROBE_BYTES; i++)
    {
        const uint8_t reply = SD_Exchange(0xFFU);

        if(reply != 0xFFU)
        {
            if(sdLateResponse == 0U)    /* keep the first */
            {
                sdLateResponse = i | ((uint32_t)reply << 16);
            }
            return;
        }
    }

    if(sdLateResponse == 0U)
    {
        sdLateResponse = 0xFFFFU;
    }
}


/*
 * Send one 512-byte block (or, with SD_TOKEN_STOP_TRAN, just end a multi
 * write) and check the card accepted it.
 */
static SD_Status_t SD_TransmitData(const uint8_t *buffer, uint8_t token)
{
    uint8_t response;

    opPhase = SD_PHASE_WRITE_WAIT;

    if(SD_WaitReady(SD_READY_TIMEOUT_MS) != SD_OK)
    {
        SD_RecordError(SD_STEP_WRITE_BUSY, SD_ERR_TIMEOUT, token);
        return SD_ERR_TIMEOUT;
    }

    opPhase = SD_PHASE_WRITE_TOKEN;
    (void)SD_Exchange(token);

    if(token == SD_TOKEN_STOP_TRAN)
    {
        return SD_OK;
    }

    if(HAL_SPI_Transmit(&SD_SPI_PORT, (uint8_t *)buffer, SD_SECTOR_SIZE, SD_SPI_TIMEOUT_MS) != HAL_OK)
    {
        SD_RecordError(SD_STEP_WRITE_DATA, SD_ERR_SPI, (uint8_t)HAL_SPI_GetError(&SD_SPI_PORT));
        return SD_ERR_SPI;
    }

    /* dummy CRC */
    opPhase = SD_PHASE_WRITE_END;
    (void)SD_Exchange(0xFFU);
    (void)SD_Exchange(0xFFU);

    response = SD_Exchange(0xFFU);

    if((response & 0x1FU) != SD_DATA_ACCEPTED)
    {
        SD_RecordError(SD_STEP_WRITE_REJECTED, SD_ERR_DATA, response);

        if(response == 0xFFU)
        {
            SD_ProbeLateResponse();
        }

        return SD_ERR_DATA;
    }

    return SD_OK;
}


/*
 * After a failed write: ask the card what it thinks happened (CMD13, R2 =
 * R1 plus a second status byte: write protect, ECC, internal error...).
 */
static void SD_ReadCardStatus(void)
{
    const uint8_t r1 = SD_SendCommand(SD_CMD13, 0U);
    uint32_t status = 0x1FFFFU;     /* no answer */

    if((r1 & 0x80U) == 0U)
    {
        status = 0x10000U | ((uint32_t)r1 << 8) | SD_Exchange(0xFFU);
    }

    SD_Deselect();

    if(sdLastCardStatus == 0U)      /* keep the first, like sdLastError */
    {
        sdLastCardStatus = status;
    }
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


static SD_Status_t SD_ReadBlocksOnce(uint8_t *buffer, uint32_t sector, uint32_t count)
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

    SD_StartOp((count == 1U) ? SD_CMD17 : SD_CMD18);

    const uint8_t r1 = SD_SendCommand(opCommand, sector);

    if(r1 != SD_R1_READY)
    {
        SD_RecordError(SD_STEP_COMMAND, SD_ERR_CMD, r1);
    }
    else if(count == 1U)
    {
        status = SD_ReceiveData(buffer, SD_SECTOR_SIZE);
    }
    else
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

    SD_StartOp((count == 1U) ? SD_CMD24 : SD_CMD25);

    if(count == 1U)
    {
        const uint8_t r1 = SD_SendCommand(SD_CMD24, sector);

        if(r1 == SD_R1_READY)
        {
            status = SD_TransmitData(buffer, SD_TOKEN_START_BLOCK);
        }
        else
        {
            SD_RecordError(SD_STEP_COMMAND, SD_ERR_CMD, r1);
        }
    }
    else
    {
        /* pre-erase hint: lets the card prepare, which speeds up multi writes */
        if(cardType != SD_CARD_V1)
        {
            (void)SD_SendCommand(SD_ACMD23, count);
        }

        const uint8_t r1 = SD_SendCommand(SD_CMD25, sector);

        if(r1 == SD_R1_READY)
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
        else
        {
            SD_RecordError(SD_STEP_COMMAND, SD_ERR_CMD, r1);
        }
    }

    SD_Deselect();

    if(status != SD_OK)
    {
        SD_ReadCardStatus();
    }

    return status;
}


/*
 * Recovery: after a failed transfer, re-initialise the card and try once
 * more. CMD0 puts the card back in a known state, whatever it thought was
 * going on (e.g. still receiving a block whose start token it missed), and
 * the retry rewrites any sector the failed attempt left corrupt. It costs
 * the start-up sequence at 332 kHz, a few ms to tens of ms, which the PSRAM
 * ring absorbs. A card that won't restart stays uninitialised, and
 * user_diskio.c then makes FatFS start it again on the next mount or open.
 */
static SD_Status_t SD_Recover(void)
{
    sdRecoveries++;

    if(SD_Init() != SD_OK)
    {
        sdRecoveryFailures++;
        return SD_ERR_INIT;
    }

    return SD_OK;
}


SD_Status_t SD_ReadBlocks(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_ReadBlocksOnce(buffer, sector, count);

    if((status != SD_OK) && (status != SD_ERR_INIT) && (SD_Recover() == SD_OK))
    {
        status = SD_ReadBlocksOnce(buffer, sector, count);
    }

    return status;
}


SD_Status_t SD_WriteBlocks(const uint8_t *buffer, uint32_t sector, uint32_t count)
{
    SD_Status_t status = SD_WriteBlocksOnce(buffer, sector, count);

    if((status != SD_OK) && (status != SD_ERR_INIT) && (SD_Recover() == SD_OK))
    {
        status = SD_WriteBlocksOnce(buffer, sector, count);
    }

    return status;
}


SD_Status_t SD_Sync(void)
{
    SD_Status_t status;

    if(cardType == SD_CARD_NONE)
    {
        return SD_ERR_INIT;
    }

    SD_StartOp(SD_OP_SYNC);
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
