/*
 * diag_stats.h
 *
 * A table of named counters for a PC to read over the link (diag_link.h),
 * like a debugger's watch window without the debugger. The table lives in
 * flash. Each entry is 32 bytes on the wire: a name of up to 27 characters
 * (NUL-padded), a type ('u' uint32_t, 'b' uint8_t, 'f' float), then the
 * variable's address, which the PC ignores.
 *
 *     static const DiagStat_t stats[] =
 *     {
 *         DIAG_STAT_U32(dspCyclesMax),
 *         DIAG_STAT_F32(batFloorDb),
 *     };
 *
 * The name is the variable's own, so it matches what a debugger shows.
 */

#ifndef DIAG_STATS_H_
#define DIAG_STATS_H_

#include <stdint.h>


typedef struct
{
    char name[27];
    char type;                      /* 'u' uint32_t, 'b' uint8_t, 'f' float */
    const volatile void *value;
} DiagStat_t;

_Static_assert(sizeof(DiagStat_t) == 32U, "a stats entry is 32 bytes on the wire");

#define DIAG_STAT_U32(var)      { #var, 'u', &(var) }
#define DIAG_STAT_U8(var)       { #var, 'b', &(var) }
#define DIAG_STAT_F32(var)      { #var, 'f', &(var) }

#define DIAG_STAT_COUNT(entries)    (sizeof(entries) / sizeof((entries)[0]))

/* A table, as the link serves it ('N', 'T', 'Z'). Define it where the
 * entries are, so their count is known:
 *
 *     static uint32_t values[DIAG_STAT_COUNT(entries)];
 *     const DiagStatTable_t myStats = { entries, DIAG_STAT_COUNT(entries), values, ResetTimings };
 */
typedef struct
{
    const DiagStat_t *entries;
    uint32_t count;
    uint32_t *values;               /* count words: the 'T' payload */
    void (*resetTimings)(void);     /* 'Z': restart the worst-case timings; may be NULL */
} DiagStatTable_t;

/* The current values, one 32-bit word each (uint8_t widened, floats as their bits) */
void Diag_StatsSnapshot(const DiagStat_t *stats, uint32_t count, uint32_t *values);

#endif /* DIAG_STATS_H_ */
