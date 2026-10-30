/*
 * psram.h
 *
 * APS6404L 8 MB SPI PSRAM on SPI1 (CE# on PB0).
 * Blocking bring-up driver: every access is a polled, interrupt-masked burst.
 */

#ifndef DRIVERS_PSRAM_H_
#define DRIVERS_PSRAM_H_

#include <stdint.h>


#define PSRAM_SIZE_BYTES    (8UL * 1024UL * 1024UL)    /* 64 Mbit */

/*
 * Max data bytes per CE# cycle. CE# may stay low for at most tCEM (8 us).
 * The polled byte loop is CPU-bound: an 8-byte read (+ 5-byte header)
 * measured 4.1 us CE# low at -Og and 5.8 us at -O0. 16 bytes would fit at
 * -Og (~6.7 us) but not at -O0 (~10 us), so 8 is safe in either build.
 * Must divide the 1 KB page size. Check PSRAM_GetMaxSelectNs() after
 * changing.
 */
#define PSRAM_MAX_BURST     8U

#define PSRAM_TCEM_NS       8000U


typedef enum
{
    PSRAM_OK = 0,
    PSRAM_ERR_ID,       /* Read ID did not return MF 0x0D / KGD 0x5D */
    PSRAM_ERR_DATA,     /* a test read back the wrong value */
    PSRAM_ERR_TIMEOUT   /* an SPI flag never set; see PSRAM_GetTimeoutStatus() */
} PSRAM_Status_t;


/*
 * Reset the device and check its ID. Call once before any other access,
 * with SPI1 otherwise idle.
 */
PSRAM_Status_t PSRAM_Init(void);
void PSRAM_GetId(uint8_t *mfid, uint8_t *kgd);

PSRAM_Status_t PSRAM_Write(uint32_t address, const uint8_t *data, uint32_t length);
PSRAM_Status_t PSRAM_Read(uint32_t address, uint8_t *data, uint32_t length);

/*
 * Destructive tests. On failure, failAddress holds the first bad address
 * (for the address-line test: the address line under test).
 */
PSRAM_Status_t PSRAM_TestAddressLines(uint32_t *failAddress);
PSRAM_Status_t PSRAM_TestFull(uint32_t *failAddress);

/*
 * Longest CE# low time seen since PSRAM_Init(), in ns. Must stay below
 * PSRAM_TCEM_NS or the PSRAM can't refresh while selected.
 */
uint32_t PSRAM_GetMaxSelectNs(void);

/* SPI1 SR captured when the last PSRAM_ERR_TIMEOUT happened */
uint32_t PSRAM_GetTimeoutStatus(void);


#endif /* DRIVERS_PSRAM_H_ */
