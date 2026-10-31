# Seeing sound: the FFT and the waterfall

*Part 4 of [Building a bat detector](00-introduction.md). The heterodyne lets you hear a narrow slice of the ultrasonic band. The waterfall lets you see all of it at once: a scrolling spectrogram from 0 to 125 kHz, where a bat pass shows up as a column of bright marks at its calling frequency before you've even found it with the tuning knob.*

---

## What the screen shows

The display is a 2.4" ILI9341 TFT, 240 × 320 pixels, used in portrait:

```
┌──────────────────────────┐
│ LO:  45.0 kHz       BAT  │  ← fixed header, 34 rows (LO readout, battery)
│                     3.9  │
├──────────────────────────┤
│ ░░░░▓░░░░░░░░░░░░░░░░░░░ │  ← newest row
│ ░░░░█░░░░░░░░░░░░░░░░░░░ │
│ ░░░░▓░░░░░░░░░░░░░░░░░░░ │    scrolling waterfall, 272 rows,
│ ░░░░░░░░░░░░░░░░░░░░░░░░ │    one row every 40 ms ≈ 10.9 s of history
│ ...                      │
├──────────────────────────┤
│  20   40   60   80  100  │  ← fixed footer, 14 rows (kHz ticks)
└──────────────────────────┘
   0 kHz ──────────► 125 kHz
```

Frequency runs left to right across the 240 columns. Time runs downwards: the newest row appears at the top and older rows slide down. Colour is loudness, from black through blue, cyan and green to red.

Getting there takes two pieces of work: an FFT fast enough to keep up with the audio, and a way to scroll a screen over a 21 MHz SPI bus without redrawing it.

## The FFT, hand-rolled

A Fast Fourier Transform turns a frame of samples into a spectrum. Each frame here is **1024 samples**:

- At 250 kHz that's 4.1 ms of audio.
- The bins are 250,000 / 1024 = **244 Hz** apart.
- There are 513 of them, from DC to Nyquist (125 kHz).

The obvious route on a Cortex-M4 is ARM's CMSIS-DSP library. I tried it on V1. The prebuilt static library brings every twiddle table for every FFT size with it, and it **overflowed the flash by about 9 KB**. So `fft.c` is hand-rolled, about 250 lines, with no library dependency.

### Packing a real FFT into a half-size complex one

An FFT is normally defined on complex numbers, but audio samples are real. Feeding 1024 real samples to a 1024-point complex FFT, with the imaginary parts set to zero, wastes half the work.

The standard trick, and the same one CMSIS's `arm_rfft_fast_f32` uses internally, is to pack the real samples in pairs as 512 complex numbers:

z[n] = x[2n] + j·x[2n+1]

Then:

1. Run a **512-point** complex FFT on z. That's about 2,300 butterfly operations: 256 per stage × 9 stages.
2. **Unpack** the 1024-point real spectrum from the result. Each output bin k combines Z[k] and the conjugate of Z[512 − k] to separate the even-sample and odd-sample spectra (A and B). They're then recombined as X[k] = A[k] + e<sup>−j2πk/1024</sup>·B[k].

The samples are packed as they arrive. `FFT_AddSamples`, called from the DSP interrupt, applies the window and writes each sample straight into the real or imaginary working array, so there's no separate input buffer.

### The sign that mattered

One of the bugs fixed on V1 was the sign of the angle in that unpack rotation. It's easy to get backwards, and a wrong sign doesn't produce an obviously broken spectrum. It produces a believable wrong one.

To show what it does, I ran the unpack both ways in numpy on a pure 45 kHz tone:

| Unpack rotation | Peak at 45 kHz | Peak at 80 kHz |
|---|---|---|
| Correct sign | 430 | 0.9 |
| Wrong sign | 184 | **389** |

With the wrong sign, the even and odd halves recombine without cancelling each other's mirror image. A 45 kHz call shows up strongest at **80 kHz** (125 − 45), with the real peak 7 dB down. On a bat detector that's a convincing ghost bat. The rule is now written in the project notes, and the generated table carries the formula in its comment: the table angle is **+2πk/N**, used as e<sup>−jθ</sup>.

> **TODO (you):** how the twiddle-sign bug showed itself on V1, and how you found it.

### The Hann window

An FFT assumes the frame repeats forever. A real frame's two ends don't line up, and the jump between them smears energy across the whole spectrum ("spectral leakage"), which would hide quiet calls beside loud ones. Each sample is multiplied by a **Hann window**, w[n] = ½(1 − cos(2πn / 1023)), which tapers both ends of the frame to zero.

The first two bins (DC and its leakage from the mic bias) are zeroed after the transform. Otherwise they'd set the colour scale and flatten everything else towards black.

## Keeping up: the 2.7× speed-up

The FFT runs in the main loop, not the interrupt. The handoff is a simple rule: **drop rather than corrupt**. While a full frame is waiting to be processed, the interrupt discards new blocks (counting them in `fftDroppedBlocks`) instead of overwriting the frame.

So the FFT never corrupts anything, but the faster it runs, the fewer blocks it drops. A frame of audio arrives every 4.1 ms. I added timing (`fftProcessCyclesAvg`, wall-clock, including any audio interrupts that preempt it) and measured:

| Unpack method | Cycles per frame | Time per frame |
|---|---|---|
| `cosf` / `sinf` per bin (original) | 981,137 | **5.77 ms** |
| Lookup table (now) | 363,198 | **2.14 ms** |

The original unpack called `cosf` and `sinf` for each of 511 bins on every frame. Software trigonometry is slow, and it was most of the FFT's time. Since the angles never change, they now come from a 4 KB table in flash, generated by the same script as the other DSP tables ([part 3](03-dsp.md)).

At 5.77 ms per frame, the FFT took longer than the 4.1 ms between frames, so it could *never* keep up with the audio. At 2.14 ms it can, with room to spare. That matters beyond a smoother display: future features such as auto-tune will read this spectrum too.

## Scrolling a screen over SPI

The display hangs off SPI2 at 21.25 MHz, shared with the SD card. Redrawing the whole waterfall area for every new row means 240 × 272 pixels × 2 bytes = 130 KB, which takes **49 ms** on the wire. That's longer than the 40 ms between rows, and the main loop would do nothing else.

The ILI9341 can scroll in hardware. Two commands do it:

- **VSCRDEF** (vertical scrolling definition) divides the panel into a fixed top area, a scrolling area and a fixed bottom area. Here: 34 rows, 272 rows and 14 rows.
- **VSCRSADD** (vertical scrolling start address) says which memory row appears at the top of the scrolling area.

The panel's memory becomes a circular buffer of rows. To add a row, the firmware writes 240 pixels (480 bytes, 0.18 ms) into the row just above the current top, then moves the start address to it. Everything else appears to shift down by one, without a single other pixel being sent.

The afiskon ILI9341 driver I'd built on didn't have these commands, so I patched them in. Hardware scrolling only works along the panel's *native* vertical axis, which is why the screen runs in portrait: rotating it to landscape in software would have made the "vertical" scroll run sideways.

## Not losing short calls

Frames arrive every 4.1 ms or so, but rows are drawn every 40 ms (25 rows a second, plenty for the eye). That's roughly ten frames per row, and a 2 ms *Myotis* call may be in only one of them. Averaging would wash it out.

So `Waterfall_Accumulate` **peak-holds**. For every FFT frame:

1. The 511 usable bins are mapped onto 240 columns, about 2.1 bins (520 Hz) per column, taking the loudest bin in each group.
2. Each column is converted to dB (20·log<sub>10</sub>).
3. Each column keeps the loudest value seen since the last row was drawn.

`Waterfall_FlushRow` turns those peaks into colours, sends the row and resets the accumulator. A call that appears in any one frame survives into the row.

The colour mapping is a five-stop gradient (black → blue → cyan → green → red) between a floor and a ceiling set by `Waterfall_SetRange`. Those were calibrated from measured dB values on V1, so they need redoing for V2's 140× front end.

## The header

The top 34 rows hold the LO readout and the battery.

- **The LO readout** is fixed at 13 characters (`LO: 120.0 kHz`), in a 16-pixel-wide font. A fixed width fits beside the battery readout, and it means a shorter number never leaves stale characters on screen. The second decimal place was dropped, because the knob moves in steps of about 30 Hz anyway.
- **The battery** sits as "BAT" over "3.9" in the remaining 26 pixels ([part 2](02-dma.md) has its accuracy problem).
- **During playback,** "PLAY 10x  20s" counts down in the LO's place ([part 8](08-time-expansion.md)).

### A bug that hasn't bitten yet

`BinToDisplayFreq`, which labels the axis, adds the LO frequency to every column's frequency. That was written for a design where the spectrum would be of the *mixed-down* signal. But the FFT is of the raw ADC input, so the axis is absolute: column 0 is always DC. The labels are only right today because the axis is drawn once, with the LO at 0. The offset needs to come out before anything redraws the axis with a real LO value.

## What I learned

- **A library's size can rule it out on a small part.** The prebuilt CMSIS-DSP library cost more flash than the whole FFT is worth.
- **A library maths call inside a per-frame loop is a hidden cost.** Tabulate it when the arguments are fixed, and measure before and after.
- **A wrong sign in a transform doesn't look broken. It looks plausible.** Check a transform against a reference (numpy) with a known tone.
- **Let the display hardware do the work.** Hardware scroll turned a 49 ms redraw into a 0.18 ms row.
- **Peak-hold anything that's sampled more slowly than the events you care about.**

---

*Next: [Part 5, bringing up V2](05-v2-bring-up.md): a new board, new pins, and one unknown at a time.*

*Previous: [Part 3, the DSP](03-dsp.md)*
