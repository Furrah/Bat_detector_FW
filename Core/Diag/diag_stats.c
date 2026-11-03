/*
 * diag_stats.c
 *
 * Copies the current values of a stats table; see diag_stats.h.
 */

#include "diag_stats.h"

#include <string.h>


void Diag_StatsSnapshot(const DiagStat_t *stats, uint32_t count, uint32_t *values)
{
    for(uint32_t i = 0; i < count; i++)
    {
        const volatile void *value = stats[i].value;

        if(stats[i].type == 'b')
        {
            values[i] = *(const volatile uint8_t *)value;
        }
        else if(stats[i].type == 'f')
        {
            const float number = *(const volatile float *)value;

            memcpy(&values[i], &number, sizeof(values[i]));
        }
        else
        {
            values[i] = *(const volatile uint32_t *)value;
        }
    }
}
