# The ring buffer: recording everything, all the time

*Part 7 of [Building a bat detector](00-introduction.md). [Part 6](06-psram-bring-up.md) got the PSRAM chip talking. This part turns it into a recorder that never stops, and it's the part where the hardware kept telling me I was wrong.*

---

## You always press record too late

A bat pass lasts a few seconds. By the time you've heard it on the heterodyne, decided it's interesting and found the RECORD button, the best calls have gone.

Commercial detectors get round this with a *pre-trigger buffer*. They record all the time into a loop that overwrites itself, and the record button saves what's already in the loop followed by whatever comes next. The same buffer makes time-expansion playback ([part 8](08-time-expansion.md)) almost free, because the last few seconds are always sitting there waiting to be slowed down.

The question is where to keep the loop. The raw audio is 250,000 samples a second, each a 12-bit ADC code stored in 16 bits: **500 kB/s**. The STM32G431 has 32 KB of RAM in total, which holds 64 ms of it. The SD card can't be the loop either. Cards pause for 100–250 ms at a time while they erase internally, and 250 ms at 500 kB/s is 125 KB, four times the size of the whole RAM.

So the V2 board has an **8 MB APS6404L PSRAM** on SPI1, alone on its bus. 8 MB is 16,384 blocks of 512 bytes. One block arrives every 1.024 ms, so the PSRAM holds **16.8 seconds** of history.

The plan was simple to say: write every audio block into the PSRAM the moment it arrives, wrap round at the end, and never stop. Saving to SD then becomes a matter of reading the ring back at whatever pace the card can manage.

## The rule that shapes everything: 8 µs

[Part 6](06-psram-bring-up.md) covers this in detail. Here's the short version.

PSRAM is "pseudo-static": it's DRAM inside, wearing an SRAM-style interface. Every bit is a tiny capacitor that leaks, so the chip refreshes the cells itself. It can only do that while it's *deselected*, so the datasheet limits how long chip-select (CE#) may stay low: **tCEM = 8 µs**.

At 42.5 MHz, 8 µs is 340 SPI clocks, or about 42 bytes, and that has to include the command and the address. So every transfer is chopped into short *bursts*. Each burst is one CE#-low period: a command byte, a 24-bit address, then the data.

Part 6's driver handled this the simple way. It polled the SPI registers with interrupts masked for each burst, and moved 8 data bytes per burst. That's fine for boot-time memory tests, but not for continuous recording:

- **CPU cost.** Writing a block that way from the audio interrupt would spin for about 110 µs per block, 11% of the CPU, doing nothing but waiting on SPI flags.
- **Interrupt latency.** Everything else would be held off for up to 7 µs at a time, dozens of times a block.
- **It can't live in the main loop.** The main loop will eventually spend 250 ms stuck inside an SD write, and the audio doesn't stop arriving while it does.

The PSRAM writes needed to happen *beside* the CPU, not *on* it.

## The design: DMA moves the bytes, an interrupt keeps time

[Part 2](02-dma.md) explains DMA in general. Here it does this:

```
write burst:  02 | A2 A1 A0 | 32 data bytes              = 36 bytes, 6.78 µs at 42.5 MHz
read burst:   0B | A2 A1 A0 | 00 (wait) | 32 data bytes  = 37 bytes, 6.96 µs
```

(It ended up as 16 data bytes. How that happened is most of this post.)

Two DMA channels on DMA2 run each burst:

- **TX** copies the bytes from a small staging buffer into the SPI data register, one at a time as the SPI asks for them.
- **RX** drains every byte the SPI receives.

SPI is full duplex: every byte clocked out clocks one in. During a write, the bytes coming back are junk, but they still have to be drained.

The RX channel is the one that signals the end of a burst, and that's deliberate. The TX channel finishes when it has loaded the last byte into the SPI's transmit FIFO, which is *before* that byte has left the chip. The RX channel finishes only when the last byte has been completely clocked in. So RX transfer-complete is the true end of the burst, and its interrupt does the time-critical work:

1. Raise CE# (first thing, before anything else).
2. Record how long CE# was low.
3. Work out what comes next.
4. Start the next burst.

```
CE#    ‾‾‾\_________________________________/‾‾‾‾‾‾‾‾‾\_______________ ...
SPI        | cmd | addr |    data bytes     |          | cmd | addr | ...
DMA        TX + RX enabled ---------------> RX complete
CPU                                         [interrupt: CE# high, start next]
```

Each 512-byte audio block becomes a chain of bursts, each one started by the end of the last. The CPU only shows up at the joins.

**There's no copy of the audio block.** The ADC fills a circular buffer in two halves (part 2). When one half is full, the DSP interrupt is handed that half, and the ADC moves on to fill the other. The half the DSP has just been given won't be overwritten for another 1.024 ms, so the ring writes straight from it. Each burst copies 32 bytes into the staging buffer behind the command and address. The whole block takes about 130 µs to write, well inside the 1.024 ms.

**Reads queue behind writes.** Read requests (from the debug link, and later from playback and the SD writer) go in a four-deep queue and run in the gaps between writes. Writes always go first. A missed write is lost audio; a late read is only late.

**Faults recover by themselves:**

- **A burst that never finishes.** If a burst is still in flight when the next block arrives (at least 1 ms, against about 7 µs expected), it's stuck. The ring resets SPI1 through the RCC, fails any queued reads so nothing waits forever, and counts a fault.
- **A late block.** If the previous block hasn't finished writing when the next one arrives, it's counted in `psramRingLateBlocks`.

## The priority rule

There's a catch in all this. The end of each burst is decided in *software*: CE# goes high when the interrupt runs. If the interrupt is late, CE# stays low too long.

On the Cortex-M4, the NVIC lets a more urgent interrupt preempt a less urgent one (a lower priority number is more urgent). The DSP runs inside the audio DMA interrupt, for about 370 µs per block. Suppose the burst interrupt had the same priority as the DSP. Then a burst that finished just as the DSP started would have to wait for it, holding CE# low for 370 µs: about **50 times** tCEM.

So the rule is that the burst interrupt (DMA2 Channel 1) is the **only** interrupt at priority 0, and everything else is at 1 or lower:

| Interrupt | Job | Priority |
|---|---|---|
| DMA2 Channel 1 | PSRAM burst complete | **0** |
| DMA1 Ch1 / Ch2, TIM6_DAC | audio ADC (the DSP runs here) and DAC | 1 |
| USB | debug link | 1 |
| DMA2 Channel 2 | PSRAM TX (never fires) | 1 |
| DMA1 Channel 3 | knobs and battery (ADC2) | 3 |

A rule like this fails silently. One CubeMX regeneration with a different default and the board still runs and the data still checks out, until a hot evening in the field. So the code checks it. At boot, `PsramRing_Start()` walks every enabled interrupt in the NVIC. If anything else is at priority 0, the ring refuses to start and the boot screen says **"Ring: NVIC prio"**, while the rest of the detector runs normally.

Keep one thing in mind for later: there is one way round this rule. `__disable_irq()` masks every interrupt, priority 0 included.

## Measure what the test can't see

Part 6 had already taught me something uncomfortable. **At room temperature, the PSRAM doesn't care about tCEM.** The refresh limit is written for the hot end of the chip's temperature range, and at 20 °C the cells hold their charge far longer than that. A data test on the bench passes whether the timing is in spec or not.

So the ring measures the spec itself, on every burst. It reads the CPU's cycle counter (DWT `CYCCNT`) when CE# goes low and again when it goes high, and keeps:

- `psramRingMaxSelectNs`: the longest CE# low time seen (must stay below 8000);
- `psramRingOverTcem`: how many bursts went over.

The prediction for 32-byte bursts: 6.78 µs on the wire for a write, about 7.2 µs once the interrupt has raised CE#. Under 8 µs, with not much to spare. I estimated the CPU cost at about 2%.

And for the data, an end-to-end check. The USB debug link ([part 9](09-bat-detector.md)) can send a 4096-sample raw snapshot of the ADC. A new command, `M`, makes the board read those same 16 blocks back *out of the PSRAM ring* and send them too. `capture.py verify` compares the two copies bit for bit. That covers the whole chain: ADC, DMA, ring write, PSRAM, ring read, USB.

## First light: perfect data, wrong timing

```
python -I Tools/capture.py verify --count 50
...
PASS: 50 snapshots, 204800 samples (400 KB) identical
```

400 KB read back identical, including across the point where the ring wraps round. Four snapshots read back after waiting 15.6 s, just short of the ring's 16.8 s, also matched. No late blocks, no faults.

Then I looked at `psramRingMaxSelectNs`: just over 9,100 ns. **Every burst held CE# low for about 9.1 µs**, over the 8 µs limit. And the data test had passed anyway, exactly as part 6 warned.

There was a second surprise. The DSP's worst-case load (`dspLoadMaxPercent`, [part 3](03-dsp.md)) had gone from 37% to 43–45%, not up by the 2% I'd estimated. I parked that one; it came back later.

## Mystery 1: where did 2 µs go?

The obvious fixes were all there: smaller bursts, a faster interrupt handler, running the handler from RAM. Each one was a guess about *which part* of the 9.1 µs was too long. So before changing anything I split the time.

A diagnostic build cut each burst's CE#-low time in two:

- **entry:** CE# low until the interrupt starts. That's the transfer plus the interrupt latency.
- **inside:** the interrupt starting until CE# goes high.

It also recorded the shortest burst and counted every burst over tCEM.

| Measurement | Value |
|---|---|
| Shortest burst | 9.07 µs |
| Longest burst | 9.18 µs |
| Bursts over tCEM | 8,563,216 out of 8,563,216 |
| CE# low → interrupt start | 8.94 µs |
| Interrupt start → CE# high | ~0.24 µs |

Each line rules something out:

- **It isn't interrupt latency.** Only 112 ns separate the shortest burst from the longest. If something were occasionally delaying the interrupt, the bursts would mostly be fine with a few long ones. Instead, every burst was long by the same amount.
- **It isn't the handler.** Only 0.24 µs is spent inside it before CE# goes high.
- **So the transfer itself was about 2 µs longer than it should be.** 36 bytes at 42.5 MHz is 6.78 µs on the wire, but the bytes were taking nearly 9 µs to go out.

That leaves two explanations: the SPI clock was slower than I thought, or there were gaps between the bytes.

The clock was easy to check: 16 MHz crystal ÷ 4 × 85 ÷ 2 = 170 MHz, APB2 ÷ 1, SPI ÷ 4 = 42.5 MHz. Correct.

So there were gaps. I went through `MX_SPI1_Init()` field by field, including the ones I'd never set:

```c
hspi1.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
```

### What NSS pulse mode is

In ordinary SPI, the master pulls chip-select low, clocks out as many bytes as it likes back to back, and raises chip-select at the end. That's how the PSRAM, the display and the SD card all work.

Some devices need chip-select to go high between *every* word, because they latch each word on CS's rising edge. Many SPI ADCs and DACs are like this. For those, the STM32's SPI has **NSS pulse mode**. The SPI hardware raises its own NSS pin for at least one clock between consecutive frames, so firmware doesn't have to toggle a pin for every word.

To fit that pulse in, the SPI **stops the clock between frames**. The pause comes from the SPI's own sequencer, so it happens *even when nothing uses the NSS pin*. This board drives every chip-select from a GPIO ("software NSS"), so the pulses went nowhere, but every byte still paid for them.

On the G4 it's bit 3 of `SPIx_CR2`. CubeMX lists it as "NSSP Mode", and its default for this family is **Enabled**.

### The arithmetic that confirmed it

With NSSP off, bursts measured **7.41–7.52 µs**, and none of the next 8.5 million went over tCEM.

The difference shows exactly what the setting cost. (9070 − 7405) ns spread over the 35 gaps between 36 bytes is **47.6 ns per byte**, which is 2.0 periods of the 42.5 MHz clock. So every byte had been taking 10 clocks instead of 8. NSSP had been costing **20% of the SPI's throughput**, on SPI1 and on SPI2, the display and SD bus, as well.

The burst now broke down like this:

| Part | Time |
|---|---|
| 36 bytes on the wire | 6.78 µs |
| DMA start, last byte drained, interrupt entry | ~0.5 µs |
| Inside the interrupt, up to CE# high | ~0.24 µs |
| **Total** | **~7.5 µs** |
| Margin to tCEM | ~0.48 µs |

I turned NSSP off on both SPIs. `PsramRing_Start()` now also refuses to run if SPI1's NSSP bit is ever set again (boot screen: **"Ring: SPI1 config"**).

An oscilloscope on SCK would have shown the gaps immediately. Splitting the time with the cycle counter found them without one, and gave me numbers to check the fix against.

## Mystery 2: the reads that ran long

0.48 µs of margin is fine for writes. A read is one byte longer, though, because the fast-read command (`0x0B`) needs a dummy byte before the data. That puts a read at about 7.7 µs.

So I measured it under `verify`, which reads the ring back as fast as it can. The longest burst reached **8.12 µs**, and 3 of about 12,800 read bursts went over tCEM.

This time it wasn't the transfer: the interrupt was late. The culprit was the loophole I mentioned earlier. `__disable_irq()` sets the Cortex-M's PRIMASK, which blocks every configurable interrupt, *priority 0 included*. The priority rule protects the burst interrupt from being preempted. It doesn't protect it from code that switches interrupts off altogether.

Two short masked sections were running during `verify`:

- **`PsramRing_ReadBlock()`**, which queues a read. It masks interrupts briefly while it touches the queue, and the debug link called it on every main-loop pass while the queue was full. So it ran constantly.
- **`PsramRing_PushBlock()`**, called once per block from the audio interrupt. In normal running it never overlaps a burst, but during back-to-back reads it can land in the middle of one.

Together they added about 0.45 µs, roughly 75 CPU cycles. Avoiding `verify` wasn't an option: the SD writer and time-expansion playback will read the PSRAM all the time. There were three ways to fix it:

1. **Keep 32-byte bursts and never mask priority 0.** Replace `__disable_irq()` with BASEPRI, which masks everything *except* priority 0, and make the read queue lock-free. That fixes today's cause. But it leaves about 0.3 µs of margin, and every critical section written anywhere in the firmware from then on would have to respect it. That's a rule I'd break myself in six months.
2. **End CE# in hardware.** Drive CE# from a timer output on PB0, so the PSRAM is deselected on time however late the interrupt is. Robust, but a big change.
3. **Halve the bursts to 16 bytes.** A write drops to 3.76 µs on the wire and a read to 3.95 µs, leaving about 3 µs of slack: roughly 10× the delay I'd just measured.

I chose option 3. It was a one-line change.

## The fix that broke the DSP

The first 16-byte build ran `verify`, and `dspLoadMaxPercent` read **110%**. An audio block had overrun its 1.024 ms deadline. That's the one thing a real-time audio system must never do.

Halving the burst size doubles the number of interrupts per block, and I hadn't measured what one interrupt cost. The number:

- The burst interrupt took **4.04 µs**.
- HAL's generic DMA handler then ran after it (CubeMX's generated `DMA2_Channel1_IRQHandler` calls ours, then HAL's), plus interrupt entry and exit.

That came to about **4.7 µs of CPU for a burst that lasts 4.5 µs**. With reads running back to back, the burst interrupt took over half the CPU. The DSP's ~380 µs of work per block, being preempted at every burst, stretched past the deadline.

This also explained the second surprise from first light. With 32-byte bursts the same cost was there, only milder: the 43–45% load during normal recording was the burst interrupt, not the 2% I'd estimated. Only back-to-back reads made it visible as an overrun.

### Why does a tiny handler take 4 µs?

4 µs at 170 MHz is about 680 cycles. That's a lot for a function that raises a pin, updates a few counters and restarts two DMA channels.

**Suspect 1: `memcpy`.** Each write burst copies 16 bytes into the staging buffer. I assumed newlib-nano's `memcpy` copied a byte at a time with a function call around it. The disassembly showed GCC had inlined it as a few word loads and stores. **Not guilty.** I'd have spent an afternoon optimising a function that wasn't even being called.

**Suspect 2: where the code runs from.** At 170 MHz the G4's flash needs 4 wait states: 5 CPU cycles per access. The ART accelerator in front of the flash hides that with a 1 KB instruction cache (its prefetch is off in this project's CubeMX settings). The interrupt path was about 1 KB of `-Og` code:

| Function | Size |
|---|---|
| Interrupt handler | 340 B |
| Start next burst | 284 B |
| Record timing | 204 B |
| Stop DMA | 80 B |

Between bursts, the DSP's loops fill the 1 KB cache with their own code. So every burst interrupt found its code evicted and ran it **cold from flash**, paying the wait states on nearly every fetch.

### One change at a time

I measured each step with `psramRingMaxIsrNs` (the CPU time per burst) and `dspLoadMaxPercent`:

| Build | Interrupt (max) | DSP load, ring writing | DSP load, during verify | CE# max |
|---|---|---|---|---|
| Handler in flash, `-Og` | 4.04 µs | not measured | **110%** | 5.04 µs |
| In RAM, `-Og`, HAL handler skipped | 3.05 µs | 46% | 70% | 5.11 µs |
| In RAM, `-O2` | **2.32–2.48 µs** | **45%** | **66%** | **4.44 µs** |

**Step 1: run it from RAM, and skip HAL's handler.** The interrupt path is marked `__RAM_FUNC`, which places it in a section the startup code copies from flash into SRAM at boot. Our handler already clears the DMA flags, so HAL's handler would find nothing to do. A `return` in the USER CODE block skips it.

That only saved 25%, and the reason is a detail of the Cortex-M4 I hadn't appreciated. The core fetches code from ordinary SRAM (at `0x2000_0000`) over its *system bus*, the same bus its data loads and stores use. When code runs from SRAM, instruction fetches and data accesses take turns, at roughly 2 cycles per instruction. `-Og` made this worse. It doesn't inline small helper functions, so every call pushed and popped registers on the stack, in that same SRAM, over that same bus.

**Step 2: compile that one file at `-O2`.** A `#pragma GCC optimize ("O2")` at the top of `psram_ring.c` does it, and the rest of the Debug build stays at `-Og`. `-O2` inlined the helpers into the handler and kept values in registers, so there was less code to fetch and fewer stack accesses fighting it for the bus. Down to **2.3 µs**.

That cost:

- **RAM:** 1 KB for the interrupt code. Free RAM went from 3.8 KB to 2.9 KB.
- **A linker warning:** "LOAD segment with RWX permissions", because RAM that holds code is writable and executable. Expected and harmless here.
- **Debugging:** `psram_ring.c` is now harder to single-step, because `-O2` optimises variables away.

## Where it ended up

- **CE# is low for at most 4.44 µs**, reads included: 3.56 µs inside tCEM, with no bursts over.
- **DSP load is 45%** while the ring records (37% without it), and 66% in the worst case, with reads running back to back.
- **Data:** `verify` reads back 400 KB identical; no late blocks, no faults.
- **The ring costs about 7% of the CPU:** 32 bursts × 2.3 µs in every 1.024 ms.
- **The last 16.8 seconds are always there.**

The diagnostics stayed in. `psramRingMaxSelectNs`, `psramRingOverTcem`, `psramRingMaxIsrNs`, `psramRingMaxEntryNs` and the burst counter are all in Live Expressions, and writing 1 to `psramRingResetStats` restarts them.

There are still options on the table if the CPU cost ever matters:

- **CCM SRAM.** The G431 has 10 KB of core-coupled RAM, which the core reaches over its instruction bus, separately from data. Code runs there at full speed with no bus contention. It needs a linker-script change to carve CCM's alias (from `0x2000_5800` up) out of the normal RAM region.
- **Flash prefetch.** Turning it on would speed up all flash code, the DSP included, but it changes the timing of the whole system. It's a separate experiment.
- **Pacing reads.** Capping read bursts per block would stop a reader from flooding the interrupt. That belongs in the SD writer's design.

## What I learned

- **A test can pass while the design is out of spec.** The data was perfect at 9.1 µs. Only measuring the spec itself, on every burst, caught it.
- **When a measurement disagrees with the calculation, split it into parts before choosing a fix.** The obvious fixes for the 9.1 µs (a faster handler, smaller bursts) would have treated the part that wasn't broken.
- **Read every field the code generator writes, not only the ones you set.** NSS pulse mode is a sensible default for an SPI ADC, and a silent 20% tax on everything else.
- **Make timing preconditions checks, not assumptions.** The ring refuses to start if the interrupt priorities or the SPI configuration are wrong.
- **Measure an interrupt's cost under its heaviest load before making it run more often.** +6% in normal running became an overrun once reads ran back to back.
- **On a Cortex-M4, RAM isn't automatically fast.** Ordinary SRAM shares a bus between code and data. Core-coupled RAM is the fast place for hot code.
- **Check the disassembly before blaming a function.** The `memcpy` I suspected wasn't even called.

---

*Next: [Part 8, time expansion](08-time-expansion.md). With 16.8 seconds always in the PSRAM, playing a bat back ten times slower turned out to need no new recording path at all, only a reader.*

*Previous: [Part 6, PSRAM bring-up](06-psram-bring-up.md)*
