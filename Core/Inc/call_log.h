/*
 * call_log.h
 *
 * The classifier's log: CALLS.CSV in the SD card's root, a line per pass it
 * reports, and with CALL_LOG_CALLS a line per call (format:
 * classifier_species.h). The per-call lines are the record to check the
 * board against the PC model, and the data to refit the rules to in spring.
 *
 * Lines are kept in a 512-byte buffer and appended a buffer at a time. Each
 * write mounts the card, appends and closes the file, so nothing is left
 * open when the recorder takes the card. Only the classifier writes, and
 * only while no recording is open, so the two never meet.
 */

#ifndef CALL_LOG_H_
#define CALL_LOG_H_

#include <stddef.h>
#include <stdint.h>

/* On by default (decided Nov 4). 0 leaves the log out. */
#define CALL_LOG_ENABLED        1

/* What goes in it. By default, one line per pass the classifier reports as a
 * bat: the verdict the header line showed (~40 B). Detections it doesn't
 * report (fewer than 3 calls, echoes, rising sounds) never reach the card.
 * 1 also logs every call (~100 B each): for checking the board against the
 * PC model (Tools/check_classifier_c.py --device), not for everyday use, as
 * it grew by ~50 KB in 3 minutes of indoor noise (Nov 4). */
#define CALL_LOG_CALLS          0

/* A line, ending in "\r\n" (length 0 is ignored) */
void CallLog_Add(const char *line, size_t length);

/* Write out what's buffered (end of a pass) */
void CallLog_Flush(void);

/* Buffers (up to 512 B of lines each) lost: no card, the card full, or a write failed */
extern volatile uint32_t callLogDropped;

#endif /* CALL_LOG_H_ */
