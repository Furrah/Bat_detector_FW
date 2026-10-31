# From ultrasound to voltage and back: the ADC and DAC

*Part 1 of [Building a bat detector](00-introduction.md). Everything digital in this project sits between two conversions: the ADC that turns the microphone's voltage into numbers, and the DAC that turns numbers back into a voltage for the speaker. This post is about those two edges, and why the sample rate ended up at exactly 250,000 samples a second.*

---

## The front end, briefly

I designed the analogue front end to be as simple as it could be.

- **Microphone:** an SPVA1A0LR5H-1 MEMS mic, rated to about 80 kHz.
- **Preamp:** a single OPA365 op-amp in a non-inverting stage, biased to mid-rail, with a fixed gain of 1 + (39k + 100k) / 1k = **140**. There's no gain pot.
- **ADC pin:** 33 Ω in series and 1 nF to ground.

Two numbers from that chain matter later.

**The op-amp's bandwidth.** The OPA365's gain-bandwidth product is 50 MHz. At a gain of 140 the closed-loop bandwidth is about 50 MHz / 140 ≈ **360 kHz**. So anything the mic picks up, up to about 360 kHz, reaches the ADC.

**The RC at the ADC pin.** 33 Ω and 1 nF have a corner at 1 / (2π × 33 Ω × 1 nF) ≈ **4.8 MHz**. That's far too high to be an anti-alias filter, and it isn't meant to be one. It's a *charge reservoir*. The STM32's ADC is a successive-approximation type: to take a sample, it connects a small internal capacitor to the pin for a short window, then disconnects it and converts. Here that window is 2.5 ADC clocks at 42.5 MHz: **59 ns**. No op-amp settles a sudden capacitive load in 59 ns. The 1 nF can, because it's about 200 times bigger than the ADC's sampling capacitor, so its voltage barely moves when the ADC takes its sip of charge. The op-amp then has the rest of the 4 µs sample period to top it up through the 33 Ω.

> **TODO (you):** why the OPA365 and a gain of 140, and anything you learned laying out the front end.

## Sampling, Nyquist and aliasing

An ADC sampling at a rate f<sub>s</sub> can only represent frequencies up to f<sub>s</sub>/2, the **Nyquist frequency**. A signal above Nyquist doesn't vanish. It *folds back* and appears at f<sub>s</sub> − f, where it's indistinguishable from a real signal at that frequency. That's **aliasing**.

V1 sampled at 192.09 kHz, so its Nyquist frequency was 96 kHz. That's below the 100 kHz the detector has to reach, and well below the lesser horseshoe bat's 105–112 kHz. With the op-amp passing everything up to 360 kHz, a 110 kHz horseshoe call would have come out at 192.09 − 110 = **82 kHz**: a perfectly believable bat call at the wrong frequency.

The textbook fix is an anti-alias filter: a steep analogue low-pass below Nyquist, ahead of the ADC. The board doesn't have one (the 4.8 MHz RC is a reservoir, not a filter), so on V2 the defence had to be the sample rate itself.

## Choosing 250 kHz

The sample clock comes from a timer, TIM2, counting at 170 MHz. Each time it overflows, it triggers a conversion. So the sample rate is 170 MHz divided by a whole number, and the choices are:

| Divisor | Sample rate | Nyquist |
|---|---|---|
| 885 (V1) | 192,090.4 Hz | 96.0 kHz |
| 680 | **250,000 Hz exactly** | 125 kHz |
| 340 | 500,000 Hz exactly | 250 kHz |

**250 kHz** puts Nyquist at 125 kHz, above the whole 20–120 kHz band. It's also an *exact* number, which matters more than it looks. A WAV file stores its sample rate as an integer, and bat-identification software reads it to label the frequency axis. 192,090.4 Hz can't be written down exactly.

500 kHz would fold nothing below 250 kHz, but it would double everything downstream: CPU time per second of audio, PSRAM bandwidth, SD bandwidth. For V2, 250 kHz cost this much:

| | 192.09 kHz | 250 kHz |
|---|---|---|
| CPU cycles per sample | 885 | 680 |
| Data rate to PSRAM and SD | 384 kB/s | 500 kB/s |
| History the 8 MB PSRAM holds | 21.8 s | 16.8 s |

To be honest about what this doesn't fix: anything between 125 and 250 kHz still folds back into the band. A Natterer's bat sweep starting at 140 kHz would briefly appear at 110 kHz. The mic is only rated to 80 kHz, so there's probably little energy up there to fold. But "probably" isn't a measurement, and a real anti-alias filter is on the V3 list.

### One number, one place

Changing the sample rate turned up something embarrassing. The rate existed in three places in the V1 code, with three different values: `192060.0f` in the DSP, `192000` in the waterfall, and the real 192,090.4 Hz that the timer actually produced. Block sizes were defined in three files too. Every one of them "worked", because each was only slightly wrong.

V2 has one header, `pipeline_config.h`, that everything else derives from, and the compiler checks the rules:

```c
#define AUDIO_TIMER_CLOCK_HZ    170000000UL
#define AUDIO_TIMER_PERIOD      680UL
#define AUDIO_SAMPLE_RATE_HZ    (AUDIO_TIMER_CLOCK_HZ / AUDIO_TIMER_PERIOD)    /* 250 000 */

_Static_assert((AUDIO_TIMER_CLOCK_HZ % AUDIO_TIMER_PERIOD) == 0UL,
        "sample rate must be a whole number of Hz for the WAV header");
```

One check can't be done at compile time. TIM2's period is set in CubeMX, and the generated code doesn't know about the header. So `main.c` compares the two at start-up and stops in `Error_Handler` if CubeMX's value ever drifts. A wrong sample rate would otherwise put every frequency on the display, and in every recording, quietly in the wrong place.

## One timer for both directions

TIM2's update event is routed (as its TRGO output) to **both** the ADC and the DAC:

- **ADC1** starts one conversion of the mic (PA0) on every tick.
- **DAC1** loads its next output value on every tick.

Input and output therefore run off the same clock, sample for sample, and can never drift apart. Whatever the delay between a sound arriving and its processed version leaving, it stays fixed.

The ADC side is configured like this:

| Setting | Value | Why |
|---|---|---|
| Resolution | 12 bits | the G4's maximum |
| ADC clock | 170 MHz / 4 = 42.5 MHz | |
| Sampling time | 2.5 clocks = 59 ns | the shortest available; the 1 nF reservoir makes it possible |
| Conversion time | 2.5 + 12.5 = 15 clocks = 0.35 µs | so the ADC is idle for 91% of each 4 µs period |
| Calibration | at every start-up | `HAL_ADCEx_Calibration_Start`, before the first conversion |
| Trigger | TIM2 TRGO | one conversion per tick, no free-running |

Each result is a 12-bit code from 0 to 4095. With the op-amp biased to mid-rail, silence sits near 2048, so the first thing the DSP does with every sample is subtract 2048.

## The DAC side

The DAC is 12 bits too, with its output buffer enabled, on PA4. It works in reverse: the DSP produces signed samples centred on zero, adds 2048, clamps the result to 0–4095, and the DAC turns it into a voltage between 0 and 3.3 V. **2048 is silence.**

From PA4 the signal goes through a two-pole RC low-pass (corner around 6–10 kHz) into a **TS4990** bridge-tied-load amplifier. The amp runs from the battery with a gain of about 1.5 and drives the speaker.

That RC filter does two jobs:

- **Reconstruction.** A DAC holds each value for a full sample period, so its output is a staircase. The steps carry copies (images) of the signal around multiples of 250 kHz. The RC smooths them away.
- **Keeping the speaker out of the ultrasound.** Everything the detector ever plays is audio: the heterodyne output is under 8 kHz, and time-expanded playback is under 8 kHz too ([part 8](08-time-expansion.md)). So a filter at 6–10 kHz costs nothing useful.

> **TODO (you):** the RC values, and why a BTL amp from the battery rather than from 3.3 V.

### The silent first boot

The first time V2 ran the audio path, it was completely silent.

The TS4990 has a standby pin, and on V2 I'd put a 100k pull-down (R9) on it, so the amp is **off** until firmware says otherwise. That's the right hardware choice: an amp that powers up on its own pops and draws current while the MCU is still in reset. But nothing in the V1 firmware knew about `AMP_EN`, because V1 didn't have one.

The fix is one line, but *where* it goes matters. The amp is enabled only after `DAC_Start()`, when the DAC is already holding steady at mid-scale. The amp wakes up to a quiet, stable input, and there's no turn-on pop.

## Volume: a knob read by software

There's no analogue volume control. The volume pot (VR1) is read by the second ADC every 10 ms ([part 2](02-dma.md) explains how), and applied as a digital gain in the DSP.

- **Square law.** The gain is (knob position)², from 0 to 1. Hearing is logarithmic: with a linear gain, all the audible change happens in the bottom quarter of the knob's travel. With square law, half-way gives 0.25, or −12 dB, and the knob feels much more even.
- **Before the conversion to integer.** The gain is applied while the sample is still floating-point, before it's truncated for the DAC. Scaling a value that's already been truncated throws away resolution at exactly the low volumes where you'd notice.
- **Silent until the first reading.** The gain starts at zero, so the detector never blasts at full volume for the first 10 ms after power-up.

On the hardware side, VR1 gave me my own bring-up bug. The RK10J12E002L's datasheet pinout is misleading, and the wiper isn't on the pin my footprint routes to PA5. On V2 a solder bridge connects the wiper; the symbol and footprint get fixed for V3.

## The budget this sets

Everything downstream works to this:

| | Value |
|---|---|
| Sample rate | 250,000 Hz |
| CPU cycles per sample | 680 |
| Samples per block | 256 |
| Block period | 1.024 ms (174,080 CPU cycles) |
| Raw data rate | 500 kB/s |

680 cycles per sample sounds generous. But the CPU can't spend an interrupt on every sample: just entering and leaving one would eat a large slice of the budget before any processing happens. Something else has to move the samples. That's [part 2](02-dma.md).

## What I learned

- **Choose the sample rate from the requirement and the clock tree together.** An integer divisor gives an exact rate, and an exact rate is what a WAV header needs.
- **Know what each component at an ADC pin is for.** The 33 Ω + 1 nF is a charge reservoir for a 59 ns sampling window, not an anti-alias filter. If the sample rate sits close to the band edge, the board needs a real filter below Nyquist.
- **If two files must agree on a number, make one derive from the other and let the compiler check it.**
- **Default-off for amplifiers and loads,** in hardware, and let firmware enable them once their input is stable.
- **Check a part's pinout against the physical part,** not only the datasheet.

---

*Next: [Part 2, DMA](02-dma.md): how 250,000 samples a second get in and out without the CPU touching them.*

*Previous: [Part 0, introduction](00-introduction.md)*
