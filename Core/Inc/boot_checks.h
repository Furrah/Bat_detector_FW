/*
 * boot_checks.h
 *
 * The power-up checks, run once before the audio starts. Results are shown
 * on the display, which must be up.
 *
 *   - SPI2 (display and SD card): set up the way the code expects.
 *   - PSRAM: reset and ID; if it answers, the PSRAM ring is started.
 *   - SD card: mount it and show its type and size.
 */

#ifndef BOOT_CHECKS_H_
#define BOOT_CHECKS_H_

#include <stdint.h>

void BootChecks_Spi2(void);

/* Returns 1 if the PSRAM answered and its ring is running */
uint8_t BootChecks_Psram(void);

void BootChecks_Sd(void);

#endif /* BOOT_CHECKS_H_ */
