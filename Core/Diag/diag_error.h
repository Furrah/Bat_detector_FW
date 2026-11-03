/*
 * diag_error.h
 *
 * Failure records: a count, and the details of the first failure since the
 * record was cleared, packed into one 32-bit word. Later failures are
 * usually knock-on effects of the first, so the first is the one worth
 * keeping. One word shows whole in a debugger's watch window and in a stats
 * readout (Tools/capture.py stats decodes them).
 *
 *     Diag_RecordFailure(&sdErrorCount, &sdLastError,
 *                        DIAG_PACK4(step, status, reply, command));
 *
 * Clear the details word to 0 to catch the next first failure.
 */

#ifndef DIAG_ERROR_H_
#define DIAG_ERROR_H_

#include <stdint.h>

#include "diag_port.h"


/* Four bytes into one word, the first in the low byte */
#define DIAG_PACK4(b0, b1, b2, b3) \
    ((uint32_t)(uint8_t)(b0) | ((uint32_t)(uint8_t)(b1) << 8) \
     | ((uint32_t)(uint8_t)(b2) << 16) | ((uint32_t)(uint8_t)(b3) << 24))

/* Count a failure; keep its details if it's the first since *first was 0.
 * Details must not be 0, which means "none". */
DIAG_INLINE void Diag_RecordFailure(volatile uint32_t *count, volatile uint32_t *first, uint32_t details)
{
    (*count)++;

    if(*first == 0U)
    {
        *first = details;
    }
}

#endif /* DIAG_ERROR_H_ */
