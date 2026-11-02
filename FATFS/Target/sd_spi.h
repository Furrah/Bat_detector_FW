/*
 * sd_spi.h
 *
 * SD card in SPI mode on SPI2, shared with the ILI9341 display (CS on PB2).
 * Blocking driver used by FatFS through user_diskio.c. Call only from
 * main-loop context, never from an ISR, so it serialises with the display.
 */

#ifndef SD_SPI_H_
#define SD_SPI_H_

#include <stdint.h>


#define SD_SECTOR_SIZE  512U


typedef enum
{
    SD_CARD_NONE = 0,
    SD_CARD_V1,         /* SD v1.x, byte addressed */
    SD_CARD_V2_SC,      /* SD v2 standard capacity, byte addressed */
    SD_CARD_V2_HC       /* SDHC/SDXC, block addressed */
} SD_CardType_t;

typedef enum
{
    SD_OK = 0,
    SD_ERR_NO_CARD,     /* no answer to CMD0: card missing */
    SD_ERR_INIT,        /* card answered, but initialisation failed */
    SD_ERR_TIMEOUT,     /* card stayed busy or never sent its data */
    SD_ERR_CMD,         /* command rejected */
    SD_ERR_DATA,        /* data block rejected or bad token */
    SD_ERR_SPI          /* HAL SPI transfer failed */
} SD_Status_t;


SD_Status_t SD_Init(void);

/* A failed read or write re-initialises the card and tries once more
 * (sdRecoveries counts these, sdRecoveryFailures the restarts that failed) */
SD_Status_t SD_ReadBlocks(uint8_t *buffer, uint32_t sector, uint32_t count);
SD_Status_t SD_WriteBlocks(const uint8_t *buffer, uint32_t sector, uint32_t count);

/* Wait until the card has finished programming the last write */
SD_Status_t SD_Sync(void);

SD_CardType_t SD_GetCardType(void);
uint32_t SD_GetSectorCount(void);

/* Result of the last SD_Init(), e.g. to tell "no card" from a failed init */
SD_Status_t SD_GetInitStatus(void);


/*
 * Diagnostics: where the last failed read or write went wrong, for the
 * debug link's stats (Tools/capture.py decodes them).
 *
 * sdLastError, sdLastCardStatus and sdLateResponse keep the first failure
 * since they were cleared (the SD test clears them; otherwise since boot).
 *
 *   sdLastError       step | status << 8 | reply << 16 | command << 24, where
 *                     the failed read or write first went wrong. reply is
 *                     the card's byte (R1, data token or data response) or,
 *                     for an SPI step, the low byte of HAL's error code.
 *   sdLastCardStatus  0x10000 | R1 << 8 | R2 from CMD13 (SEND_STATUS) right
 *                     after a failed write; 0x1FFFF if the card didn't
 *                     answer; 0 if not read
 *   sdLateResponse    after a write's data response read as 0xFF: how many
 *                     more bytes were clocked before the card answered
 *                     (low 16 bits) and its first byte (bits 16-23); 0xFFFF
 *                     if it never answered in 1024 bytes; 0 if not probed
 *   sdBusyMaxUs       longest wait for the card to stop being busy
 *   sdUseDma          1: 512-byte blocks by DMA, 0: polled (the default; DMA
 *                     writes occasionally lose a block's start token, diary 40)
 */
typedef enum
{
    SD_STEP_NONE = 0,
    SD_STEP_SELECT,             /* still busy when selected for a command */
    SD_STEP_COMMAND,            /* no or bad R1 */
    SD_STEP_READ_TOKEN,         /* no or bad data token */
    SD_STEP_READ_DATA,          /* SPI/DMA failure receiving a block */
    SD_STEP_WRITE_BUSY,         /* still busy before the next block or the stop token */
    SD_STEP_WRITE_DATA,         /* SPI/DMA failure sending a block */
    SD_STEP_WRITE_REJECTED      /* data response wasn't "accepted" */
} SD_Step_t;

/*
 * Driver self-checks, in each byte exchange:
 *
 *   sdHalFailures / sdHalFailure  1-byte HAL transfers that failed (the
 *                     byte is dropped). First: HAL status | State << 4 |
 *                     ErrorCode low byte << 8 | Lock << 16 | phase << 17 |
 *                     command << 24
 *   sdStaleRx / sdStaleRxFirst    times SPI2's receive FIFO held leftover
 *                     bytes or an overrun before an exchange (flushed).
 *                     First: SPI2 SR | phase << 16 | command << 24
 *
 * The phase is the protocol step the next byte belongs to.
 */
typedef enum
{
    SD_PHASE_IDLE = 0,
    SD_PHASE_DESELECT,          /* the clock after CS goes high */
    SD_PHASE_SELECT,            /* dummy byte and busy wait after CS goes low */
    SD_PHASE_COMMAND,           /* the 6-byte command frame */
    SD_PHASE_R1,                /* waiting for R1 */
    SD_PHASE_WRITE_WAIT,        /* busy wait before a block's token */
    SD_PHASE_WRITE_TOKEN,       /* a write's start (or stop) token */
    SD_PHASE_WRITE_END,         /* CRC and data response after a block */
    SD_PHASE_READ_TOKEN,        /* waiting for a read's data token */
    SD_PHASE_READ_END,          /* CRC after a read block */
    SD_PHASE_PROBE              /* the late-response probe */
} SD_Phase_t;

extern volatile uint32_t sdHalFailures;
extern volatile uint32_t sdHalFailure;
extern volatile uint32_t sdStaleRx;
extern volatile uint32_t sdStaleRxFirst;

extern volatile uint32_t sdErrorCount;
extern volatile uint32_t sdLastError;
extern volatile uint32_t sdLastCardStatus;
extern volatile uint32_t sdLateResponse;
extern volatile uint32_t sdBusyMaxUs;
extern volatile uint8_t  sdUseDma;
extern volatile uint32_t sdRecoveries;
extern volatile uint32_t sdRecoveryFailures;


#endif /* SD_SPI_H_ */
