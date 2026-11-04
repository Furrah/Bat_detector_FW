/*
 * recorder.c
 *
 * WAV recording to the SD card; see recorder.h.
 *
 * The audio goes PSRAM ring -> slot -> file. There are 16 slots of one
 * block (512 bytes) each: one chunk of 8 is written to the card while the
 * next 8 load from the ring. Block b always uses slot b % 16. Blocks
 * saveNext .. readNext-1 have their ring reads queued or landed. A chunk is
 * converted to signed 16-bit in its slots just before it's written.
 *
 * File layout: a 512-byte header in sector 0, then one 512-byte audio
 * block per sector. The header fills a whole sector to keep the audio
 * sector-aligned; with the usual 44-byte header every chunk would straddle
 * sectors, and FatFS would read-modify-write the partial ones. The first
 * chunk is 7 blocks, so later chunks of 8 start on file sectors that are
 * multiples of 8. With 32 KB clusters a chunk then never crosses a
 * cluster, and each f_write reaches the card as one 8-sector write.
 *
 * Every 2 s the file is synced (FatFS writes its size and FAT) and the
 * header's sizes are updated. The header goes straight to its sector with
 * disk_write: f_lseek back to the start would walk the whole cluster
 * chain. Writing behind FatFS's back is safe because the recorder only
 * writes whole sectors, which FatFS passes to the card without caching.
 */

#include "recorder.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "app_fatfs.h"
#include "pipeline_config.h"
#include "../Drivers/psram_ring.h"


#define SECTOR_BYTES        512U
#define CHUNK_BLOCKS        8U                  /* sectors per f_write (diary 39) */
#define SLOTS               (2U * CHUNK_BLOCKS) /* one chunk written while the next loads */
#define POLL_MS             20U                 /* longest one Recorder_Poll() keeps the main loop */
#define SYNC_MS             2000U               /* most audio a power cut can lose */
#define DRAIN_MS            50U                 /* wait for reads in flight when finishing */

#define NAME_PREFIX         "BAT"
#define NAME_SUFFIX         ".WAV"
#define NAME_DIGITS         5U
#define MAX_NUMBER          99999U

#define PRETRIGGER_BLOCKS   ((RECORDER_PRETRIGGER_SECONDS * AUDIO_SAMPLE_RATE_HZ) / AUDIO_BLOCK_SIZE)
#define MAX_BLOCKS          ((RECORDER_MAX_SECONDS * AUDIO_SAMPLE_RATE_HZ) / AUDIO_BLOCK_SIZE)

#define ADC_MIDSCALE        2048U               /* (code - 2048) x 16 is the 16-bit sample */
#define SAMPLE_SHIFT        4U

_Static_assert(PSRAM_RING_BLOCK_BYTES == SECTOR_BYTES, "one audio block must fill one sector");
_Static_assert(_MAX_SS == SECTOR_BYTES, "FatFS must use 512-byte sectors");
_Static_assert(PRETRIGGER_BLOCKS <= (PSRAM_RING_BLOCKS - 1024U), "the pre-trigger must leave 1 s of the ring spare");
_Static_assert(((uint64_t)(MAX_BLOCKS + 1U) * SECTOR_BYTES) < 0xFFFFFFFFULL, "FAT32 files are limited to 4 GB");


/* Sector 0 of the file. All fields little-endian, as the M4 is. */
typedef struct
{
    char     riffId[4];         /* "RIFF" */
    uint32_t riffBytes;         /* file size - 8 */
    char     waveId[4];         /* "WAVE" */
    char     fmtId[4];          /* "fmt " */
    uint32_t fmtBytes;          /* 16 */
    uint16_t format;            /* 1: PCM */
    uint16_t channels;
    uint32_t sampleRateHz;
    uint32_t byteRate;
    uint16_t blockAlign;        /* bytes per sample frame */
    uint16_t bitsPerSample;
    char     padId[4];          /* "JUNK": readers skip it. Room for GUANO metadata later. */
    uint32_t padBytes;
    uint8_t  pad[460];
    char     dataId[4];         /* "data" */
    uint32_t dataBytes;
} WavHeader_t;

_Static_assert(sizeof(WavHeader_t) == SECTOR_BYTES, "the WAV header must fill one sector");
_Static_assert(offsetof(WavHeader_t, dataBytes) == 508U, "WAV header layout");


static uint8_t slots[SLOTS * PSRAM_RING_BLOCK_BYTES] __ALIGNED(4);
static volatile uint8_t slotDone[SLOTS];

static uint8_t state = RECORDER_IDLE;
static uint8_t endStatus = RECORDER_OK;     /* why the recording is ending */
static uint8_t resultPending = 0;           /* ended; Recorder_TakeResult() not called yet */

static FIL file;

static uint32_t saveNext;                   /* next block to write to the file */
static uint32_t readNext;                   /* next block to ask the ring for */
static uint32_t stopBlock;                  /* first block not to record */

static uint32_t startTick;
static uint32_t lastSyncTick;


/* The first error (or the time limit) is the one reported */
static void Recorder_SetEnd(Recorder_Status_t status)
{
    if(endStatus == RECORDER_OK)
    {
        endStatus = status;
    }
}


/* Stop taking audio at block: what's queued before it is still saved */
static void Recorder_EndAt(uint32_t block, Recorder_Status_t why)
{
    if(block < saveNext)
    {
        block = saveNext;
    }

    if(block < stopBlock)
    {
        stopBlock = block;
    }

    Recorder_SetEnd(why);
    state = RECORDER_SAVING;
}


/*
 * The first block: the pre-trigger's length behind the ring, but not from
 * before the ring last resumed (playback pauses it). Rounded up to 1 more
 * than a multiple of 8, so the chunks after the header line up (see top).
 */
static uint32_t Recorder_FirstBlock(uint32_t written)
{
    const uint32_t resumed = PsramRing_GetResumeBlock();
    uint32_t first = (written > PRETRIGGER_BLOCKS) ? (written - PRETRIGGER_BLOCKS) : 0U;

    if(first < resumed)
    {
        first = resumed;
    }

    return ((first + (CHUNK_BLOCKS - 2U)) & ~(CHUNK_BLOCKS - 1U)) + 1U;
}


static void Recorder_BuildHeader(void *sector, uint32_t dataBytes)
{
    WavHeader_t *header = sector;

    memset(header, 0, sizeof(*header));

    memcpy(header->riffId, "RIFF", 4);
    header->riffBytes = (sizeof(WavHeader_t) - 8U) + dataBytes;
    memcpy(header->waveId, "WAVE", 4);

    memcpy(header->fmtId, "fmt ", 4);
    header->fmtBytes = 16U;
    header->format = 1U;
    header->channels = 1U;
    header->sampleRateHz = AUDIO_SAMPLE_RATE_HZ;
    header->byteRate = AUDIO_SAMPLE_RATE_HZ * sizeof(int16_t);
    header->blockAlign = sizeof(int16_t);
    header->bitsPerSample = 16U;

    memcpy(header->padId, "JUNK", 4);
    header->padBytes = sizeof(header->pad);

    memcpy(header->dataId, "data", 4);
    header->dataBytes = dataBytes;
}


/* BATnnnnn.WAV -> nnnnn; anything else -> 0 */
static uint32_t Recorder_FileNumber(const char *name)
{
    const uint32_t prefix = sizeof(NAME_PREFIX) - 1U;
    uint32_t number = 0U;

    if((strlen(name) != (prefix + NAME_DIGITS + sizeof(NAME_SUFFIX) - 1U))
            || (strncmp(name, NAME_PREFIX, prefix) != 0)
            || (strcmp(&name[prefix + NAME_DIGITS], NAME_SUFFIX) != 0))
    {
        return 0U;
    }

    for(uint32_t i = prefix; i < (prefix + NAME_DIGITS); i++)
    {
        if((name[i] < '0') || (name[i] > '9'))
        {
            return 0U;
        }

        number = (number * 10U) + (uint32_t)(name[i] - '0');
    }

    return number;
}


/* One more than the highest BATnnnnn.WAV in the root folder */
static uint32_t Recorder_NextNumber(void)
{
    DIR folder;
    FILINFO info;
    uint32_t highest = 0U;

    if(f_opendir(&folder, "/") != FR_OK)
    {
        return 1U;      /* f_open finds out what's wrong */
    }

    while((f_readdir(&folder, &info) == FR_OK) && (info.fname[0] != '\0'))
    {
        const uint32_t number = Recorder_FileNumber(info.fname);

        if(number > highest)
        {
            highest = number;
        }
    }

    (void)f_closedir(&folder);
    return highest + 1U;
}


/* Mount the card, create the next file and write its header (no audio yet) */
static Recorder_Status_t Recorder_OpenFile(void)
{
    char name[13];
    UINT done = 0U;
    FRESULT fresult = f_mount(&USERFatFs, USERPath, 1);
    uint32_t number;

    if(fresult == FR_NO_FILESYSTEM)
    {
        return RECORDER_ERR_NOT_FAT;
    }

    if(fresult != FR_OK)
    {
        return RECORDER_ERR_NO_CARD;
    }

    number = Recorder_NextNumber();

    do
    {
        if(number > MAX_NUMBER)
        {
            return RECORDER_ERR_FILE;
        }

        snprintf(name, sizeof(name), NAME_PREFIX "%05lu" NAME_SUFFIX, (unsigned long)number);
        fresult = f_open(&file, name, FA_CREATE_NEW | FA_WRITE);    /* never overwrites */
        number++;
    } while(fresult == FR_EXIST);

    if(fresult != FR_OK)
    {
        return RECORDER_ERR_FILE;
    }

    Recorder_BuildHeader(slots, 0U);
    fresult = f_write(&file, slots, sizeof(WavHeader_t), &done);

    if((fresult != FR_OK) || (done != sizeof(WavHeader_t)))
    {
        (void)f_close(&file);
        (void)f_unlink(name);
        return (fresult == FR_OK) ? RECORDER_ERR_FULL : RECORDER_ERR_WRITE;
    }

    return RECORDER_OK;
}


/*
 * Make the file readable as it stands: FatFS's size and FAT first, then
 * the header's sizes. In that order, a power cut between the two leaves a
 * header that claims less audio than the file holds, never more.
 * sector: a free 512-byte slot to build the header in.
 */
static uint8_t Recorder_Sync(uint8_t *sector)
{
    FATFS *fs = file.obj.fs;
    uint8_t ok = (f_sync(&file) == FR_OK);

    if(ok)
    {
        /* the file's first sector (FatFS's clust2sect) */
        const DWORD first = fs->database + ((file.obj.sclust - 2U) * fs->csize);

        Recorder_BuildHeader(sector, (uint32_t)f_size(&file) - sizeof(WavHeader_t));
        ok = (disk_write(fs->drv, sector, first, 1U) == RES_OK);
    }

    lastSyncTick = HAL_GetTick();
    return ok;
}


/* All of blocks first .. first+count-1 have arrived from the ring (or failed) */
static uint8_t Recorder_Landed(uint32_t first, uint32_t count)
{
    for(uint32_t i = 0; i < count; i++)
    {
        if(slotDone[(first + i) % SLOTS] == PSRAM_RING_PENDING)
        {
            return 0U;
        }
    }

    return 1U;
}


/* Convert blocks first .. first+count-1 to WAV samples in their slots and
 * write them in one f_write. Chunks never wrap round the slots. 0 = failed. */
static uint8_t Recorder_WriteChunk(uint32_t first, uint32_t count)
{
    const uint32_t slot = first % SLOTS;
    uint8_t *data = &slots[slot * PSRAM_RING_BLOCK_BYTES];
    uint16_t *sample = (uint16_t *)data;
    const UINT bytes = count * PSRAM_RING_BLOCK_BYTES;
    UINT done = 0U;

    /* (code - 2048) x 16, in unsigned arithmetic: the same bits as the
     * signed result, without shifting a negative number */
    for(uint32_t i = 0; i < (count * AUDIO_BLOCK_SIZE); i++)
    {
        sample[i] = (uint16_t)((sample[i] - ADC_MIDSCALE) << SAMPLE_SHIFT);
    }

    /* a block the ring failed to read holds stale data: save silence instead */
    for(uint32_t i = 0; i < count; i++)
    {
        if(slotDone[slot + i] == PSRAM_RING_FAILED)
        {
            memset(&data[i * PSRAM_RING_BLOCK_BYTES], 0, PSRAM_RING_BLOCK_BYTES);
        }
    }

    if(f_write(&file, data, bytes, &done) != FR_OK)
    {
        Recorder_SetEnd(RECORDER_ERR_WRITE);
        return 0U;
    }

    if(done != bytes)
    {
        Recorder_SetEnd(RECORDER_ERR_FULL);     /* FatFS wrote what fitted */
        return 0U;
    }

    return 1U;
}


/* The recording is over: close the file */
static void Recorder_Finish(void)
{
    const uint32_t drainStart = HAL_GetTick();
    uint32_t block = saveNext;

    /* Reads still in flight (after an error) write into the slots: let them
     * land before the next recording reuses them. The ring fails a stuck
     * read within a block, so this is short. */
    while((block != readNext) && ((HAL_GetTick() - drainStart) < DRAIN_MS))
    {
        if(slotDone[block % SLOTS] != PSRAM_RING_PENDING)
        {
            block++;
        }
    }

    /* the header's sizes come from the file's size, so they're right even after a failed write */
    if(!Recorder_Sync(slots))
    {
        Recorder_SetEnd(RECORDER_ERR_WRITE);
    }

    if(f_close(&file) != FR_OK)
    {
        Recorder_SetEnd(RECORDER_ERR_WRITE);
    }

    resultPending = 1U;
    state = RECORDER_IDLE;
}


Recorder_Status_t Recorder_Start(void)
{
    Recorder_Status_t status;
    uint32_t first;

    if(state != RECORDER_IDLE)
    {
        return RECORDER_ERR_BUSY;
    }

    if(!PsramRing_IsRecording())
    {
        return RECORDER_ERR_NO_RING;
    }

    /* Fix the start before anything slow (mounting, the folder scan), so
     * the pre-trigger is measured from the press */
    startTick = HAL_GetTick();
    first = Recorder_FirstBlock(PsramRing_GetWrittenBlocks());

    status = Recorder_OpenFile();

    if(status != RECORDER_OK)
    {
        return status;
    }

    saveNext = first;
    readNext = first;
    stopBlock = first + MAX_BLOCKS;
    endStatus = RECORDER_OK;
    resultPending = 0U;

    lastSyncTick = HAL_GetTick();
    state = RECORDER_RECORDING;

    return RECORDER_OK;
}


void Recorder_Stop(void)
{
    if(state == RECORDER_RECORDING)
    {
        Recorder_EndAt(PsramRing_GetPushedBlocks(), RECORDER_OK);   /* up to the block being processed now */
    }
}


void Recorder_Poll(void)
{
    if(state == RECORDER_IDLE)
    {
        return;
    }

    const uint32_t entry = HAL_GetTick();

    do
    {
        const uint32_t written = PsramRing_GetWrittenBlocks();

        if((state == RECORDER_RECORDING) && (written >= stopBlock))
        {
            Recorder_EndAt(stopBlock, RECORDER_TIME_LIMIT);
        }

        if(!PsramRing_IsRecording() && (written < stopBlock))
        {
            Recorder_EndAt(written, RECORDER_ERR_RING_PAUSED);  /* no more audio is coming */
        }

        /* 1. Queue ring reads into the free slots, up to the end */
        const uint32_t readEnd = (written < stopBlock) ? written : stopBlock;

        while((readNext < readEnd) && ((readNext - saveNext) < SLOTS))
        {
            const uint32_t slot = readNext % SLOTS;
            const PsramRing_ReadResult_t read = PsramRing_ReadBlock(readNext,
                    &slots[slot * PSRAM_RING_BLOCK_BYTES], &slotDone[slot]);

            if(read == PSRAM_RING_READ_BUSY)
            {
                break;          /* the ring's queue is full: try again */
            }

            if(read == PSRAM_RING_READ_INVALID)
            {
                /* overwritten before we got to it. Skipping ahead would splice
                 * the audio silently: save what's queued, then stop. */
                Recorder_EndAt(readNext, RECORDER_ERR_TOO_SLOW);
                break;
            }

            readNext++;
        }

        /* 2. The next chunk runs to the next multiple of 8 blocks, or to the end */
        const uint32_t chunkStart = saveNext;
        uint32_t chunkEnd = (saveNext | (CHUNK_BLOCKS - 1U)) + 1U;

        if(chunkEnd > stopBlock)
        {
            chunkEnd = stopBlock;
        }

        if(chunkEnd == chunkStart)
        {
            Recorder_Finish();      /* everything up to stopBlock is saved */
            return;
        }

        if(readNext < chunkEnd)
        {
            if(readNext >= written)
            {
                break;              /* caught up: wait for more audio */
            }

            continue;               /* the ring's read queue was full: go round again */
        }

        if(!Recorder_Landed(chunkStart, chunkEnd - chunkStart))
        {
            continue;               /* still loading */
        }

        /* 3. Write it */
        if(!Recorder_WriteChunk(chunkStart, chunkEnd - chunkStart))
        {
            Recorder_Finish();      /* endStatus says why */
            return;
        }

        saveNext = chunkEnd;

        /* 4. Sync every 2 s. The chunk's first slot is free to build the
         * header in: reads only run up to 16 blocks ahead of saveNext. */
        if(((HAL_GetTick() - lastSyncTick) >= SYNC_MS)
                && !Recorder_Sync(&slots[(chunkStart % SLOTS) * PSRAM_RING_BLOCK_BYTES]))
        {
            Recorder_SetEnd(RECORDER_ERR_WRITE);
            Recorder_Finish();
            return;
        }
    } while((HAL_GetTick() - entry) < POLL_MS);
}


Recorder_State_t Recorder_GetState(void)
{
    return (Recorder_State_t)state;
}


uint32_t Recorder_GetElapsedMs(void)
{
    return (state == RECORDER_RECORDING) ? (HAL_GetTick() - startTick) : 0U;
}


Recorder_Status_t Recorder_TakeResult(void)
{
    if(!resultPending)
    {
        return RECORDER_OK;
    }

    resultPending = 0U;
    return (Recorder_Status_t)endStatus;
}


const char *Recorder_StatusText(Recorder_Status_t status)
{
    switch(status)
    {
    case RECORDER_TIME_LIMIT:       return "REC: max time";
    case RECORDER_ERR_NO_RING:      return "REC: no ring ";
    case RECORDER_ERR_BUSY:         return "REC: busy    ";
    case RECORDER_ERR_NO_CARD:      return "REC: no card ";
    case RECORDER_ERR_NOT_FAT:      return "REC: not FAT ";
    case RECORDER_ERR_FILE:         return "REC: no file ";
    case RECORDER_ERR_WRITE:        return "REC: SD error";
    case RECORDER_ERR_FULL:         return "REC: SD full ";
    case RECORDER_ERR_TOO_SLOW:     return "REC: SD slow ";
    case RECORDER_ERR_RING_PAUSED:  return "REC: paused  ";
    default:                        return "REC: error   ";
    }
}
