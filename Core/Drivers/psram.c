/*
 * psram.c
 *
 * APS6404L 8 MB SPI PSRAM driver (SPI mode, single data line).
 *
 * Every command is one CE# cycle built by PSRAM_Burst(). It drives SPI1 at
 * register level with interrupts masked, because CE# must not stay low for
 * longer than tCEM (8 us): the PSRAM can't refresh while it is selected.
 * The HAL SPI calls cost several us each in the -O0 build, and an audio ISR
 * landing mid-burst would hold CE# low for hundreds of us.
 */


#include "psram.h"

#include "main.h"


#define PSRAM_SPI_PORT          hspi1
extern SPI_HandleTypeDef PSRAM_SPI_PORT;

#define PSRAM_CMD_READ_ID       0x9FU
#define PSRAM_CMD_FAST_READ     0x0BU   /* 8 wait cycles; plain read 0x03 is limited to 33 MHz */
#define PSRAM_CMD_WRITE         0x02U
#define PSRAM_CMD_RESET_ENABLE  0x66U
#define PSRAM_CMD_RESET         0x99U

#define PSRAM_MFID              0x0DU
#define PSRAM_KGD_PASS          0x5DU

#define PSRAM_PAGE_BYTES        1024U

/*
 * Deadline for a whole burst, from CE# low: 10 us at 170 MHz. A normal
 * burst takes ~5-6 us, so this only trips on a fault.
 */
#define PSRAM_BURST_TIMEOUT_CYCLES  1700U

_Static_assert((PSRAM_PAGE_BYTES % PSRAM_MAX_BURST) == 0U,
        "PSRAM_MAX_BURST must divide the 1 KB page so no burst crosses a page");


static uint8_t psramMfid = 0;
static uint8_t psramKgd = 0;
static uint32_t maxSelectCycles = 0;
static uint32_t timeoutStatus = 0;



/*
 * Send one byte and store the byte clocked in. Returns 0 if a flag didn't
 * arrive before the burst deadline (start + PSRAM_BURST_TIMEOUT_CYCLES).
 * The deadline is only checked while actually waiting, so the normal path
 * costs no extra time inside CE#. DR must be accessed as 8 bits: a 16-bit
 * write would pack and send two bytes.
 */
__STATIC_FORCEINLINE uint32_t PSRAM_SpiByte(SPI_TypeDef *spi, uint8_t out, uint8_t *in, uint32_t start)
{
    while((spi->SR & SPI_SR_TXE) == 0U)
    {
        if((DWT->CYCCNT - start) > PSRAM_BURST_TIMEOUT_CYCLES)
        {
            return 0U;
        }
    }

    *(__IO uint8_t *)&spi->DR = out;

    while((spi->SR & SPI_SR_RXNE) == 0U)
    {
        if((DWT->CYCCNT - start) > PSRAM_BURST_TIMEOUT_CYCLES)
        {
            return 0U;
        }
    }

    *in = *(__IO uint8_t *)&spi->DR;

    return 1U;
}


/*
 * One CE# cycle: the header, then `length` data bytes either sent from
 * txData or, if txData is NULL, received into rxData. Records how long
 * CE# was low, measured with the DWT cycle counter. A stuck SPI flag ends
 * the burst with PSRAM_ERR_TIMEOUT instead of hanging.
 */
static PSRAM_Status_t PSRAM_Burst(
        const uint8_t *header,
        uint32_t headerLength,
        const uint8_t *txData,
        uint8_t *rxData,
        uint32_t length)
{
    SPI_TypeDef *spi = PSRAM_SPI_PORT.Instance;
    uint32_t primask = __get_PRIMASK();
    uint32_t ok = 1U;
    uint32_t start;
    uint32_t cycles;
    uint8_t discard;

    __disable_irq();

    SPI1_PSRAM_CS_GPIO_Port->BRR = SPI1_PSRAM_CS_Pin;
    start = DWT->CYCCNT;

    for(uint32_t i = 0; (i < headerLength) && ok; i++)
    {
        ok = PSRAM_SpiByte(spi, header[i], &discard, start);
    }

    /* separate loops keep per-byte branches out of the timed section */
    if(txData != NULL)
    {
        for(uint32_t i = 0; (i < length) && ok; i++)
        {
            ok = PSRAM_SpiByte(spi, txData[i], &discard, start);
        }
    }
    else
    {
        for(uint32_t i = 0; (i < length) && ok; i++)
        {
            ok = PSRAM_SpiByte(spi, 0x00U, &rxData[i], start);
        }
    }

    while(ok && ((spi->SR & SPI_SR_BSY) != 0U))
    {
        if((DWT->CYCCNT - start) > PSRAM_BURST_TIMEOUT_CYCLES)
        {
            ok = 0U;
        }
    }

    cycles = DWT->CYCCNT - start;
    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;

    __set_PRIMASK(primask);

    if(!ok)
    {
        timeoutStatus = spi->SR;
        return PSRAM_ERR_TIMEOUT;
    }

    if(cycles > maxSelectCycles)
    {
        maxSelectCycles = cycles;
    }

    return PSRAM_OK;
}


/*
 * Change the SPI clock between transfers. Only called while idle
 * (BSY clear, FIFOs empty), so disabling the SPI here is safe.
 */
static void PSRAM_SetBaudPrescaler(SPI_TypeDef *spi, uint32_t prescaler)
{
    CLEAR_BIT(spi->CR1, SPI_CR1_SPE);
    MODIFY_REG(spi->CR1, SPI_CR1_BR, prescaler);
    SET_BIT(spi->CR1, SPI_CR1_SPE);
}


static PSRAM_Status_t PSRAM_WriteByte(uint32_t address, uint8_t value)
{
    return PSRAM_Write(address, &value, 1U);
}


/*
 * Read one byte and compare it: PSRAM_ERR_DATA on a mismatch.
 */
static PSRAM_Status_t PSRAM_CheckByte(uint32_t address, uint8_t expected)
{
    uint8_t value;
    PSRAM_Status_t status = PSRAM_Read(address, &value, 1U);

    if((status == PSRAM_OK) && (value != expected))
    {
        status = PSRAM_ERR_DATA;
    }

    return status;
}


/*
 * Test data for the full test. Every byte sees both this value and its
 * inverse, so every cell is checked at 0 and 1.
 */
static uint8_t PSRAM_Pattern(uint32_t address, uint32_t invert)
{
    uint8_t value = (uint8_t)(address ^ (address >> 8) ^ (address >> 16));

    return invert ? (uint8_t)~value : value;
}



PSRAM_Status_t PSRAM_Init(void)
{
    SPI_TypeDef *spi = PSRAM_SPI_PORT.Instance;
    PSRAM_Status_t status;
    uint8_t command;
    uint8_t header[4] = { PSRAM_CMD_READ_ID, 0x00U, 0x00U, 0x00U };    /* address is don't-care */
    uint8_t id[2] = { 0x00U, 0x00U };

    /* DWT cycle counter, used to measure CE# low time */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    HAL_GPIO_WritePin(SPI1_PSRAM_CS_GPIO_Port, SPI1_PSRAM_CS_Pin, GPIO_PIN_SET);

    /*
     * Reset and Read ID run at 21 MHz, well inside every command's limit.
     * The tests then run at the CubeMX rate (42.5 MHz), so an ID that passes
     * with data tests that fail points at signal integrity, not the driver.
     * This also enables the SPI, which HAL otherwise only does on its first
     * transfer.
     */
    PSRAM_SetBaudPrescaler(spi, SPI_BAUDRATEPRESCALER_8);

    while((spi->SR & SPI_SR_RXNE) != 0U)
    {
        (void)*(__IO uint8_t *)&spi->DR;
    }

    /* power-up (tPU = 150 us) has long passed by the time this runs */
    command = PSRAM_CMD_RESET_ENABLE;
    status = PSRAM_Burst(&command, 1U, NULL, NULL, 0U);

    if(status == PSRAM_OK)
    {
        command = PSRAM_CMD_RESET;
        status = PSRAM_Burst(&command, 1U, NULL, NULL, 0U);
    }

    if(status == PSRAM_OK)
    {
        HAL_Delay(1);   /* generous margin for reset recovery */
        status = PSRAM_Burst(header, sizeof(header), NULL, id, sizeof(id));
    }

    psramMfid = id[0];
    psramKgd = id[1];

    PSRAM_SetBaudPrescaler(spi, PSRAM_SPI_PORT.Init.BaudRatePrescaler);

    if(status != PSRAM_OK)
    {
        return status;
    }

    if((psramMfid != PSRAM_MFID) || (psramKgd != PSRAM_KGD_PASS))
    {
        return PSRAM_ERR_ID;
    }

    return PSRAM_OK;
}


void PSRAM_GetId(uint8_t *mfid, uint8_t *kgd)
{
    *mfid = psramMfid;
    *kgd = psramKgd;
}


PSRAM_Status_t PSRAM_Write(uint32_t address, const uint8_t *data, uint32_t length)
{
    PSRAM_Status_t status = PSRAM_OK;
    uint8_t header[4];

    while((length > 0U) && (status == PSRAM_OK))
    {
        /* stop at the next PSRAM_MAX_BURST boundary, so no burst crosses a page */
        uint32_t chunk = PSRAM_MAX_BURST - (address % PSRAM_MAX_BURST);

        if(chunk > length)
        {
            chunk = length;
        }

        header[0] = PSRAM_CMD_WRITE;
        header[1] = (uint8_t)(address >> 16);
        header[2] = (uint8_t)(address >> 8);
        header[3] = (uint8_t)address;

        status = PSRAM_Burst(header, sizeof(header), data, NULL, chunk);

        address += chunk;
        data += chunk;
        length -= chunk;
    }

    return status;
}


PSRAM_Status_t PSRAM_Read(uint32_t address, uint8_t *data, uint32_t length)
{
    PSRAM_Status_t status = PSRAM_OK;
    uint8_t header[5];

    while((length > 0U) && (status == PSRAM_OK))
    {
        uint32_t chunk = PSRAM_MAX_BURST - (address % PSRAM_MAX_BURST);

        if(chunk > length)
        {
            chunk = length;
        }

        header[0] = PSRAM_CMD_FAST_READ;
        header[1] = (uint8_t)(address >> 16);
        header[2] = (uint8_t)(address >> 8);
        header[3] = (uint8_t)address;
        header[4] = 0x00U;      /* the 8 wait cycles */

        status = PSRAM_Burst(header, sizeof(header), NULL, data, chunk);

        address += chunk;
        data += chunk;
        length -= chunk;
    }

    return status;
}


/*
 * Walking-ones address test (after Michael Barr's memTestAddressBus).
 * Writes only to power-of-two offsets, so it takes a few ms. Catches
 * address lines stuck high, stuck low or shorted together, which a plain
 * data pattern can miss because two aliased addresses read back the same.
 */
PSRAM_Status_t PSRAM_TestAddressLines(uint32_t *failAddress)
{
    const uint8_t pattern = 0xAAU;
    const uint8_t antipattern = 0x55U;
    PSRAM_Status_t status = PSRAM_OK;

    for(uint32_t offset = 1U; (offset < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); offset <<= 1)
    {
        *failAddress = offset;
        status = PSRAM_WriteByte(offset, pattern);
    }

    /* address bits stuck high: writing address 0 would land on another offset */
    if(status == PSRAM_OK)
    {
        *failAddress = 0U;
        status = PSRAM_WriteByte(0U, antipattern);
    }

    for(uint32_t offset = 1U; (offset < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); offset <<= 1)
    {
        *failAddress = offset;
        status = PSRAM_CheckByte(offset, pattern);
    }

    if(status == PSRAM_OK)
    {
        *failAddress = 0U;
        status = PSRAM_WriteByte(0U, pattern);
    }

    /* address bits stuck low or shorted: each test write must land only on its own offset */
    for(uint32_t test = 1U; (test < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); test <<= 1)
    {
        *failAddress = test;
        status = PSRAM_WriteByte(test, antipattern);

        if(status == PSRAM_OK)
        {
            status = PSRAM_CheckByte(0U, pattern);
        }

        for(uint32_t offset = 1U; (offset < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); offset <<= 1)
        {
            if(offset != test)
            {
                status = PSRAM_CheckByte(offset, pattern);
            }
        }

        if(status == PSRAM_OK)
        {
            status = PSRAM_WriteByte(test, pattern);
        }
    }

    return status;
}


/*
 * Write and verify all 8 MB twice: an address-derived pattern, then its
 * inverse. Slow (tens of seconds) with the polled driver.
 */
PSRAM_Status_t PSRAM_TestFull(uint32_t *failAddress)
{
    PSRAM_Status_t status = PSRAM_OK;
    uint8_t buffer[PSRAM_MAX_BURST];

    for(uint32_t invert = 0U; (invert < 2U) && (status == PSRAM_OK); invert++)
    {
        for(uint32_t address = 0U; (address < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); address += PSRAM_MAX_BURST)
        {
            for(uint32_t i = 0U; i < PSRAM_MAX_BURST; i++)
            {
                buffer[i] = PSRAM_Pattern(address + i, invert);
            }

            *failAddress = address;
            status = PSRAM_Write(address, buffer, PSRAM_MAX_BURST);
        }

        for(uint32_t address = 0U; (address < PSRAM_SIZE_BYTES) && (status == PSRAM_OK); address += PSRAM_MAX_BURST)
        {
            *failAddress = address;
            status = PSRAM_Read(address, buffer, PSRAM_MAX_BURST);

            for(uint32_t i = 0U; (i < PSRAM_MAX_BURST) && (status == PSRAM_OK); i++)
            {
                if(buffer[i] != PSRAM_Pattern(address + i, invert))
                {
                    *failAddress = address + i;
                    status = PSRAM_ERR_DATA;
                }
            }
        }
    }

    return status;
}


uint32_t PSRAM_GetMaxSelectNs(void)
{
    return (maxSelectCycles * 1000U) / (SystemCoreClock / 1000000U);
}


uint32_t PSRAM_GetTimeoutStatus(void)
{
    return timeoutStatus;
}
