# Building a bat detector

*Part 0: what this series is about, and how it fits together.*

---

## Listening to something you can't hear

On a summer evening, bats are calling all around you, and you hear none of it. They find their way and their food by echolocation: short, loud calls whose echoes tell them what's ahead. Almost all of it is above the range of human hearing.

The numbers make the point. Human hearing tops out at around 20 kHz when you're young and falls with age. European bats call from about **10 kHz to well over 100 kHz**:

- The common pipistrelle, the bat you're most likely to meet in a garden, calls at around **45 kHz**.
- Noctules and serotines sit lower, at 20–30 kHz.
- The lesser horseshoe holds a pure tone at **105–112 kHz**.
- The *Myotis* species sweep through 100 kHz or more of bandwidth in a few milliseconds.

The calls are short too, typically 1 to 30 ms, a few to a few dozen times a second.

A **bat detector** turns those calls into something a person can hear. There are three classic ways to do it:

- **Heterodyne.** Mix the signal with a tunable oscillator (the "LO") and listen to the difference frequency. Tune to 45 kHz, and a 47 kHz call comes out as a 2 kHz tone. It's live and sensitive, but you only hear a narrow band around wherever you've tuned.
- **Frequency division.** Count zero crossings and divide by 10. You hear every frequency at once, but most of the call's detail is lost.
- **Time expansion.** Record at full bandwidth, then play it back 10× slower, so every frequency drops by 10× and every call lasts 10× longer. This keeps everything, but it isn't live.

Serious detectors also record the full-bandwidth signal to a card, so the calls can be identified later from their spectrograms.

> **TODO (you):** how the project started: what got you into bats, and why build a detector rather than buy one.

## What I set out to build

The benchmark I picked is the **Pipistrelle Mini**, a commercial handheld detector. It has heterodyne with auto-tune, time expansion, a spectrum display and microSD recording. My aim was to beat it on capability at a competitive cost, while learning analogue and digital electronics properly along the way.

In numbers, the requirements were:

- **Frequency range 20–120 kHz.** Anything up to 100 kHz must be detected.
- **Handheld and battery powered.** One LiPo cell.
- **Live listening** (heterodyne), plus a **live spectrum display** (a scrolling waterfall).
- **Recording**, with a pre-trigger: pressing RECORD should keep the seconds *before* the press.
- **Time-expansion playback.**

## The hardware

This series is about the second board, V2. It's a four-layer PCB I designed in KiCad, built around an **STM32G431**: a 170 MHz Cortex-M4 with an FPU, 128 KB of flash and **32 KB of RAM**. That last number shapes a lot of this story.

```
 MEMS mic ─► OPA365, gain 140× ─► ADC1 ─DMA─► RAM ─► DSP ─► DAC ─DMA─► TS4990 amp ─► speaker
                                    ▲                 │
                        TIM2: 250 kHz sample clock    ├─► PSRAM ring buffer, 8 MB (SPI1, DMA2)
                                                      ├─► bat-call detector ─► LEDs
                                                      └─► FFT ─► waterfall on a 2.4" TFT (SPI2)

 LO knob, volume, battery ─► ADC2 ─DMA─► RAM           microSD (SPI2)     USB-C: charging + debug link
```

Going through the chain:

- **Microphone:** an SPVA1A0LR5H-1 MEMS mic, rated to about 80 kHz.
- **Preamp:** a single OPA365 op-amp stage with a gain of 140, biased to mid-rail.
- **ADC:** the STM32's ADC1, triggered by a timer at exactly **250,000 samples a second**. [Part 1](01-adc-and-dac.md) explains why that number.
- **Output:** the STM32's DAC drives a TS4990 bridge amplifier and a small speaker, with a volume knob.
- **Display:** an ILI9341 2.4" TFT showing a scrolling spectrum.
- **Memory:** an 8 MB APS6404L PSRAM on its own SPI bus, which records continuously, and a microSD slot for keeping recordings.
- **Power and USB:** a 3.7 V LiPo with a USB-C charger. USB also carries a debug link to a PC.

### V1 came first

V2 isn't my first board. V1 worked, and I finished its firmware in August 2026. It sampled at 192 kHz. Alongside the digital mixer in the firmware, it had an analogue mixer on the board, fed by a square-wave LO from one of the STM32's timers. And it had a touch screen.

For V2 I dropped the analogue mixer, added the PSRAM, the SD card and an amplifier enable line, and moved most of the pins. I ported the firmware at the end of October 2026, and most of this series is the story of that port.

> **TODO (you):** what V1 looked like and what you learned from it; why the STM32G431; a photo of each board.

## How the firmware fits together

Nearly everything runs from one heartbeat: a **block** of 256 samples, which arrives every **1.024 ms**.

1. A timer (TIM2) ticks 250,000 times a second. Each tick starts an ADC conversion and updates the DAC.
2. DMA ([part 2](02-dma.md)) moves each ADC result into a buffer with two halves, without the CPU's help. When a half fills, the CPU gets an interrupt.
3. In that interrupt, the DSP ([part 3](03-dsp.md)) processes the block: filtering, heterodyne mixing and volume. It writes the result into the half of the DAC's buffer that the DAC isn't currently playing.
4. The same block is also pushed into the PSRAM ring buffer ([part 7](07-psram-ring-buffer.md)) and fed to the bat-call detector ([part 9](09-bat-detector.md)).
5. The main loop does everything that isn't time-critical: the FFT and the waterfall display ([part 4](04-fft-waterfall.md)), reading the knobs, the USB debug link, and soon the SD card.

Three numbers come up again and again, because almost every design decision is about one of them:

| Constraint | Value | Why it matters |
|---|---|---|
| Time per block | **1.024 ms** (174,080 CPU cycles) | All the audio processing for a block must finish before the next one arrives, or the sound breaks up |
| RAM | **32 KB** | Holds 64 ms of raw audio. Every buffer is a negotiation |
| PSRAM chip-select | **8 µs** maximum | The PSRAM can't refresh while it's selected, so every transfer is cut into bursts |

## How I worked

A few habits ran through the whole project. They come up in almost every post.

- **One unknown at a time.** Bring-up went clocks and LED, then display, audio, knobs, PSRAM and SD, each step adding one new thing. When something broke, there was only one suspect.
- **Measure, don't hope.** The CPU's cycle counter times every block, every FFT frame and every PSRAM burst. Estimates were often wrong, sometimes by a factor of three, and the measurements are what found the real problems.
- **Check against an independent reference.** The DSP's output was cross-checked against numpy on raw samples captured over USB. The PSRAM was checked by reading recordings back and comparing them bit for bit.
- **Write it down.** Every design decision went into an engineering diary, with the reasoning, the numbers, the alternatives and the lesson. This series is built from that diary.

The tools were STM32CubeIDE and CubeMX (the HAL where it's fast enough, registers where it isn't), KiCad for the boards, LTspice for the analogue front end, and Python with numpy for analysis.

## The series

0. **Introduction** (this post)
1. [From ultrasound to voltage and back: the ADC and DAC](01-adc-and-dac.md)
2. [DMA: letting hardware move the samples](02-dma.md)
3. [The DSP: a 1 ms deadline](03-dsp.md)
4. [Seeing sound: the FFT and the waterfall](04-fft-waterfall.md)
5. [Bringing up V2, one unknown at a time](05-v2-bring-up.md)
6. [PSRAM part 1: the 8 µs rule](06-psram-bring-up.md)
7. [PSRAM part 2: the ring buffer](07-psram-ring-buffer.md)
8. [Time expansion](08-time-expansion.md)
9. [Teaching it to hear bats](09-bat-detector.md)
10. [Lessons, and what's next](10-lessons-and-next.md)

The posts are written to be read in order, but each one stands on its own. If you only read one, make it [part 7](07-psram-ring-buffer.md). It has two mysteries, a fix that broke something else, and the most lessons per page.

---

*Next: [Part 1, the ADC and DAC](01-adc-and-dac.md)*
