# Lessons, and what's next

*Part 10 of [Building a bat detector](00-introduction.md). Where the project stands at the end of October 2026, what's still to build, what I'd change on the next board, and the lessons from all the posts in one place.*

---

## Where it stands

After two days of porting and a lot of measuring, V2 does this:

- **Live heterodyne** across 0–120 kHz, with digital mixing, a square-law volume knob, and the amp enabled without a pop. ([parts 1](01-adc-and-dac.md), [3](03-dsp.md))
- **A scrolling spectrogram** of 0–125 kHz, with peak-hold so short calls survive. ([part 4](04-fft-waterfall.md))
- **An always-on recorder:** the last 16.8 seconds of raw audio are always in the PSRAM, verified bit for bit. ([parts 6](06-psram-bring-up.md), [7](07-psram-ring-buffer.md))
- **Time-expansion playback** of the last 2 seconds at 10×. ([part 8](08-time-expansion.md))
- **A bat-call detector** in monitor-only mode, and a USB measurement link to tune it with. ([part 9](09-bat-detector.md))

The budgets, at the time of writing:

| Resource | Used | Note |
|---|---|---|
| DSP load, worst case | 66% | 45% while recording; 37% for the heterodyne alone |
| RAM | 31.0 KB of 32 KB | 1.7 KB free; `DEBUG_CAPTURE 0` frees 8 KB |
| Flash | 83.3 KB of 128 KB | at `-Og` |
| PSRAM chip-select, worst case | 4.44 µs of 8 µs | |

## The SD card: written, not yet tested

The SD driver (`sd_spi.c`) is written. I had no card to test it with, so only its "no card" path has run on hardware. The choices:

- **SPI mode.** The G431 has no dedicated SD-card peripheral (SDMMC).
- **HAL calls, not registers.** Unlike the PSRAM, SD cards have no maximum select time, and the display on the same bus already uses HAL. One access style per bus avoids surprises.
- **Every wait is bounded:** 1 s for card start-up, 500 ms for a busy card, 200 ms for a read's start token.
- **Bus manners on the shared SPI2:** MOSI is held high while reading, and one extra clock after deselecting makes the card release MISO for the display.
- **A pre-erase hint** (`ACMD23`) before multi-sector writes, to shorten the card's internal stalls.
- **FatFS settings for a small chip:** `_FS_TINY` on (saving 512 bytes of RAM), long file names and exFAT off. So cards must be FAT32, which cards up to 32 GB are out of the box.

The boot sequence already mounts the card and shows its type and size, and holding PLAYBACK at power-up runs a 256 KB write/read speed test. That's the first thing to try with a card in the slot.

## What's next

**1. Recording to SD.** The RECORD button writes the PSRAM's pre-trigger audio to the card, followed by the live stream, as a WAV file. The architecture follows from the numbers:

- **SD writes can stall 100–250 ms.** At 500 kB/s that's up to 125 KB arriving while the card is busy. The PSRAM absorbs it: the ring keeps recording from the interrupt side, never waiting on the main loop, and the SD writer reads it back at whatever pace the card allows.
- **Reads need pacing.** Back-to-back ring reads took the DSP to 66% ([part 7](07-psram-ring-buffer.md)). The SD writer should spread its reads over each block period, not grab them as fast as it can.
- **RAM:** the SD write buffer needs room, so the 8 KB debug-capture buffer will be turned off or shrunk.
- **WAV headers that bat software reads,** with the exact 250,000 Hz sample rate ([part 1](01-adc-and-dac.md)) and, ideally, the GUANO metadata chunk many bat tools use.
- **USB mass storage later.** The card is meant to appear on a PC over USB. FatFS and USB must never touch the card at the same time, which needs a proper ownership state machine. And because no VBUS sense reaches the MCU, USB attach has to be inferred from USB reset and suspend events.

**2. Playback, version 2.** Start from the detector's last bat pass rather than "the last 2 seconds", and let a knob set the factor.

**3. Squelch, stage 2,** once the detector has been tuned in the field ([part 9](09-bat-detector.md)).

**4. Finishing the DSP chain:**

- keep samples floating-point between stages;
- a band-pass around the 1–2 kHz output;
- review the notch filter and `dsp_filters.h`;
- recalibrate `DSP_GAIN` and the waterfall's range for the 140× front end;
- remove the LO offset from the waterfall's axis ([part 4](04-fft-waterfall.md)).

**5. Smaller items:**

- **The battery gauge.** It needs VREFINT, which is on the audio ADC.
- **Gain range switching or AGC.**
- **Housekeeping:** the RTC prescalers, disconnecting TIM3, the DAC start order, and tracking down the 24.95 kHz line.

**Open decisions:**

- Fit Y1, the 32.768 kHz crystal, so WAV files get real timestamps?
- Which control forces the sound on when squelch is active?
- GPS, which would fix the clock across power gaps: in or out?
- Should the MEMS mic's response be shaped at the top of the band, or left flat?
- USB audio (the detector as a 384 kHz USB microphone): deferred.

## What I'd change on V3

| Change | Why | Post |
|---|---|---|
| A real anti-alias filter below Nyquist | the 33 Ω + 1 nF is a reservoir, not a filter; 125–250 kHz still folds into the band | [1](01-adc-and-dac.md) |
| Fix VR1's symbol and footprint | the wiper isn't where the datasheet suggests; V2 needs a solder bridge | [1](01-adc-and-dac.md) |
| Pull-ups on every chip-select, and on SD MISO | defined bus state before firmware runs; an empty slot reads 0xFF | [5](05-v2-bring-up.md), this post |
| VBUS to a GPIO through a divider | firmware can't see USB being plugged in | this post |
| Plan the battery gauge's reference | VDDA falls with the battery near empty | [2](02-dma.md) |
| Check the USB-C socket's joints, and test both orientations | one dry joint made USB work one way round | [9](09-bat-detector.md) |
| Decide on Y1 | the RTC stops at power-off without it | [5](05-v2-bring-up.md) |

> **TODO (you):** anything else on your V3 list from the layout side.

## Lessons, all in one place

### Hardware

- Check DNP parts against the clock configuration. Every enabled oscillator is a boot dependency. ([5](05-v2-bring-up.md))
- Put pull-ups on every chip-select. ([5](05-v2-bring-up.md))
- Make amplifiers and loads default-off in hardware, and let firmware enable them. ([1](01-adc-and-dac.md))
- Check the slowest and the fastest clock each peripheral can reach. ([5](05-v2-bring-up.md))
- If the sample rate is close to the band edge, put a real anti-alias filter below Nyquist. ([1](01-adc-and-dac.md))
- Check part pinouts against a physical part, not just the datasheet. ([1](01-adc-and-dac.md))
- Plan the battery gauge's reference. ([2](02-dma.md))
- Route VBUS to a GPIO so firmware can see USB.
- Test a USB-C socket in both plug orientations at bring-up. ([9](09-bat-detector.md))

### Firmware

- Bring up one unknown at a time. ([5](05-v2-bring-up.md))
- Prefer names and checks that turn mistakes into compile errors. ([1](01-adc-and-dac.md), [5](05-v2-bring-up.md))
- Keep one source of truth for rates and sizes, checked by the compiler. ([1](01-adc-and-dac.md))
- Every wait loop gets a timeout. ([6](06-psram-bring-up.md))
- Measure timing with the cycle counter. Don't trust a test that can pass while the design is out of spec. ([6](06-psram-bring-up.md), [7](07-psram-ring-buffer.md))
- Instrument real-time code before adding work to it. ([3](03-dsp.md))
- Measure real-time margins at the optimisation level you run, and mark shared variables `volatile`. ([3](03-dsp.md))
- Set the GPIO speed of fast pins. ([6](06-psram-bring-up.md))
- Read every setting CubeMX generates for a peripheral, not just the ones you changed. ([5](05-v2-bring-up.md), [7](07-psram-ring-buffer.md))
- Anything calculated once and never written again belongs in flash, generated by a script tied to the configuration. ([3](03-dsp.md))
- Tabulate library maths calls in per-frame loops, and measure before and after. ([4](04-fft-waterfall.md))
- Check a transform against a reference with a known input. A wrong sign looks plausible. ([4](04-fft-waterfall.md))
- Ask the hardware where it is (DMA counters) rather than assuming a timing relationship. ([2](02-dma.md))
- Reserve the stack you need, so the linker can catch RAM overflow. ([5](05-v2-bring-up.md))
- When a timing limit depends on interrupt latency, check the priorities at start-up and measure the limit on every cycle. ([7](07-psram-ring-buffer.md))
- When a measurement disagrees with the calculation, split it into parts before choosing a fix. ([7](07-psram-ring-buffer.md))
- Measure an interrupt's cost under its heaviest load before making it run more often. ([7](07-psram-ring-buffer.md))
- On a Cortex-M4, put hot code in core-coupled RAM. Ordinary SRAM shares a bus between code and data. ([7](07-psram-ring-buffer.md))
- Check the disassembly before blaming a function. ([7](07-psram-ring-buffer.md))
- Run new control logic in "monitor only" mode before it drives hardware. ([9](09-bat-detector.md))
- Build the measurement path before tuning, and have the analysis check itself against the device. ([9](09-bat-detector.md))
- When USB fails, read the line state before blaming the clock. ([9](09-bat-detector.md))
- Choose a time-expansion factor from the whole output chain's passband. ([8](08-time-expansion.md))

### Process

- **Measure, don't hope.** Most of the real problems in this project (a DSP missing its deadline, a 20% slower SPI, an interrupt costing three times its estimate) were found by a number I'd made the firmware record, not by a symptom.
- **Write the decision down while it's fresh,** with the numbers and the alternatives. This series exists because of an engineering diary kept along the way.

> **TODO (you):** a closing paragraph: what you'd tell someone starting a project like this, and what you're most looking forward to (first recordings in the field?).

---

*Previous: [Part 9, teaching it to hear bats](09-bat-detector.md)* · *Back to the [introduction](00-introduction.md)*
