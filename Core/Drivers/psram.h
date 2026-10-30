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
 * In the -O0 Debug build the polled byte loop takes ~80 cycles/byte
 * (~0.47 us: 32 on the wire, the rest stack loads/stores), so 8 data bytes
 * + a 5-byte read header is ~6.2 us, while 16 would be ~10 us. Must divide
 * the 1 KB page size. Check PSRAM_GetMaxSelectNs() after changing.
 */
#define PSRAM_MAX_BURST     8U

#define PSRAM_TCEM_NS       8000U


typedef enum
{
    PSRAM_OK = 0,
    PSRAM_ERR_ID,       /* Read ID did not return MF 0x0D / KGD 0x5D */
    PSRAM_ERR_DATA      /* a test read back the wrong value */
} PSRAM_Status_t;


/*
 * Reset the device and check its ID. Call once before any other access,
 * with SPI1 otherwise idle.
 */
PSRAM_Status_t PSRAM_Init(void);
void PSRAM_GetId(uint8_t *mfid, uint8_t *kgd);

void PSRAM_Write(uint32_t address, const uint8_t *data, uint32_t length);
void PSRAM_Read(uint32_t address, uint8_t *data, uint32_t length);

/*
 * Destructive tests. On PSRAM_ERR_DATA, failAddress holds the first bad
 * address (for the address-line test: the address line under test).
 */
PSRAM_Status_t PSRAM_TestAddressLines(uint32_t *failAddress);
PSRAM_Status_t PSRAM_TestFull(uint32_t *failAddress);

/*
 * Longest CE# low time seen since PSRAM_Init(), in ns. Must stay below
 * PSRAM_TCEM_NS or the PSRAM can't refresh while selected.
 */
uint32_t PSRAM_GetMaxSelectNs(void);


#endif /* DRIVERS_PSRAM_H_ */
