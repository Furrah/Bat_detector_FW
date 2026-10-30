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

---

## Planned decisions (agreed, not built yet)

### P1. Reclaim about 8 KB of RAM: built, see entries 26–29.

### P2. PSRAM as the recording buffer, written from the interrupt side (planned)
- **Plan:** PSRAM writes must never wait on the main loop, which may be stuck inside an SD write. So one interrupt-driven, DMA-driven SPI1 engine owns the PSRAM.
  - It uses DMA2, away from the audio channels on DMA1.
  - Bursts are 32 bytes at most, which is about 7 µs at wire speed.
  - The SD writer and playback send read requests to it.
- **Why:** SD cards can stall for 100–250 ms while erasing. At 500 kB/s that's up to 125 KB of audio, far more than the RAM. The PSRAM absorbs it.

### P3. Time expansion by upsampling at the full DAC rate (planned)
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
  - Pulses are 1–100 ms runs more than 10 dB above the floor; runs longer than 150 ms count as noise.
  - Two pulses within 600 ms switch the amp on, and it stays on for 2 s after the last pulse.
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
- Measure timing with the cycle counter, and don't trust a test that can pass while out of spec (entries 11, 12, 19).
- Keep one source of truth for rates and sizes, checked by the compiler (entry 17).
- Measure real-time margins at the optimisation level you run, and mark shared variables `volatile` (entry 20).
- Set the GPIO speed of fast pins (entry 14).
- Anything calculated once at boot and never written again belongs in flash; generate it with a script tied to the config (entry 26).
- Ask the hardware where it is (DMA counters) rather than assuming timing relationships (entry 27).
- Reserve the stack you actually need, so the linker can catch RAM overflow (entry 28).
- Run new control logic in "monitor only" mode before it drives hardware (entry 30).
- Check CubeMX's generated values against the HAL's own defaults for the family; read the USB line state before blaming the clock (entry 31).
- Build the measurement path before tuning, and have the analysis check itself against the device (entry 32).

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
