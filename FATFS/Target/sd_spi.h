/*
 * sd_spi.h
 *
 * SD card in SPI mode on SPI2, shared with the display (SD CS on PB2).
 * Blocking; FatFS uses it through user_diskio.c. Main loop only, so it
 * takes turns with the display.
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

/* A failed read or write re-initialises the card and tries once more */
SD_Status_t SD_ReadBlocks(uint8_t *buffer, uint32_t sector, uint32_t count);
SD_Status_t SD_WriteBlocks(const uint8_t *buffer, uint32_t sector, uint32_t count);

/* Wait until the card has finished programming the last write */
SD_Status_t SD_Sync(void);

SD_CardType_t SD_GetCardType(void);
uint32_t SD_GetSectorCount(void);

/* Result of the last SD_Init(), e.g. to tell "no card" from a failed init */
SD_Status_t SD_GetInitStatus(void);

#endif /* SD_SPI_H_ */
