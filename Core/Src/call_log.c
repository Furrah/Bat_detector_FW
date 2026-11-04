/*
 * call_log.c
 *
 * CALLS.CSV on the SD card; see call_log.h.
 */

#include "call_log.h"

#include <string.h>

#include "main.h"
#include "app_fatfs.h"
#include "ff.h"
#include "../DSP/classifier_species.h"


#define CALL_LOG_FILE           "CALLS.CSV"
#define CALL_LOG_BUFFER         512U

/* After a failed write, the card isn't tried again for this long: a
 * missing card shouldn't cost a mount attempt every few calls */
#define CALL_LOG_RETRY_MS       10000U


volatile uint32_t callLogDropped = 0;

#if CALL_LOG_ENABLED

static char buffer[CALL_LOG_BUFFER];
static uint32_t used = 0;
static FIL file;
static uint8_t failed = 0;
static uint32_t failedTick = 0;


/* Append the buffer to the file (creating it with its header line) */
static uint8_t CallLog_Write(void)
{
    UINT written = 0;
    uint8_t ok;

    if(f_mount(&USERFatFs, USERPath, 1) != FR_OK)
    {
        return 0U;
    }
    if(f_open(&file, CALL_LOG_FILE, FA_OPEN_APPEND | FA_WRITE) != FR_OK)
    {
        return 0U;
    }

    ok = 1U;
    if(f_size(&file) == 0U)
    {
        const char *header = CALL_LOG_CALLS ? clsCsvHeader : clsCsvPassHeader;
        const UINT length = (UINT)strlen(header);
        ok = (f_write(&file, header, length, &written) == FR_OK) && (written == length);
    }
    if(ok)
    {
        ok = (f_write(&file, buffer, used, &written) == FR_OK) && (written == used);
    }
    if(f_close(&file) != FR_OK)
    {
        ok = 0U;
    }
    return ok;
}

#endif /* CALL_LOG_ENABLED */


void CallLog_Flush(void)
{
#if CALL_LOG_ENABLED
    if(used == 0U)
    {
        return;
    }

    if(failed && ((HAL_GetTick() - failedTick) < CALL_LOG_RETRY_MS))
    {
        callLogDropped += 1U;       /* a buffer's worth: counted once */
    }
    else if(CallLog_Write())
    {
        failed = 0U;
    }
    else
    {
        failed = 1U;
        failedTick = HAL_GetTick();
        callLogDropped += 1U;
    }
    used = 0U;
#endif
}


void CallLog_Add(const char *line, size_t length)
{
#if CALL_LOG_ENABLED
    if((length == 0U) || (length > CALL_LOG_BUFFER))
    {
        return;
    }
    if((used + length) > CALL_LOG_BUFFER)
    {
        CallLog_Flush();
    }
    memcpy(&buffer[used], line, length);
    used += length;
#else
    (void)line;
    (void)length;
#endif
}
