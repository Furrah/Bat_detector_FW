# Bat detector: engineering diary

This diary records why the design is the way it is. Each entry covers:

- what was decided;
- the situation that forced the decision;
- the reasoning, with numbers;
- what else was considered;
- how it turned out;
- the lesson to carry into new designs.

Entries are in order, with the newest at the bottom. Decisions that are agreed but not built yet are marked **(planned)**. A summary of lessons and a template for new entries are at the end.

---

## 2026-10-29: Starting the V2 port

### 1. Port first, features second
- **Decision:** get the existing V1 firmware (heterodyne, FFT waterfall) running on the V2 board before adding PSRAM/SD recording or time expansion.
- **Context:** Many pins moved between V1 and V2. The display, the SPI buses, the buttons and the LEDs all changed. V2 also adds new hardware: PSRAM on its own bus, the SD card sharing the display's bus, and an amplifier enable line.
- **Why:** It separates "is the board right?" from "is the new code right?". The bring-up order added one unknown at a time: clocks + LED → display → ADC/DSP/DAC + amplifier → ADC2 knobs → PSRAM → SD.
- **Outcome:** Every problem we hit had one obvious suspect: the volume pot wiring, then the SPI pin speed.
- **Lesson:** Order the bring-up so each step adds only one new unknown.

### 2. Regenerate from the .ioc, and name pins after the schematic
- **Decision:** Change pins in CubeMX and regenerate, rather than editing generated code. User code goes only inside `USER CODE` blocks. Rename pin labels to match the schematic: `RECORD_BUTTON`, `PLAYBACK_BUTTON`, `LED0`, `SPI2_SD_CARD_CS`. Delete the leftover V1 pin PB9.
- **Context:** `main.c`/`main.h` were still generated from the V1 .ioc. Several labels didn't match the schematic (`LED_0`, `SPI1_SD_CARD_CS` for a pin on SPI2).
- **Why:** The generated macros (e.g. `LED0_Pin`) are the interface between CubeMX and the code. With matching names, a wrong pin shows up as a compile error instead of silently driving the wrong pin. The leftover PB9 `RECORD_BUTTON` would have compiled, then read an unconnected pin forever.
- **Lesson:** A compile error is the cheapest bug there is. Choose names so that wrong code fails to build.

### 3. LSE off, RTC clocked from HSE/32
- **Decision:** Disable the 32.768 kHz oscillator (LSE) and clock the RTC from the 16 MHz crystal divided by 32 (500 kHz).
- **Context:** The 32.768 kHz crystal Y1 is marked DNP (do not populate) on the schematic. With LSE enabled and no crystal, `HAL_RCC_OscConfig` times out after 5 s. `Error_Handler` then disables interrupts and loops forever, so the board looks dead.
- **Trade-off:** The RTC stops when the power switch is off, so WAV files can't carry real timestamps until Y1 is fitted. The RTC prescalers must also be set for 500 kHz: asynchronous 124, synchronous 3999, because (124+1)×(3999+1) = 500,000. CubeMX's defaults (127/255) are for 32.768 kHz. *Still to do.*
- **Lesson:** Every oscillator enabled in CubeMX is a boot dependency. Check DNP marks on the BOM against the clock configuration.

### 4. Deselect every chip-select before the first SPI transfer
- **Decision:** In user code, drive the SD and PSRAM chip-selects high before the display is initialised. CubeMX should also set these pins' output level to High.
- **Context:** `MX_GPIO_Init` drives every output low unless told otherwise. The PSRAM chip-select has a 100k pull-up on the board, but the GPIO overrides it. The SD chip-select has no pull-up at all. With the display and SD chip-selects both low, the SD card would see the display's start-up traffic.
- **Lesson:** On a shared bus, every chip-select must be defined from the first instruction. Pull-ups on chip-select lines keep devices quiet during reset and programming, before firmware runs.

### 5. Amplifier enabled last
- **Decision:** Drive `AMP_EN` high only after `DAC_Start()`, when the DAC is already outputting mid-scale (2048).
- **Context:** The TS4990 amplifier's standby pin has a 100k pull-down (R9), so the amp is off by default. That's a good hardware choice. But nothing drove `AMP_EN` high, so V2 was silent.
- **Why:** The amp wakes up to a steady mid-rail input, so there's no turn-on pop.
- **Lesson:** Anything that makes noise or draws current should be off by default in hardware, and enabled by firmware once its input is stable.

### 6. APB1 bus clock at /2 so the SD card can initialise
- **Decision:** Run the APB1 bus at 85 MHz (/2). SPI2 then gives 332 kHz at /256 for SD start-up, and 21.25 MHz at /4 for data and the display (the same speed as V1).
- **Context:** SD cards must be initialised at 400 kHz or less. SPI2 is clocked from APB1. At 170 MHz its slowest setting, /256, is 664 kHz, which is too fast.
- **Why it's safe:** STM32 timers receive double the bus clock whenever the bus prescaler isn't 1. So TIM2, the sample clock, stays at 170 MHz.
- **Lesson:** Check the *slowest* clock a peripheral can produce, not just the fastest.

---

## 2026-10-29/30: Inputs and outputs

### 7. ADC2: one DMA scan for the three knobs and sensors
- **Decision:** ADC2 scans the LO pot (IN2), volume (IN13) and battery (IN3) as three ranks, started by software every 10 ms. Results arrive by DMA: DMA1 channel 3, Normal mode, low priority, interrupt priority 3 (below the audio DMA at 0). Each channel samples for 247.5 cycles.
- **Why DMA rather than polling:** In scan mode the ranks convert back-to-back, about 6 µs each. If the audio interrupt landed between two polls, the next result would overwrite one not yet read. DMA can't miss a result.
- **Why long sampling:** The battery divider (1 MΩ/1 MΩ) has a 500 kΩ source impedance. The 100 nF capacitor on that pin supplies the charge for each conversion. At 3 conversions every 10 ms, 247.5 cycles costs nothing.
- **Why not the "ADC1 and ADC2 global interrupt":** Completion is signalled by the DMA interrupt. The shared ADC interrupt would only add overrun reporting.
- **Guards:** `ADC_Init` stops if the CubeMX rank count ever changes. ADC2 is now calibrated at start-up; V1 never calibrated it.
- **Lesson:** Put slow housekeeping on its own ADC and DMA channel, at low priority, so it can never disturb the real-time path.

### 8. Volume as square-law digital gain
- **Decision:**
  - The gain is (knob position)², from 0 to 1.
  - It's applied in `DSP_ProcessBlock` before the float-to-integer conversion, and read once per block.
  - Output stays silent until the first knob reading.
- **Why square law:** Hearing is logarithmic. With a linear gain, all the audible change happens at the bottom of the knob's travel. With square law, half-way on the knob gives a gain of 0.25 (−12 dB).
- **Why before the conversion to integer:** Scaling a value that's already been truncated to an integer throws away resolution at low volume.
- **Hardware note:** VR1's datasheet pinout was misleading, and the wiper was on a different pin. Fixed on V2 with a solder bridge; the footprint needs fixing for V3.
- **Lesson:** Check a part's pinout against the physical part before laying out, not only against the datasheet.

### 9. Battery readout layout, and a known accuracy limit
- **Decision:** Show "BAT" above "3.9" as two small lines at the top right of the screen, averaged over 1 s.
- **Why two lines:** The LO label is 13 characters × 16 px starting at x=5, so it ends at x=213. That leaves 26 px. "3.9V" in the 7-px font needs 28 px, and the display driver wraps once x + width ≥ 240.
- **Known limit:** The reading is ratiometric to VDDA, the 3.3 V regulator output. Once the MCP1700 regulator drops out near empty, VDDA falls with the battery. So the gauge reads high exactly when the battery is low. The fix needs the chip's internal reference (VREFINT), which is only on ADC1, the audio ADC. Deferred.
- **Lesson:** When measuring a supply, ask what the ADC reference does at the end of the range you care about.

---

## 2026-10-30: PSRAM bring-up

### 10. A register-level, interrupt-masked PSRAM driver
- **Decision:** Drive the SPI1 registers directly, with interrupts masked for the length of each burst.
- **Context:** The APS6404L PSRAM can keep chip-select low for at most tCEM = 8 µs, because it can't refresh itself while selected. 8 µs at 42.5 MHz is 340 clocks.
- **Why not the HAL:** At `-O0`, `HAL_SPI_Transmit`/`Receive` cost several microseconds per call. That uses most of the 8 µs before any data moves.
- **Why mask interrupts:** An audio interrupt in the middle of a burst would hold chip-select low for hundreds of microseconds. Masking interrupts for about 5 µs is harmless against a 1.024 ms audio block.
- **Details:**
  - Reads use the Fast Read command `0x0B`; plain Read `0x03` is limited to 33 MHz.
  - The data register is accessed as 8 bits, because a 16-bit write would send two bytes.
  - No burst crosses a 1 KB page.
- **Lesson:** When a device has a maximum select time, the driver's timing is part of meeting the spec. Design for it explicitly.

### 11. Burst size worked out from the disassembly
- **Decision:** 8 data bytes per burst.
- **Reasoning:** The `-O0` disassembly showed about 80 cycles per byte: 32 on the wire and about 45 of stack loads and stores. A read is 8 data bytes + 5 command bytes = 13 bytes, about 6.2 µs. With 16 data bytes it would be about 10 µs, over the 8 µs limit.
- **Outcome:** Chip-select-low time measured 5.0 µs at `-O0`, 5.8 µs at `-O0` once timeouts were added (entry 14), and 4.1 µs at `-Og`.
- **Lesson:** For tight timing, read the generated code and count instructions, then measure.

### 12. Measure the chip-select time on every burst
- **Decision:** Time every burst with the CPU's cycle counter (DWT) and show the worst case on the boot screen, in red if it's over 8 µs.
- **Why:** At room temperature the PSRAM tolerates tCEM violations, so a passing data test proves nothing about timing. Only measuring the select time does.
- **Lesson:** When a test can pass while the design is out of spec, measure the spec directly.

### 13. ID read at 21 MHz, tests at 42.5 MHz
- **Decision:** Reset the PSRAM and read its ID at 21 MHz, then run the data tests at the full 42.5 MHz.
- **Why:** If the ID is correct at 21 MHz but the data fails at 42.5 MHz, the fault is signal integrity, not driver logic. One boot separates the two kinds of fault.
- **Outcome:** That's exactly the pattern that pointed to the real fault (entry 14).

### 14. The PSRAM hang: SPI pins left at Low GPIO speed
- **Symptom:** The ID read was correct, then the board hung.
- **Diagnosis:** Suspending in the debugger showed it stuck in the wait for RXNE (byte received) on the first byte at 42.5 MHz. The STM32 SPI master's receiver is clocked by SCK as it comes back in through the pin. A Low-speed pin can't toggle at 42.5 MHz, so the 8 clock edges never arrive. Setting the pins to High speed fixed it.
- **Changes that followed:**
  - Every wait loop now has a deadline, set once per burst, so the normal path costs almost nothing. A bus fault shows `SPI timeout SR=…` on screen instead of freezing the boot.
  - Never open the SPI data register in the debugger's register view. The debugger's read takes the byte off the receive FIFO, which can cause the same hang artificially.
- **Lesson:** Set the GPIO speed of every fast peripheral pin; CubeMX defaults to Low. Every wait loop needs a timeout.

### 15. Address-line test as well as a pattern test
- **Decision:**
  - **Every boot:** a walking-ones address-line test. It writes only to power-of-two addresses and takes about 4 ms.
  - **On demand (hold RECORD at power-up):** a full 8 MB pattern test followed by its inverse, taking about 35 s at `-O0`.
- **Why:** A stuck or shorted address line makes two addresses share the same cells. A plain data pattern can still read back correctly from shared cells. The walking-ones test catches address lines that are stuck high, stuck low or shorted together. The full test checks that every bit holds both 0 and 1.
- **Outcome:** ID 0D/5D, address test OK.

---

## 2026-10-30: Sample rate and the processing pipeline

### 16. Sample rate of exactly 250 kHz
- **Decision:** Sample at 250 kHz exactly: 170 MHz / 680 (TIM2 auto-reload 679).
- **Context:** The detector must reach 100 kHz, but at 192.09 kHz the Nyquist limit is 96 kHz. There's also no real anti-alias filter. The 33 Ω + 1 nF at the ADC pin cuts off at 4.8 MHz, because it's a charge reservoir for the ADC, not a filter. The op-amp passes signals up to about 360 kHz. So a 110 kHz call would have appeared at 82 kHz.
- **Why exactly 250 kHz:** The WAV header stores an integer sample rate, and bat-ID software uses it.
- **Costs:**

  | | 192.09 kHz | 250 kHz |
  |---|---|---|
  | CPU cycles per sample | 885 | 680 |
  | Data rate to PSRAM/SD | 384 kB/s | 500 kB/s |
  | PSRAM history | 21.8 s | 16.8 s |

- **Lesson:** Choose the sample rate from the requirement and the clock tree together. Integer divisors give exact rates.

### 17. `pipeline_config.h`: one source of truth
- **Decision:** One header holds the sample rate, block size, FFT length and LO limit.
  - The module constants (`ADC_BUFFER_SIZE`, `SAMPLE_RATE`, …) are aliases of it.
  - The compiler checks two things: that the sample rate is a whole number, and that the LO stays below Nyquist.
  - At start-up, the firmware checks that CubeMX's TIM2 period still matches the header.
- **Context:** The sample rate existed three times with three different values (192060, 192000, and the real 192,090.4), and block sizes were defined in three files.
- **Lesson:** If two files must agree, make one derive from the other and let the compiler check it.

### 18. LO up to 120 kHz, with a fixed-width label
- **Decision:** The LO's maximum is now 120 kHz, just under the 125 kHz Nyquist limit. The label is a fixed 13 characters (`LO: 120.0 kHz`).
- **Why:**
  - It fits beside the battery readout.
  - It no longer leaves stale characters on screen when the number gets shorter.
  - The second decimal place was noise anyway, since the pot moves in steps of about 30 Hz.

### 19. Measure the DSP load
- **Decision:** Time every `DSP_ProcessBlock` call with the cycle counter and keep the worst case as a percentage of the block period (`dspLoadMaxPercent`).
- **Outcome:**
  - It showed 101% at 250 kHz with `-O0`: the interrupt was overrunning its deadline.
  - At 192 kHz the same code would have been at about 78%, so V1 was already near the edge without anyone knowing.
- **Lesson:** Instrument real-time code before adding more work to it.

### 20. Debug build compiled at `-Og`
- **Decision:** Compile the Debug configuration with `-Og` instead of `-O0`.
- **Why it's faster:** At `-O0` every variable lives in RAM and is reloaded on every use. `Biquad_Process`, for example:

  | | `-O0` | `-Og` |
  |---|---|---|
  | Instructions | 55 | 24 |
  | Of which RAM loads/stores | ~39 | 13 |
  | Filter pointer reloaded from the stack | 15 times | never |

- **Results:**

  | | `-O0` | `-Og` |
  |---|---|---|
  | DSP load | 101% (687 cycles/sample) | 37% (~252 cycles/sample) |
  | Flash used | 95.2 KB | 66.8 KB |
  | PSRAM chip-select time | 5.8 µs | 4.1 µs |

- **What we gave up:** Some local variables show as "optimized out" in the debugger, and single-stepping can jump between lines. The filter output is bit-for-bit identical, because it's the same operations in the same order.
- **Requirement this creates:** Every variable shared between interrupts and the main loop must be `volatile`. Checked, and they all are.
- **Lesson:** `-O0` is a debugging aid, not a neutral baseline. Measure real-time margins in the build you actually run.

---

## 2026-10-30: SD card

### 21. SD driver: SPI mode, HAL calls, in `FATFS/Target`
- **Decision:** Write the SD driver for SPI mode, using HAL SPI calls, and put it in `FATFS/Target`.
- **Why each choice:**
  - **SPI mode:** the G431 has no dedicated SD-card peripheral (SDMMC).
  - **HAL instead of registers:** SD cards have no maximum select time, and the display on the same bus uses the HAL. One access style per bus avoids surprises.
  - **`FATFS/Target`:** both include lists already reach it, and it's where ST puts FatFS disk drivers.
- **Details:**
  - Every wait is bounded: 1 s for card start-up, 500 ms for a busy card, 200 ms for a read's start token.
  - MOSI is held high (0xFF) while reading.
  - There's one extra clock after deselecting, so the card releases MISO for the display.
  - A pre-erase hint (`ACMD23`) is sent before multi-sector writes.
- **FatFS settings:**
  - `_FS_TINY` is on, saving 512 B of RAM.
  - Long file names and exFAT are off, to save RAM and flash. So cards must be FAT32, which cards up to 32 GB are by default.
- **Status:** There's no card yet, so only the "no card" path has been tested. A pull-up on SD MISO (PB14) is recommended so an empty slot reads 0xFF.

### 22. Boot self-tests
- **Decision:** Every boot runs the PSRAM ID and address test and an SD mount. Holding a button at power-up runs a longer test: RECORD for the full PSRAM test, PLAYBACK for the SD speed test.
- **Why:** All of it runs before the audio starts, so the buses are idle. Problems then show up on screen without needing a debugger.
- **Lesson:** Built-in self-tests make faults visible in the field.

---

## Process and tooling

### 23. Checkpoint commits on a `v2` branch
- **Decision:** Make a checkpoint commit before big changes, on a `v2` branch.
- **Why:** Each step then appears as a reviewable diff, and V1 stays buildable from the `dev`/`main` history.

### 24. Keep unused modules, disconnect them
- **Decision:** `ili9341_touch` and `tim3_lo` stay in the project; only their includes and calls were removed.
- **Why:** Both may be reused on future builds. The TIM3 waveform technique is written up in `Docs/timer_waveform_output.md`.

### 25. Build-system traps found
- **`main.c` has its own include list in `.cproject`.** It's a per-file override, so new header folders must be added there as well as at project level.
- **CubeIDE generates the Debug makefiles.** New files only appear in them after a build inside the IDE.
- **GCC 14 treats calling an undeclared function as an error.** Removing an include means removing its calls too.
- **The .map file lists garbage-collected symbols under "Discarded input sections".** They use no memory, so don't count them when budgeting RAM.

---

## 2026-10-30: RAM reclaim

### 26. Constant tables moved from RAM to flash, generated by a script
- **Decision:** Move the Hann window (4 KB), FFT twiddles (2 KB) and heterodyne sine table (1 KB) out of RAM and into flash as `const` tables. `Tools/gen_dsp_tables.py` (Python) generates them into `Core/DSP/Tables/`.
- **Context:** Recording needs about 5.5 KB more RAM: a 4 KB SD write buffer, PSRAM DMA buffers and a bigger stack. Only 5.5 KB was free. These tables were calculated at boot and never written again.
- **Why a script rather than hand-typed tables:**
  - The tables can be reproduced exactly.
  - The script reads `AUDIO_FFT_LENGTH` and `LUT_SIZE`, and static asserts in `fft.c`/`dsp.c` fail the build if a table is stale.
- **Why the script copies the firmware's float32 arithmetic:** It computes the angle in double, rounds to float, then takes the sine or cosine. The values therefore match the old boot-time tables to within one float LSB, so the behaviour and the waterfall calibration don't move.
- **Trade-off:** Flash reads have wait states at 170 MHz, partly hidden by the flash accelerator. The oscillator's sine lookup and the FFT window read now come from flash for every sample, so the DSP load may change by a percent or so. *Measure it.*
- **Outcome:** RAM −7 KB, flash +7 KB. DSP load went from 37% to 36%, so reading the sine and window from flash cost nothing measurable.
- **Lesson:** Anything calculated once at boot and never written again is a constant. It belongs in flash.

### 27. The DAC writes straight into the free half of its buffer
- **Decision:** `DAC_WriteBlock` picks the half of the circular DAC buffer that the DMA isn't reading, and copies the block there. It decides from the DMA's remaining count (CNDTR): more than 256 left means the DMA is still in the first half. The queue and the DAC interrupt callbacks are gone.
- **Context:** Each block used to be copied twice: into `dacQueue` in the ADC interrupt, then into the DAC buffer in the DAC interrupt. The 1 KB queue was sized as a whole buffer but only half of it was ever used.
- **Why:**
  - It saves 1 KB of RAM and one copy per block.
  - It no longer depends on the DAC and ADC interrupts firing in a particular order, or on which was started first.
- **Timing margin:** The block is written about 0.4 ms after the ADC half completes (the DSP time at 37% load). At that point the DAC is about 0.6 ms away from the half being written.
- **Guard:** `dacLateBlocks` counts blocks where the DMA moved into the half being written during the copy. It should stay at 0.
- **Lesson:** Ask the hardware where it is (here, the DMA counter) rather than assuming a timing relationship.

### 28. Stack raised to 2 KB
- **Decision:** The CubeMX minimum stack size is now 2 KB (0x800).
- **Why:** FatFS and the SD driver call deep chains of functions. The linker's reservation is only a safety check; the stack really grows into whatever RAM is free, without warning. Reserving the stack you need makes the build fail if static data ever grows into that space.
- **Lesson:** Reserve the stack you need, so the linker can warn you before the hardware fails.

### 29. FFT unpack rotation taken from a table
- **Decision:** Replace the 511 `cosf` + 511 `sinf` calls per FFT frame with a 4 KB table. Add timing of `FFT_Process` (`fftProcessCyclesAvg/Max`) and a temporary switch (`FFT_UNPACK_USE_TABLE`) so the two versions can be compared.
- **Why:** By estimate, software trig is most of `FFT_Process`'s time; the 512-point FFT itself is about 2,300 butterfly operations. A faster FFT means fewer dropped frames, so a smoother waterfall and better input for later FFT-based features such as auto-tune.
- **Outcome:** Measured on hardware (`fftProcessCyclesAvg`, wall-clock including audio interrupts):

  | Unpack method | Cycles per frame | Time per frame |
  |---|---|---|
  | `cosf`/`sinf` (old) | 981,137 | 5.77 ms |
  | Table (new) | 363,198 | 2.14 ms |

  That's 2.7× faster. The old version took longer than the 4.1 ms between frames, so the FFT could never keep up with the audio. The switch has been removed and the table version kept.
- **Lesson:** A library maths call inside a per-frame loop is a hidden cost. Tabulate it when the arguments are fixed, and measure before and after so the gain is a number, not a hope.

---

## 2026-10-30: Bat-call detector

### 30. Bat-call detector, stage 1 (monitor only)
- **Decision:** Detect bat calls continuously in the audio interrupt, from the energy of each block above 10 kHz, using a pulse-and-rhythm rule. In stage 1 the result only drives the LEDs and debug counters, and the amplifier stays on. The LEDs were repurposed for this:
  - **LED0:** a bat pass is in progress.
  - **LED1:** a 50 ms flash for every bat-like pulse.

  They replace V1's LO-band LEDs, which the screen made redundant.
- **Context:** The goal is to switch the amplifier on only when bats are present (entry P4). A detector that controls hardware has to be trusted first, so it runs in parallel with what you hear and the waterfall until it's tuned.
- **Why measure block energy rather than use the FFT:** The FFT only sees frames the main loop has time to process. Even at 2.14 ms per frame it can drop frames, and a 1–5 ms *Myotis* call can fall into a gap. Block energy sees every call, and costs one multiply-add per sample on the existing high-pass output.
- **Why 10 kHz as the lower band edge:** It keeps the free-tailed bat (9–14 kHz), the greater noctule (11.7–13.9 kHz) and social calls (14–22 kHz), which `Docs/European Bat Calls … .md` says not to reject. Wind and handling noise are about 40 dB down at 1 kHz.
- **The parameters, and where they come from:**

  | Parameter | Value | Reason (from the bat document) |
  |---|---|---|
  | Threshold | floor + 10 dB | starting value, to be tuned in the field |
  | Bat-like pulse | 1–100 ms | calls are 0.8 ms (long-eared) to ~30 ms (greater noctule); horseshoe CF tones run tens of ms |
  | Continuous noise | longer than 150 ms | no call lasts this long; insects and interference do |
  | Trigger | 2 pulses within 600 ms | the slowest caller, the greater noctule, calls every 364–468 ms; one click shouldn't trigger |
  | Hold | 2 s after the last pulse | about 4× the longest call interval |
  | Noise floor | falls with τ ≈ 0.25 s, rises with τ ≈ 4 s, frozen during pulses | a 10–30 ms call hardly moves it; quiet returns quickly |
  | Warm-up | 0.5 s after reset | the filters settle and the floor is learnt |

- **A subtle point:** The floor is frozen during a pulse, but is released again after 150 ms of continuous loudness. Otherwise a steady noise that started loud would leave the detector stuck "inside a pulse" forever, never learning the new floor.
- **Implementation:** `Core/DSP/bat_detect.c`, called once per block from `DSP_ProcessBlock`. All timing counts blocks (`AUDIO_BLOCK_US` = 1024 µs, from `pipeline_config.h`), so nothing in the interrupt calls `HAL_GetTick()`. The debug counters are `batEnergyDb`, `batFloorDb`, `batPulses`, `batTriggers`, `batNoiseRuns` and `batPresent`.
- **Cost:** About 0.7% CPU (estimated), 48 bytes of RAM, 0.6 KB of flash.
- **Known limits:**
  - Pulsed ultrasonic insects and jingling keys can trigger it.
  - Bats quieter than +10 dB are missed.
  - The first call of a pass never counts as a trigger.
- **Outcome:** Builds. Field tuning pending.
- **Lesson:** Stage new behaviour as "monitor only" before letting it control hardware. You learn its false-alarm and miss rates without paying for them.

### 31. USB enumeration: the CRS trim, and a USB-C socket that works one way round (Oct 30)
- **Decision:**
  - Lock HSI48 to the PC's 1 ms start-of-frame packets with the CRS (clock recovery system), set in CubeMX (CRS SYNC = USB_FS).
  - Put the CRS trim back to 64 in `main.c`'s `USER CODE SysInit` block.
  - Use the working plug orientation until J8 is reflowed.
- **Context:** The USB virtual COM port never appeared. Windows logged "Unknown USB Device (Device Descriptor Request Failed)" on every attempt.
- **Why the CRS (with numbers):**
  - Full-speed USB needs its clock within ±0.25%.
  - USB runs from HSI48, because the PLL can't make 48 MHz while the core runs at 170 MHz: the 340 MHz VCO divided by Q = 2/4/6/8 gives 170/85/56.7/42.5 MHz.
  - ST specifies HSI48's factory trim at about ±3% over temperature on sibling parts. That's 12× too loose, and the detector will be used outdoors at night.
- **The trim value:** CubeMX writes `HSI48CalibrationValue = 32` and won't let you change it. That's the middle of the 6-bit trim on older parts (e.g. L4). The G4's trim is 7 bits, and its own HAL defines the middle as 64 (`RCC_CRS_HSI48CALIBRATION_DEFAULT`). 32 starts the oscillator about 32 steps (~4–5%) slow, possibly too far off to decode the frame packets it needs in order to correct itself.
- **What actually fixed it: flipping the plug.** With the trim fixed it still failed. Register reads in the failing orientation:

  | Register | Value | Meaning |
  |---|---|---|
  | `USB->FNR` | 0xC000 | D+ and D− both high; no frame ever decoded |
  | `USB->DADDR` | 0 | the PC's bus reset was never seen |
  | last setup request | all zero | the board never received a request |
  | `CRS->CR` | 0x4060 | TRIM still 64: no frames to trim against |

  That fits D− not being connected in that orientation: a floating D− reads high, and without D− the board can't see the reset. Flipped, the board enumerated at once as COM8 (VID 0483, PID 5740). Suspected cause: a dry joint on J8 A7 or B7 (DN1/DN2).
- **Was the CRS needed?** Not proven: the socket alone explains the failure. It stays, because the temperature range demands it. TRIM after lock: (to be read).
- **Alternatives considered:** Run the core at 144 MHz so the PLL also makes 48 MHz (VCO 288 MHz, R = 2, Q = 6). It still gives an exact 250 kHz (576 cycles per sample), but costs 15% of the CPU. Rejected; the CRS is the standard crystal-less USB solution.
- **Outcome:** USB CDC works in one plug orientation. J8 reflow pending.
- **Lesson:** When USB fails, read the line state (`FNR` RXDP/RXDM) and whether a reset was seen (`DADDR`) before touching clocks; they separate wiring from timing in one look. Test a USB-C socket in both orientations at bring-up. Check CubeMX's generated values against the HAL's own defaults for the family.

### 32. USB debug capture: measure before tuning (Oct 30)
- **Decision:** Add a command-driven data capture over the USB virtual COM port (`Core/Src/debug_link.c`), and PC scripts to record and analyse it (`Tools/capture.py`, `Tools/analyse_capture.py`).
- **Context:** In a quiet room the detector counts false pulses, and the waterfall shows energy near 60 kHz. Tuning thresholds by eye would be guesswork. V2 has no UART pins broken out, but USB CDC was already in the .ioc.
- **Why this shape (with numbers):**
  - **No raw streaming.** Raw audio at 250 kHz × 2 bytes is 500 kB/s, and buffering it would need RAM the board doesn't have. Instead there are two capture modes:
    - **Snapshots:** 4096 consecutive raw samples (16.4 ms, 61 Hz FFT bins), on request.
    - **Energy stream:** a 20-byte record per 1.024 ms block (19.5 kB/s): the detector's input energy, the full-band energy, the floor and the counters. The analysis replays the detector offline from these.
  - **One shared 8 KB buffer.** It holds either the snapshot or a 409-record energy FIFO, so the PC can fall behind by 0.42 s before records are dropped. Drops are counted.
  - **Framing:** a header, payload and CRC-32 (zlib-compatible), with a sequence number to spot lost frames. Each frame goes out as three USB transfers, so the buffer is released only once its payload has gone. Commands are acted on only between frames, so a command never changes data that's being sent.
  - **Interrupt cost:** a 512-byte copy per block in snapshot mode, or about 256 integer multiply-adds per block in energy mode (under 1% of a block). To confirm with `dspLoadMaxPercent`.
  - **Cost:** RAM +8.3 KB (4.3 KB left), flash +1.5 KB. `DEBUG_CAPTURE 0` removes it all, ready for PSRAM/SD recording.
- **Making the analysis check itself:**
  - The spectrum is calibrated in dBFS against a full-scale sine.
  - A Python copy of the firmware's 10 kHz high-pass predicts the block energy the detector should see from the raw samples.
  - The detector replay runs in float32 and is checked against the board's own floor and pulse counts before its threshold sweep is trusted.
  - On synthetic data: a 3-code 60 kHz tone read −56.6 dBFS (expected −56.7), the predicted energy 36.93 dB (expected 36.9), and the replay reproduced the reference floor and pulse count exactly.
- **Alternatives considered:**
  - **SWO trace:** needs PB3, which is SPI1 SCK for the PSRAM.
  - **Debugger memory dumps:** slow and manual.
  - **UART:** no pins.
  - **Streaming raw audio:** too much bandwidth and RAM, as above.
- **Outcome:** Builds (no warnings with `-Wextra`); scripts tested offline. Hardware test pending.
- **Lesson:** Build the measurement path before tuning, and make the analysis prove itself against the device (the replay cross-check) before trusting its conclusions.

### 33. Quiet-room false pulses are the room, not the detector (Oct 30)
- **Decision:** Leave the detector's parameters (entry 30) unchanged, and tune them in the field.
- **Context:** The first captures with entry 32's tools: 20 raw snapshots and 60 s of block energies, indoors.
- **What the captures showed (with numbers):**
  - All 57 false pulses per minute were single 1 ms blocks, 10.0–13.9 dB above the floor: the top 0.1% of a background whose block energy spans 10 dB between its 1st and 99th percentiles.
  - **Replayed threshold sweep:**

    | Threshold | False pulses/min | False triggers/min |
    |---|---|---|
    | +8 dB | 378 | 1 |
    | +10 dB (current) | 57 | 6 |
    | +12 dB | 11 | 0 |
    | +15 dB | 0 | 0 |

  - **Where the detector's input power came from:** 76% from 20–30 kHz (a 24.95 kHz line on a 20–27 kHz hump), 9% from the 58–68 kHz hump seen on the waterfall, and 3% from the audio band. The energy was modulated at about 74 Hz, close to the display's frame rate.
  - **Cross-check:** the firmware's energy (median 45.3 dB) agreed with numpy's model of the same high-pass on the raw samples (45.0 dB).
- **Outdoors:** By eye on the waterfall, the 60 kHz hump and most of the other noise disappeared, so they were sources in the room. No outdoor capture yet.
- **Open question:** Is the 24.95 kHz line on the board (the display's ~25.6 kHz nominal line rate fits) or in the room? A display-unplugged or outdoor energy capture would settle it.
- **Lesson:** Find out where noise comes from before changing thresholds. Indoors is a poor proxy for the field: rooms are full of ultrasonic sources.

### 34. The PSRAM ring: DMA bursts, one interrupt at the top (Oct 30)
- **Decision:** Record every raw ADC block into the PSRAM continuously, with an engine driven by DMA and interrupts (`Core/Drivers/psram_ring.c`). The last 16.8 s are always available for recording and time expansion (plan P2).
- **How it works:**
  - **Bursts:** each 512-byte block is written as 16 bursts of 32 bytes. One burst is one CE# cycle, moved entirely by DMA2: TX from a staging buffer, RX draining every byte.
  - **Burst-complete interrupt:** the RX transfer-complete interrupt raises CE# first, then starts the next burst.
  - **Reads:** queued (4 deep) and run between writes; writes always go first.
- **Why these numbers:**
  - **Burst size:** a write burst is 36 bytes, which takes 6.8 µs at 42.5 MHz; a fast-read burst is 37 bytes, 7.0 µs. With the interrupt raising CE#, it's low for about 7.2 µs against tCEM = 8 µs. The margin is small, so CE# is timed on every burst (`psramRingMaxSelectNs`). If it ever exceeds 7.5 µs, 16-byte bursts (~4 µs) are a one-line change.
  - **No copy of the audio:** the bursts read straight from the ADC buffer half the DSP has just been given. The ADC is filling the other half, so the writes have 1.024 ms and need about 130 µs.
  - **Loads:** about 12% of SPI1's time and an estimated 2% of the CPU. RAM +0.3 KB, flash +2.1 KB.
- **The priority rule:** The burst-complete interrupt (DMA2 Channel 1) must be the only one at priority 0. The DSP interrupt runs for about 370 µs per block. If it, or USB, could delay the burst interrupt, CE# would stay low about 50× too long. The PSRAM can't refresh while selected, and at room temperature a data test can still pass (entry 12). `PsramRing_Start()` checks every enabled interrupt. If the priorities are wrong, it refuses to start and the boot screen shows "Ring: NVIC prio", while the rest of the board runs normally.
- **Fault handling:**
  - **Stuck burst:** a burst still in flight when the next block arrives (at least 1 ms against ~8 µs expected) resets SPI1 through RCC, fails any queued reads and counts a fault.
  - **Late block:** a block that hasn't finished writing when the next arrives is counted in `psramRingLateBlocks`.
- **Proof, end to end:** The debug link's `M` command reads the last raw snapshot's blocks back from the ring. `capture.py verify` checks them bit for bit against the snapshot, so the check covers the whole chain: ADC, write, PSRAM, read and USB.
- **Alternatives considered:**
  - **Polled writes from the audio interrupt with interrupts masked:** about 110 µs of CPU per block (11%), with everything else held off 7 µs at a time.
  - **CE# driven by a timer output on PB0:** bounds CE# in hardware whatever the interrupt latency, but is more complex. It's the fallback if the priority rule ever can't hold.
  - **Copying each block into an SRAM FIFO first:** 1 KB+ of RAM for a deadline that already has an 8× margin.
- **Outcome:** Works on hardware, after one fix (entry 35).
  - **Priorities:** set in CubeMX as required: DMA2 Ch1 alone at 0; DMA1 Ch1/Ch2, TIM6_DAC, USB and DMA2 Ch2 at 1; DMA1 Ch3 at 3.
  - **Data:** `capture.py verify --count 50` passed: 400 KB read back bit-identical, including across the ring's wrap. Four snapshots read back after being held for 15.6 s also matched. Late blocks 0, faults 0.
  - **CE# time: out of spec at first.** Every burst held CE# low for 9.1 µs against the 8 µs limit, while the data test passed. That's entry 12's warning come true: a data test at room temperature can't see a refresh violation. The cause was a CubeMX default that put 2 idle clocks after every byte (entry 35). With it off, write bursts measure 7.41–7.52 µs and none of 8.5 million exceed tCEM. Read bursts (one byte longer, expected ~7.7 µs) are still to be measured under `verify`.
  - **CPU cost: the estimate was wrong.** The DSP's measured worst-case load rose from 37% to 43–45%, not by 2%. Each burst interrupt takes 3.9–4.1 µs, and 16 per block come to about 65 µs of every 1.024 ms (6%). That's about 670 CPU cycles for a short handler. The suspects are flash wait states (4 at 170 MHz, prefetch off, the handler evicted from the 1 KB instruction cache by the DSP between bursts) and newlib-nano's byte-at-a-time `memcpy`. Not yet investigated.
  - **Diagnostics kept:** `psramRingMinSelectNs`, `psramRingMaxEntryNs` (CE# low to interrupt start), `psramRingMaxIsrNs` (CPU time per burst), `psramRingOverTcem`, `psramRingBursts`, and `psramRingResetStats` (write 1 to restart them).
- **Lesson:** When a hardware timing limit depends on interrupt latency, make the latency a checked precondition rather than an assumption, and measure the limit on every cycle. Measuring it is what caught the problem: the data test passed regardless.

### 35. SPI "NSS pulse mode": two idle clocks after every byte (Oct 30)
- **Decision:** Turn off NSS pulse mode (NSSP) on SPI1 and SPI2 in CubeMX. `PsramRing_Start()` now refuses to run if it's ever on again (boot screen: "Ring: SPI1 config").
- **Context:** The PSRAM ring (entry 34) passed its data test, but every burst held CE# low for 9.1 µs, over the PSRAM's 8 µs tCEM. The design had predicted about 7.2 µs.
- **What NSSP is:**
  - In ordinary (Motorola-format) SPI, the master pulls the chip select low, clocks any number of bytes, and releases it at the end of the transaction. Each byte, or frame, follows the last with no gap.
  - Some devices need the chip select to go high between *every* frame, because they latch each word on the rising edge of CS. Many SPI ADCs and DACs work like this. For them, STM32's SPI has NSS pulse mode. When it's on, the SPI hardware raises its own NSS pin for at least one clock period between consecutive frames, so firmware doesn't have to toggle a pin for every word.
  - To fit that pulse in, the SPI stops SCK between frames. It only applies to a master using CPHA = 0 (data captured on the first clock edge), which is the most common SPI mode and the one the PSRAM, display and SD card use.
  - The pause comes from the SPI's own sequencer, so it happens even when nothing uses the NSS pin. This board drives every chip select from a GPIO ("software NSS"), so the pulse went nowhere, but every byte still paid for it.
  - On the STM32G4 it's the NSSP bit (bit 3) of `SPIx_CR2`. CubeMX shows it as "NSSP Mode" under the SPI's parameter settings, and its default for the G4 is Enabled.
- **How it was found (with numbers):**
  - **Split the time before guessing the cause.** A diagnostic build cut each burst's CE# low time into two parts, using the cycle counter: CE# low to the start of the interrupt (the transfer plus interrupt latency), and the start of the interrupt to CE# high. It also recorded the shortest burst, and counted the bursts over tCEM.
  - **Results:** shortest 9.07 µs, longest 9.18 µs, and all 8,563,216 bursts over tCEM. CE# low to interrupt start: 8.94 µs. Inside the interrupt: about 0.24 µs.
  - **What that ruled out:** with only 112 ns between the shortest and longest burst, the cause wasn't occasional interrupt latency. With 0.24 µs in the interrupt, it wasn't the handler. The transfer itself was taking 2 µs longer than it should.
  - **The clock was right:** 16 MHz crystal ÷4 × 85 ÷2 = 170 MHz, APB2 ÷1, SPI ÷4 = 42.5 MHz.
  - **So the bytes had gaps between them,** and the generated `MX_SPI1_Init()` had `NSSPMode = SPI_NSS_PULSE_ENABLE`.
  - **After turning it off:** 7.41–7.52 µs, and none of 8.5 million bursts over tCEM. The difference, (9070 − 7405) ns over 35 byte boundaries, is 47.6 ns per byte: 2.0 periods of the 42.5 MHz clock. So each byte took 10 clocks instead of 8, and the SPI lost 20% of its throughput.
  - **Where the 7.5 µs goes now:** 6.78 µs on the wire (36 bytes × 8 bits at 42.5 MHz); about 0.5 µs for the DMA to start, drain the last byte and raise the interrupt; and about 0.24 µs inside the interrupt before CE# goes high. That leaves 0.48 µs of margin.
- **Alternatives considered:**
  - **16-byte bursts:** would have got under 8 µs even with the gaps (about 5.4 µs), but at twice the interrupts per block, and with the 20% throughput loss still there.
  - **Raising CE# sooner, or running the handler from zero-wait-state CCM RAM:** could save at most the 0.24 µs measured inside the interrupt; the extra time wasn't there.
  - **An oscilloscope on CE# and SCK:** would have shown the gaps directly. Splitting the time with the cycle counter pointed to them without one.
- **Outcome:** Write bursts were within tCEM with 0.48 µs to spare. Reads, a byte longer, reached 8.12 µs under `verify`, with 3 bursts over, which led to 16-byte bursts (entry 36). SPI2 (display and SD card) no longer pays the same 20% overhead; its speed-up isn't measured yet.
- **Lesson for future designs:**
  - When a measured time disagrees with the calculation, split it into parts and measure each one before choosing a fix. The obvious fixes here (a faster handler, smaller bursts) would have treated the wrong part.
  - Read every field the code generator writes for a peripheral, not just the ones you set. A default can be meant for a different kind of device.

### 36. 16-byte bursts, and the burst interrupt moved to RAM (Oct 30)
- **Decision:**
  - Write and read the PSRAM in 16-byte bursts instead of 32.
  - Run the burst interrupt's code from RAM, and compile `psram_ring.c` at `-O2` with a `#pragma` in the file. The rest of the Debug build stays at `-Og`.
  - Skip HAL's DMA handler after ours, with a `return` in the USER CODE block of `DMA2_Channel1_IRQHandler`.
- **Context:** With NSSP off (entry 35), write bursts took 7.4–7.5 µs. `capture.py verify` makes the ring read back 16 blocks per snapshot. During verify, the longest burst reached 8.12 µs, and 3 of about 12,800 read bursts went over tCEM.
- **Why reads went over (with numbers):**
  - A read burst is one byte longer than a write, because the fast-read command needs a wait byte. That makes it about 7.6–7.7 µs.
  - The extra ~0.45 µs (about 75 CPU cycles) came from code that masks all interrupts with `__disable_irq()`, which blocks even the priority-0 burst interrupt. Two such sections run during verify:
    - `PsramRing_ReadBlock()`, called on every main-loop pass while the read queue is full.
    - `PsramRing_PushBlock()`. In normal running it never overlaps a burst, but it can land in the middle of a read.
  - The SD writer and time expansion will read the PSRAM constantly, so this had to be fixed, not avoided.
- **Why 16-byte bursts:**
  - Time on the wire drops to 3.76 µs for a write (20 bytes) and 3.95 µs for a read (21 bytes). That leaves about 3 µs to absorb delays, roughly 10× the delay seen.
  - **Alternative: keep 32 bytes and never mask the burst interrupt.** Replace `__disable_irq()` with BASEPRI, which masks everything except priority 0, and make the read queue lock-free. That fixes today's cause, but leaves 0.3 µs of margin that every future critical section anywhere in the firmware would have to respect.
  - **Alternative: end CE# with a timer output on PB0.** That's immune to latency, but a large change.
- **The cost turned out to be large.** Halving the burst size doubles the interrupts per block. The first 16-byte build peaked at 110% DSP load during verify: a block overran its 1.024 ms deadline.
  - Each burst interrupt took 4.04 µs (measured). Add HAL's DMA handler after it, plus interrupt entry and exit: about 4.7 µs of CPU for a burst that lasts 4.5 µs.
  - With reads running back to back, the interrupt took over half the CPU, and the DSP's ~380 µs of work per block stretched past the deadline.
  - 32-byte bursts had the same problem, only milder. In normal running, writes alone cost an affordable +6%. Only the back-to-back reads exposed it.
- **Why the interrupt was so slow:**
  - **Not the copy.** I had suspected the 16-byte `memcpy`, assuming newlib-nano copies a byte at a time. The disassembly showed GCC inlines it.
  - **Size, and where it ran from.** The interrupt path was about 1 KB of `-Og` code: handler 340 B, burst start 284 B, timing 204 B, DMA stop 80 B. Between bursts, the DSP pushes it out of the flash's 1 KB instruction cache. So every burst ran it cold from flash, with 4 wait states at 170 MHz and prefetch off.
- **Fixes, one at a time,** measured with `psramRingMaxIsrNs` and `dspLoadMaxPercent`:

  | Build | Interrupt (max) | DSP load, ring writing | DSP load, during verify | CE# max |
  |---|---|---|---|---|
  | Handler in flash, `-Og` | 4.04 µs | not measured | 110% | 5.04 µs |
  | In RAM, `-Og`, HAL handler skipped | 3.05 µs | 46% | 70% | 5.11 µs |
  | In RAM, `-O2` | 2.32–2.48 µs | 45% | 66% | 4.44 µs |

  - **Why RAM alone saved only 25%.** The Cortex-M4 fetches code from ordinary RAM (at 0x2000_0000) over its system bus. The code's own loads and stores use the same bus, so the two take turns, at about 2 cycles per instruction. `-Og` made this worse: it doesn't inline the small helper functions, so every call pushes and pops registers on the stack in that same RAM.
  - **What `-O2` changed.** It inlined the helpers into the handler and kept values in registers.
- **Costs:**
  - **RAM:** 1.0 KB for the interrupt code, copied from flash at boot. Free RAM went from 3.8 KB to 2.9 KB.
  - **A linker warning:** "LOAD segment with RWX permissions", because RAM that holds code is both writable and executable. Expected and harmless here.
  - **Debugging:** at `-O2`, `psram_ring.c` is harder to step through, because variables are optimised out.
- **Alternatives considered for the CPU cost:**
  - **CCM SRAM at 0x1000_0000.** The G431's 10 KB of core-coupled RAM is fetched over the instruction bus, separately from data, so code runs there without contention. It needs a linker-script change to carve its alias (from 0x2000_5800 up) out of the normal RAM region. This is the next step if the CPU cost matters later.
  - **Flash prefetch** (off in CubeMX). It would speed up all flash code, the DSP included. It's a separate experiment, because it changes the whole system's timing.
  - **Pacing reads.** Limiting read bursts per block would stop a reader from taking the CPU in bursts. That decision belongs to the SD writer's design.
- **Outcome:**
  - **CE#:** at most 4.44 µs with reads running, which is 3.56 µs inside tCEM. No bursts over.
  - **DSP load:** 45% while the ring writes (37% without the ring) and 66% in the worst case, with reads running back to back.
  - **Data:** no late blocks or faults, and `verify` read back 400 KB identical.
  - **Net cost:** at 2.3 µs and 32 bursts per block, writing the ring costs about 7% of the CPU.
- **Lessons for future designs:**
  - Measure an interrupt's cost under the heaviest load it will see, not only in normal running. +6% in normal running became an overrun once reads ran back to back.
  - Before making an interrupt run more often, measure what one costs.
  - On a Cortex-M4, running code from RAM isn't automatically fast. Ordinary SRAM shares the system bus between code and data; use the core-coupled RAM for hot code if the part has it.
  - Check the disassembly before blaming a function. The suspected `memcpy` wasn't even called.

## 2026-10-30: Time-expansion playback

### 37. PLAYBACK replays the last 2 s ten times slower (Oct 30)
- **Decision:** Pressing PLAYBACK replays the last 2 s of raw audio from the PSRAM ring at 10× slower, taking 20 s; pressing again stops it. Code: `Core/DSP/playback.c`. Version 1 of plan P3.
- **What time expansion is, and why it's worth having:**
  - A heterodyne detector mixes the signal with a local oscillator and plays the difference. You hear only a band about ±8 kHz around the LO, folded about it, so a call's shape is mostly lost. It's live, though.
  - Time expansion records at full bandwidth and plays it back slower. Every frequency is divided by the factor, and every duration multiplied by it: a 45 kHz pipistrelle call at 10× becomes 4.5 kHz, and a 5 ms call becomes 50 ms. The whole call survives: the FM sweep, the constant-frequency tail, harmonics and rhythm. That's the timbre the heterodyne loses, and what bat-ID work relies on.
  - The PSRAM ring (entries 34–36) is always recording, so the bats you've just heard on the heterodyne are already stored.
- **Why a fixed 10×, rather than scaling each call to 2 kHz:**
  - **Pitch identifies species.** At 10×, a noctule (20 kHz) plays at 2 kHz, a pipistrelle (45 kHz) at 4.5 kHz, and a *Myotis* sweep from 8 to 3 kHz. Scaling every call to 2 kHz would make them all sound alike.
  - **Predictable duration:** 2 s always takes 20 s. Auto-scaling a pipistrelle would need about 22×, and a lesser horseshoe (110 kHz) 55×, so 2 s would take 110 s.
  - **10× is the convention** that reference recordings use.
  - **It fits the hardware.** The mic's 20–80 kHz becomes 2–8 kHz, inside the speaker's range and below the amp's 6–10 kHz input filter.
  - **Cleaner sound.** From about 20× up, the interpolation images (below) fall near 10 kHz, which is audible, and would need a proper anti-imaging filter.
  - The engine steps through the recording with a 32-bit fractional position, so any factor works. A knob-set factor, or an automatic one, is a later option.
- **How it works:**
  - **Upsampling at the full DAC rate, as planned in P3.** The DAC keeps running at 250 kHz. Each output sample steps 1/10 of a sample through the recording, interpolating linearly between recorded samples. The unwanted images land around multiples of 25 kHz: ultrasonic, and filtered by the amp's input RC.
  - **The same 10 kHz high-pass as the heterodyne path** is applied to the recorded samples first. It removes the ADC's mid-rail bias and audible room noise, which slowed 10× would become rumble below 1 kHz.
  - **Two block buffers.** Ring blocks are read into two 512-byte slots: one plays for 10 ms while the next loads in about 0.25 ms. A block that isn't loaded in time plays as silence and is counted in `playbackUnderruns`.
  - **The ring pauses while playback runs.** 20 s of playing would otherwise write 20 s of new audio into a 16.8 s ring, overwriting the recording being played. Blocks arriving while paused aren't recorded, and recording resumes when playback ends. The ring's stuck-burst check now counts time in blocks even while paused, so a stuck read is still recovered.
  - **Live monitoring continues.** The heterodyne output is replaced while playback runs, and its mixer and filters are skipped. The detector, debug capture and waterfall carry on with the live input.
  - **Gain:** the playback gain is half `DSP_GAIN`, so playback is as loud as the heterodyne for the same input (the mixer halves the amplitude). It's scaled by the volume knob as usual.
  - **Display:** "PLAY 10x  20s" counts down where the LO readout normally is, which returns afterwards. "PLAY: no ring" appears if the PSRAM ring isn't running.
  - **The button** is debounced over 30 ms. A press left over from the power-up SD test has to be released first.
- **Costs:** RAM +1.1 KB (two block buffers and state), leaving about 1.7 KB free. Flash +1.3 KB. CPU: less than the heterodyne while playing; one block read per 10 ms.
- **Alternatives considered:**
  - **Slowing the DAC clock to 25 kHz:** the images would land in the audio band (P3).
  - **Keeping the ring recording during playback:** only possible if the segment × factor stays under the ring's 16.8 s (1.6 s at 10×, 0.8 s at 20×).
  - **Auto-scaling each call to 2 kHz:** see above.
- **Outcome:** Works on hardware first time. Ultrasound made near the mic (keys, fingertips) plays back slowed and lower, the countdown and stop work, and the LO readout returns. Measured: `playbackUnderruns` 0, ring faults 0, no bursts over tCEM, and the DSP's worst-case load unchanged at 46%. `capture.py verify` passes afterwards (160 KB identical), so the ring records normally again.
- **Lessons for future designs:**
  - Pick the time-expansion factor from the whole chain's passband (mic to 80 kHz, the amp's 6–10 kHz input filter, the speaker), not from a target pitch alone.
  - An always-on recording buffer makes features like this cheap. Playback needed no new capture path, only a reader.

---

## 2026-11-02: SD card bring-up

### 38. SD card: FAT32 on SDHC up to 32 GB; single-sector headroom too thin to rely on (Nov 2)
- **Decision:**
  - SD cards are FAT32 on SDHC cards of 32 GB or less, with an MBR partition table, formatted with the SD Association's SD Card Formatter. FatFS stays built without exFAT.
  - The SD recorder's write size is chosen from a sustained write test under recording load, not from the boot speed test. At boot, with nothing else running, single-sector writes (CMD24) give 1.33× headroom over 500 kB/s, which is too thin to rely on.
- **Context:** The first card, a 16 GB SDHC, arrived for bring-up of the driver written on Oct 30: `sd_spi.c` under FatFS R0.12c, with `_FS_TINY` on, LFN off, exFAT off and `_MULTI_PARTITION` 0.
- **Why this format (with numbers):**
  - **No exFAT.** Every card over 32 GB (SDXC) ships as exFAT, and SD Card Formatter reformats it as exFAT because the SD spec says to. Turning exFAT on in R0.12c needs LFN as well: with `_USE_LFN 1` that's 512 B of LFN buffer plus 608 B of directory buffer, about 1.1 KB of the 1.7 KB of RAM left, plus a few KB of flash (estimated, not measured). Shipping exFAT in a product may also need a licence from Microsoft.
  - **32 GB is enough.** At 500 kB/s a 32 GB card holds 17.7 h, and this 16 GB card 8.7 h. FAT32's 4 GiB file limit is 2.4 h per file, the same limit as a WAV's 32-bit RIFF size field.
  - **MBR, one partition.** With `_MULTI_PARTITION` 0, FatFS mounts the first FAT partition in the MBR. R0.12c can't read GPT (that arrived in R0.14). A card with several partitions, such as one from a Raspberry Pi, would mount its small boot partition.
  - **SD Card Formatter rather than Windows.** It follows the SD File System Specification: 32 KB clusters on SDHC, with the data area aligned to the card's 4 MB allocation unit. Windows uses 16 KB clusters on 16–32 GB cards (8 KB on 8–16 GB) and doesn't align the data area. Speed Class figures assume the spec layout. With 32 KB clusters one FAT32 sector maps 4 MB, about 8 s of recording, so FAT updates are half as frequent as with 16 KB.
- **Measured (16 GB SDHC):**
  - **Boot screen:** "SD SDHC 14912MB" (30,539,776 sectors). That line only appears once the card's start-up commands (CMD0, CMD8, ACMD41, then CMD58 with the CCS bit set), the CSD read and FatFS's mount have all succeeded.
  - **Speed test** (hold PLAYBACK at power-up; it runs before the audio starts): W666 R648 kB/s, and the readback matches the pattern.
  - **PC check:** Windows reads `SDTEST.BIN` as 262,144 bytes, all matching the pattern (checked with numpy).
- **Why the boot speed test doesn't settle the recorder's write size:**
  - The test writes in 64-byte `f_write` calls. With `_FS_TINY` these reach the driver one sector at a time, so the card only ever sees CMD24 and CMD17.
  - 666 kB/s is 0.75 ms per sector, while the data takes 0.19 ms on the wire at 21.25 MHz (SPI2: 85 MHz / 4). The other ~0.56 ms is the card's wait after each command, the command bytes sent one at a time through HAL, FatFS's per-call cost and the test's own pattern loop. These haven't been measured separately yet.
  - Recording needs 500 kB/s, so the headroom is 1.33×, measured with nothing else running. Recording is harder in three ways:
    - **CPU:** the DSP and ring interrupts take 45% of the CPU while the ring writes, and 66% while it's also read back to back. The SD writer reads every block back out of the ring, so recording sits in that range. The FFT and waterfall also run in the main loop and take whatever time is left.
    - **SPI2:** the waterfall draws on the same bus as the card.
    - **Stalls:** the test writes for 0.4 s, too short to catch the 100–250 ms stalls cards produce now and then. A 250 ms stall leaves a 125 KB backlog. The PSRAM holds it, but at 166 kB/s of spare rate it takes 0.75 s to clear, and only while the average rate stays above 500 kB/s.
  - CPU load slows the CPU part of each sector but not the card's waits. If all the overhead is card waiting, recording would still see about 666 kB/s; if it's all CPU time, about 230–370 kB/s at 45–66% load. 500 kB/s falls inside that range, so this test can't predict the outcome.
  - The driver already has CMD25 (with the ACMD23 pre-erase hint) and CMD18, but neither has run yet.
- **Alternatives considered:**
  - **exFAT:** see above.
  - **FAT32 on a 64 GB+ card** with a third-party formatter. FatFS handles FAT32 up to 2 TB, but it's outside the SD spec for SDXC, and the alignment is lost unless the tool aligns the data area itself.
  - **Formatting with Windows:** smaller clusters, no alignment.
  - **Going straight to a multi-sector pass in the boot test:** it changes two things at once (the transfer size and untested driver code) and still says nothing about recording load.
- **Outcome:** SD bring-up passes: mount, single-sector write and read, and an independent check on the PC. Next: a sustained single-sector write test under recording load, about 60 s with the audio, ring and waterfall running. If the average is well clear of 500 kB/s and the stalls fit in the PSRAM, the first recorder uses single-sector writes. If not, a multi-sector pass comes next.
- **Lessons for future designs:**
  - Check a storage self-test with an independent reader. A readback through the same driver passes even when a consistent addressing bug puts the data in the wrong place.
  - Make a speed test match the real workload: transfer size, CPU load, bus sharing and duration. Otherwise, say what it leaves out. This one measures the slowest transfer size in the easiest conditions.
- **Corrections (from entry 39's measurements):** the card has 30,540,800 sectors, not 30,539,776. The boot test's "kB/s" are KiB/s, so 666 KiB/s is 682 kB/s and the boot headroom was 1.36×, not 1.33×.

### 39. Multi-sector SD writes (8 at a time); the FFT at -O2; next, SPI2 DMA (Nov 2)
- **Decision:**
  - Single-sector writes can't keep up with recording under load (0.82–0.88×). Writes of 8 sectors (CMD25 multi-block) reach 1.41× while catching up a pre-trigger, and 2.0× once caught up.
  - `fft.c` builds at `-O2` and reverses bits with `RBIT`: 1.84× faster. CMSIS-DSP was considered and rejected.
  - Next: move the SD data blocks on SPI2 to DMA, then measure again.
- **Context:** entry 38 left the write size to a sustained test under load. Three tools were built for it:
  - `sd_bench.c` with `capture.py sdtest`: copies the ring's live audio to `SDBENCH.RAW`, starting a 5 s pre-trigger behind, with everything else running. The writer runs ahead of the FFT in the main loop, as a recorder would have to.
  - `capture.py stats`: reads every Live Expressions counter over USB and flags any out of limits.
  - `capture.py fftcheck`: the board's FFT result for a captured frame, against numpy.
- **Measured, single sector (first run, FFT still at `-Og`):** 411 kB/s = 0.82×. The backlog grew from 5 s to 15.6 s of the ring's 16.8 s in 60 s. Most `f_write` calls took 1–2 ms (0.75 ms at boot), the slowest 82 ms. DSP worst case 54%.
- **Measured, sweep of sectors per write (FFT at `-O2`; 60 s each, 5 s pre-trigger, 0 blocks lost in every run):**

  | Sectors | Rate catching up | Headroom | `f_write` time per sector | Busy once caught up | Slowest write | FFT drops |
  |---|---|---|---|---|---|---|
  | 1 | 440 kB/s | 0.88× | 1.02 ms | never caught up | 81.7 ms | 82% |
  | 4 | 601 kB/s | 1.20× | 0.75 ms | 57% | 15.6 ms | 56% |
  | 8 | 704 kB/s | 1.41× | 0.64 ms | 50% | 14.3 ms | 50% |
  | 16 | 636 kB/s | 1.27× | 0.47 ms | 48% | 16.3 ms | 55% |

- **Measured, FFT:**
  - `-Og` → `-O2`: 398k → 216k cycles per frame on average (2.34 → 1.27 ms, interrupts included); worst case 2.75 → 1.67 ms.
  - Blocks the FFT drops: 36% → 20%, with no SD traffic. DSP worst case 46% → 44% (`FFT_AddSamples` runs in the audio interrupt).
  - `fftcheck`: the board matches numpy's `rfft` to 6.8×10⁻⁸ of the largest bin (−143 dB) over 10 frames: float32 rounding.
- **Why:**
  - **The interrupts stretch the writer.** They take 45–54% of the CPU, which roughly doubles any CPU-bound work: 0.75 ms per sector at boot became 1.0–1.2 ms.
  - **Multi-sector writes remove the per-command costs.** Time per sector falls from 1.02 to 0.47 ms between 1 and 16 sectors.
  - **8 beats 16 overall.** At 16 the whole 8 KB buffer is one chunk, so the next chunk's ring reads wait for the write: only 58% of the catch-up time was spent writing, against 88% at 8.
  - **The data transfer is now the main cost.** At 16 sectors there's no per-command overhead left, yet a sector takes 0.47 ms against 0.19 ms on the wire. That points at the polled 512-byte transfer being stretched by interrupts. It's an inference, not a measured split, and DMA is the test of it.
  - **Multi-sector writes also cut the worst write** from 82 ms to 14–16 ms, over 60 s per setting on a freshly formatted card.
  - **The ring's read queue went from 4 to 16** (+144 B), so a whole chunk can be queued while the last one is written.
  - **Recording costs elsewhere:** the DSP worst case rose to 63%, because the ring now reads back to back, and the FFT drops half its blocks.
- **Alternatives considered:**
  - **SPI2 at 42.5 MHz (÷2):** above SPI mode's 25 MHz without CMD6 high-speed mode; 11.8 ns of MISO margin against the card's allowed 14 ns output delay; and an estimated gain to only 450–490 kB/s single-sector. Rejected.
  - **Splitting a sector's time first:** skipped for measuring multi-sector writes directly. The sweep's per-sector times gave most of the answer anyway.
  - **CMSIS-DSP for the FFT:** `arm_rfft_fast_f32` needs a 4 KB output buffer (1.3 KB free). `arm_cfft_f32` in place would fit and might be ~3× faster (Arm quotes ~55k cycles for a 1024-point real FFT; unverified). It would need vendored source built at `-O3`. `-O2` gave 1.84× for ten lines. Rejected.
  - **Accepting 8 sectors at 1.41× and moving on to WAV:** deferred until DMA has been measured.
- **Outcome:** 8-sector writes keep up with recording, by a thin margin by our thresholds (below 1.6× while catching up). The ring stayed in spec throughout: CE# worst 5.22 µs, no faults or late blocks.
  - Found on the way: ST's FatFS glue (`diskio.c`) initialises the card only once per boot. A card that fails the boot check returns `FR_DISK_ERR` until the next reset. A fix is still to be proposed.
- **Lessons for future designs:**
  - Benchmark storage under the real load. The interrupts doubled the time per sector.
  - Multi-block writes pay off most when the writer can read ahead: double-buffering beat a bigger chunk.
  - Try the optimisation level of one hot file before swapping in a library.
  - Check what generated glue code does after a failure, not only when it succeeds.

### 40. SD data blocks stay polled; DMA dropped over an unexplained fault; recovery added (Nov 2)
- **Decision:**
  - The SD driver sends and receives its 512-byte data blocks polled, and the recorder will use 8-sector multi-block writes.
  - SPI2 DMA was tried, first through HAL and then at register level. It's kept in the code behind `sdUseDma`, switched off.
  - Added:
    - a failed read or write now re-initialises the card and retries once;
    - a fix to ST's FatFS glue, so a card that failed to start is retried on the next mount or open;
    - driver diagnostics in `capture.py stats`;
    - one test file per setting, and a PC checker (`Tools/check_raw.py`).
- **Context:** entry 39 chose DMA to stop the interrupts stretching each polled 512-byte transfer.
- **What happened (with numbers):**
  - **HAL DMA** (SPI2_RX on DMA1 Ch4, TX on Ch5) gave no gain where it could be measured: single-sector writes ran at 442 kB/s, against 441 polled.
    - Writes failed after 36k, 7.8k and 48k blocks. The data response was 0xFF, and every later write failed until a reset.
    - The multi-sector sweep never ran long enough to measure DMA's real benefit.
  - **A/B in one build:** polled ran 103,390 blocks clean; DMA failed at 47,882.
  - **The failure's signature:**
    - the response to CMD24 was 0xFF (none at all);
    - CMD13 afterwards was clean (R1 0x00, R2 0x00);
    - a probe that keeps clocking found the card answering 6 to 281 bytes late, with 0xFE, 0xFC or 0xF9, or once a byte-aligned 0xE5 ("accepted").

    The card missed the block's start token, then locked onto a token-like bit pattern in the audio data and took the 512 bytes from there as the block. Since CRC checking is off in SPI mode, it writes that shifted block without complaint.
  - **Ruled out, one diagnostic at a time:**
    - HAL refusing a 1-byte transfer: 0 cases;
    - leftover data in SPI2's receive FIFO: 0;
    - busy timeouts: worst wait 11 ms, against a 500 ms timeout;
    - card errors: CMD13 always clean;
    - chip-select glitches from code: every GPIOB write is atomic;
    - HAL's interrupt handling: register-level DMA, full duplex, with no interrupts and no receive overruns, failed the same way after 4.2 s.
  - **Unexplained:** why only DMA triggers it. One hypothesis, not verified: a false "ready" before CMD24 while the card was still programming.
  - **After the desyncs, the card stopped answering CMD0**, and only a power cycle brought it back.
    - The first polled 8-sector run after them failed at 5.9 s, and the recovery couldn't restart the card.
    - The same "no answer to CMD0" state was seen at 12:38, before any DMA code existed, after the USB cables were swapped. Flashing resets the MCU but not the card.
  - **After a power cycle and a fresh format:**
    - 8-sector polled writes ran 180 s and 180,880 blocks (92.6 MB) with no errors, recoveries or self-check anomalies;
    - rate: 1.38× while catching up (a 5 s pre-trigger cleared in 13.1 s), 1.93× once caught up;
    - worst write 22.2 ms, worst busy wait 16.1 ms, against 11.8 s of ring slack.
  - **PC check:**
    - all 92,610,560 bytes present, and every 16-bit word a 12-bit ADC code;
    - the largest step across a block boundary (71) no bigger than inside a block (94);
    - chkdsk clean.
- **Alternatives considered:**
  - **Keep debugging DMA:** a flight recorder of the last ~96 bytes exchanged, and a pull-up on MISO (V2 has none; PB14 floats when the card is deselected).
  - **Fall back to polled.** Chosen: polled meets the requirement, DMA showed no measured gain, and its fault is rare but unexplained.
- **Costs:** flash 88.7 KB, RAM about 1.0 KB free. The CubeMX SPI2 DMA setup stays, because the dormant code relies on its DMAMUX routing. It costs ~0.2 KB of RAM for two DMA handles; remove it in CubeMX if that RAM is needed.
- **Lessons for future designs:**
  - Measure an optimisation's gain before chasing a fault in it. DMA gained nothing at single-sector, and its multi-sector gain was never measured.
  - An SD card in SPI mode that misses a start token writes a shifted block without complaint. Count every abnormal response, probe what the card does next, and verify the data on the PC.
  - A card that has been fed garbage can need a power cycle; CMD0 alone may not bring it back. Power-cycle the card (S1) between failing tests.
  - FatFS writes a file's directory entry only on `f_sync` or `f_close`, so an aborted write leaves an empty file and lost cluster chains. The recorder should `f_sync` periodically.

## 2026-11-02: WAV recording

### 41. RECORD writes a WAV file: 5 s of pre-trigger, a sector-sized header, synced every 2 s (Nov 2)
- **Decision:** RECORD starts a recording in `recorder.c` and a second press stops it.
  - Each recording is a new file, `BATnnnnn.WAV`, in the card's root folder: mono 16-bit PCM at 250 kHz, the raw ADC signal.
  - It starts 5 s before the press, taken from the PSRAM ring, and carries on with the live audio. Files stop at 10 minutes (300 MB).
  - It writes the same way as the SD test that kept up (entries 39–40): 8 sectors per `f_write`, polled, double-buffered from the ring, ahead of the FFT in the main loop.
- **Context:** entries 38–40 proved the card can keep up under the full recording load. This turns that test into the product feature.
- **Why (with numbers):**
  - **Samples are `(code − 2048) × 16`.**
    - The ADC's full scale becomes 16-bit full scale, so levels in dBFS mean the same in any software.
    - The low 4 bits are always 0. The PC checker uses that as a corruption test: a block shifted on the card sets them in 15 of every 16 samples.
    - The front end's actual mid-rail bias error stays in the file as a small DC offset; bat software filters it out.
  - **A 512-byte header, not the usual 44 bytes.**
    - With 44 bytes, every 4 KB chunk would start 44 bytes into a sector. FatFS would read, merge and rewrite the two partial sectors of every chunk, and the 8-sector writes measured in entry 39 would be lost.
    - Padding the header to one sector (a 460-byte `JUNK` chunk, which every reader skips) keeps the audio sector-aligned. The pad leaves room for GUANO metadata later.
    - The first block is chosen so that the first chunk is 7 blocks and every later chunk starts on a multiple of 8 file sectors. With 32 KB clusters, a chunk then never crosses a cluster boundary, so each `f_write` is still one 8-sector write.
  - **Synced every 2 s, with the header patched in place.**
    - FatFS writes a file's size only on `f_sync` or `f_close` (entry 40), and the WAV header's two size fields are unknown until the end. A power cut would otherwise leave an empty or unreadable file.
    - The sync writes FatFS's size and FAT first, then the header. In that order, a power cut between them leaves a header that claims less audio than the file holds, never more.
    - The header goes straight to its sector with `disk_write`. The `f_lseek(0)`, write, seek-to-end alternative makes FatFS walk the cluster chain from the start on the way back: one FAT sector per 128 clusters (8.4 s of audio), so 72 sector reads per sync for a 10-minute file.
    - Writing behind FatFS's back is safe here because the recorder only ever writes whole sectors. FatFS sends those straight to the card and never caches them.
    - Estimated cost: about 5 small writes and 1 read per sync, ~10 ms every 2 s (0.5%). Still to be measured (`recMaxSyncUs`).
  - **A 5 s pre-trigger.** That's the setting entry 40 measured: it cleared in 13.1 s at 1.38×, against the ring's 16.8 s.
    - Until it has caught up, the file runs behind the live audio. Stopping writes out the backlog first ("REC saving"). A 3 s recording with 5 s of pre-trigger needs about 2.8 s of saving.
  - **The pre-trigger never reaches back past a playback pause.**
    - The ring's block numbers carry straight on across a pause (playback pauses it for 20 s).
    - Without a guard, a recording made right after playback would join audio from before the playback to audio from after it, 20 s apart, and nothing in the file would show it.
    - `PsramRing_GetResumeBlock()` gives the first block after the last pause.
  - **If the ring overwrites audio not yet saved, the recording stops** ("REC: SD slow").
    - The SD test skips ahead instead. In a recording, a silent splice would give wrong call intervals.
    - A block the ring fails to read is saved as silence and counted (`recReadFailures`, which must stay 0).
  - **The recorder borrows the debug capture's 8 KB buffer.** RAM had 1.0 KB free, so there's no room for a second buffer.
    - The debug link gained a "lent" mode: snapshot, energy, read-back, FFT capture and the SD test are refused while it's lent.
    - With `DEBUG_CAPTURE 0`, the same functions hand out a plain 8 KB buffer, so the recorder works either way.
    - Lending is refused while the debug link is using the buffer ("REC: busy"). That only happens with a PC connected and a capture running.
  - **The card is mounted afresh at every start**, so a card inserted after boot, or swapped, is picked up.
    - Remounting re-reads the volume but doesn't restart a card that is already running.
    - The folder scan for the next number reads 16 names per sector: about 60 ms for 1000 files.
    - The start position is fixed before that work, so it doesn't eat into the pre-trigger.
  - **PLAYBACK is refused while a recording is open** ("PLAY: busy"), because playback pauses the ring the recording reads from. RECORD during playback shows "REC: playing".
- **Alternatives considered:**
  - **Refactor `sd_bench.c`'s write loop into a shared engine:** rejected for now. It would put the verified benchmark at risk. `recorder.c` repeats about 100 lines of it, specialised (fixed 8 sectors, WAV conversion, stop handling, syncs).
  - **Patch the header only at `f_close`, and repair files after a power cut with a PC tool:** simpler, but a power cut is the case that matters.
  - **Pre-allocate the file with `f_expand` and stream one long CMD25:** possibly faster, but a bigger change. Parked.
  - **Header sizes set to the 10-minute maximum up front:** depends on how tolerant each reader is of a short file. Rejected.
  - **GUANO metadata now:** deferred. The header's pad already has room for it.
- **Costs:** flash 88.7 → 90.3 KB (`recorder.c` is 2.2 KB). RAM 1008 → 848 B free: its own `FIL`, flags, state and 7 counters. The 8 KB buffer is shared, not added.
- **Outcome:** works on hardware (Nov 2).
  - **First card (a microSD in an SD adapter):** 6 recordings, all valid WAV files.
    - Three were shorter than the 5 s pre-trigger, so they ended before writing out the audio up to the press; one was empty.
    - The board had restarted before its counters could be read, so the cause isn't known. The card was also coming and going in its adapter at the time (entry 42).
  - **After reformatting (6 recordings, 6.5–10.9 s each, 50.2 s and 25.1 MB in all):**
    - every file passes `check_raw.py` and Python's `wave` module, with every sample a 12-bit code × 16;
    - no step across a block boundary is bigger than the steps inside blocks;
    - all stopped with the button (`recLastEnd` 0), and the last file's 6341 blocks match `recBlocks` exactly.
  - **The pre-trigger lines up.** The RECORD button's click lands at 4.96–4.98 s in two files: the 5.00 s mark, less the 30 ms debounce.
    - Claps 1–3 s before the press are in the files.
    - The stop click is 40–50 ms before each file's end: the debounce, plus up to 20 ms while `Recorder_Poll` holds the main loop.
  - **Measured on the last recording:**
    - slowest 8-sector write 15.3 ms;
    - slowest sync 6.1 ms, under the ~10 ms estimate, so at most 0.3% of the time;
    - most backlog 4917 blocks: the pre-trigger itself, against ~16,300 before audio is lost;
    - no SD errors or recoveries, no ring faults, CE# worst 5.22 µs, DSP worst 63%.
  - The DC offset is −48 codes in every file: the bias sits slightly below mid-rail.
  - **Card removed and put back while running:** records again without a restart, once PB14 has its pull-up (entry 42).
  - **Still to test:** a recording to the 10-minute limit, a power cut mid-recording (S1), and a recording straight after a playback.
- **Lessons for future designs:**
  - Keep file writes sector-aligned from the first byte. A header that doesn't fill a whole sector misaligns everything after it.
  - Order the steps of a sync so that an interruption leaves a file that under-claims its contents.
  - A buffer's block numbering can be continuous while its time isn't. Mark the discontinuities.

### 42. Internal pull-up on SD MISO (PB14); the microSD adapter was unreliable (Nov 2)
- **Decision:** PB14 (SPI2 MISO) uses the G4's internal pull-up, about 40 kΩ, set in CubeMX. V2 has no external pull-up there.
- **Context:**
  - The board was started with no card in the slot. The driver didn't report "no card": it reported CMD0 failing with the card "still busy", after a 500 ms wait (`sdBusyMaxUs` 499.7 ms, reply 0x00).
  - With nothing driving MISO, the pin floats, and here it read low. 0x00 is what a busy card sends, so a missing card looked like a busy one.
  - Over the same days, the microSD card in its SD adapter kept vanishing and coming back as the card was moved in the adapter.
- **Why:**
  - A pull-up makes an undriven MISO read 0xFF, which the driver already treats as "no card". A card that drops out mid-write then fails at once, instead of after a 500 ms busy wait on every attempt.
  - The SD spec asks for 10–100 kΩ pull-ups on the card's data lines. While the card drives the line, a weak pull-up makes no difference.
  - Entries 4 and 21 already list an SD MISO pull-up as a hardware lesson. This is the firmware stand-in for V2.
- **The adapter:**
  - Intermittent contact fits the "card not found, then found again" behaviour, and possibly the times the card stopped answering until power-cycled (entry 40).
  - It doesn't explain entry 40's DMA-only fault: polled transfers ran clean on the same card and adapter, in the same build.
- **Alternatives considered:** a 47 kΩ external resistor on V3 (planned with the hardware fixes). Leaving it floating: rejected, because it turns a missing card into a 500 ms timeout.
- **Outcome:** the generated change is just PB14's `Pull` in `HAL_SPI_MspInit`, with its speed and alternate function unchanged. Verified on hardware (Nov 2):
  - **A card removed and put back while the board runs** records again on the next RECORD, with no restart. This is the first hardware test of entry 40's re-initialise-on-the-next-mount path.
  - **A card pulled during a recording** (`BAT00009`, 223 blocks in):
    - the card rejected a CMD25 data block, then didn't answer CMD13;
    - both recovery attempts failed, and the recording ended with "REC: SD error";
    - the next RECORD with the slot empty showed "REC: no card".
  - **No 500 ms waits:** the longest busy wait throughout was 19 ms, where the floating MISO would have cost 500 ms per attempt.
  - **The cost of pulling the card:** a recording that loses its card can't close its file, so the file stays as it was at its last sync. Here that was empty, because the pull came before the first sync at 2 s. The unsynced clusters are lost chains until chkdsk frees them.
- **Lessons for future designs:**
  - An input that floats when nothing drives it can read as a valid state. Here 0x00 meant "busy". Give it a pull to the value that means "nothing there".
  - Avoid card adapters in a device that gets handled: use the slot's native card size.

## 2026-11-03: Species classifier

### 43. Species classifier: modelled on the PC, deferred on the device (Nov 3)
- **Decision:** Don't build the species classifier in `Docs/Bat Species Classifier.md` (Oct 29) into the V2 firmware.
  - Keep its numpy model, `Tools/classify.py` (stages 1–3: frames, segmenter, features), as a PC tool for the recordings.
  - Add stages 4–6 (shape groups, species rules, voting) there, and fit the rules to this detector's own recordings once bats fly again in spring.
  - Revisit classification on the device only once the PC model has proven accurate on real passes.
- **Context:**
  - The doc was written for 500 ksps, the day before the switch to 250 kHz (entry 16). Its frames at 250 kHz are a 128-point FFT with a 64-sample hop: the same 0.512 ms window and 1.95 kHz bins, a frame every 0.256 ms, four per audio block.
  - There are no bat recordings yet; the recorder's files so far are finger rubs. Few bats fly between about November and March.
  - The model was tested on the doc's bench signals, made by `Tools/bat_signals.py`:
    - The noise is shaped from the Oct 30 quiet-room capture: 7.2 codes rms above 10 kHz, plus the capture's steady lines as tones (24.92 kHz at 13.4 codes is the largest).
    - It matches the capture within 0.3 dB in every 10 kHz band from 10 to 125 kHz.
- **Why defer (with numbers):**
  - **RAM:** about 3.6 KB at 128 points (the doc's 5.4 KB was for 256 points), against 848 B free.
    - The doc hoped the 10 KB CCM SRAM was free. It isn't: the linker script maps all 32 KB as one region, and the CCM is its top 10 KB.
    - Removing the debug features frees about 0.5 KB (`debug_link` state 328 B, `sd_bench` 196 B). The 8 KB capture buffer is the recorder's now.
    - The one large source is the waterfall FFT's 6 KB (`cfftRe`, `cfftIm`, `fftMagnitude`), at the price of 1.95 kHz waterfall bins instead of 244 Hz.
  - **CPU:** the doc's budget assumed CMSIS `arm_rfft_fast_f32`, which was rejected (entry 39).
    - Scaling the hand-rolled 1024-point frame (216k cycles at `-O2`, interrupts included) by N log N gives 10–19k cycles per 128-point frame.
    - At 3,906 frames/s that is 23–44% of the CPU, continuously, at interrupt priority. Not measured.
    - The DSP interrupt already peaks at 63% while recording, and the polled SD writer runs in what is left.
  - **Microphone:** rated to about 80 kHz, so the lesser horseshoe (105–112 kHz) may never be seen.
  - **Data:** the rule ranges are literature values. They have to be refitted through this front end, which needs recorded bats.
- **What the model found:** five changes from the doc, all needed before the bench signals passed.
  1. **Noise floor bias.** The floor tracker rises 32× slower than it falls (1/2048 against 1/64 per frame), so it settles below the mean noise power: −6.1 dB measured, −7 dB in theory for Gaussian noise.
     - The peak of 57 noise bins is typically 6.4 dB above the mean. So pure noise read a median 11.4 dB against `t_on` 12 dB: 29,000 false calls a minute with the doc's values.
     - SNR is now measured against the mean noise (+6 dB, which folds into the threshold constants in C), and `t_off` rises from 6 to 9 dB, which 2% of noise frames reach. 60 s of noise then gave no false calls.
  2. **Peak frequency.** The bin is chosen on the whitened spectrum (power over floor) but interpolated on the raw power.
     - When the raw maximum was the neighbouring bin, the parabola's offset clamped at ±0.5 bin, and tracks stuck at half-bin values (26.4 kHz = bin 13.5). It was worst beside the 24.9 kHz line, which raises the floor in its bins.
     - Fix: step once to a louder raw neighbour, then interpolate. Noctule tails crossing the line now read within 0.4 kHz.
  3. **Call span.** A call is measured over the frames at or above max(`t_on`, peak − 24 dB). With the doc's relative test alone, a noise frame at the end of a call became its end frequency (29 kHz on a 20 kHz call).
  4. **Characteristic frequency.** The doc takes the flattest single step in the last 40%. On a QCF tail every step is about as flat, so noise picked the point: 24.0–25.1 kHz on a 25 → 24 kHz tail. It's now the end of the flattest part: the slope is fitted over 4 frames (0.77 ms) and the latest minimum wins.
  5. **Known limit.** A frame's frequency is the centre of the part of the call inside its window, which at the ends is 0.25–0.35 ms inside the call.
     - A 100 → 30 kHz sweep in 3 ms reads 57–63 kHz of bandwidth; the pipistrelle's 70 kHz start reads about 65.
     - Rules on bandwidth or start frequency (the doc's Natterer's rule, bandwidth ≥ 80 kHz) must be fitted to this measure.
- **Outcome** (bench signals at +20 dB re the noise rms above 10 kHz; `python -I Tools/classify.py --selftest` passes):
  - Every call found in all six sets, none split.
  - 82 kHz tone: peak within 0.01 kHz, duration −0.6 ms.
  - Pipistrelle and noctule characteristic frequency within 0.6 kHz.
  - A 40 kHz tone 10 dB above the calls is whitened out; the calls under it are all found.
  - Pipistrelle found 20/20 at −5 dB re the noise rms (a 4-code amplitude), 1/20 at −10 dB.
  - Echoes show up as extra calls; the doc's stage 6 rejects them.
  - The model runs at about 2.7 s per minute of audio.
- **Alternatives considered:**
  - **Build it anyway, taking the waterfall FFT's RAM:** possible, but the CPU cost is unmeasured and likely to collide with recording, and there's no data to fit the rules. Rejected for now.
  - **Classify on demand:** after a pass, run the classifier over the PSRAM ring in the main loop, borrowing the recorder's 8 KB buffer while it isn't recording. It needs no real-time CPU and fits the RAM. The first option if on-device ID is revisited.
  - **A larger part on V3,** for example an STM32G474 (128 KB RAM) in LQFP48. Pin compatibility with the G431 to be checked.
- **Lessons for future designs:**
  - Model a detector on the PC against the device's measured noise before porting it. Thresholds written to an estimator's nominal meaning can sit inside its noise.
  - A minimum-tracking floor reads below the mean noise, and the peak of N noise bins reads about ln N above it. Set thresholds against both.
  - Pick a spectral peak and interpolate it on the same spectrum, or re-centre before interpolating.
  - Budget RAM from the linker map, not the part's headline numbers: the G431's CCM is part of its 32 KB.

## 2026-11-03: Detector LEDs and auto-tune

### 44. Detector: +12 dB, three pulses to a pass, LED1 only during a pass; finger rubs still trigger it (Nov 3)
- **Decision:**
  - Raise the bat detector's threshold from 10 to 12 dB above its floor.
  - Need three pulses, each within 600 ms of the one before, to start a pass (it was two).
  - Flash LED1 for a call only while a pass is in progress.
- **Context:** Both LEDs were firing with no bats about, and rubbing fingers together 50 cm from the microphone triggers a pass.
- **Why (with numbers):** The Oct 30 quiet-room capture (entry 33, one minute of block energies) was replayed through the detector with each threshold and trigger rule. No bats were present, so every pulse and pass is false.

  | Threshold | False pulses/min | False passes/min (2 / 3 / 4 pulses) | LED0 lit (2 / 3 / 4 pulses) |
  |---|---|---|---|
  | +10 dB (was) | 57 | 6 / 2 / 2 | 59% / 32% / 28% |
  | +11 dB | 24 | 3 / 1 / 0 | 11% / 3% / 0% |
  | +12 dB (now) | 11 | 0 / 0 / 0 | 0% |
  | +13 dB | 3 | 0 / 0 / 0 | 0% |

  - The 59% explains the complaint: in that room LED0 was on more often than off.
  - **Three pulses:** each extra pulse multiplies the chance of random pulses chaining into a pass by the chance of another within 600 ms, about 0.1 at 11 a minute. So it makes false passes about 10× rarer. A real pass pays one more call interval, 0.1–0.2 s at 5–10 calls a second.
  - **LED1:** the 11 remaining false pulses a minute no longer flash it, because LED1 only shows the calls of a pass.
- **Finger rubs are not fixed by this, and can't be by a threshold.**
  - Rubbing fingers makes real broadband ultrasound; it's the usual way to test a bat detector. At 50 cm it is far above any sensible threshold.
  - The detector measures only the energy above 10 kHz, so a rub and a call look the same to it.
  - Telling them apart needs a feature that differs in shape, not level: a call is a tone or a sweep, a rub is broadband hiss.
  - Next: design that test on the PC first, as in entry 43, against the finger-rub recordings on the SD card and the synthetic calls (`Tools/bat_signals.py`).
- **Limits found while modelling auto-tune (entry 45):**
  - A pipistrelle at +12 dB relative to the noise isn't detected at all, though at +20 dB it is. The 24.95 kHz line makes up 76% of the detector's input energy (entry 33). By comparison, entry 43's 128-point whitened frames found the same call at −5 dB.
  - A steady 40 kHz tone 10 dB louder than the calls (a bush cricket) hides them completely.
  - Both point the same way as the finger rubs: a detector that looks at the spectrum, not just the band energy.
- **Alternatives considered:**
  - +15 dB: no false pulses in the capture either, but 3 dB less sensitive than +12 for no measured gain.
  - Four pulses: no better than three at +12 dB, and slower.
- **Outcome:** built (`bat_detect.c`, `main.c`'s LED code), and `analyse_capture.py`'s replay updated to match. Replays of captures recorded before this change will now disagree with the board's counters.
  - **On the board (Nov 3), at the desk**, a 20 s energy capture (`20261103_093947_energy_desk-nov3`) showed 180 pulses a minute, in irregular clusters. They peaked 37–40 dB above the floor, with almost all their energy above 10 kHz: desk sounds such as typing and handling, which are rich in ultrasound.
  - The replayed sweep still gave 3 passes a minute at every threshold from +6 to +20 dB. That confirms it: what's left is loud real ultrasound, for the shape test to reject, not a threshold to tune.
- **Lessons for future designs:**
  - A threshold sets the false-alarm rate on background noise. It can't reject a loud sound that isn't a bat; that takes a feature that differs between the two.
  - Replay recorded data through every candidate rule before choosing one: the 59% came from the data, not from an estimate.

### 45. Auto-tune, and tuning modes on a long PLAYBACK press (Nov 3)
- **Decision:**
  - Add auto-tune (`Core/DSP/autotune.c`), which points the heterodyne LO at the bats being heard.
  - Holding PLAYBACK for 0.8 s switches between manual tuning (the knob) and auto. A short press still plays back, but now acts on release.
- **How auto-tune works:**
  - **Its input:** the waterfall's rows. Every 40 ms each row holds the peak level in each of its 240 columns (about 0.52 kHz each), so auto-tune needs no FFT of its own and no frame buffer.
  - **A noise floor per column**, learnt only between passes:
    - It falls fast (a quarter of the way per row) and rises 0.5 dB every fourth row, so it sits near the quiet end of the noise.
    - A steady tone, such as the 24.95 kHz line or a cricket, becomes part of its column's floor and can't win.
    - It's stored as int8 in half-dB steps: 240 B, against 960 B as floats.
  - **During a pass**, each row with a call gives the column furthest above its floor (at least 10 dB, 10–120 kHz).
    - It steps to a louder neighbour before interpolating between columns, the lesson of entry 43.
    - That gives the call's loudest frequency to about 0.1 kHz.
  - **Setting the LO:**
    - The LO goes 1.5 kHz below the median of the last 8 calls, so a pipistrelle's 45 kHz tail beats at an audible 1.5 kHz rather than near 0 Hz.
    - It moves only for a change of more than 2 kHz, at most once a second, in 0.1 kHz steps, and it holds between passes.
    - Each pass starts from its own calls.
- **The modes:**
  - **Manual:** the knob sets the LO, as before.
  - **Auto:** the header shows "AUTO 43.5kHz" and the knob is ignored, so a knock in a pocket can't retune it.
  - **Switching:** auto starts from the current LO and moves when the bats do; going back to manual returns to the knob's position.
  - A "MODE: AUTO" or "MODE: MANUAL" message shows for 2 s. The board starts in manual.
  - Auto-tune learns in both modes, so it's ready the moment auto is selected.
- **Model, before the board:** auto-tune was ported line for line to Python and fed synthetic calls in the board's noise. The pipeline was the detector at its entry-44 settings, then 1024-point frames, then the waterfall's columns and rows.

  | Signal | Calls measured (median) | LO set to |
  |---|---|---|
  | Pipistrelle, 70 → 45 kHz | 45.6 kHz | 44.4 kHz, 0.4 s into the pass |
  | Greater horseshoe, 82 kHz CF | 82.1 kHz | 80.6 kHz |
  | Noctule, tails alternating 20 / 24 kHz | both notes | 19.4–24.1 kHz, one note or between |
  | *Myotis* sweep 100 → 30 kHz | 41–69 kHz | 55–68 kHz |
  | Noise only | none | never moves |

  - **Dropped frames:** the FFT drops about 20% of its blocks normally and about half while recording (entry 39).
    - With 20% or 50% of frames dropped at random, the pipistrelle and horseshoe tuned the same in every run.
    - With recorder-like bursts of 3–5 dropped frames, 1 run in 30 tuned to 57.5 kHz: the kept frames held only the calls' opening sweep.
    - The first test dropped exactly every other frame, which locks onto the call rhythm and failed far more often. That was an artefact of the test.
  - **The *Myotis* spread** comes from the synthetic sweep being equally loud along its length. Real calls have a peak-energy frequency.
- **Costs:** 1.8 KB of flash and 304 B of RAM (about 540 B left). The CPU cost is negligible: 25 rows a second in the main loop.
- **Alternatives considered:**
  - The classifier's tuner (doc, entry 43): parked with the classifier.
  - A separate FFT for auto-tune: no RAM for it.
  - Tuning to the peak itself (offset 0): a QCF tail would beat near 0 Hz, inaudible.
  - Freezing auto-tune while recording: rejected; 1 mis-tune in 30 runs isn't worth losing the tracking.
  - Voting only with the strongest calls: tried, no help. When the frames miss a stretch of tails, there's nothing strong to vote for.
- **Limits:**
  - It hears only what the detector calls a pass, so entry 44's sensitivity limits apply (quiet bats, loud insects).
  - FM bats tune somewhere along their sweep, wherever the energy peaks.
- **Outcome:** built. `capture.py stats` shows `autoTuneCalls`, `autoTuneRetunes` and `autoTuneLastHz`.
  - **On the bench (Nov 3):** auto-tune works. Driven by finger rubs and desk sounds for 15 minutes, it measured 1,782 calls and moved the LO 110 times, at most once a second as designed. The mode switch and the "AUTO" label work.
  - Other counters were unaffected: DSP peak 44%, CE# 4.77 µs, no ring or SD faults, and 20.6% of FFT blocks dropped (the usual 20%).
  - How accurately it tunes on real bats waits for a field test in spring. Until then, the model above is the evidence.
- **Lessons for future designs:**
  - Feed a new feature from data another module already computes before adding a pipeline for it. Here the waterfall's rows saved an FFT and 2 KB of RAM.
  - Test against realistic data loss. A drop pattern that locks onto the signal's rhythm exaggerates failures (1 in 6 against 1 in 30), and real call intervals vary anyway.

---

## Planned decisions (agreed, not built yet)

### P1. Reclaim about 8 KB of RAM: built, see entries 26–29.

### P2. PSRAM as the recording buffer, written from the interrupt side (ring built, entry 34; SD writer built, entry 41)
- **Plan:** PSRAM writes must never wait on the main loop, which may be stuck inside an SD write. So one interrupt-driven, DMA-driven SPI1 engine owns the PSRAM.
  - It uses DMA2, away from the audio channels on DMA1.
  - Bursts are 32 bytes at most, which is about 7 µs at wire speed. (Built with 16-byte bursts for margin; entry 36.)
  - The SD writer and playback send read requests to it.
- **Why:** SD cards can stall for 100–250 ms while erasing. At 500 kB/s that's up to 125 KB of audio, far more than the RAM. The PSRAM absorbs it.

### P3. Time expansion by upsampling at the full DAC rate (version 1 built, entry 37)
- **Plan:** When PLAYBACK is pressed, freeze the ring buffer. Then replay it 10× slower by interpolating samples at the full 250 kHz DAC rate, rather than slowing the DAC clock.
- **Why:**
  - Upsampling puts the unwanted image frequencies near 250 kHz. A slowed DAC (25 kHz) would put them in the audio band (25 kHz minus the tone), where the amplifier's ~6–10 kHz RC filter only partly removes them.
  - It needs no timer reconfiguration.
  - Freezing the buffer matters: playing 2 s at 10× takes 20 s, and a running buffer would overwrite it.

### P4. Amplifier gating from a bat-call detector (stage 1 built, entry 30; stage 2 planned)
- **Stage 2 (planned):** Switch `AMP_EN` from the detector, with:
  - a ~10 ms digital fade around wake-up and standby;
  - amp off when the volume is at minimum;
  - amp forced on during time-expansion playback.

  The squelch-off (always-on) control is deferred.
- **Plan:** Detect bat calls continuously in the audio interrupt, and switch `AMP_EN` on only while bats are around.
- **Why not use the FFT:** The FFT only sees frames the main loop has time to process, so a 1–5 ms *Myotis* call can fall into a dropped frame. Measuring each 1.024 ms block's energy sees every call.
- **The detector:**
  - It uses the energy of the existing 10 kHz high-pass output, which costs about 0.5% CPU.
  - A noise floor follows the energy down quickly and up slowly.
  - Pulses are 1–100 ms runs more than 12 dB above the floor (10 dB until entry 44); runs longer than 150 ms count as noise.
  - Three pulses, each within 600 ms of the one before (two until entry 44), switch the amp on, and it stays on for 2 s after the last pulse.
  - The timings come from `Docs/European Bat Calls — Species, Frequencies & Acoustic ID.md`: call lengths of 0.8–30 ms (tens of ms for horseshoes), and up to 468 ms between greater noctule calls.
  - A 10 kHz lower band edge keeps the free-tailed bat (9–14 kHz) and social calls (14–22 kHz).
- **Honest benefit:** The amp draws about 4 mA of an estimated 60–100 mA system total, so it's a few % of battery life. The bigger gain is silence instead of hiss between bats.
- **Stage 1:** Monitor only. The amp stays on; detections and counters are shown so the detector can be tuned before it controls the amp.

---

## Lessons for new designs (summary)

**Hardware**
- Check DNP parts against the clock configuration (entry 3).
- Put pull-ups on every chip-select, and on SD MISO (entries 4, 21).
- Default-off for amplifiers and loads; let firmware enable them (entry 5).
- Confirm the slowest and fastest clock each peripheral can reach (entry 6).
- If the sample rate is close to the band edge, put a real anti-alias filter below Nyquist (entry 16).
- Check part pinouts against a physical part, not just the datasheet (entry 8).
- Plan the battery gauge's reference: VDDA isn't stable near empty (entry 9).
- Route VBUS to a GPIO through a divider so firmware can see USB being plugged in. V2 lacks this.
- Test a USB-C socket in both plug orientations at bring-up; one dry joint makes it work one way only (entry 31).

**Firmware**
- Bring up one unknown at a time (entry 1).
- Prefer names and checks that turn mistakes into compile errors (entries 2, 17).
- Every wait loop gets a timeout (entries 14, 21).
- Measure timing with the cycle counter, and don't trust a test that can pass while out of spec (entries 11, 12, 19, 34).
- Keep one source of truth for rates and sizes, checked by the compiler (entry 17).
- Measure real-time margins at the optimisation level you run, and mark shared variables `volatile` (entry 20).
- Set the GPIO speed of fast pins (entry 14).
- Anything calculated once at boot and never written again belongs in flash; generate it with a script tied to the config (entry 26).
- Ask the hardware where it is (DMA counters) rather than assuming timing relationships (entry 27).
- Reserve the stack you actually need, so the linker can catch RAM overflow (entry 28).
- Run new control logic in "monitor only" mode before it drives hardware (entry 30).
- Check CubeMX's generated values against the HAL's own defaults for the family; read the USB line state before blaming the clock (entry 31).
- Build the measurement path before tuning, and have the analysis check itself against the device (entry 32).
- When a timing limit depends on interrupt latency, check the priorities at start-up and measure the limit on every cycle (entry 34).
- When a measured time disagrees with the calculation, split it into parts and measure each before choosing a fix (entry 35).
- Read every setting CubeMX generates for a peripheral, not just the ones you changed. Its G4 SPI default (NSS pulse mode) costs 20% of the bus speed (entry 35).
- Measure an interrupt's CPU cost under its heaviest load before making it run more often (entry 36).
- On a Cortex-M4, put hot code in core-coupled RAM if the part has it: ordinary SRAM shares the bus between code and data (entry 36).
- Check the disassembly before blaming a function (entry 36).
- Choose a time-expansion factor from the whole output chain's passband, not just a target pitch (entry 37).
- Check a storage self-test with an independent reader: a readback through the same driver can't catch a consistent addressing bug (entry 38).
- Make a speed test match the real workload (transfer size, CPU load, bus sharing, duration), or say what it leaves out (entry 38).
- Benchmark storage under the real load; interrupts doubled the SD time per sector (entry 39).
- Multi-block writes pay off most when the writer reads ahead: double-buffering beat a bigger chunk (entry 39).
- Try the optimisation level of one hot file before swapping in a library (entry 39).
- Check what generated glue code does after a failure, not only when it succeeds (entry 39).
- Measure an optimisation's gain before chasing a fault in it (entry 40).
- Without CRC, an SD card that misses a start token writes a shifted block silently: probe abnormal responses and verify data on the PC (entry 40).
- Power-cycle an SD card between failing tests; CMD0 alone may not bring it back (entry 40).
- Keep file writes sector-aligned from the first byte: pad headers to a whole sector (entry 41).
- Order a sync so that an interruption leaves a file that under-claims its contents (entry 41).
- A buffer's block numbers can run on while its time doesn't (a pause): mark the gaps (entry 41).
- Pull a floating input to the value that means "nothing there"; on SD MISO a floating 0x00 reads as "busy" (entry 42).
- Avoid card adapters in a handheld device; use the slot's native card size (entry 42).
- Model a detector on the PC against the device's measured noise before porting it; the design's thresholds sat inside the noise (entry 43).
- A minimum-tracking floor reads below the mean noise, and the peak of N noise bins about ln N above it: set thresholds against both (entry 43).
- Pick a spectral peak and interpolate it on the same spectrum, or re-centre first (entry 43).
- Budget RAM from the linker map: the G431's 10 KB CCM is part of its 32 KB (entry 43).
- A threshold sets the false-alarm rate on background; rejecting loud non-bat sounds needs a feature that differs in shape, not level (entry 44).
- Replay recorded data through every candidate rule before choosing one (entry 44).
- Feed a new feature from data another module already computes before adding a pipeline (entry 45).
- Test against realistic data loss: a pattern that locks onto the signal's rhythm exaggerates failures (entry 45).

---

## Template for new entries

```
### N. Title (date)
- **Decision:**
- **Context:**
- **Why (with numbers):**
- **Alternatives considered:**
- **Outcome:**
- **Lesson for future designs:**
```
