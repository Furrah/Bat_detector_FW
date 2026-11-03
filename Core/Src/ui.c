/*
 * ui.c
 *
 * The front panel; see ui.h.
 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

#include "main.h"
#include "fonts.h"
#include "ili9341.h"
#include "pipeline_config.h"
#include "recorder.h"
#include "waterfall.h"
#include "../DSP/autotune.h"
#include "../DSP/bat_detect.h"
#include "../DSP/dsp.h"
#include "../DSP/fft.h"
#include "../DSP/playback.h"
#include "../Drivers/adc.h"


#define TICK_MS                 10U         /* knobs, buttons, header line, LEDs */
#define ROW_MS                  40U         /* a waterfall row */

#define VDDA_MV                 3300U       /* ADC reference: VDDA = 3V3 rail */
#define VBAT_DIVIDER            2U          /* R21/R22, 1M/1M */
#define VBAT_UPDATE_MS          1000U

/* The battery readout sits in the top right of the header as two Font_7x10
 * lines. The LO label (13 chars of Font_16x26 from x=5) ends at x=213,
 * which leaves too little room for "3.9V" on one line. */
#define VBAT_TEXT_X             218U
#define VBAT_TEXT_Y             5U

/* LED1 flash per call; long enough to see (calls are 1-30 ms) */
#define PULSE_LED_MS            50U

/* PLAYBACK and RECORD must read the same for 3 ticks to count */
#define BUTTON_DEBOUNCE_TICKS   3U

/* PLAYBACK held this long (ticks) switches the tuning mode */
#define LONG_PRESS_TICKS        80U

/* How long a message ("PLAY: busy", "REC: SD full", ...) stays up */
#define LABEL_MESSAGE_MS        2000U

/* What the header line shows, so it's only redrawn when that changes */
#define LABEL_LO                0U
#define LABEL_MESSAGE           1U
#define LABEL_SAVING            2U
#define LABEL_PLAYBACK          0x10000U    /* + seconds left */
#define LABEL_RECORDING         0x20000U    /* + seconds recorded */

/* Tuning modes, in the order a long PLAYBACK press steps through them */
#define TUNE_MANUAL             0U          /* the LO knob sets the LO */
#define TUNE_AUTO               1U          /* auto-tune follows the bats (autotune.h) */
#define TUNE_MODES              2U

/* LO knob changes smaller than this (of 4095) are noise */
#define KNOB_DEADBAND           16

/* A knob reading that can't happen: the next real one is applied */
#define KNOB_UNREAD             0xFFFFU


static uint32_t tick = 0;                   /* HAL tick of the current 10 ms tick */
static uint32_t frequency = 0;              /* the LO, Hz */
static uint8_t  tuneMode = TUNE_MANUAL;
static uint16_t knob = 0;                   /* the LO knob's last applied reading */

/* Both buttons start as "held", so a press left over from a power-up test
 * (PLAYBACK: SD speed, RECORD: full PSRAM) has to be released before it counts */
static uint8_t  playbackButton = 1;
static uint8_t  playbackDebounce = 0;
static uint32_t playbackHeldTicks = 0;
static uint8_t  playbackLongDone = 1;       /* this press is used up (a long press, or the power-up one) */
static uint8_t  recordButton = 1;
static uint8_t  recordDebounce = 0;

static uint32_t headerLabel = LABEL_LO;     /* LABEL_...: what the header line shows */
static uint32_t messageUntil = 0;           /* a message stays up until this tick */

static uint32_t lastPulseCount = 0;
static uint32_t pulseLedOffTick = 0;

static uint32_t lastRowTick = 0;
static uint32_t lastBatteryTick = 0;
static uint32_t batterySum = 0;
static uint32_t batteryCount = 0;


/* The LO readout: "LO:  45.0 kHz" in manual, "AUTO  43.5kHz" in auto.
 * Fixed 13-character width, clear of the battery readout. */
static void UI_ShowLo(void)
{
    char text[32];

    snprintf(text, sizeof(text), (tuneMode == TUNE_AUTO) ? "AUTO %3lu.%lukHz" : "LO: %3lu.%lu kHz",
            frequency / 1000, (frequency % 1000) / 100);
    ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}

/* Move the heterodyne LO, and show it unless something else has the header line */
static void UI_SetLo(uint32_t hz)
{
    frequency = hz;
    DSP_SetMixerFrequency((float)frequency);

    if(headerLabel == LABEL_LO)
    {
        UI_ShowLo();
    }
}

/* A message in the header line for LABEL_MESSAGE_MS (13 characters) */
static void UI_ShowMessage(const char *text)
{
    ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
    headerLabel = LABEL_MESSAGE;
    messageUntil = tick + LABEL_MESSAGE_MS;
}


/* Use the ADC2 scan started on the last tick (no result yet means no
 * change), then start the next: LO knob, volume knob, battery voltage */
static void UI_ReadKnobs(void)
{
    uint16_t reading = knob;

    if(ADC2_ScanReady())
    {
        reading = ADC2_Get(ADC2_LO_POT);

        DSP_SetVolume((float)ADC2_Get(ADC2_VOLUME) / 4095.0f);

        batterySum += ADC2_Get(ADC2_VBAT);
        batteryCount++;
    }

    ADC2_StartScan();

    /* The LO knob tunes in manual mode only; auto ignores it */
    if((tuneMode == TUNE_MANUAL) && (reading != KNOB_UNREAD) && (abs((int)reading - (int)knob) > KNOB_DEADBAND))
    {
        knob = reading;
        UI_SetLo(((uint32_t)reading * AUDIO_LO_MAX_HZ) / 4095U);
    }
}


/*
 * PLAYBACK, short press: replay the last 2 s ten times slower, or stop. It
 * acts on release, so it can be told from a long press. Not while a
 * recording is open: playback pauses the ring it reads from.
 * PLAYBACK held for 0.8 s: the next tuning mode.
 */
static void UI_PlaybackButton(void)
{
    const uint8_t down = (HAL_GPIO_ReadPin(PLAYBACK_BUTTON_GPIO_Port, PLAYBACK_BUTTON_Pin) == GPIO_PIN_RESET);

    if(down == playbackButton)
    {
        playbackDebounce = 0;
    }
    else if(++playbackDebounce >= BUTTON_DEBOUNCE_TICKS)
    {
        playbackButton = down;
        playbackDebounce = 0;

        if(playbackButton)
        {
            playbackHeldTicks = 0;          /* pressed: time the hold */
            playbackLongDone = 0;
        }
        else if(!playbackLongDone && Playback_IsActive())
        {
            Playback_Stop();
        }
        else if(!playbackLongDone)
        {
            const Playback_Status_t status = (Recorder_GetState() != RECORDER_IDLE) ? PLAYBACK_ERR_BUSY : Playback_Start();

            messageUntil = tick;            /* the playback label replaces any message */

            if(status != PLAYBACK_OK)
            {
                UI_ShowMessage((status == PLAYBACK_ERR_BUSY) ? "PLAY: busy   " : "PLAY: no ring");
            }
        }
    }

    if(playbackButton && !playbackLongDone && (++playbackHeldTicks >= LONG_PRESS_TICKS))
    {
        playbackLongDone = 1;
        tuneMode = (uint8_t)((tuneMode + 1U) % TUNE_MODES);

        if(tuneMode == TUNE_AUTO)
        {
            AutoTune_Restart(frequency);    /* the LO stays put until the bats move it */
        }
        else
        {
            knob = KNOB_UNREAD;             /* back to the knob's setting on its next reading */
        }

        UI_ShowMessage((tuneMode == TUNE_AUTO) ? "MODE: AUTO   " : "MODE: MANUAL ");
    }
}


/*
 * RECORD: record to SD from 5 s before the press; press again to stop. Not
 * during playback: the ring is paused, so there's no live audio. A press
 * while the last recording is still saving is ignored.
 */
static void UI_RecordButton(void)
{
    const uint8_t down = (HAL_GPIO_ReadPin(RECORD_BUTTON_GPIO_Port, RECORD_BUTTON_Pin) == GPIO_PIN_RESET);

    if(down == recordButton)
    {
        recordDebounce = 0;
        return;
    }

    if(++recordDebounce < BUTTON_DEBOUNCE_TICKS)
    {
        return;
    }

    recordButton = down;
    recordDebounce = 0;

    if(recordButton && (Recorder_GetState() == RECORDER_RECORDING))
    {
        Recorder_Stop();
    }
    else if(recordButton && (Recorder_GetState() == RECORDER_IDLE))
    {
        /* mounts the card and scans its folder: can hold the main loop ~0.1 s */
        const char *message = Playback_IsActive() ? "REC: playing " : NULL;

        if(message == NULL)
        {
            const Recorder_Status_t status = Recorder_Start();

            if(status != RECORDER_OK)
            {
                message = Recorder_StatusText(status);
            }
        }

        messageUntil = tick;

        if(message != NULL)
        {
            UI_ShowMessage(message);
        }
    }
}


/*
 * The header line: a message for 2 s, else the playback countdown
 * ("PLAY 10x  20s"), the recording time ("REC   1:23"), or "REC saving"
 * while the backlog is written out. The LO readout comes back when none of
 * them is up.
 */
static void UI_UpdateHeader(void)
{
    char text[32];

    /* a recording that ended by itself (an error, or the time limit) says why */
    const Recorder_Status_t recordEnd = Recorder_TakeResult();

    if(recordEnd != RECORDER_OK)
    {
        UI_ShowMessage(Recorder_StatusText(recordEnd));
    }

    const Recorder_State_t recordState = Recorder_GetState();

    if((int32_t)(tick - messageUntil) < 0)
    {
        /* a message is up: leave it */
    }
    else if(Playback_IsActive())
    {
        const uint32_t seconds = (Playback_GetRemainingMs() + 999U) / 1000U;

        if(headerLabel != (LABEL_PLAYBACK + seconds))
        {
            headerLabel = LABEL_PLAYBACK + seconds;
            snprintf(text, sizeof(text), "PLAY %lux %3lus", (unsigned long)PLAYBACK_EXPANSION, seconds);
            ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
        }
    }
    else if(recordState == RECORDER_RECORDING)
    {
        const uint32_t seconds = Recorder_GetElapsedMs() / 1000U;

        if(headerLabel != (LABEL_RECORDING + seconds))
        {
            headerLabel = LABEL_RECORDING + seconds;
            snprintf(text, sizeof(text), "REC %3lu:%02lu   ", seconds / 60U, seconds % 60U);
            ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_RED, ILI9341_BLACK);
        }
    }
    else if(recordState == RECORDER_SAVING)
    {
        if(headerLabel != LABEL_SAVING)
        {
            headerLabel = LABEL_SAVING;
            ILI9341_WriteString(5, 5, "REC saving   ", Font_16x26, ILI9341_RED, ILI9341_BLACK);
        }
    }
    else if(headerLabel != LABEL_LO)
    {
        headerLabel = LABEL_LO;
        UI_ShowLo();
    }
}


/* LED0: a bat pass is in progress. LED1: a flash for each call during a
 * pass; pulses outside a pass stay dark (diary 44). */
static void UI_UpdateLeds(void)
{
    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, BatDetect_IsBatPresent() ? GPIO_PIN_SET : GPIO_PIN_RESET);

    const uint32_t pulseCount = BatDetect_GetPulseCount();

    if(pulseCount != lastPulseCount)
    {
        lastPulseCount = pulseCount;

        if(BatDetect_IsBatPresent())
        {
            pulseLedOffTick = tick + PULSE_LED_MS;
            HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
        }
    }
    else if((int32_t)(tick - pulseLedOffTick) >= 0)
    {
        HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
    }
}


void UI_Init(void)
{
    lastRowTick = HAL_GetTick();
    lastBatteryTick = HAL_GetTick();

    Waterfall_SetLOFrequency(frequency);
}


void UI_Poll(void)
{
    if((HAL_GetTick() - tick) < TICK_MS)
    {
        return;
    }

    tick = HAL_GetTick();

    UI_ReadKnobs();
    UI_PlaybackButton();
    UI_RecordButton();
    UI_UpdateHeader();
    UI_UpdateLeds();
}


void UI_PollDisplay(void)
{
    if(fftMagnitudeReady)
    {
        fftMagnitudeReady = 0;
        Waterfall_Accumulate(fftMagnitude, FFT_NUM_BINS);
    }

    const uint32_t now = HAL_GetTick();

    if((now - lastRowTick) >= ROW_MS)
    {
        uint16_t rowFrames;
        uint32_t tunedHz;

        /* auto-tune reads the row before it's drawn and cleared; it learns
         * in both modes but only moves the LO in auto */
        if(AutoTune_AddRow(Waterfall_GetRow(&rowFrames), rowFrames, &tunedHz) && (tuneMode == TUNE_AUTO))
        {
            UI_SetLo(tunedHz);
        }

        Waterfall_FlushRow();
        lastRowTick = now;
    }

    /* Battery: the 10 ms readings averaged over each second. Ratiometric to
     * VDDA, so it reads high once the MCP1700 drops out near empty. */
    if(((now - lastBatteryTick) >= VBAT_UPDATE_MS) && (batteryCount > 0U))
    {
        char text[8];
        const uint32_t mv = (batterySum / batteryCount) * VDDA_MV * VBAT_DIVIDER / 4095U;
        const uint32_t tenths = (mv + 50U) / 100U;

        snprintf(text, sizeof(text), "%lu.%lu", tenths / 10U, tenths % 10U);
        ILI9341_WriteString(VBAT_TEXT_X, VBAT_TEXT_Y, "BAT", Font_7x10, ILI9341_WHITE, ILI9341_BLACK);
        ILI9341_WriteString(VBAT_TEXT_X, VBAT_TEXT_Y + 12U, text, Font_7x10, ILI9341_WHITE, ILI9341_BLACK);

        batterySum = 0;
        batteryCount = 0;
        lastBatteryTick = now;
    }
}
