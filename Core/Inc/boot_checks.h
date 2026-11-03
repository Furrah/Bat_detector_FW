/*
 * boot_checks.h
 *
 * The power-up checks, run once before the audio starts, while the buses
 * are otherwise idle. Results are shown on the display, which must be up.
 *
 *   - SPI2 (display and SD card) set-up.
 *   - PSRAM: ID and address lines (hold RECORD at power-up for the full
 *     8 MB pattern test, ~35 s); if it passes, the PSRAM ring is started.
 *   - SD card: mount and show its type and size (hold PLAYBACK at power-up
 *     for a 256 KB write/read speed test, which leaves SDTEST.BIN).
 */

#ifndef BOOT_CHECKS_H_
#define BOOT_CHECKS_H_

#include <stdint.h>

void BootChecks_Spi2(void);

/* Returns 1 if the PSRAM passed and its ring is running */
uint8_t BootChecks_Psram(void);

void BootChecks_Sd(void);

#endif /* BOOT_CHECKS_H_ */
