# Bringing up V2, one unknown at a time

*Part 5 of [Building a bat detector](00-introduction.md). Parts 1–4 describe a signal chain that worked on V1. This post is about moving it onto a new board, where nearly every pin had moved, and the habits that kept each problem small.*

---

## What changed between the boards

V2 isn't a tidied-up V1. I redesigned it as a four-layer board, and the changes touch almost everything the firmware talks to:

| | V1 | V2 |
|---|---|---|
| Heterodyne | analogue mixer on the board, square-wave LO from TIM3, plus the digital mixer | fully digital; no mixer on the board |
| SPI1 | touch controller | **PSRAM** (8 MB APS6404L), alone on the bus |
| SPI2 | ILI9341 display | display **and SD card**, shared; the display has its own chip-select |
| Touch | wired | not wired |
| Amplifier | no enable line | **`AMP_EN`**, off by default (R9 pull-down) |
| Buttons, LEDs | V1 pins | moved to new pins |
| Sample rate | 192.09 kHz | 250 kHz ([part 1](01-adc-and-dac.md)) |

> **TODO (you):** why four layers, what you changed in the layout from V1, and anything that went wrong building the board.

## Port first, features second

The firmware port started on 29 October 2026. The temptation with a new board is to start on the new features straight away: the PSRAM and the SD card were why V2 existed. I didn't. The first goal was to get V1's features (heterodyne, waterfall, knobs) running on V2, unchanged, before writing a line of new code.

The reason is diagnosis. If you bring up new hardware and new code together and something doesn't work, there are two places to look, and every symptom fits both. With V1's proven code on V2's new hardware, a fault could only be the hardware or the port.

The bring-up order added **one unknown at a time**:

1. Clocks and an LED
2. Display
3. ADC → DSP → DAC, and the amplifier
4. ADC2: knobs and battery
5. PSRAM
6. SD card

It paid off. Every problem I hit had one obvious suspect: the volume pot's wiring at step 3, the SPI pin speed at step 5.

## Step 0: regenerate, and name pins after the schematic

V2's `main.c` and `main.h` were still generated from V1's CubeMX project. I changed the pins in CubeMX and regenerated, rather than editing the generated code, because anything outside the `USER CODE BEGIN/END` blocks is wiped on the next regeneration.

I also renamed the pin labels to match the schematic: `RECORD_BUTTON`, `PLAYBACK_BUTTON`, `LED0`, `SPI2_SD_CARD_CS`. That sounds cosmetic, but it isn't. CubeMX turns each label into macros (`LED0_Pin`, `LED0_GPIO_Port`), and those macros are the interface between the hardware description and the code. With matching names, code that refers to a pin that has moved or been renamed **fails to compile** instead of quietly driving the wrong pin. One example: a leftover V1 label put `RECORD_BUTTON` on PB9, which isn't connected to anything on V2. It would have compiled happily and read a floating pin forever.

## Step 1: the board that looked dead

The 32.768 kHz crystal for the real-time clock, Y1, is marked **DNP** (do not populate) on the V2 BOM. But the CubeMX project still had the low-speed external oscillator (LSE) enabled.

With LSE enabled and no crystal, `HAL_RCC_OscConfig` waits for the oscillator to start, times out after 5 seconds, and calls `Error_Handler`. That disables interrupts and loops forever. No LED, no display, nothing: **the board looks dead**, when really it's sitting in an error loop that nothing reports.

The fix was to turn LSE off and clock the RTC from the 16 MHz crystal divided by 32 (500 kHz). It has a cost: the RTC now stops when the power switch is off, so WAV recordings can't carry real timestamps until Y1 is fitted. Whether to fit it is still an open decision. The RTC's prescalers also have to match its new clock: (124 + 1) × (3999 + 1) = 500,000, where CubeMX's defaults (127/255) are for 32.768 kHz. That's still to do.

**Lesson:** every oscillator enabled in CubeMX is a boot dependency. Check the DNP marks on the BOM against the clock configuration.

## Step 2: the display, and the chip-selects nobody set

SPI2 is shared by the display (chip-select on PA8) and the SD card (PB2). Each device should only listen while its chip-select is low.

`MX_GPIO_Init` drives every output pin **low** at start-up unless CubeMX is told otherwise, and that includes chip-selects. My hardware had partly anticipated this:

- The PSRAM's chip-select has a 100k pull-up, so it's deselected during reset and programming. But as soon as `MX_GPIO_Init` runs, the GPIO overrides the pull-up and drives it low.
- The SD card's chip-select has no pull-up at all.

With both the display and the SD card selected, the SD card would see the display's start-up traffic as commands. The fix: drive every chip-select high in user code before the first SPI transfer, and set the pins' initial output level to High in CubeMX.

**Lesson:** on a shared bus, every chip-select must be defined from the first instruction. Pull-ups on chip-select lines cover reset and programming, before firmware runs, so put them on all of them.

### The SD card's slowest clock

SD cards must be initialised at **400 kHz or less** before they'll switch to a fast clock. SPI2 is clocked from the APB1 bus, and its slowest setting is APB1 ÷ 256. At 170 MHz that's 664 kHz: too fast.

So APB1 now runs at 85 MHz (÷2). SPI2 then gives 332 kHz at ÷256 for initialisation, and 21.25 MHz at ÷4 for data and the display, the same speed as V1.

But TIM2, the sample clock, is also on APB1. Didn't halving APB1 halve the sample rate? No, because of an STM32 detail that's easy to forget: **timers receive double the bus clock whenever the bus prescaler isn't 1**. With APB1 at ÷2, the APB1 timer clock is still 170 MHz, and `pipeline_config.h`'s arithmetic still holds.

**Lesson:** check the *slowest* clock a peripheral can produce, not just the fastest.

## Step 3 onwards

The rest of the bring-up is covered in the posts about each subsystem:

- **Audio.** It was silent until firmware drove `AMP_EN` ([part 1](01-adc-and-dac.md)). The volume pot's wiper was on the wrong pin, needing a solder bridge ([part 1](01-adc-and-dac.md)). The DSP was missing its deadline at 250 kHz ([part 3](03-dsp.md)).
- **Knobs and battery.** Moved to a DMA scan on ADC2 ([part 2](02-dma.md)).
- **PSRAM.** A hang that turned out to be a pin-speed setting ([part 6](06-psram-bring-up.md)).
- **SD card.** The driver is written, but I had no card to test it with ([part 10](10-lessons-and-next.md)).
- **USB.** A clock-trim value, and a USB-C socket that only works one way round ([part 9](09-bat-detector.md)).

## CubeMX defaults worth checking

A pattern emerged over the port. Several problems came from a CubeMX default that suits some other kind of device, sitting quietly in a field I'd never touched. Here are all of them in one place:

| Default | What it does | Fix |
|---|---|---|
| GPIO output level Low | every chip-select starts selected | set chip-select pins to High |
| SPI pins at `GPIO_SPEED_FREQ_LOW` | pads can't toggle at 42.5 MHz; SPI1 hung ([part 6](06-psram-bring-up.md)) | High or Very High for anything above ~20 MHz |
| SPI data size 4 bits (on G4) | sends nibbles | set 8 bits explicitly |
| SPI "NSSP Mode" Enabled (on G4) | 2 idle clocks after every byte: 20% slower ([part 7](07-psram-ring-buffer.md)) | Disabled |
| `HSI48CalibrationValue = 32` | the middle of a 6-bit trim; the G4's is 7-bit, middle 64 ([part 9](09-bat-detector.md)) | write 64 in user code |
| RTC prescalers 127/255 | correct for 32.768 kHz, wrong for 500 kHz | 124/3999 |
| LSE enabled | boot hangs without the crystal | off while Y1 is DNP |

**Lesson:** read every field the code generator writes for a peripheral, not just the ones you changed.

## Build-system traps

A few things about the toolchain cost me time and are worth writing down:

- **`main.c` has its own include-path list** in CubeIDE's `.cproject`, a per-file override. A header folder added at project level also has to be added to `main.c`'s list, or `main.c` alone can't find it.
- **CubeIDE generates the Debug makefiles.** New source files only appear in them after a build inside the IDE, so a command-line build can't see a new file until then.
- **GCC 14 treats calling an undeclared function as an error,** not a warning. Removing a header include means removing the calls that needed it.
- **The `.map` file lists garbage-collected symbols under "Discarded input sections".** They use no memory, so don't count them when budgeting RAM.
- **Starting a debug session rebuilds and flashes.** If you want a "before" measurement of the old code, take it before launching the debugger on the new.

### A 2 KB stack, reserved on purpose

FatFS and the SD driver call deep chains of functions, so the stack reservation went up to 2 KB. The linker's stack reservation is only a check: the stack really grows down into whatever RAM is free, with no warning when it collides with data. Reserving the stack you need means that if static data ever grows into that space, **the build fails**, instead of the hardware failing at run time.

## Housekeeping habits

- **Checkpoint commits on a `v2` branch.** Each step of the port is a reviewable diff, and V1 stays buildable from the older history.
- **Disconnect, don't delete.** V1's touch-screen driver hardcodes `hspi1`, PB4 and PA10, which on V2 are the PSRAM's bus. `tim3_lo.c` drove V1's analogue LO. Neither has a job on V2, but both stay in the project with their includes and calls removed, and the TIM3 technique is written up in `Docs/timer_waveform_output.md`. A future board may want them.

## What I learned

- **Bring up one unknown at a time.** Proven code on new hardware, then new code on proven hardware.
- **Choose names so that mistakes fail to compile.** A compile error is the cheapest bug there is.
- **Every enabled oscillator is a boot dependency.** Check them against what's actually fitted.
- **Define every chip-select from the first instruction,** and pull them all up in hardware.
- **Check the slowest clock a peripheral can produce,** and remember that STM32 timer clocks double when their bus is divided.
- **CubeMX's defaults are somebody else's design decisions.** Read them.

---

*Next: [Part 6, PSRAM bring-up](06-psram-bring-up.md): 8 MB of memory with an 8 µs rule.*

*Previous: [Part 4, the FFT and the waterfall](04-fft-waterfall.md)*
