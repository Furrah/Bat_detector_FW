/*
 * host_main.c - runs the firmware's classifier (Core/DSP/classifier_calls.c,
 * classifier_species.c) on the PC, for Tools/check_classifier_c.py.
 *
 *     classifier_host <codes.u16>
 *
 * Reads raw 12-bit ADC codes (uint16, little-endian), feeds them through
 * in hops as the board does, and prints the board's call-log CSV lines.
 */

#include <stdio.h>
#include <stdlib.h>

#include "classifier_calls.h"
#include "classifier_species.h"

static void Emit(ClsCall_t *call)
{
    ClsVerdict_t verdict;
    char line[160];

    if(ClsSpecies_AddCall(call, &verdict) && ClsSpecies_FormatVerdict(&verdict, 0U, line, sizeof(line)))
    {
        fputs(line, stdout);
    }
    if(ClsSpecies_FormatCall(call, 0U, line, sizeof(line)))
    {
        fputs(line, stdout);
    }
}

int main(int argc, char **argv)
{
    uint16_t hop[CLS_HOP];
    ClsCall_t call;
    ClsVerdict_t verdict;
    char line[160];
    FILE *in;

    if(argc != 2)
    {
        fprintf(stderr, "usage: classifier_host <codes.u16>\n");
        return 2;
    }
    in = fopen(argv[1], "rb");
    if(in == NULL)
    {
        perror(argv[1]);
        return 1;
    }

    ClsCalls_Reset();
    ClsSpecies_Reset();
    fputs(clsCsvHeader, stdout);

    while(fread(hop, sizeof(uint16_t), CLS_HOP, in) == CLS_HOP)
    {
        if(ClsCalls_ProcessHop(hop, &call))
        {
            Emit(&call);
        }
    }
    fclose(in);

    if(ClsCalls_Flush(&call))
    {
        Emit(&call);
    }
    if(ClsSpecies_Flush(&verdict) && ClsSpecies_FormatVerdict(&verdict, 0U, line, sizeof(line)))
    {
        fputs(line, stdout);
    }
    return 0;
}
