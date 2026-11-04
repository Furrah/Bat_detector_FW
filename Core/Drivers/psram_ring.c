/*
 * psram_ring.c
 *
 * The PSRAM ring; see psram_ring.h.
 *
 * Each burst is one CE# cycle, moved entirely by DMA (DMA2 Ch2 sends, Ch1
 * receives every byte clocked back):
 *   write  0x02 + 24-bit address + 16 data bytes             20 bytes, 3.8 us at 42.5 MHz
 *   read   0x0B + 24-bit address + 1 wait byte + 16 bytes    21 bytes, 4.0 us
 * The receive channel finishing means the burst is over: its interrupt
 * raises CE# first, then starts the next burst. CE# is low for at most
 * ~4.5 us, against the PSRAM's 8 us limit (tCEM).
 *
 * A block (512 bytes) is 32 write bursts, about 250 us of the 1.024 ms
 * before the next block arrives. Writes go before reads.
 *
 * The burst interrupt runs ~31,000 times a second, so it runs from RAM
 * (__RAM_FUNC) and this file builds at -O2: from flash it cost ~4 us a
 * burst, from RAM about 2.3 us. Its helpers are forced inline: left to the
 * compiler, an unrelated edit once made two of them calls (+45 cycles).
 */

#include "psram_ring.h"

#include <string.h>

#include "main.h"

#pragma GCC optimize ("O2")

extern SPI_HandleTypeDef hspi1;
extern DMA_HandleTypeDef hdma_spi1_rx;
extern DMA_HandleTypeDef hdma_spi1_tx;

/* 16 data bytes per burst: 32 took reads to 8.1 us, over tCEM */
#define BURST                   16U
#define BURSTS_PER_BLOCK        (PSRAM_RING_BLOCK_BYTES / BURST)

#define WRITE_HEADER            4U          /* command + 24-bit address */
#define READ_HEADER             5U          /* command + address + 8 wait cycles */

#define CMD_WRITE               0x02U
#define CMD_FAST_READ           0x0BU       /* plain read (0x03) is limited to 33 MHz */

#define RING_IRQn               DMA2_Channel1_IRQn
#define LAST_IRQn               CORDIC_IRQn     /* the G431's last interrupt */

#define READ_QUEUE              16U         /* the recorder queues a whole 8-block chunk */
#define READ_GUARD              64U         /* reads keep 64 blocks (65 ms) clear of the writer */

/* SCK/MISO/MOSI (PB3-5) must be at least High speed: the top bit of each OSPEEDR field */
#define SPI1_PINS_FAST          ((1UL << 7) | (1UL << 9) | (1UL << 11))

_Static_assert((PSRAM_RING_BLOCK_BYTES % BURST) == 0U, "bursts must tile a block");
_Static_assert((1024U % BURST) == 0U, "a burst must not cross a 1 KB PSRAM page");
_Static_assert((PSRAM_SIZE_BYTES % PSRAM_RING_BLOCK_BYTES) == 0U, "blocks must tile the PSRAM");

typedef enum
{
    BURST_WRITE = 0,
    BURST_READ
} BurstKind_t;

typedef struct
{
    uint32_t          block;
    uint8_t          *dest;
    volatile uint8_t *done;
} ReadRequest_t;

static volatile uint8_t  running = 0;
static volatile uint8_t  paused = 0;            /* not recording; reads still run */
static volatile uint8_t  busy = 0;              /* a burst is in flight */
static volatile uint32_t pushedBlocks = 0;      /* blocks handed over by the ADC */
static volatile uint32_t writtenBlocks = 0;     /* blocks completely written */
static volatile uint32_t resumeBlock = 0;       /* first block recorded since the last pause */
static volatile uint32_t blockTicks = 0;        /* PushBlock calls: the stuck-burst clock */

/* The block being written; writeSource is NULL when there's none */
static const uint8_t * volatile writeSource = NULL;
static uint32_t writeBlock;
static uint32_t writeBurst;

static ReadRequest_t readQueue[READ_QUEUE];
static volatile uint32_t readHead = 0;
static volatile uint32_t readCount = 0;
static uint32_t readBurst = 0;

/* The burst in flight */
static BurstKind_t burstKind;
static uint32_t burstBlock;
static uint32_t burstStartedAt;                 /* blockTicks when it started */

static uint8_t txWrite[WRITE_HEADER + BURST];
static uint8_t txRead[READ_HEADER + BURST];     /* zeros after the header */
static uint8_t rxBuffer[READ_HEADER + BURST];

static DMA_Channel_TypeDef *rxChannel;
static DMA_Channel_TypeDef *txChannel;
static DMA_TypeDef *rxDma;
static DMA_TypeDef *txDma;
static uint32_t rxFlagShift;
static uint32_t txFlagShift;

/* SPI1 settings, to restore after a fault resets it */
static uint32_t spiCr1;
static uint32_t spiCr2;


__STATIC_FORCEINLINE uint32_t PsramRing_BlockAddress(uint32_t block)
{
    return (block % PSRAM_RING_BLOCKS) * PSRAM_RING_BLOCK_BYTES;
}


__STATIC_FORCEINLINE void PsramRing_PutHeader(uint8_t *header, uint8_t command, uint32_t address)
{
    header[0] = command;
    header[1] = (uint8_t)(address >> 16);
    header[2] = (uint8_t)(address >> 8);
    header[3] = (uint8_t)address;
}


static __RAM_FUNC void PsramRing_StopDma(void)
{
    rxChannel->CCR &= ~DMA_CCR_EN;
    txChannel->CCR &= ~DMA_CCR_EN;
    rxDma->IFCR = DMA_IFCR_CGIF1 << rxFlagShift;
    txDma->IFCR = DMA_IFCR_CGIF1 << txFlagShift;
}


/*
 * Start the next burst: the pending write first, else the oldest read.
 * Only called with no burst in flight, from the burst interrupt or with
 * interrupts masked.
 */
static __RAM_FUNC void PsramRing_StartBurst(void)
{
    const uint8_t *tx;
    uint32_t length;

    if(writeSource != NULL)
    {
        const uint32_t offset = writeBurst * BURST;

        PsramRing_PutHeader(txWrite, CMD_WRITE, PsramRing_BlockAddress(writeBlock) + offset);
        memcpy(&txWrite[WRITE_HEADER], writeSource + offset, BURST);

        tx = txWrite;
        length = sizeof(txWrite);
        burstKind = BURST_WRITE;
        burstBlock = writeBlock;
    }
    else if(readCount > 0U)
    {
        const ReadRequest_t *request = &readQueue[readHead];

        /* txRead[4] stays 0: the 8 wait cycles */
        PsramRing_PutHeader(txRead, CMD_FAST_READ, PsramRing_BlockAddress(request->block) + (readBurst * BURST));

        tx = txRead;
        length = sizeof(txRead);
        burstKind = BURST_READ;
        burstBlock = request->block;
    }
    else
    {
        busy = 0U;
        return;
    }

    busy = 1U;
    burstStartedAt = blockTicks;

    rxChannel->CMAR = (uint32_t)rxBuffer;
    rxChannel->CNDTR = length;
    rxChannel->CCR |= DMA_CCR_EN;

    txChannel->CMAR = (uint32_t)tx;
    txChannel->CNDTR = length;

    SPI1_PSRAM_CS_GPIO_Port->BRR = SPI1_PSRAM_CS_Pin;   /* CE# low */
    txChannel->CCR |= DMA_CCR_EN;                       /* the burst starts here */
}


/*
 * A burst failed or never finished. Release the PSRAM, reset SPI1 and drop
 * the work in hand. Pending reads are failed, so nothing waits forever.
 */
static void PsramRing_Fault(void)
{
    SPI_TypeDef *spi = hspi1.Instance;

    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;
    PsramRing_StopDma();

    __HAL_RCC_SPI1_FORCE_RESET();
    __HAL_RCC_SPI1_RELEASE_RESET();
    spi->CR2 = spiCr2;
    spi->CR1 = spiCr1 & ~SPI_CR1_SPE;
    spi->CR1 = spiCr1;

    writeSource = NULL;

    while(readCount > 0U)
    {
        *readQueue[readHead].done = PSRAM_RING_FAILED;
        readHead = (readHead + 1U) % READ_QUEUE;
        readCount--;
    }

    readBurst = 0U;
    busy = 0U;
}


PsramRing_Status_t PsramRing_Start(void)
{
    SPI_TypeDef *spi = hspi1.Instance;

    /* What the burst timing depends on, each a CubeMX setting that was once
     * wrong: DMA on DMA2 Ch1/Ch2, 42.5 MHz (/4), 8-bit frames with an 8-bit
     * receive threshold, NSS pulse mode off (it adds 2 clocks a byte and
     * pushed bursts past tCEM), and fast pins (on Low speed the SPI hung). */
    if((hdma_spi1_rx.Instance != DMA2_Channel1) || (hdma_spi1_tx.Instance != DMA2_Channel2)
            || ((spi->CR1 & SPI_CR1_BR) != SPI_BAUDRATEPRESCALER_4)
            || ((spi->CR2 & SPI_CR2_DS) != SPI_DATASIZE_8BIT)
            || ((spi->CR2 & SPI_CR2_FRXTH) == 0U)
            || ((spi->CR2 & SPI_CR2_NSSP) != 0U)
            || ((GPIOB->OSPEEDR & SPI1_PINS_FAST) != SPI1_PINS_FAST))
    {
        return PSRAM_RING_ERR_CONFIG;
    }

    /* The TX channel never interrupts; CubeMX enables its vector anyway */
    HAL_NVIC_DisableIRQ(DMA2_Channel2_IRQn);

    /* The burst interrupt must be alone at priority 0, so nothing can delay it */
    if(NVIC_GetPriority(RING_IRQn) != 0U)
    {
        return PSRAM_RING_ERR_PRIORITY;
    }

    for(int32_t irq = 0; irq <= (int32_t)LAST_IRQn; irq++)
    {
        if((irq != (int32_t)RING_IRQn) && (NVIC_GetEnableIRQ((IRQn_Type)irq) != 0U)
                && (NVIC_GetPriority((IRQn_Type)irq) == 0U))
        {
            return PSRAM_RING_ERR_PRIORITY;
        }
    }

    rxChannel = hdma_spi1_rx.Instance;
    txChannel = hdma_spi1_tx.Instance;
    rxDma = hdma_spi1_rx.DmaBaseAddress;
    txDma = hdma_spi1_tx.DmaBaseAddress;
    rxFlagShift = hdma_spi1_rx.ChannelIndex & 0x1FU;
    txFlagShift = hdma_spi1_tx.ChannelIndex & 0x1FU;

    /* CubeMX set direction, increments, byte width and priority; add the
     * interrupts while the channel is disabled */
    PsramRing_StopDma();
    rxChannel->CPAR = (uint32_t)&spi->DR;
    txChannel->CPAR = (uint32_t)&spi->DR;
    rxChannel->CCR |= DMA_CCR_TCIE | DMA_CCR_TEIE;

    memset(txRead, 0, sizeof(txRead));

    /* drain anything the boot-time driver left, then hand the SPI to the DMA */
    while((spi->SR & SPI_SR_FRLVL) != 0U)
    {
        (void)*(__IO uint8_t *)&spi->DR;
    }

    SET_BIT(spi->CR2, SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
    SET_BIT(spi->CR1, SPI_CR1_SPE);
    spiCr1 = spi->CR1;
    spiCr2 = spi->CR2;

    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;
    running = 1U;

    return PSRAM_RING_OK;
}


uint8_t PsramRing_IsRunning(void)
{
    return running;
}


void PsramRing_SetPaused(uint8_t pause)
{
    /* pushedBlocks doesn't move while paused, so it's the next block to be recorded */
    if(paused && !pause)
    {
        resumeBlock = pushedBlocks;
    }

    paused = pause;
}


uint32_t PsramRing_GetResumeBlock(void)
{
    return resumeBlock;
}


uint8_t PsramRing_IsRecording(void)
{
    return running && !paused;
}


uint32_t PsramRing_GetPushedBlocks(void)
{
    return pushedBlocks;
}


uint32_t PsramRing_GetWrittenBlocks(void)
{
    return writtenBlocks;
}


void PsramRing_PushBlock(const uint16_t *block)
{
    if(!running)
    {
        return;
    }

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    /* A burst takes ~5 us: one started before the last block arrived is
     * stuck. Checked while paused too, so a stuck read is still recovered. */
    if(busy && (burstStartedAt != blockTicks))
    {
        PsramRing_Fault();
    }

    blockTicks++;

    if(!paused)
    {
        /* if the last block isn't finished, its rest is lost: the new one takes over */
        writeSource = (const uint8_t *)block;
        writeBlock = pushedBlocks;
        writeBurst = 0U;
        pushedBlocks++;

        if(!busy)
        {
            PsramRing_StartBurst();
        }
    }

    __set_PRIMASK(primask);
}


PsramRing_ReadResult_t PsramRing_ReadBlock(uint32_t block, void *dest, volatile uint8_t *done)
{
    PsramRing_ReadResult_t result;

    if(!running)
    {
        return PSRAM_RING_READ_INVALID;
    }

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t written = writtenBlocks;

    if((block >= written) || ((written - block) > (PSRAM_RING_BLOCKS - READ_GUARD)))
    {
        result = PSRAM_RING_READ_INVALID;
    }
    else if(readCount == READ_QUEUE)
    {
        result = PSRAM_RING_READ_BUSY;
    }
    else
    {
        ReadRequest_t *request = &readQueue[(readHead + readCount) % READ_QUEUE];

        request->block = block;
        request->dest = dest;
        request->done = done;
        *done = PSRAM_RING_PENDING;
        readCount++;

        if(!busy)
        {
            PsramRing_StartBurst();
        }

        result = PSRAM_RING_READ_QUEUED;
    }

    __set_PRIMASK(primask);

    return result;
}


__RAM_FUNC void PsramRing_IRQHandler(void)
{
    const uint32_t flags = rxDma->ISR >> rxFlagShift;

    if((flags & (DMA_ISR_TCIF1 | DMA_ISR_TEIF1)) == 0U)
    {
        return;
    }

    /* end the CE# cycle before anything else */
    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;
    PsramRing_StopDma();

    if((flags & DMA_ISR_TEIF1) != 0U)
    {
        PsramRing_Fault();
    }
    else if(burstKind == BURST_WRITE)
    {
        /* unless a newer block has taken over, move on to the next burst */
        if((writeSource != NULL) && (burstBlock == writeBlock))
        {
            writeBurst++;

            if(writeBurst == BURSTS_PER_BLOCK)
            {
                writeSource = NULL;
                writtenBlocks = writeBlock + 1U;
            }
        }
    }
    else
    {
        ReadRequest_t *request = &readQueue[readHead];

        memcpy(request->dest + (readBurst * BURST), &rxBuffer[READ_HEADER], BURST);
        readBurst++;

        if(readBurst == BURSTS_PER_BLOCK)
        {
            *request->done = PSRAM_RING_DONE;
            readBurst = 0U;
            readHead = (readHead + 1U) % READ_QUEUE;
            readCount--;
        }
    }

    PsramRing_StartBurst();
}
