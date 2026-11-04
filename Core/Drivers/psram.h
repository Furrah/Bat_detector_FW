/*
 * psram.h
 *
 * APS6404L 8 MB SPI PSRAM on SPI1 (CE# on PB0). This only resets the chip
 * and checks its ID at boot; from then on the PSRAM ring (psram_ring.h)
 * owns SPI1 and does all reads and writes.
 */

#ifndef DRIVERS_PSRAM_H_
#define DRIVERS_PSRAM_H_

#include <stdint.h>

#define PSRAM_SIZE_BYTES    (8UL * 1024UL * 1024UL)

typedef enum
{
    PSRAM_OK = 0,
    PSRAM_ERR_ID,           /* no reply, or not an APS6404L */
    PSRAM_ERR_SPI           /* SPI1 stuck: a flag never came */
} PSRAM_Status_t;

/* Reset the chip and check its ID. Before PsramRing_Start, with SPI1 idle. */
PSRAM_Status_t PSRAM_Init(void);

#endif /* DRIVERS_PSRAM_H_ */
