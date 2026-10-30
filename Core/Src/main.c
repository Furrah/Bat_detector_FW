/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "app_fatfs.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "math.h"

#include "pipeline_config.h"
#include "adc.h"
#include "dsp.h"
#include "bat_detect.h"
#include "playback.h"
#include "debug_link.h"
#include "dac.h"
#include "psram.h"
#include "psram_ring.h"
#include "sd_spi.h"
#include "tim3_lo.h"
#include "fft.h"
#include "ili9341.h"
#include "fonts.h"

#include "waterfall.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define VDDA_MV            3300U   // ADC reference: VDDA = 3V3 rail
#define VBAT_DIVIDER       2U      // R21/R22, 1M/1M
#define VBAT_UPDATE_MS     1000U

// Battery readout sits in the top-right of the waterfall header as two
// Font_7x10 lines. The LO label (up to 13 chars of Font_16x26 from x=5)
// ends at x=213, which leaves too little room for "3.9V" on one line.
#define VBAT_TEXT_X        218U
#define VBAT_TEXT_Y        5U

// LED1 flash per detected bat pulse; long enough to see (calls are 1-30 ms)
#define PULSE_LED_MS       50U

// PLAYBACK button: must read the same for 3 x 10 ms ticks to count
#define PLAYBACK_DEBOUNCE_TICKS  3U

// How long "PLAY: no ring" / "PLAY: busy" stays up before the LO readout returns
#define PLAYBACK_MESSAGE_MS      2000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_adc2;

DAC_HandleTypeDef hdac1;
DMA_HandleTypeDef hdma_dac1_ch1;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
DMA_HandleTypeDef hdma_spi1_rx;
DMA_HandleTypeDef hdma_spi1_tx;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_DAC1_Init(void);
static void MX_TIM3_Init(void);
static void MX_SPI2_Init(void);
static void MX_ADC2_Init(void);
static void MX_TIM2_Init(void);
static void MX_SPI1_Init(void);
static void MX_RTC_Init(void);
/* USER CODE BEGIN PFP */
static void ShowLoLabel(uint32_t frequency);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
//volatile uint32_t dspOutputCount = 0;
//
//void DSP_OutputCallback(
//        const uint16_t *output,
//        uint32_t length)
//{
//    dspOutputCount++;
//
//    DAC_WriteBlock(output, length);
//}






/*
 * PSRAM bring-up check, run once at boot before the audio pipeline starts,
 * so SPI1 is otherwise idle. The address-line test always runs (a few ms).
 * The full 8 MB pattern test runs only if RECORD is held at power-up,
 * because the polled -O0 driver takes ~35 s for it.
 */
static PSRAM_Status_t PSRAM_BootCheck(void)
{
    char text[24];
    uint8_t mfid;
    uint8_t kgd;
    uint32_t failAddress = 0;
    uint32_t selectNs;
    uint8_t fullTest = (HAL_GPIO_ReadPin(RECORD_BUTTON_GPIO_Port, RECORD_BUTTON_Pin) == GPIO_PIN_RESET);

    PSRAM_Status_t status = PSRAM_Init();

    PSRAM_GetId(&mfid, &kgd);
    snprintf(text, sizeof(text), "PSRAM ID %02X %02X", mfid, kgd);
    ILI9341_WriteString(5, 40, text, Font_11x18, ILI9341_WHITE, ILI9341_BLACK);

    if(status == PSRAM_OK)
    {
        if(fullTest)
        {
            ILI9341_WriteString(5, 65, "Full test ~35s...", Font_11x18, ILI9341_WHITE, ILI9341_BLACK);
            status = PSRAM_TestFull(&failAddress);
        }
        else
        {
            status = PSRAM_TestAddressLines(&failAddress);
        }
    }

    if(status == PSRAM_OK)
    {
        ILI9341_WriteString(5, 90, fullTest ? "8MB test OK" : "Address test OK",
                Font_11x18, ILI9341_GREEN, ILI9341_BLACK);
    }
    else if(status == PSRAM_ERR_ID)
    {
        ILI9341_WriteString(5, 90, "FAIL: bad ID", Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(status == PSRAM_ERR_TIMEOUT)
    {
        // SPI1 SR decodes which flag never arrived (TXE bit 1, RXNE bit 0, BSY bit 7)
        snprintf(text, sizeof(text), "SPI timeout SR=%04lX", PSRAM_GetTimeoutStatus());
        ILI9341_WriteString(5, 90, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        snprintf(text, sizeof(text), "FAIL at 0x%06lX", failAddress);
        ILI9341_WriteString(5, 90, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }

    // CE# low time must stay within tCEM, or the PSRAM can't refresh
    selectNs = PSRAM_GetMaxSelectNs();
    snprintf(text, sizeof(text), "CE max %lu.%lu us", selectNs / 1000U, (selectNs % 1000U) / 100U);
    ILI9341_WriteString(5, 115, text, Font_11x18,
            (selectNs > PSRAM_TCEM_NS) ? ILI9341_RED : ILI9341_WHITE, ILI9341_BLACK);

    HAL_Delay((status == PSRAM_OK) ? 1500U : 5000U);

    return status;
}


#define SD_TEST_FILE_BYTES  (256UL * 1024UL)

static uint8_t SD_TestPattern(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16));
}


/*
 * Write, read back and verify a 256 KB file and show the speeds. Uses
 * 64-byte f_write/f_read calls: FatFS gathers them in its 512-byte sector
 * buffer, so the card only sees single-sector writes. Recording will use
 * multi-sector writes once RAM is reclaimed, so this figure is pessimistic.
 */
static void SD_SpeedTest(void)
{
    char text[24];
    uint8_t buffer[64];
    UINT done;
    FRESULT result;
    FRESULT closeResult;
    uint32_t start;
    uint32_t writeMs = 1U;
    uint32_t readMs = 1U;
    uint32_t badOffset = 0xFFFFFFFFU;

    ILI9341_WriteString(5, 175, "SD speed test...", Font_11x18, ILI9341_WHITE, ILI9341_BLACK);

    result = f_open(&USERFile, "SDTEST.BIN", FA_CREATE_ALWAYS | FA_WRITE);

    if(result == FR_OK)
    {
        start = HAL_GetTick();

        for(uint32_t offset = 0U; (offset < SD_TEST_FILE_BYTES) && (result == FR_OK); offset += sizeof(buffer))
        {
            for(uint32_t i = 0U; i < sizeof(buffer); i++)
            {
                buffer[i] = SD_TestPattern(offset + i);
            }

            result = f_write(&USERFile, buffer, sizeof(buffer), &done);

            if((result == FR_OK) && (done != sizeof(buffer)))
            {
                result = FR_DENIED;     // card full
            }
        }

        // closing flushes the last sector, so it counts towards the write time
        closeResult = f_close(&USERFile);
        writeMs = HAL_GetTick() - start + 1U;

        if(result == FR_OK)
        {
            result = closeResult;
        }
    }

    if(result == FR_OK)
    {
        result = f_open(&USERFile, "SDTEST.BIN", FA_READ);
    }

    if(result == FR_OK)
    {
        start = HAL_GetTick();

        for(uint32_t offset = 0U; (offset < SD_TEST_FILE_BYTES) && (result == FR_OK)
                && (badOffset == 0xFFFFFFFFU); offset += sizeof(buffer))
        {
            result = f_read(&USERFile, buffer, sizeof(buffer), &done);

            if((result == FR_OK) && (done != sizeof(buffer)))
            {
                result = FR_INT_ERR;    // file shorter than written
            }

            for(uint32_t i = 0U; (i < sizeof(buffer)) && (result == FR_OK); i++)
            {
                if((buffer[i] != SD_TestPattern(offset + i)) && (badOffset == 0xFFFFFFFFU))
                {
                    badOffset = offset + i;
                }
            }
        }

        readMs = HAL_GetTick() - start + 1U;
        (void)f_close(&USERFile);
    }

    if(result != FR_OK)
    {
        snprintf(text, sizeof(text), "SD test err %d", (int)result);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(badOffset != 0xFFFFFFFFU)
    {
        snprintf(text, sizeof(text), "SD bad at 0x%05lX", badOffset);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        // kB/s = 256 KB / seconds
        snprintf(text, sizeof(text), "W%lu R%lu kB/s",
                (SD_TEST_FILE_BYTES / 1024UL) * 1000UL / writeMs,
                (SD_TEST_FILE_BYTES / 1024UL) * 1000UL / readMs);
        ILI9341_WriteString(5, 175, text, Font_11x18, ILI9341_GREEN, ILI9341_BLACK);
    }
}


/*
 * SD card check, run once at boot after the PSRAM check. Mounts the card
 * and shows its type and size. Hold PLAYBACK at power-up to also run the
 * 256 KB write/read speed test.
 */
static void SD_BootCheck(void)
{
    char text[24];
    FRESULT result;
    uint8_t speedTest = (HAL_GPIO_ReadPin(PLAYBACK_BUTTON_GPIO_Port, PLAYBACK_BUTTON_Pin) == GPIO_PIN_RESET);

    // 1 = mount now, so a missing card shows up here rather than on first use
    result = f_mount(&USERFatFs, USERPath, 1);

    if(result == FR_NOT_READY)
    {
        if(SD_GetInitStatus() == SD_ERR_NO_CARD)
        {
            ILI9341_WriteString(5, 150, "SD: no card", Font_11x18, ILI9341_RED, ILI9341_BLACK);
        }
        else
        {
            snprintf(text, sizeof(text), "SD: init fail %d", (int)SD_GetInitStatus());
            ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
        }
    }
    else if(result == FR_NO_FILESYSTEM)
    {
        // exFAT cards (64 GB+) land here: FatFS is built without exFAT
        ILI9341_WriteString(5, 150, "SD: not FAT32", Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else if(result != FR_OK)
    {
        snprintf(text, sizeof(text), "SD: mount err %d", (int)result);
        ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_RED, ILI9341_BLACK);
    }
    else
    {
        snprintf(text, sizeof(text), "SD %s %luMB",
                (SD_GetCardType() == SD_CARD_V2_HC) ? "SDHC" : "SDSC",
                SD_GetSectorCount() / 2048UL);
        ILI9341_WriteString(5, 150, text, Font_11x18, ILI9341_GREEN, ILI9341_BLACK);

        if(speedTest)
        {
            SD_SpeedTest();
        }
    }

    HAL_Delay(1500U);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* CubeMX writes HSI48CalibrationValue = 32, but the G4's CRS TRIM is 7 bits
   * with 64 as the middle; 32 would start HSI48 ~4-5% slow. TRIM is read-only
   * while auto-trim runs, so pause it to write the value. */
  CLEAR_BIT(CRS->CR, CRS_CR_AUTOTRIMEN);
  MODIFY_REG(CRS->CR, CRS_CR_TRIM, RCC_CRS_HSI48CALIBRATION_DEFAULT << CRS_CR_TRIM_Pos);
  SET_BIT(CRS->CR, CRS_CR_AUTOTRIMEN);

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_DAC1_Init();
  MX_TIM3_Init();
  MX_USB_Device_Init();
  MX_SPI2_Init();
  MX_ADC2_Init();
  MX_TIM2_Init();
  MX_SPI1_Init();
  MX_RTC_Init();
  if (MX_FATFS_Init() != APP_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN 2 */

  // Enable Flush-to-Zero (FZ) mode on the Cortex-M4 FPU
  // This prevents denormal numbers from causing CPU lag e.g. in the  biquad filter
  __set_FPSCR(__get_FPSCR() | (1UL << 24));

  ADC_Init();
  DSP_Init();
  BatDetect_Init();
  DAC_Init();
  FFT_Init();


  // MX_GPIO_Init leaves every SPI chip-select low. Deselect SD and PSRAM
  // before the first SPI2 transfer so the SD card doesn't see display traffic.
  HAL_GPIO_WritePin(SPI2_SD_CARD_CS_GPIO_Port, SPI2_SD_CARD_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SPI1_PSRAM_CS_GPIO_Port, SPI1_PSRAM_CS_Pin, GPIO_PIN_SET);
  ILI9341_Unselect();
  ILI9341_Init();

  HAL_GPIO_WritePin(DISPLAY_LED_GPIO_Port, DISPLAY_LED_Pin, GPIO_PIN_SET); // turn screen on
  ILI9341_FillScreen(ILI9341_BLACK);

  // Hold RECORD at power-up for the full 8 MB test. Waterfall_Init clears
  // the result off the screen afterwards.
  if(PSRAM_BootCheck() == PSRAM_OK)
  {
      // From here SPI1 belongs to the ring, which keeps the last 16.8 s of
      // raw audio. Started before the ADC, so ring block 0 is the first block.
      PsramRing_Status_t ringStatus = PsramRing_Start();

      if(ringStatus != PSRAM_RING_OK)
      {
          ILI9341_WriteString(5, 140, (ringStatus == PSRAM_RING_ERR_PRIORITY) ? "Ring: NVIC prio" : "Ring: SPI1 config",
                  Font_11x18, ILI9341_RED, ILI9341_BLACK);
          HAL_Delay(5000);
      }
  }

  // Hold PLAYBACK at power-up for the SD write/read speed test
  SD_BootCheck();

  Waterfall_Init();
  Waterfall_SetRange(-10.0f, 20.0f);


  ADC_SetHalfBufferCallback(DSP_ProcessBlock);
  ADC_SetFullBufferCallback(DSP_ProcessBlock);

  DSP_SetOutputCallback(DAC_WriteBlock);

  HAL_TIM_Base_Start(&htim2);

  DAC_Start();


  ADC_Start();

  // TS4990 is held in standby by R9 until AMP_EN goes high. Enable it only
  // once the DAC is already outputting mid-scale, to avoid a turn-on pop.
  HAL_GPIO_WritePin(AMP_EN_GPIO_Port, AMP_EN_Pin, GPIO_PIN_SET);


  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);

  uint32_t frequency = 0;

  static uint16_t lastADC = 0;
  static uint32_t lastRead = 0;
  char text[32];

  uint32_t last_flush_tick = HAL_GetTick();
  uint32_t last_vbat_tick = HAL_GetTick();
  uint32_t vbat_sum = 0;
  uint32_t vbat_count = 0;

  uint32_t lastPulseCount = 0;
  uint32_t pulseLedOffTick = 0;

  // Starts as "held", so a press left over from the power-up SD test has
  // to be released before it counts
  uint8_t playbackButton = 1;
  uint8_t playbackDebounce = 0;
  uint32_t playbackLabel = 0;           // seconds shown in the playback label; 0 = LO label shown
  uint32_t playbackMessageUntil = 0;

  Waterfall_SetLOFrequency(frequency);


  while (1)
  {

	    if(HAL_GetTick() - lastRead >= 10)
	    {
	        lastRead = HAL_GetTick();

	        // Use the scan started on the previous tick, then start the next one.
	        // No result yet means no change.
	        uint16_t adc = lastADC;

	        if(ADC2_ScanReady())
	        {
	            adc = ADC2_Get(ADC2_LO_POT);

	            DSP_SetVolume((float)ADC2_Get(ADC2_VOLUME) / 4095.0f);

	            vbat_sum += ADC2_Get(ADC2_VBAT);
	            vbat_count++;
	        }

	        ADC2_StartScan();

	        if(abs((int)adc - (int)lastADC) > 16)
	        {
	            frequency =((uint32_t)adc * AUDIO_LO_MAX_HZ) / 4095U;

	            DSP_SetMixerFrequency((float)frequency);
	            LO_SetFrequency(frequency);

	            lastADC = adc;

	            // the playback label has the spot while it's up
	            if(playbackLabel == 0)
	            {
	                ShowLoLabel(frequency);
	            }
	        }

	        // PLAYBACK: replay the last 2 s ten times slower; press again to stop
	        uint8_t playbackDown = (HAL_GPIO_ReadPin(PLAYBACK_BUTTON_GPIO_Port, PLAYBACK_BUTTON_Pin) == GPIO_PIN_RESET);

	        if(playbackDown == playbackButton)
	        {
	            playbackDebounce = 0;
	        }
	        else if(++playbackDebounce >= PLAYBACK_DEBOUNCE_TICKS)
	        {
	            playbackButton = playbackDown;
	            playbackDebounce = 0;

	            if(playbackButton && Playback_IsActive())
	            {
	                Playback_Stop();
	            }
	            else if(playbackButton)
	            {
	                Playback_Status_t status = Playback_Start();

	                playbackLabel = UINT32_MAX;         // redraw now
	                playbackMessageUntil = lastRead;

	                if(status != PLAYBACK_OK)
	                {
	                    ILI9341_WriteString(5, 5, (status == PLAYBACK_ERR_BUSY) ? "PLAY: busy   " : "PLAY: no ring",
	                            Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
	                    playbackMessageUntil = lastRead + PLAYBACK_MESSAGE_MS;
	                }
	            }
	        }

	        // Playback label: "PLAY 10x  20s", counting down. Put the LO
	        // readout back once playback (or an error message) is over.
	        if(Playback_IsActive())
	        {
	            uint32_t seconds = (Playback_GetRemainingMs() + 999U) / 1000U;

	            if(seconds != playbackLabel)
	            {
	                playbackLabel = seconds;
	                snprintf(text, sizeof(text), "PLAY %lux %3lus", (unsigned long)PLAYBACK_EXPANSION, seconds);
	                ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
	            }
	        }
	        else if((playbackLabel != 0) && ((int32_t)(lastRead - playbackMessageUntil) >= 0))
	        {
	            playbackLabel = 0;
	            ShowLoLabel(frequency);
	        }

	        // Bat detector, stage 1 (monitor only, the amp stays on):
	        // LED0 = bat pass in progress, LED1 = flash for each bat-like pulse
	        HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin,
	                BatDetect_IsBatPresent() ? GPIO_PIN_SET : GPIO_PIN_RESET);

	        uint32_t pulseCount = BatDetect_GetPulseCount();

	        if(pulseCount != lastPulseCount)
	        {
	            lastPulseCount = pulseCount;
	            pulseLedOffTick = lastRead + PULSE_LED_MS;
	            HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
	        }
	        else if((int32_t)(lastRead - pulseLedOffTick) >= 0)
	        {
	            HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
	        }
	    }

	    // USB debug capture: act on PC commands, send any data that's ready
	    DebugLink_Poll();

	    FFT_Process();

	    if (fftMagnitudeReady) {
	        fftMagnitudeReady = 0;
	        Waterfall_Accumulate(fftMagnitude, FFT_NUM_BINS);
	    }

	    uint32_t now = HAL_GetTick();
	    if ((now - last_flush_tick) >= 40) {
	        Waterfall_FlushRow();
	        last_flush_tick = now;
	    }

	    // Battery: average the 10 ms readings over each second. Ratiometric to
	    // VDDA, so it reads high once the MCP1700 drops out near empty.
	    if ((now - last_vbat_tick) >= VBAT_UPDATE_MS && vbat_count > 0) {
	        uint32_t vbat_mv = (vbat_sum / vbat_count) * VDDA_MV * VBAT_DIVIDER / 4095U;
	        uint32_t tenths = (vbat_mv + 50U) / 100U;

	        snprintf(text, sizeof(text), "%lu.%lu", tenths / 10U, tenths % 10U);
	        ILI9341_WriteString(VBAT_TEXT_X, VBAT_TEXT_Y, "BAT", Font_7x10, ILI9341_WHITE, ILI9341_BLACK);
	        ILI9341_WriteString(VBAT_TEXT_X, VBAT_TEXT_Y + 12U, text, Font_7x10, ILI9341_WHITE, ILI9341_BLACK);

	        vbat_sum = 0;
	        vbat_count = 0;
	        last_vbat_tick = now;
	    }


    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_CRSInitTypeDef pInit = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable the SYSCFG APB clock
  */
  __HAL_RCC_CRS_CLK_ENABLE();

  /** Configures CRS
  */
  pInit.Prescaler = RCC_CRS_SYNC_DIV1;
  pInit.Source = RCC_CRS_SYNC_SOURCE_USB;
  pInit.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  pInit.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,1000);
  pInit.ErrorLimitValue = 34;
  pInit.HSI48CalibrationValue = 32;

  HAL_RCCEx_CRSConfig(&pInit);
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T2_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.GainCompensation = 0;
  hadc2.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.NbrOfConversion = 3;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc2.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_2;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_13;
  sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_3;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief DAC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_DAC1_Init(void)
{

  /* USER CODE BEGIN DAC1_Init 0 */

  /* USER CODE END DAC1_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC1_Init 1 */

  /* USER CODE END DAC1_Init 1 */

  /** DAC Initialization
  */
  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK)
  {
    Error_Handler();
  }

  /** DAC channel OUT1 config
  */
  sConfig.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;
  sConfig.DAC_DMADoubleDataMode = DISABLE;
  sConfig.DAC_SignedFormat = DISABLE;
  sConfig.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  sConfig.DAC_Trigger = DAC_TRIGGER_T2_TRGO;
  sConfig.DAC_Trigger2 = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  sConfig.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
  sConfig.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DAC1_Init 2 */

  /* USER CODE END DAC1_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  hrtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.SubSeconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 0x1;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 7;
  hspi2.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi2.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 679;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  // pipeline_config.h is the authoritative sample rate: catch a CubeMX
  // TIM2 period that no longer matches it
  if (htim2.Init.Period != (AUDIO_TIMER_PERIOD - 1U))
  {
    Error_Handler();
  }

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 3777;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  /* DMA1_Channel2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
  /* DMA1_Channel3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
  /* DMA2_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Channel1_IRQn);
  /* DMA2_Channel2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Channel2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA2_Channel2_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, SPI1_PSRAM_CS_Pin|SPI2_SD_CARD_CS_Pin|AMP_EN_Pin|DISPLAY_RESET_Pin
                          |DISPLAY_DC_Pin|LED1_Pin|LED0_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, SPI2_DISPLAY_CS_Pin|DISPLAY_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : RECORD_BUTTON_Pin PLAYBACK_BUTTON_Pin */
  GPIO_InitStruct.Pin = RECORD_BUTTON_Pin|PLAYBACK_BUTTON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : SPI1_PSRAM_CS_Pin SPI2_SD_CARD_CS_Pin */
  GPIO_InitStruct.Pin = SPI1_PSRAM_CS_Pin|SPI2_SD_CARD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : AMP_EN_Pin DISPLAY_RESET_Pin DISPLAY_DC_Pin LED1_Pin
                           LED0_Pin */
  GPIO_InitStruct.Pin = AMP_EN_Pin|DISPLAY_RESET_Pin|DISPLAY_DC_Pin|LED1_Pin
                          |LED0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : SPI2_DISPLAY_CS_Pin */
  GPIO_InitStruct.Pin = SPI2_DISPLAY_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SPI2_DISPLAY_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DISPLAY_LED_Pin */
  GPIO_InitStruct.Pin = DISPLAY_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DISPLAY_LED_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

// LO readout at the top left. Fixed 13-char width: ends at x=213, clear of
// the battery readout. The playback label uses the same spot.
static void ShowLoLabel(uint32_t frequency)
{
    char text[32];

    snprintf(text, sizeof(text), "LO: %3lu.%lu kHz", frequency / 1000, (frequency % 1000) / 100);
    ILI9341_WriteString(5, 5, text, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
