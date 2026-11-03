/*
 * psram_ring.c
 *
 * PSRAM ring buffer engine; see psram_ring.h.
 *
 * Each burst is one CE# cycle, moved entirely by DMA:
 *   write  0x02 + 24-bit address + 16 data bytes             20 bytes, 3.8 us at 42.5 MHz
 *   read   0x0B + 24-bit address + 1 wait byte + 16 bytes    21 bytes, 4.0 us
 * The TX channel feeds SPI1 from a staging buffer and the RX channel drains
 * every byte clocked in. RX transfer-complete marks the end of the burst:
 * the interrupt raises CE# first, then starts the next burst. CE# low time
 * is measured on every burst (psramRingMaxSelectNs).
 *
 * On top of the wire time come ~0.5 us of DMA start, RX drain and interrupt
 * entry, and ~0.24 us in the interrupt up to CE# high (measured with 32-byte
 * bursts). It needs SPI1's NSS pulse mode off (see PsramRing_Start): with
 * it on, 32-byte bursts took 9.1 us.
 *
 * A block is 32 write bursts, copied from the ADC buffer half the DSP has
 * just been given. The ADC fills the other half meanwhile, so the writes
 * have one block period (1.024 ms) and need roughly 250 us. Writes go
 * before reads.
 *
 * The burst interrupt's code (marked __RAM_FUNC) runs from RAM. From flash
 * it cost ~4 us per burst: the DSP evicts it from the 1 KB instruction
 * cache between bursts, so it ran cold at 4 wait states every time.
 */


#include "psram_ring.h"

#include <string.h>

#include "main.h"


/*
 * This file builds at -O2 whatever the project setting (-Og in Debug). The
 * burst interrupt runs 32+ times per block; at -Og its helpers aren't
 * inlined, and it took 3.05 us per burst even from RAM. At -O2: 2.3 us.
 */
#pragma GCC optimize ("O2")


extern SPI_HandleTypeDef hspi1;
extern DMA_HandleTypeDef hdma_spi1_rx;
extern DMA_HandleTypeDef hdma_spi1_tx;


/*
 * Data bytes per burst. 32 measured 7.5 us for writes, but reads (a byte
 * longer) reached 8.1 us when a masked section delayed the interrupt. 16
 * measures 4.44 us at most, reads included: 3.5 us clear of the 8 us tCEM,
 * at twice the interrupts.
 */
#define PSRAM_RING_BURST            16U
#define PSRAM_RING_BURSTS_PER_BLOCK (PSRAM_RING_BLOCK_BYTES / PSRAM_RING_BURST)

#define PSRAM_RING_WRITE_HEADER     4U      /* command + 24-bit address */
#define PSRAM_RING_READ_HEADER      5U      /* command + address + 8 wait cycles */

#define PSRAM_CMD_WRITE             0x02U
#define PSRAM_CMD_FAST_READ         0x0BU   /* plain read 0x03 is limited to 33 MHz */

#define PSRAM_PAGE_BYTES            1024U

/* The burst-complete interrupt, and the last interrupt number on the G431 */
#define PSRAM_RING_IRQn             DMA2_Channel1_IRQn
#define PSRAM_RING_LAST_IRQn        CORDIC_IRQn

/* 16 so the SD writer can queue a whole multi-sector chunk while the last
 * one is written (recorder.c). 12 bytes each. */
#define PSRAM_RING_READ_QUEUE       16U

/* Reads keep this many blocks (65 ms) clear of the writer, so it can't lap them */
#define PSRAM_RING_READ_GUARD       64U

_Static_assert((PSRAM_RING_BLOCK_BYTES % PSRAM_RING_BURST) == 0U, "bursts must tile a block");
_Static_assert((PSRAM_PAGE_BYTES % PSRAM_RING_BURST) == 0U, "a burst must not cross a 1 KB PSRAM page");
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


/* Debug view: watch these in Live Expressions */
volatile uint32_t psramRingBlocks = 0;          /* blocks completely written */
volatile uint32_t psramRingMaxSelectNs = 0;     /* longest CE# low time; must stay below 8000 */
volatile uint32_t psramRingLateBlocks = 0;      /* blocks still being written when the next arrived */
volatile uint32_t psramRingFaults = 0;          /* failed or stuck bursts, recovered */
volatile uint32_t psramRingReads = 0;           /* blocks read back */

/*
 * Timing diagnostics. CE# low splits into "entry" (CE# low until the
 * interrupt starts: the transfer plus interrupt latency) and the time inside
 * the interrupt up to CE# high. Write 1 to psramRingResetStats to start over.
 */
volatile uint32_t psramRingMinSelectNs = 0;     /* shortest CE# low time */
volatile uint32_t psramRingMaxEntryNs = 0;      /* worst CE# low to interrupt start */
volatile uint32_t psramRingMaxIsrNs = 0;        /* worst time inside the interrupt: CPU cost per burst */
volatile uint32_t psramRingOverTcem = 0;        /* bursts with CE# low longer than tCEM */
volatile uint32_t psramRingBursts = 0;          /* bursts timed */
volatile uint32_t psramRingResetStats = 0;

static volatile uint8_t  running = 0;
static volatile uint8_t  paused = 0;            /* not recording; reads still run */
static volatile uint8_t  busy = 0;              /* a burst is in flight */
static volatile uint32_t pushedBlocks = 0;
static volatile uint32_t resumeBlock = 0;       /* first block recorded since the last pause ended */
static volatile uint32_t blockTicks = 0;        /* PushBlock calls, paused or not: the stuck-burst clock */

/* The block being written. writeSource is NULL when there's none. */
static const uint8_t * volatile writeSource = NULL;
static uint32_t writeBlock;
static uint32_t writeBurst;

static ReadRequest_t readQueue[PSRAM_RING_READ_QUEUE];
static volatile uint32_t readHead = 0;
static volatile uint32_t readCount = 0;
static uint32_t readBurst = 0;

/* The burst in flight */
static BurstKind_t burstKind;
static uint32_t burstBlock;
static uint32_t burstStart;                     /* DWT cycle count at CE# low */
static uint32_t burstStartedAt;                 /* blockTicks when it started */

static uint32_t maxSelectCycles = 0;
static uint32_t minSelectCycles = UINT32_MAX;
static uint32_t maxEntryCycles = 0;
static uint32_t maxIsrCycles = 0;
static uint32_t tcemCycles;

static uint8_t txWrite[PSRAM_RING_WRITE_HEADER + PSRAM_RING_BURST];
static uint8_t txRead[PSRAM_RING_READ_HEADER + PSRAM_RING_BURST];      /* zeros after the header */
static uint8_t rxBuffer[PSRAM_RING_READ_HEADER + PSRAM_RING_BURST];

static DMA_Channel_TypeDef *rxChannel;
static DMA_Channel_TypeDef *txChannel;
static DMA_TypeDef *rxDma;
static DMA_TypeDef *txDma;
static uint32_t rxFlagShift;
static uint32_t txFlagShift;

/* SPI1 settings to restore after a fault resets it */
static uint32_t spiCr1;
static uint32_t spiCr2;



static __RAM_FUNC uint32_t PsramRing_CyclesToNs(uint32_t cycles)
{
    return (cycles * 1000U) / (SystemCoreClock / 1000000U);
}


/* Called from the interrupt once CE# is high, so none of this counts against tCEM */
static __RAM_FUNC void PsramRing_RecordTiming(uint32_t selectCycles, uint32_t entryCycles)
{
    if(psramRingResetStats != 0U)
    {
        maxSelectCycles = 0U;
        minSelectCycles = UINT32_MAX;
        maxEntryCycles = 0U;
        maxIsrCycles = 0U;
        psramRingMaxSelectNs = 0U;
        psramRingMinSelectNs = 0U;
        psramRingMaxEntryNs = 0U;
        psramRingMaxIsrNs = 0U;
        psramRingOverTcem = 0U;
        psramRingBursts = 0U;
        psramRingResetStats = 0U;
    }

    psramRingBursts++;

    if(selectCycles > tcemCycles)
    {
        psramRingOverTcem++;
    }

    if(selectCycles > maxSelectCycles)
    {
        maxSelectCycles = selectCycles;
        psramRingMaxSelectNs = PsramRing_CyclesToNs(selectCycles);
    }

    if(selectCycles < minSelectCycles)
    {
        minSelectCycles = selectCycles;
        psramRingMinSelectNs = PsramRing_CyclesToNs(selectCycles);
    }

    if(entryCycles > maxEntryCycles)
    {
        maxEntryCycles = entryCycles;
        psramRingMaxEntryNs = PsramRing_CyclesToNs(entryCycles);
    }
}


static __RAM_FUNC uint32_t PsramRing_BlockAddress(uint32_t block)
{
    return (block % PSRAM_RING_BLOCKS) * PSRAM_RING_BLOCK_BYTES;
}


static __RAM_FUNC void PsramRing_PutHeader(uint8_t *header, uint8_t command, uint32_t address)
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
        const uint32_t offset = writeBurst * PSRAM_RING_BURST;

        PsramRing_PutHeader(txWrite, PSRAM_CMD_WRITE, PsramRing_BlockAddress(writeBlock) + offset);
        memcpy(&txWrite[PSRAM_RING_WRITE_HEADER], writeSource + offset, PSRAM_RING_BURST);

        tx = txWrite;
        length = sizeof(txWrite);
        burstKind = BURST_WRITE;
        burstBlock = writeBlock;
    }
    else if(readCount > 0U)
    {
        const ReadRequest_t *request = &readQueue[readHead];

        /* txRead[4] stays 0: the 8 wait cycles */
        PsramRing_PutHeader(txRead, PSRAM_CMD_FAST_READ,
                PsramRing_BlockAddress(request->block) + (readBurst * PSRAM_RING_BURST));

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

    SPI1_PSRAM_CS_GPIO_Port->BRR = SPI1_PSRAM_CS_Pin;
    burstStart = DWT->CYCCNT;

    txChannel->CCR |= DMA_CCR_EN;       /* the burst starts here */
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
        readHead = (readHead + 1U) % PSRAM_RING_READ_QUEUE;
        readCount--;
    }

    readBurst = 0U;
    busy = 0U;
    psramRingFaults++;
}



PsramRing_Status_t PsramRing_Start(void)
{
    SPI_TypeDef *spi = hspi1.Instance;

    /* The burst timing above assumes 8-bit frames at 42.5 MHz (/4), and the
     * interrupt handled is DMA2 Channel 1's. NSS pulse mode (NSSP, CubeMX's
     * default) idles SCK for 2 clocks after every byte to pulse the NSS pin,
     * even with software NSS: 9.1 us bursts, over tCEM. */
    if((hdma_spi1_rx.Instance != DMA2_Channel1) || (hdma_spi1_tx.Instance != DMA2_Channel2)
            || ((spi->CR1 & SPI_CR1_BR) != SPI_BAUDRATEPRESCALER_4)
            || ((spi->CR2 & SPI_CR2_DS) != SPI_DATASIZE_8BIT)
            || ((spi->CR2 & SPI_CR2_FRXTH) == 0U)
            || ((spi->CR2 & SPI_CR2_NSSP) != 0U))
    {
        return PSRAM_RING_ERR_CONFIG;
    }

    /* The TX channel never interrupts; CubeMX enables its vector anyway */
    HAL_NVIC_DisableIRQ(DMA2_Channel2_IRQn);

    /* The burst interrupt must be able to preempt everything else */
    if(NVIC_GetPriority(PSRAM_RING_IRQn) != 0U)
    {
        return PSRAM_RING_ERR_PRIORITY;
    }

    for(int32_t irq = 0; irq <= (int32_t)PSRAM_RING_LAST_IRQn; irq++)
    {
        if((irq != (int32_t)PSRAM_RING_IRQn)
                && (NVIC_GetEnableIRQ((IRQn_Type)irq) != 0U)
                && (NVIC_GetPriority((IRQn_Type)irq) == 0U))
        {
            return PSRAM_RING_ERR_PRIORITY;
        }
    }

    tcemCycles = (PSRAM_TCEM_NS * (SystemCoreClock / 1000000U)) / 1000U;

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

    /* drain anything the polled driver left, then hand the SPI to the DMA */
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
    /* pushedBlocks doesn't move while paused, so this is the next block to
     * be recorded; set before recording restarts */
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
    return psramRingBlocks;
}


void PsramRing_PushBlock(const uint16_t *block)
{
    uint32_t primask;

    if(!running)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    /* A burst takes ~5 us; one started before the previous block arrived is
     * stuck. Checked while paused too, so a stuck read is still recovered. */
    if(busy && (burstStartedAt != blockTicks))
    {
        PsramRing_Fault();
    }

    blockTicks++;

    if(!paused)
    {
        if(writeSource != NULL)
        {
            psramRingLateBlocks++;      /* the rest of the previous block is lost */
        }

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
    uint32_t primask;
    uint32_t written;

    if(!running)
    {
        return PSRAM_RING_READ_INVALID;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    written = psramRingBlocks;

    if((block >= written) || ((written - block) > (PSRAM_RING_BLOCKS - PSRAM_RING_READ_GUARD)))
    {
        result = PSRAM_RING_READ_INVALID;
    }
    else if(readCount == PSRAM_RING_READ_QUEUE)
    {
        result = PSRAM_RING_READ_BUSY;
    }
    else
    {
        ReadRequest_t *request = &readQueue[(readHead + readCount) % PSRAM_RING_READ_QUEUE];

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
    const uint32_t entry = DWT->CYCCNT;
    const uint32_t flags = rxDma->ISR >> rxFlagShift;
    uint32_t cycles;
    uint32_t isrCycles;

    if((flags & (DMA_ISR_TCIF1 | DMA_ISR_TEIF1)) == 0U)
    {
        return;
    }

    /* end the CE# cycle before anything else */
    SPI1_PSRAM_CS_GPIO_Port->BSRR = SPI1_PSRAM_CS_Pin;
    cycles = DWT->CYCCNT - burstStart;

    PsramRing_StopDma();
    PsramRing_RecordTiming(cycles, entry - burstStart);

    if((flags & DMA_ISR_TEIF1) != 0U)
    {
        PsramRing_Fault();
    }
    else if(burstKind == BURST_WRITE)
    {
        /* unless a newer block has taken over (late), move on to the next burst */
        if((writeSource != NULL) && (burstBlock == writeBlock))
        {
            writeBurst++;

            if(writeBurst == PSRAM_RING_BURSTS_PER_BLOCK)
            {
                writeSource = NULL;
                psramRingBlocks = writeBlock + 1U;
            }
        }
    }
    else
    {
        ReadRequest_t *request = &readQueue[readHead];

        memcpy(request->dest + (readBurst * PSRAM_RING_BURST),
                &rxBuffer[PSRAM_RING_READ_HEADER], PSRAM_RING_BURST);
        readBurst++;

        if(readBurst == PSRAM_RING_BURSTS_PER_BLOCK)
        {
            *request->done = PSRAM_RING_DONE;
            psramRingReads++;
            readBurst = 0U;
            readHead = (readHead + 1U) % PSRAM_RING_READ_QUEUE;
            readCount--;
        }
    }

    PsramRing_StartBurst();

    isrCycles = DWT->CYCCNT - entry;

    if(isrCycles > maxIsrCycles)
    {
        maxIsrCycles = isrCycles;
        psramRingMaxIsrNs = PsramRing_CyclesToNs(isrCycles);
    }
}
