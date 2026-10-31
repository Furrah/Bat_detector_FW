# Teaching it to hear bats

*Part 9 of [Building a bat detector](00-introduction.md). A detector that knows when a bat is calling can switch the speaker on only when there's something to hear. Building the detection was the easy part. Finding out why it fired in an empty room meant building a measurement link over USB first, and getting USB to work at all meant flipping a plug over.*

---

## Why detect bats at all

Between bats, a heterodyne detector plays hiss. On a long evening that's tiring to listen to, and it masks the first faint call of an approaching bat. The plan is **squelch**: switch the amplifier on (through `AMP_EN`, [part 1](01-adc-and-dac.md)) only while bats are around.

To be honest about the benefit: the amp draws about 4 mA of an estimated 60–100 mA for the whole system, so battery life improves by a few percent at most. The real gain is silence instead of hiss.

There's a risk, though. A detector that switches hardware has to be trusted, because a missed bat is a bat you never hear. So it's being built in two stages:

- **Stage 1 (built): monitor only.** The detector runs and reports on two LEDs and in debug counters, while the amp stays on. I can see its false alarms and misses without paying for them.
- **Stage 2 (after field testing): it drives `AMP_EN`.**

## The detector

### Energy, not the FFT

The FFT would seem the natural input: it knows the spectrum. But the FFT runs in the main loop and drops frames when it's busy ([part 4](04-fft-waterfall.md)). A *Myotis* call lasts 1–5 ms, so it can fall entirely into a dropped frame.

So the detector works on the **energy of every block**. The DSP already high-passes every sample at 10 kHz ([part 3](03-dsp.md)). Squaring and summing that output over each 1.024 ms block costs one multiply-add per sample, and it sees every block, so it sees every call.

The 10 kHz edge is deliberately low. It keeps the free-tailed bat (9–14 kHz), the greater noctule (11.7–13.9 kHz) and social calls (14–22 kHz). My bat-call reference notes say not to throw those away. Below the edge, wind and handling noise at 1 kHz are about 40 dB down.

### Pulses and rhythm

Each block's energy goes through three steps:

1. **Noise floor.** A tracker follows the energy *down* quickly (time constant 0.25 s) and *up* slowly (4 s), and is **frozen during a pulse**. A 10–30 ms call hardly moves it, while a change in the background is learnt within seconds.
2. **Pulses.** A run of blocks more than **10 dB above the floor** is a pulse. A run of 1–100 ms is **bat-like**. A run longer than 150 ms is **continuous noise**, because no bat call lasts that long, but insects and electrical interference do.
3. **Trigger.** **Two** bat-like pulses within **600 ms** mean a bat is present. The "present" state holds for **2 s** after the last pulse.

Every number came from the call data:

| Parameter | Value | Reason |
|---|---|---|
| Threshold | floor + 10 dB | a starting value, to be tuned in the field |
| Bat-like pulse | 1–100 ms | calls run from 0.8 ms (long-eared bats) to ~30 ms (greater noctule); horseshoe tones run tens of ms |
| Continuous noise | longer than 150 ms | no call lasts this long |
| Trigger | 2 pulses within 600 ms | the slowest caller, the greater noctule, calls every 364–468 ms; one click shouldn't trigger |
| Hold | 2 s after the last pulse | about 4× the longest call interval, so the sound doesn't chop when a bat turns away |
| Floor | falls with τ ≈ 0.25 s, rises with τ ≈ 4 s, frozen during pulses | a call barely moves it; quiet returns quickly |
| Warm-up | 0.5 s after reset | the filters settle and the floor is learnt |

One subtle point: the floor is frozen during a pulse, but it's **released again after 150 ms** of continuous loudness. Otherwise a steady noise that started suddenly would leave the detector stuck "inside a pulse" forever, never learning that this is the new background.

All the timing counts blocks (1,024 µs each, from `pipeline_config.h`) rather than calling `HAL_GetTick()` inside the interrupt. It costs about 0.7% of the CPU (estimated), 48 bytes of RAM and 0.6 KB of flash.

On the board, the two LEDs were repurposed (V1 used them to show the LO's band, which the screen made redundant):

- **LED0:** a bat pass is in progress.
- **LED1:** a 50 ms flash for every bat-like pulse.

**Known limits:** pulsed ultrasonic insects and jingling keys can trigger it; bats quieter than +10 dB are missed; and the first call of a pass never counts as a trigger, only the second.

### The first surprise

In a quiet room, with no bats anywhere near, LED1 flickered. The pulse counter climbed, and the waterfall showed a patch of energy around 60 kHz. I could have nudged the threshold up until the flicker stopped. But that would have been tuning by eye, against a room I knew nothing about, and it would cost sensitivity outdoors where it matters.

I needed data: the raw samples, and the detector's view of them, on a PC where I could analyse them properly. The obvious route was USB.

## Getting USB to work

V2 has no UART pins broken out, but USB CDC (a virtual COM port) was already in the CubeMX project. Plugged in, it never appeared. Windows logged **"Unknown USB Device (Device Descriptor Request Failed)"** every time.

### The clock that wasn't accurate enough

Full-speed USB needs its 48 MHz clock within **±0.25%**. On this board that clock can't come from the PLL: the core runs at 170 MHz from a 340 MHz VCO, and the PLL's Q divider (2, 4, 6 or 8) gives 170, 85, 56.7 or 42.5 MHz, never 48. So USB runs from the internal **HSI48** RC oscillator, whose factory trim is good to only about ±3% over temperature on sibling parts. That's 12× too loose, on a device meant for outdoor use at night.

The standard fix is the **CRS** (clock recovery system). It measures HSI48 against the 1 ms start-of-frame packets the PC sends, and trims it continuously. I enabled it in CubeMX (CRS SYNC = USB_FS).

CubeMX then wrote `HSI48CalibrationValue = 32`, in a field I couldn't edit. 32 is the middle of the 6-bit trim on older parts such as the L4. The G4's trim is **7 bits**, and its own HAL defines the middle as **64** (`RCC_CRS_HSI48CALIBRATION_DEFAULT`). Starting at 32 would put HSI48 about 4–5% slow, possibly too far off to decode the very packets it needs in order to correct itself. A few lines in `main.c`'s `USER CODE SysInit` block put it back to 64.

### It still failed

With the trim fixed, Windows still reported the same error. So I stopped changing things and read the USB peripheral's registers in the failing state:

| Register | Value | Meaning |
|---|---|---|
| `USB->FNR` | 0xC000 | both data lines read high; no frame ever decoded |
| `USB->DADDR` | 0 | the PC's bus reset was never seen |
| Last setup request | all zero | the board never received a request |
| `CRS->CR` | 0x4060 | TRIM still 64: no frames to trim against |

Both lines high is wrong. On an idle full-speed bus, D+ is pulled high and **D− is low**. A D− that reads high looks *floating*, as if it isn't connected. Without D−, the board can't see the PC's reset, so it never gets as far as a descriptor request. That's not a clock problem at all: it's wiring.

USB-C sockets carry D− twice, once for each orientation of the plug. So I **flipped the plug**. The board enumerated at once as COM8.

The suspect is a dry joint on one of the socket's two D− pins (J8 A7 or B7). It needs reflowing. Until then, there's a "right way up" for the cable.

Was the CRS change needed? Not proven: the socket alone explains the failure. It stays, because the temperature range demands it.

**Lesson:** when USB fails, read the line state (`FNR`) and whether a reset was seen (`DADDR`) before touching the clocks. They separate wiring from timing in one look. And test a USB-C socket in both orientations at bring-up.

## A measurement link over USB

With USB working, I built a command-driven capture link (`debug_link.c` on the board; `capture.py` and `analyse_capture.py` on the PC).

**Why not just stream the raw audio?** 250,000 samples × 2 bytes is 500 kB/s, and buffering it would need RAM the board doesn't have. Instead the PC sends single-byte commands:

| Command | Reply |
|---|---|
| `I` | info: build, uptime, sample rate, block size |
| `R` | a raw snapshot: 4,096 consecutive samples (16.4 ms, 61 Hz FFT bins) |
| `E` / `S` | start / stop an energy stream: a 20-byte record per 1.024 ms block (19.5 kB/s), with the detector's input energy, the full-band energy, the floor and the counters |
| `M` | the last snapshot, read back from the PSRAM ring ([part 7](07-psram-ring-buffer.md)) |

The details that make it robust:

- **One shared 8 KB buffer** holds either the snapshot or a 409-record energy FIFO. The PC can fall 0.42 s behind before records are dropped, and drops are counted.
- **Framing:** a header, the payload, and a CRC-32 (zlib-compatible, so Python checks it in one call), with a sequence number to spot lost frames.
- **Three USB transfers per frame** (header, payload, CRC), so the buffer is only reused once its payload has actually gone.
- **Commands are only acted on between frames,** so a command can never change data that's being sent.
- **Cost:** 8.3 KB of RAM and 1.5 KB of flash. Setting `DEBUG_CAPTURE 0` removes all of it.

The alternatives didn't fit. SWO trace needs PB3, which is the PSRAM's SPI clock. There are no UART pins. Debugger memory dumps are slow and manual.

### Making the analysis check itself

An analysis script that's wrong is worse than none, because you believe it. So before trusting `analyse_capture.py`, I made it prove itself:

- **The spectrum is calibrated in dBFS** against a full-scale sine.
- **A Python copy of the firmware's 10 kHz high-pass** predicts, from the raw samples, the block energy the detector should see.
- **The detector is replayed offline,** in float32 like the firmware, and checked against the board's own floor and pulse counts *before* its threshold sweep is believed.
- **Synthetic tests first:** a 60 kHz tone of amplitude 3 codes read −56.6 dBFS (expected −56.7); the predicted energy was 36.93 dB (expected 36.9); the replay reproduced the reference floor and pulse count exactly.

## The room, not the detector

The first real captures, indoors, were 20 raw snapshots and 60 seconds of block energies. They told a clear story.

**The false pulses were the top of the background.** All 57 false pulses per minute were single 1 ms blocks, 10.0–13.9 dB above the floor. The background's block energy spans 10 dB between its 1st and 99th percentiles, so these were its top 0.1%, just poking over a 10 dB threshold.

**The replayed threshold sweep:**

| Threshold | False pulses/min | False triggers/min |
|---|---|---|
| +8 dB | 378 | 1 |
| +10 dB (current) | 57 | 6 |
| +12 dB | 11 | 0 |
| +15 dB | 0 | 0 |

**Where the energy came from:**

- **76%** from 20–30 kHz: a sharp line at **24.95 kHz** on top of a hump from 20 to 27 kHz;
- **9%** from a 58–68 kHz hump, the patch on the waterfall;
- **3%** from the audible band;
- and the energy was modulated at about **74 Hz**, close to the display's frame rate.

**The cross-check held:** the firmware's block energy (median 45.3 dB) agreed with numpy's model of the same high-pass applied to the raw samples (45.0 dB). The board and the analysis agree on what the detector sees.

Then I took the detector outside. By eye on the waterfall, the 60 kHz hump and most of the other noise **disappeared**. They were sources in the room. Indoors there are plenty of candidates (switch-mode power supplies, LED drivers, chargers), and I haven't tracked down which ones these were.

**The decision:** leave the detector's parameters as they are, and tune them in the field, where it matters. Raising the threshold to silence a room would cost sensitivity to real bats outdoors.

**The open question:** is the 24.95 kHz line on the board or in the room? The display's nominal line rate (~25.6 kHz) fits suspiciously well, and so does the 74 Hz modulation. An energy capture with the display unplugged, or outdoors, would settle it.

## Stage 2, later

When the detector has been tuned in the field, it will drive `AMP_EN`, with:

- a ~10 ms digital fade around wake-up and standby, so the amp doesn't click;
- the amp off whenever the volume is at minimum;
- the amp forced on during time-expansion playback.

Which control forces the sound on permanently ("squelch off") is still undecided.

## What I learned

- **Stage new control logic as "monitor only" before it drives hardware.** You learn its false-alarm and miss rates without paying for them.
- **Build the measurement path before tuning,** and make the analysis prove itself against the device before trusting its conclusions.
- **Find out where noise comes from before changing thresholds.** Indoors is a poor proxy for the field.
- **When USB fails, read the line state before blaming the clock.** A floating D− reads high.
- **Test a USB-C socket in both orientations.** One dry joint makes it work only one way round.
- **Check generated values against the HAL's own defaults for the family.** CubeMX's HSI48 trim was right for a different chip.

---

*Next: [Part 10, lessons and what's next](10-lessons-and-next.md).*

*Previous: [Part 8, time expansion](08-time-expansion.md)*
