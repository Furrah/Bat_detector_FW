# The DSP: a 1 ms deadline

*Part 3 of [Building a bat detector](00-introduction.md). [Part 2](02-dma.md) delivers a block of 256 samples every 1.024 ms and hands it to one function, `DSP_ProcessBlock`. This post is about what that function does, how a one-bit mistake in it turned the detector into a feedback loop, and how I found out V2's version was missing its deadline.*

---

## What happens to a block

Every block goes through this chain, sample by sample, inside the ADC's DMA interrupt:

```
ADC code ─► −2048 ─► 10 kHz high-pass ─┬─► ×8 ─► mixer (× LO) ─► 8 kHz low-pass ×2 ─► × volume ─► +2048, clamp ─► DAC
                                       │
                                       └─► Σ x² over the block ─► bat detector (part 9)

raw ADC codes ─► PSRAM ring (part 7), FFT (part 4), USB debug capture (part 9)
```

1. **Remove the bias.** The op-amp sits at mid-rail, so each 12-bit code has 2048 subtracted to centre it on zero.
2. **High-pass at 10 kHz.** This removes what's left of the bias, and everything audible.
3. **Gain** (×8, a fixed `DSP_GAIN`).
4. **Mix** with the local oscillator: the heterodyne itself.
5. **Low-pass at 8 kHz,** twice. This keeps only the audible difference frequency.
6. **Volume:** the square-law gain from the knob ([part 1](01-adc-and-dac.md)).
7. **Back to DAC codes:** add 2048 and clamp to 0–4095.

The finished block goes to `DAC_WriteBlock` ([part 2](02-dma.md)). Along the way, the same block feeds the bat detector, the FFT and the debug link.

## The heterodyne, digitally

A heterodyne detector rests on one trigonometric identity:

sin(a) · sin(b) = ½ [cos(a − b) − cos(a + b)]

Multiply the input by a sine wave at the LO frequency, and every component of the input splits into two: one at the *difference* frequency and one at the *sum*. With the LO at 45 kHz, a pipistrelle call at 47 kHz becomes 2 kHz (audible) plus 92 kHz (ultrasonic). The 8 kHz low-pass throws away the sum and anything else outside the window. What's left is the sound of everything within about **±8 kHz of the LO**, shifted down to audio.

Two properties of this are worth knowing:

- **It folds.** 43 kHz and 47 kHz both come out as 2 kHz. You hear how far a call is from the LO, not which side it's on.
- **It halves the amplitude.** Note the ½ in the identity. That's why time-expansion playback ([part 8](08-time-expansion.md)) uses half the heterodyne's gain to sound equally loud.

### V1 had a mixer on the board

On V1 I'd also built an analogue mixer, fed by a square-wave LO from TIM3. A timer channel makes a clean, CPU-free square wave, and the technique is written up in `Docs/timer_waveform_output.md`. A square wave has a catch as an LO, though. It contains odd harmonics (3f, 5f, …), and each one mixes with the input too, so a 45 kHz LO also listens at 135 kHz.

V2 has no analogue mixer: the heterodyne is entirely in software, and TIM3 has no job left (it's still running, waiting to be disconnected).

> **TODO (you):** what the analogue mixer on V1 was like to use, and why you moved to fully digital mixing on V2.

### The oscillator: a phase accumulator

The LO is a **numerically controlled oscillator (NCO)**. A 32-bit `phase` variable represents one full cycle as 0 to 2³². Every sample, a fixed `phaseIncrement` is added, and the integer simply wraps round at the end of each cycle:

```c
phaseIncrement = frequency × 2³² / sample_rate;     // 45 kHz → 773,094,113
```

At 250 kHz, one step of the increment is 250,000 / 2³² ≈ 58 µHz, so the frequency resolution is effectively perfect. The top bits of `phase` index a one-cycle sine table, and the mixer is an integer multiply:

```c
int32_t DSP_Mixer(int32_t sample)
{
    uint32_t index = phase >> 23;               // top 9 bits: 0..511
    int32_t oscillator = dspSineTable[index];   // 512 entries, ±32767
    int32_t mixed = (sample * oscillator) >> 15;
    phase += phaseIncrement;
    return mixed;
}
```

### The bug that made it howl

The sine table has 512 entries: 2⁹. A 32-bit phase shifted right by 23 leaves the top 9 bits, which index the whole table.

The first version shifted by **24**. That leaves only the top 8 bits, indexes 0 to 255: the *first half* of the table. So over each LO cycle the oscillator traced only the positive half of a sine, from 0 up to the peak and back to 0, and then started again. Its output never went negative. A waveform like that is mostly DC: its average is 2/π, about **64% of full scale**, with the LO frequency and its harmonics riding on top.

Multiplying by a constant doesn't shift anything; it just passes the input through. So instead of moving the ultrasound down to audio, the mixer passed the input straight to the speaker, including the audible sound in the room. And the most prominent audible sound in the room was the speaker. The mic picked it up, the mixer passed it on, the speaker played it louder, and the loop closed: **acoustic feedback**.

The fix was one character, `>> 24` to `>> 23`, and it's in the commit that marked V1 complete.

> **TODO (you):** what the feedback sounded like, and how you tracked it down.

## The filters

All three filters are **biquads**: second-order IIR filters, each five multiplies per sample. They're designed as Butterworth sections by the bilinear transform, with the cutoff pre-warped (K = tan(π·f<sub>c</sub>/f<sub>s</sub>)) so that the corner lands where it's asked to.

**The 10 kHz high-pass** sits at the front, and it does several jobs at once:

- It removes the ADC's DC bias. Silence isn't exactly code 2048: the FFT code has a comment that the real offset is nearer 2079.
- It removes the audible band. A second-order filter falls at 12 dB per octave, so wind and handling noise at 1 kHz are about 40 dB down. That includes the speaker's own output, which is a second line of defence against the feedback loop above.
- Its output also feeds the bat detector, which needs exactly that band ([part 9](09-bat-detector.md)).
- Its corner is 10 kHz, not 20, on purpose. The free-tailed bat (9–14 kHz), the greater noctule (around 12–14 kHz) and many social calls (14–22 kHz) live down there.

V1 started with a DC blocker and a 50 Hz notch filter in this position. The high-pass replaced both. The notch is still initialised but not connected. It's on the list to review, together with the rest of `dsp_filters.h`.

**The two 8 kHz low-passes** come after the mixer. They remove the sum frequencies and anything outside the listening window.

## Measuring the deadline

Moving V2 to 250 kHz cut the time per sample from 885 cycles to 680. I didn't know whether the DSP would still fit, so before adding anything else I made the firmware tell me.

`ADC_RunBlockCallback` reads the cycle counter before and after each `DSP_ProcessBlock` and keeps the worst case as a percentage of the block period: `dspLoadMaxPercent`, visible in CubeIDE's Live Expressions.

The first reading at 250 kHz: **101%**. The DSP was taking 1.03 ms to process 1.024 ms of audio. It was missing its deadline, so in the worst case the DMA was overwriting the block while it was still being processed.

The more uncomfortable finding was about V1. At 192 kHz the same code would have been at about **78%**. V1 had been running close to the edge since August without my knowing, because nothing was measuring it.

## `-O0` isn't neutral

The Debug configuration compiled at `-O0`, no optimisation, as CubeIDE sets it by default. I'd always thought of `-O0` as the honest baseline, with optimisation as a bonus. The disassembly of the filter function shows why that's wrong. At `-O0`, every variable lives in RAM and is reloaded from the stack on every use:

| `Biquad_Process` | `-O0` | `-Og` |
|---|---|---|
| Instructions | 55 | 24 |
| Of which RAM loads and stores | ~39 | 13 |
| Filter pointer reloaded from the stack | 15 times | never |

Three biquads per sample, 250,000 samples a second, and most of the work at `-O0` was shuffling values between registers and the stack. Switching the Debug build to `-Og` (GCC's "optimise, but keep it debuggable" level) changed this:

| | `-O0` | `-Og` |
|---|---|---|
| DSP load | 101% (687 cycles/sample) | **37%** (~252 cycles/sample) |
| Flash used | 95.2 KB | 66.8 KB |
| PSRAM chip-select time ([part 6](06-psram-bring-up.md)) | 5.8 µs | 4.1 µs |

**What I gave up:** some local variables now show as "optimized out" in the debugger, and single-stepping can jump between lines. The audio output is bit-for-bit identical, because it's the same operations in the same order.

**What it requires:** every variable shared between an interrupt and the main loop must be `volatile`, or the optimiser can keep a stale copy in a register. `-O0` hides missing `volatile`s, because it reloads everything anyway. I checked them all, and they were already marked.

## Moving constant tables into flash

With the CPU sorted, RAM became the problem. Recording to SD needed about 5.5 KB more, and only 5.5 KB was free.

Three tables were sitting in RAM:

| Table | Size |
|---|---|
| Hann window for the FFT | 4 KB |
| FFT twiddle factors | 2 KB |
| Heterodyne sine table | 1 KB |

All three were calculated at boot with `sinf` and `cosf` and never written again. Anything calculated once and never written again is a **constant**, and constants belong in flash.

Rather than paste numbers into a C file, a Python script (`Tools/gen_dsp_tables.py`) generates them into `Core/DSP/Tables/`. Two details made this safe:

- **The tables can't go stale.** The script reads `AUDIO_FFT_LENGTH` and `LUT_SIZE` from the firmware's headers, and static asserts in `fft.c` and `dsp.c` fail the build if the tables were generated for different sizes.
- **The values didn't change.** The script reproduces the firmware's float32 arithmetic exactly: the angle is computed in double, rounded to float, then passed to sine or cosine. The new tables match the old boot-time ones to within one float LSB, so neither the audio nor the waterfall's calibration moved.

Flash is slower than RAM: at 170 MHz every flash access needs 4 wait states, partly hidden by the flash accelerator's cache. The sine table and the window are now read from flash for every sample, so I expected the DSP load to rise a little. I measured it: 37% before, **36%** after. Reading the tables from flash cost nothing measurable, and RAM gained 7 KB.

## Where the DSP stands

| Condition | Worst-case DSP load |
|---|---|
| Heterodyne only, `-Og` | 37% |
| Plus the PSRAM ring recording ([part 7](07-psram-ring-buffer.md)) | 45% |
| Time-expansion playback running ([part 8](08-time-expansion.md)) | 46% |
| Ring reading back to back (worst case) | 66% |

What's still to do:

- **Float throughout.** Between stages, the sample is converted back to `int32_t` (for example `sample = (int32_t)highPassed`), so each stage truncates. The plan is to keep it floating-point from end to end.
- **A band-pass** around the 1–2 kHz output, and a decision about the notch filter.
- **Recalibrate `DSP_GAIN`** (and the waterfall's range) for V2's 140× front end. The current values were tuned on V1.

## What I learned

- **Instrument real-time code before adding more work to it.** V1 was at 78% of its deadline and I had no idea.
- **`-O0` is a debugging aid, not a neutral baseline.** Measure real-time margins in the build you actually run, and mark every shared variable `volatile`.
- **Anything calculated once and never written again belongs in flash,** generated by a script tied to the configuration, so it can't go stale.
- **Check the bit widths in index arithmetic.** A shift of 24 instead of 23 turned a mixer into a feedback loop.
- **Measure before and after,** so a gain is a number and not a hope. The "slower" flash tables cost nothing.

---

*Next: [Part 4, the FFT and the waterfall](04-fft-waterfall.md): seeing the calls as well as hearing them.*

*Previous: [Part 2, DMA](02-dma.md)*
