/*
 * stats_table.c
 *
 * The counters the PC reads ('N' names them, 'T' sends their values; see
 * Core/Diag/diag_stats.h). The names are the variables' own, as a debugger's
 * Live Expressions shows them. Tools/capture.py flags the ones that must
 * stay 0 and those with limits.
 */

#include "stats_table.h"

#include "main.h"
#include "debug_link.h"
#include "recorder.h"
#include "sd_spi.h"
#include "../DSP/autotune.h"
#include "../DSP/bat_detect.h"
#include "../DSP/fft.h"
#include "../DSP/playback.h"
#include "../Drivers/adc.h"
#include "../Drivers/dac.h"
#include "../Drivers/psram_ring.h"


static const DiagStat_t entries[] =
{
    DIAG_STAT_U32(SystemCoreClock),
    { "blockCount",             'u', &debugBlockCount },        /* audio blocks since reset */

    /* Audio interrupt (adc.c): worst-case DSP_ProcessBlock */
    DIAG_STAT_U32(dspCyclesMax),
    DIAG_STAT_U32(dspLoadMaxPercent),
    DIAG_STAT_U32(dacLateBlocks),

    /* Main loop: FFT_Process */
    DIAG_STAT_U32(fftProcessCyclesAvg),
    DIAG_STAT_U32(fftProcessCyclesMax),
    DIAG_STAT_U32(fftDroppedBlocks),

    /* PSRAM ring */
    DIAG_STAT_U32(psramRingBlocks),
    DIAG_STAT_U32(psramRingReads),
    DIAG_STAT_U32(psramRingBursts),
    DIAG_STAT_U32(psramRingMaxSelectNs),
    DIAG_STAT_U32(psramRingMinSelectNs),
    DIAG_STAT_U32(psramRingMaxEntryNs),
    DIAG_STAT_U32(psramRingMaxIsrNs),
    DIAG_STAT_U32(psramRingOverTcem),
    DIAG_STAT_U32(psramRingLateBlocks),
    DIAG_STAT_U32(psramRingFaults),

    /* Playback */
    DIAG_STAT_U32(playbackUnderruns),

    /* SD card driver (sd_spi.h) */
    DIAG_STAT_U32(sdErrorCount),
    DIAG_STAT_U32(sdLastError),
    DIAG_STAT_U32(sdLastCardStatus),
    DIAG_STAT_U32(sdLateResponse),
    DIAG_STAT_U32(sdBusyMaxUs),
    DIAG_STAT_U32(sdHalFailures),
    DIAG_STAT_U32(sdHalFailure),
    DIAG_STAT_U32(sdStaleRx),
    DIAG_STAT_U32(sdStaleRxFirst),
    DIAG_STAT_U32(sdRecoveries),
    DIAG_STAT_U32(sdRecoveryFailures),

    /* WAV recorder (recorder.h); all but recFile are for the current or last recording */
    DIAG_STAT_U32(recFile),
    DIAG_STAT_U32(recBlocks),
    DIAG_STAT_U32(recMaxBacklog),
    DIAG_STAT_U32(recMaxWriteUs),
    DIAG_STAT_U32(recMaxSyncUs),
    DIAG_STAT_U32(recReadFailures),
    DIAG_STAT_U32(recLastEnd),

    /* Auto-tune (autotune.h) */
    DIAG_STAT_U32(autoTuneCalls),
    DIAG_STAT_U32(autoTuneRetunes),
    DIAG_STAT_U32(autoTuneLastHz),

    /* Bat detector */
    DIAG_STAT_F32(batEnergyDb),
    DIAG_STAT_F32(batFloorDb),
    DIAG_STAT_U32(batPulses),
    DIAG_STAT_U32(batTriggers),
    DIAG_STAT_U32(batNoiseRuns),
    DIAG_STAT_U8(batPresent),
};

static uint32_t values[DIAG_STAT_COUNT(entries)];      /* the 'T' payload */


/* 'Z': restart the worst-case timings, so the next reading covers only what follows */
static void Stats_ResetTimings(void)
{
    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    /* the audio interrupt updates these two together */
    dspCyclesMax = 0U;
    dspLoadMaxPercent = 0U;

    __set_PRIMASK(primask);

    fftProcessCyclesMax = 0U;       /* main loop, like this */
    sdBusyMaxUs = 0U;
    psramRingResetStats = 1U;       /* the ring's interrupt clears its own at the end of the next burst */
}


const DiagStatTable_t statsTable =
{
    entries,
    DIAG_STAT_COUNT(entries),
    values,
    Stats_ResetTimings
};
