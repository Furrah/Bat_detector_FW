/*
 * autotune.c
 *
 * Auto-tune from the waterfall's rows; see autotune.h.
 */

#include "autotune.h"

#include "bat_detect.h"
#include "pipeline_config.h"
#include "waterfall.h"


/* Calls are looked for in the band the LO can reach */
#define AUTOTUNE_MIN_HZ         10000.0f
#define AUTOTUNE_MAX_HZ         ((float)AUDIO_LO_MAX_HZ)

/* A call's column must stand this far above that column's floor */
#define AUTOTUNE_MIN_EXCESS_DB  10

/* The LO follows the median of the last 8 calls, once there are 3 */
#define AUTOTUNE_CALLS          8U
#define AUTOTUNE_MIN_CALLS      3U

/* Retune only for a bigger move than this, and at most once per 25 rows (1 s) */
#define AUTOTUNE_HYSTERESIS_HZ  2000U
#define AUTOTUNE_HOLD_ROWS      25U

/* The LO moves in whole 0.1 kHz, as the display shows it */
#define AUTOTUNE_LO_STEP_HZ     100U

/*
 * Per-column floor in half-dB steps, as int8: -64 to +63.5 dB (the FFT's
 * levels run about -30 to +50 dB, and the waterfall's rows start at its
 * palette floor). It falls a quarter of the way to a quieter row each row
 * (~0.16 s) and rises half a dB every 4th row (3 dB/s), so it sits near
 * the quiet end of the noise. It's learnt only between passes: during a
 * pass, calls would lift it. 240 bytes, against 960 as floats.
 */
#define STEPS_PER_DB            2
#define FLOOR_FALL_DIV          4
#define FLOOR_RISE_EVERY        4U

/* The first second of rows only learns the floor, faster */
#define FLOOR_LEARN_ROWS        25U


static int8_t   floorSteps[WF_SCREEN_W];
static uint32_t rows = 0;                   /* rows with FFT frames in them */
static uint16_t colMin = 0;                 /* the search band, in columns */
static uint16_t colMax = 0;

static uint32_t lastPulses = 0;
static uint8_t  callLastRow = 0;
static uint8_t  wasPresent = 0;

static uint16_t calls[AUTOTUNE_CALLS];      /* loudest frequencies, 10 Hz units */
static uint8_t  callCount = 0;
static uint8_t  callNext = 0;

static uint32_t tunedLoHz = 0;              /* the LO last asked for */
static uint32_t rowsSinceRetune = AUTOTUNE_HOLD_ROWS;


static int32_t ToSteps(float db)
{
    const float steps = db * (float)STEPS_PER_DB;

    if(steps >= 127.0f)
    {
        return 127;
    }
    if(steps <= -128.0f)
    {
        return -128;
    }
    return (int32_t)(steps + ((steps >= 0.0f) ? 0.5f : -0.5f));
}


static void LearnFloor(const float *rowDb)
{
    const uint8_t learning = (rows <= FLOOR_LEARN_ROWS);
    const uint8_t rise = ((rows % FLOOR_RISE_EVERY) == 0U);

    for(uint16_t col = 0; col < WF_SCREEN_W; col++)
    {
        const int32_t level = ToSteps(rowDb[col]);
        int32_t floor = floorSteps[col];

        if(rows == 1U)
        {
            floor = level;
        }
        else if(learning)
        {
            floor += (level - floor) / 2;
        }
        else if(level < floor)
        {
            floor -= (floor - level + FLOOR_FALL_DIV - 1) / FLOOR_FALL_DIV;
        }
        else if((level > floor) && rise)
        {
            floor++;
        }

        floorSteps[col] = (int8_t)floor;
    }
}


/*
 * The column furthest above its floor, if any is far enough, as a
 * frequency. The pick is on the level above the floor (so steady tones
 * never win), but the frequency is the call's own: step to a louder
 * neighbour first, or the parabola's offset would clamp at half a column
 * (the lesson of diary 43).
 */
static uint8_t FindCall(const float *rowDb, float *hz)
{
    int32_t bestExcess = AUTOTUNE_MIN_EXCESS_DB * STEPS_PER_DB - 1;
    int32_t best = -1;

    for(uint16_t col = colMin; col <= colMax; col++)
    {
        const int32_t excess = ToSteps(rowDb[col]) - floorSteps[col];

        if(excess > bestExcess)
        {
            bestExcess = excess;
            best = col;
        }
    }

    if(best < 0)
    {
        return 0U;
    }

    if((best < (WF_SCREEN_W - 1)) && (rowDb[best + 1] > rowDb[best]))
    {
        best++;
    }
    else if((best > 0) && (rowDb[best - 1] > rowDb[best]))
    {
        best--;
    }

    float offset = 0.0f;

    if((best > 0) && (best < (WF_SCREEN_W - 1)))
    {
        const float a = rowDb[best - 1];
        const float b = rowDb[best];
        const float c = rowDb[best + 1];
        const float curve = a - 2.0f * b + c;

        if(curve < 0.0f)
        {
            offset = 0.5f * (a - c) / curve;
            offset = (offset > 0.5f) ? 0.5f : ((offset < -0.5f) ? -0.5f : offset);
        }
    }

    *hz = Waterfall_ColumnToHz((float)best + offset);
    return 1U;
}


/* Median of the calls held, in Hz */
static uint32_t MedianHz(void)
{
    uint16_t sorted[AUTOTUNE_CALLS];

    for(uint8_t i = 0; i < callCount; i++)
    {
        uint16_t value = calls[i];
        uint8_t j = i;

        while((j > 0U) && (sorted[j - 1U] > value))
        {
            sorted[j] = sorted[j - 1U];
            j--;
        }
        sorted[j] = value;
    }

    const uint8_t mid = callCount / 2U;
    const uint32_t median = ((callCount & 1U) != 0U) ? sorted[mid]
            : ((uint32_t)sorted[mid - 1U] + sorted[mid]) / 2U;

    return median * 10U;
}


void AutoTune_Init(void)
{
    rows = 0;
    callCount = 0;
    callNext = 0;
    callLastRow = 0;
    wasPresent = 0;
    lastPulses = BatDetect_GetPulseCount();
    tunedLoHz = 0;
    rowsSinceRetune = AUTOTUNE_HOLD_ROWS;

    colMin = 0;
    while((colMin < (WF_SCREEN_W - 1)) && (Waterfall_ColumnToHz((float)colMin) < AUTOTUNE_MIN_HZ))
    {
        colMin++;
    }

    colMax = WF_SCREEN_W - 1;
    while((colMax > colMin) && (Waterfall_ColumnToHz((float)colMax) > AUTOTUNE_MAX_HZ))
    {
        colMax--;
    }
}


void AutoTune_Restart(uint32_t currentLoHz)
{
    tunedLoHz = currentLoHz;
    rowsSinceRetune = AUTOTUNE_HOLD_ROWS;
}


uint8_t AutoTune_AddRow(const float *rowDb, uint16_t frames, uint32_t *loHz)
{
    /* The detector sees every block; the FFT runs a little behind it, so a
     * call counted during one row can land in the next row's frames */
    const uint32_t pulses = BatDetect_GetPulseCount();
    const uint8_t callNow = (pulses != lastPulses);
    const uint8_t callInRow = callNow || callLastRow;
    const uint8_t present = BatDetect_IsBatPresent();

    lastPulses = pulses;
    callLastRow = callNow;

    if(rowsSinceRetune < AUTOTUNE_HOLD_ROWS)
    {
        rowsSinceRetune++;
    }

    /* a pass has ended: the next starts from its own calls (the LO holds) */
    if(wasPresent && !present)
    {
        callCount = 0;
        callNext = 0;
    }
    wasPresent = present;

    if(frames == 0U)
    {
        return 0U;      /* the main loop had no time for the FFT: an empty row */
    }

    rows++;

    if(!present || (rows <= FLOOR_LEARN_ROWS))
    {
        if(!callInRow || (rows <= FLOOR_LEARN_ROWS))
        {
            LearnFloor(rowDb);
        }
        return 0U;
    }

    float hz;

    if(!callInRow || !FindCall(rowDb, &hz))
    {
        return 0U;
    }

    calls[callNext] = (uint16_t)((hz + 5.0f) / 10.0f);
    callNext = (uint8_t)((callNext + 1U) % AUTOTUNE_CALLS);
    if(callCount < AUTOTUNE_CALLS)
    {
        callCount++;
    }

    if(callCount < AUTOTUNE_MIN_CALLS)
    {
        return 0U;
    }

    /* below the calls' loudest frequency, in the display's 0.1 kHz steps */
    uint32_t target = MedianHz();

    target = (target > AUTOTUNE_OFFSET_HZ) ? (target - AUTOTUNE_OFFSET_HZ) : 0U;
    target = ((target + AUTOTUNE_LO_STEP_HZ / 2U) / AUTOTUNE_LO_STEP_HZ) * AUTOTUNE_LO_STEP_HZ;
    if(target > AUDIO_LO_MAX_HZ)
    {
        target = AUDIO_LO_MAX_HZ;
    }

    const uint32_t change = (target > tunedLoHz) ? (target - tunedLoHz) : (tunedLoHz - target);

    if((change <= AUTOTUNE_HYSTERESIS_HZ) || (rowsSinceRetune < AUTOTUNE_HOLD_ROWS))
    {
        return 0U;
    }

    tunedLoHz = target;
    rowsSinceRetune = 0U;
    *loHz = target;
    return 1U;
}
