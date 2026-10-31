# DMA: letting hardware move the samples

*Part 2 of [Building a bat detector](00-introduction.md). [Part 1](01-adc-and-dac.md) set the pace: 250,000 samples a second in, and 250,000 out. This post is about how they move without the CPU touching them, and how the CPU still knows exactly when to do its work.*

---

## The problem with one interrupt per sample

The obvious way to read an ADC is to let it raise an interrupt when each conversion finishes, read the result in the handler, and write the processed value to the DAC. On an Arduino that's how you'd start, and at a few kilohertz it works fine.

At 250 kHz the budget is 680 CPU cycles per sample ([part 1](01-adc-and-dac.md)). Every interrupt costs a fixed overhead on top of whatever the handler does: the core stacks eight registers on entry and restores them on exit, and HAL's generic handlers add their own bookkeeping. Then the processing itself needs about 250 cycles a sample in the optimised build, and needed 687 in the unoptimised one ([part 3](03-dsp.md)). Paying interrupt overhead half a million times a second (in and out) on top of that would leave nothing for the main loop: the display, the knobs, USB.

There's a better split of the work. Moving a sample from the ADC's data register into RAM needs no intelligence at all. It's a copy, triggered by an event. That's exactly what **DMA** is for.

## What DMA is

A DMA controller (Direct Memory Access) is a second, very simple bus master that sits beside the CPU. You give a DMA *channel*:

- a **source address** and a **destination address**;
- a **count** of items to move (in the `CNDTR` register);
- the **item size** (8, 16 or 32 bits);
- whether each address **increments** after each item or stays fixed;
- which **request** to wait for.

Each time the request fires, the channel moves one item and decrements `CNDTR`. It uses the bus in the gaps between the CPU's accesses and runs no instructions, so the CPU doesn't notice.

On the STM32G4, a **DMAMUX** sits in front of the DMA controllers and routes any peripheral's request to any channel. That's why CubeMX lets you pick "ADC1 → DMA1 Channel 1" from a list rather than consulting a fixed table.

Two more features make DMA useful for streaming audio:

- **Circular mode.** When `CNDTR` reaches zero, the channel reloads it and starts again from the beginning of the buffer, forever.
- **Half-transfer and transfer-complete interrupts.** The channel can interrupt the CPU when it's half-way through the buffer, and again when it reaches the end.

Put those together and you get the pattern the whole firmware is built on.

## Ping-pong buffering

The ADC's DMA channel is set up like this:

| Setting | Value |
|---|---|
| Channel | DMA1 Channel 1, request: ADC1 |
| Source | `ADC1->DR`, fixed |
| Destination | `adcBuffer[512]`, incrementing |
| Item size | 16 bits |
| Mode | circular |
| Channel priority | high |

TIM2 triggers a conversion every 4 µs. When it finishes, the ADC raises a DMA request, and the channel copies the 16-bit result into the next slot of `adcBuffer`. After 256 samples, the half-transfer interrupt fires. After 512, transfer-complete fires, and the channel wraps back to the start.

```
time ─────────────────────────────────────────────────────────────────►
           1.024 ms            1.024 ms            1.024 ms
DMA     [ fill half A ]     [ fill half B ]     [ fill half A ]   ...
                       ▲                   ▲                   ▲
                 half-transfer      transfer-complete     half-transfer
CPU                    [ DSP on A ]        [ DSP on B ]        [ DSP on A ]
```

While the DMA fills one half, the CPU processes the other: hence "ping-pong". Each half is one **block** of 256 samples, and the CPU gets a whole block period, **1.024 ms**, to finish with it before the DMA comes back round and overwrites it.

In code, the two HAL callbacks hand the right half to the same processing function:

```c
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if(hadc == &hadc1)
    {
        PsramRing_PushBlock(&adcBuffer[0]);          // raw block into the PSRAM ring
        ADC_RunBlockCallback(halfCallback, &adcBuffer[0]);   // DSP_ProcessBlock
    }
}
```

`ADC_RunBlockCallback` wraps the call with the CPU's cycle counter (DWT `CYCCNT`) and keeps the worst case as a percentage of the block period, `dspLoadMaxPercent`. That number turned out to be the most important one in the project. At 100%, the DMA overwrites the half the DSP is still working on. Long before that, the main loop starves, and the display stops updating. [Part 3](03-dsp.md) is the story of watching it.

Compared with an interrupt per sample, the CPU now takes 2 interrupts per 256 samples, and does all its processing in tight loops over a block.

## The DAC side, and asking the hardware where it is

The output mirrors the input. DMA1 Channel 2 streams a 512-sample `dacBuffer` into the DAC's data register in circular mode. On each TIM2 tick, the DAC takes its next value and asks for another.

The question is how the processed block gets *into* `dacBuffer`, into a half the DMA isn't currently reading.

**V1's answer** used the DAC DMA's own interrupts:

1. In the ADC interrupt, the DSP copied its output block into a separate `dacQueue` and set a flag.
2. In the DAC's half-transfer or transfer-complete interrupt, the block was copied from `dacQueue` into the half of `dacBuffer` that had just finished playing.

It worked, but:

- Every block was **copied twice**.
- `dacQueue` was sized as a whole buffer (1 KB), and only half of it was ever used.
- It depended on the ADC and DAC interrupts firing in a particular order, which depended on which was started first.

**V2's answer** asks the DMA where it is. `CNDTR`, the remaining count, says exactly which half the DMA is reading. If more than one block is left, it's still in the first half, so the second half is free:

```c
void DAC_WriteBlock(const uint16_t *input, uint32_t length)
{
    uint32_t inFirstHalf = (__HAL_DMA_GET_COUNTER(hdac1.DMA_Handle1) > DAC_BLOCK_SIZE);
    uint16_t *half = inFirstHalf ? &dacBuffer[DAC_BLOCK_SIZE] : &dacBuffer[0];

    for(uint32_t i = 0; i < DAC_BLOCK_SIZE; i++)
    {
        half[i] = input[i];
    }

    /* if the DMA changed half during the copy, it reached the block being written */
    if((__HAL_DMA_GET_COUNTER(hdac1.DMA_Handle1) > DAC_BLOCK_SIZE) != inFirstHalf)
    {
        dacLateBlocks++;
    }
}
```

One copy, no queue, no DAC interrupts, and no dependence on start order. It saved 1 KB of RAM, and RAM was about to become the scarcest thing on the board.

**Is there enough margin?** The ADC and DAC are clocked by the same timer, so their positions are locked together. The DSP finishes its block about 0.4 ms after the ADC half completes (at 37% load). At that moment the DAC is about 0.6 ms away from reaching the half being written. The second `CNDTR` check is the guard: `dacLateBlocks` counts any block where the DMA crossed into the half during the copy. It should stay at zero.

### A loose end: start order

My notes from V1 say the safe start order is DAC, then ADC, then the timer, because starting the timer first can underrun the DAC on its very first trigger. The current `main.c` starts TIM2 first, and it works. With the `CNDTR` approach the order no longer matters for *where* blocks land, but I haven't checked the DAC's underrun flag after start-up. It's on the list.

## The second ADC: knobs and the battery

The audio ADC never stops, so the slow inputs get their own: **ADC2** reads the LO pot, the volume pot and the battery (through a 1 MΩ/1 MΩ divider) as a three-channel scan.

- **When:** started by software every 10 ms from the main loop. Each scan uses the result of the previous one, then starts the next.
- **How:** DMA1 Channel 3, normal (one-shot) mode, low channel priority. Its interrupt is at NVIC priority 3, below everything audio.

**Why DMA, for three numbers?** In scan mode the ranks convert back to back, about 6 µs each. If the main loop polled them and the audio interrupt landed between two polls, which it does for hundreds of microseconds at a time, the next conversion would overwrite a result that hadn't been read yet. DMA can't miss one.

**Why such long sampling (247.5 cycles)?** The battery divider has a source impedance of 500 kΩ. Through that, the ADC's sampling capacitor would take far too long to charge. A 100 nF capacitor on the pin supplies the charge instead, and the long sampling window gives everything time to settle. Three conversions every 10 ms cost nothing.

There are two guards:

- `ADC_Init` stops if the CubeMX rank count ever changes, because the code indexes the results by position.
- ADC2 is now calibrated at start-up. V1 never calibrated it.

### The battery reading, and its blind spot

The battery voltage is averaged over a second and shown as "BAT" above "3.9" in the top-right corner of the screen. (It's two lines because the LO label fills 213 of the 240 pixels, and "3.9V" in the small font needs 28.)

It has a known limit. The ADC measures relative to VDDA, which is the 3.3 V regulator output. Near empty, the MCP1700 regulator drops out and VDDA starts falling with the battery. The reading is a ratio, so it stays flat while the battery falls: **the gauge reads high exactly when the battery is low**. The fix is to measure the chip's internal reference (VREFINT) and correct for VDDA. But VREFINT is only on ADC1, which is busy with audio. Deferred.

## DMA everywhere

By the end of the V2 port, four DMA channels were running:

| Channel | Moves | Mode | Covered in |
|---|---|---|---|
| DMA1 Ch1 | ADC1 → `adcBuffer` (audio in) | circular | this post |
| DMA1 Ch2 | `dacBuffer` → DAC1 (audio out) | circular | this post |
| DMA1 Ch3 | ADC2 → knob and battery readings | one-shot | this post |
| DMA2 Ch1 + Ch2 | SPI1 ↔ PSRAM bursts | one-shot, chained by interrupt | [part 7](07-psram-ring-buffer.md) |

The last row is a different use of DMA. It isn't streaming: it moves 20 bytes in a hurry, under a hard deadline, and an interrupt chains one burst to the next. That's where most of the trouble in this project happened.

The bus bandwidth all this uses is tiny. Audio in and out is 250,000 × 2 bytes × 2 directions = 1 MB/s, and the PSRAM adds 0.5 MB/s. The AHB bus at 170 MHz can move hundreds of megabytes a second.

## What I learned

- **Let hardware move data.** The CPU's time is for decisions; copying a register into RAM isn't one.
- **Ping-pong buffering turns a per-sample deadline into a per-block one.** The DSP gets 1.024 ms for 256 samples instead of 4 µs for one.
- **Ask the hardware where it is** (here, the DMA's remaining count) rather than relying on interrupts firing in a particular order.
- **Give slow housekeeping its own ADC and DMA channel, at low priority,** so it can never disturb the real-time path.
- **When measuring a supply, ask what the ADC's reference does at the end of the range you care about.**

---

*Next: [Part 3, the DSP](03-dsp.md): what happens to a block in its 1.024 ms, and how I found out it was taking 1.03.*

*Previous: [Part 1, the ADC and DAC](01-adc-and-dac.md)*
