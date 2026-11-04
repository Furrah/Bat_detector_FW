/*
 * classifier.c
 *
 * The classifier as a reader of the PSRAM ring; see classifier.h.
 *
 * A session runs from 2 s before the detector's pass began until the pass
 * is over and the classifier has caught up with the ring. Its frame 0 is
 * the first sample of its first block, so a call's ring position is
 * sessionBlock * 4 + its frame (4 hops of 64 samples per block).
 *
 * Blocks are read one at a time into two slots, as playback does: one is
 * processed while the next loads.
 */

#include "classifier.h"

#include "main.h"
#include "bat_detect.h"
#include "call_log.h"
#include "classifier_calls.h"
#include "classifier_species.h"
#include "playback.h"
#include "recorder.h"
#include "../Drivers/psram_ring.h"


#define CLS_PRE_BLOCKS          (2000000UL / AUDIO_BLOCK_US)        /* 2 s before the pass: 1953 */
#define CLS_MAX_BEHIND_BLOCKS   (12000000UL / AUDIO_BLOCK_US)       /* 12 s: then skip ahead */
#define CLS_BLOCKS_PER_POLL     4U          /* ~0.5 ms of main-loop time per poll */
#define CLS_HOPS_PER_BLOCK      (AUDIO_BLOCK_SIZE / CLS_HOP)        /* 4 */
#define CLS_NO_BLOCK            UINT32_MAX

_Static_assert(CLS_MAX_BEHIND_BLOCKS < (PSRAM_RING_BLOCKS - 1024U), "stay well clear of the ring's writer");


volatile uint32_t classifierBlockCyclesMax = 0;
volatile uint32_t classifierSkips = 0;

static uint8_t  running;
static uint32_t sessionBlock;               /* ring block of frame 0 */
static uint32_t nextBlock;                  /* next block to process */
static uint32_t requestBlock;               /* next block to ask the ring for */

static uint16_t slots[2][AUDIO_BLOCK_SIZE]; /* block n is read into slot n % 2 */
static volatile uint8_t slotDone[2] = { PSRAM_RING_DONE, PSRAM_RING_DONE };
static uint32_t slotBlock[2] = { CLS_NO_BLOCK, CLS_NO_BLOCK };

static uint8_t  lastPresent;
static uint8_t  passPending;                /* the detector started a pass we haven't begun on */
static uint32_t passStartBlock;

static char     verdictText[14];
static uint8_t  verdictNew;


/* A sequence has finished: if it's a bat, show it and log it */
static void Classifier_Verdict(const ClsVerdict_t *v)
{
    char line[64];

    if(!v->reported)
    {
        return;     /* fewer than CLS_SEQ_MIN_CALLS calls: not a bat */
    }
    if(!CLASSIFIER_REPORT_FAINT && ClsSpecies_IsFaint(v->taxon))
    {
        return;     /* the faint-sweep group: too often noise (classifier.h) */
    }

    ClsSpecies_HeaderText(v, verdictText);
    verdictNew = 1U;
    CallLog_Add(line, ClsSpecies_FormatVerdict(v, sessionBlock * CLS_HOPS_PER_BLOCK, line, sizeof(line)));
}


/* A call has ended: name it (and log it, if every call is being logged) */
static void Classifier_Call(ClsCall_t *call)
{
    ClsVerdict_t verdict;

    if(ClsSpecies_AddCall(call, &verdict))
    {
        Classifier_Verdict(&verdict);
    }

#if CALL_LOG_CALLS
    char line[160];
    CallLog_Add(line, ClsSpecies_FormatCall(call, sessionBlock * CLS_HOPS_PER_BLOCK, line, sizeof(line)));
#endif
}


static void Classifier_EndSession(void)
{
    ClsCall_t call;
    ClsVerdict_t verdict;

    if(ClsCalls_Flush(&call))
    {
        Classifier_Call(&call);
    }
    if(ClsSpecies_Flush(&verdict))
    {
        Classifier_Verdict(&verdict);
    }
    CallLog_Flush();
    running = 0U;
}


/* Ask the ring for the next blocks, up to the one after the block being processed */
static void Classifier_RequestBlocks(uint32_t written)
{
    while((requestBlock < written) && (requestBlock <= (nextBlock + 1U)))
    {
        const uint32_t slot = requestBlock & 1U;
        const PsramRing_ReadResult_t result = PsramRing_ReadBlock(requestBlock, slots[slot], &slotDone[slot]);

        if(result == PSRAM_RING_READ_BUSY)
        {
            return;
        }
        if(result == PSRAM_RING_READ_INVALID)
        {
            slotDone[slot] = PSRAM_RING_FAILED;
        }
        slotBlock[slot] = requestBlock;
        requestBlock++;
    }
}


void Classifier_Init(void)
{
    /* the cycle counter, for classifierBlockCyclesMax */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    ClsSpecies_SetRegions(CLS_REGION_MAINLAND);
    running = 0U;
    passPending = 0U;
    lastPresent = 0U;
    verdictNew = 0U;
}


void Classifier_Poll(void)
{
    if(!PsramRing_IsRunning())
    {
        return;
    }

    /* note a pass starting even while a recording or playback holds us off */
    const uint8_t present = BatDetect_IsBatPresent();

    if(present && !lastPresent && !running && !passPending)
    {
        const uint32_t pushed = PsramRing_GetPushedBlocks();

        passPending = 1U;
        passStartBlock = (pushed > CLS_PRE_BLOCKS) ? (pushed - CLS_PRE_BLOCKS) : 0U;
    }
    lastPresent = present;

    /* recordings and playback come first; playback also pauses the ring */
    if((Recorder_GetState() != RECORDER_IDLE) || Playback_IsActive())
    {
        return;
    }

    const uint32_t written = PsramRing_GetWrittenBlocks();
    const uint32_t resume = PsramRing_GetResumeBlock();
    uint32_t oldest = (written > CLS_MAX_BEHIND_BLOCKS) ? (written - CLS_MAX_BEHIND_BLOCKS) : 0U;

    if(oldest < resume)
    {
        oldest = resume;    /* never across a pause: audio from either side doesn't join */
    }

    /* fallen too far behind, or the ring paused since: start again on newer audio */
    if(running && (nextBlock < oldest))
    {
        classifierSkips++;
        Classifier_EndSession();
        if(present)
        {
            passPending = 1U;
            passStartBlock = oldest;
        }
    }

    if(!running && passPending)
    {
        /* the last session's final reads may still be landing in the slots */
        if((slotDone[0] == PSRAM_RING_PENDING) || (slotDone[1] == PSRAM_RING_PENDING))
        {
            return;
        }

        sessionBlock = (passStartBlock > oldest) ? passStartBlock : oldest;
        nextBlock = sessionBlock;
        requestBlock = sessionBlock;
        slotBlock[0] = CLS_NO_BLOCK;
        slotBlock[1] = CLS_NO_BLOCK;
        ClsCalls_Reset();
        ClsSpecies_Reset();
        passPending = 0U;
        running = 1U;
    }

    if(!running)
    {
        return;
    }

    Classifier_RequestBlocks(written);

    for(uint32_t n = 0; n < CLS_BLOCKS_PER_POLL; n++)
    {
        const uint32_t slot = nextBlock & 1U;

        if((slotBlock[slot] != nextBlock) || (slotDone[slot] == PSRAM_RING_PENDING))
        {
            break;      /* not asked for yet, or still loading */
        }

        if(slotDone[slot] == PSRAM_RING_FAILED)
        {
            classifierSkips++;      /* overwritten, or a ring fault: drop rather than corrupt */
            Classifier_EndSession();
            return;
        }

        uint32_t cycles = 0U;
        ClsCall_t call;

        for(uint32_t h = 0; h < CLS_HOPS_PER_BLOCK; h++)
        {
            const uint32_t start = DWT->CYCCNT;
            const uint8_t ended = ClsCalls_ProcessHop(&slots[slot][h * CLS_HOP], &call);

            cycles += DWT->CYCCNT - start;      /* the frames only, not the SD writes of the log */
            if(ended)
            {
                Classifier_Call(&call);
            }
        }

        if(cycles > classifierBlockCyclesMax)
        {
            classifierBlockCyclesMax = cycles;
        }

        nextBlock++;
        Classifier_RequestBlocks(written);
    }

    /* the pass is over (2 s after its last pulse) and everything up to now is done */
    if(!present && (nextBlock >= written))
    {
        Classifier_EndSession();
    }
}


uint8_t Classifier_TakeVerdict(char *text)
{
    if(!verdictNew)
    {
        return 0U;
    }
    for(uint32_t i = 0; i < sizeof(verdictText); i++)
    {
        text[i] = verdictText[i];
    }
    verdictNew = 0U;
    return 1U;
}
