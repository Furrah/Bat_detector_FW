/*
 * stats_table.h
 *
 * The timing and health counters the PC reads over the USB link
 * (python -I Tools/capture.py stats). Add a counter to the table in
 * stats_table.c and the PC picks it up by name.
 */

#ifndef STATS_TABLE_H_
#define STATS_TABLE_H_

#include "diag_stats.h"

extern const DiagStatTable_t statsTable;

#endif /* STATS_TABLE_H_ */
