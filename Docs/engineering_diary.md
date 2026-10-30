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

## Planned decisions (agreed, not built yet)

### P1. Reclaim about 8 KB of RAM (planned)
- **Plan:**
  - The Hann window (4 KB), FFT twiddle factors (2 KB) and sine table (1 KB) become fixed tables in flash. They never change after boot.
  - `dacQueue` (1 KB) goes away: the DSP writes its output straight into the free half of the DAC buffer.
- **Why:** Recording needs a 4 KB SD write buffer for fast multi-sector writes, PSRAM DMA buffers, and a 2 KB stack for FatFS. Only about 5.5 KB of RAM is free now. Flash has about 61 KB free.

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

**Firmware**
- Bring up one unknown at a time (entry 1).
- Prefer names and checks that turn mistakes into compile errors (entries 2, 17).
- Every wait loop gets a timeout (entries 14, 21).
- Measure timing with the cycle counter, and don't trust a test that can pass while out of spec (entries 11, 12, 19).
- Keep one source of truth for rates and sizes, checked by the compiler (entry 17).
- Measure real-time margins at the optimisation level you run, and mark shared variables `volatile` (entry 20).
- Set the GPIO speed of fast pins (entry 14).

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
