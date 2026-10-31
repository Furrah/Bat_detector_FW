# PSRAM part 1: the 8 µs rule

*Part 6 of [Building a bat detector](00-introduction.md). V2's headline new part is an 8 MB PSRAM on its own SPI bus: enough to hold the last 16.8 seconds of raw audio, all the time. Before it could do that, it had to talk. This post is about the first driver, the one timing rule that shapes everything about the chip, and a hang that came down to one setting on a pin.*

---

## Why a PSRAM

The STM32G431 has 32 KB of RAM. At 500 kB/s of raw audio, that's 64 ms. Recording needs far more buffer than that, for two reasons:

- **Pre-trigger.** By the time you press RECORD, the bat you heard is already in the past. The recorder needs the last few *seconds*.
- **SD stalls.** SD cards pause for 100–250 ms at a time while they erase internally. 250 ms of audio is 125 KB, four times the whole RAM, and it has to go somewhere while the card catches up.

So I put an **APS6404L** on V2: 64 Mbit (8 MB) of PSRAM with a serial SPI interface, alone on SPI1. At 500 kB/s, 8 MB holds **16.8 seconds**.

## What PSRAM is, and the rule that comes with it

"PSRAM" means *pseudo-static* RAM. Inside, it's DRAM: every bit is a tiny capacitor that slowly leaks its charge and has to be refreshed regularly. On the outside, it behaves like simple static RAM. You send a command, an address and data, and the chip hides the refresh from you.

It hides the refresh by doing it **whenever it isn't selected**. That's the catch. While chip-select (CE#) is low, the chip can't refresh. So the datasheet limits how long CE# may stay low:

**tCEM = 8 µs**

At 42.5 MHz, SPI1's clock, 8 µs is 340 clocks, or about 42 bytes. That has to include the command and the address. So every transfer has to be cut into short **bursts**, each with its own CE#-low period:

```
write:  CE#↓  02 | A2 A1 A0 | data …          CE#↑
read:   CE#↓  0B | A2 A1 A0 | 00 | data …     CE#↑     (0B = fast read, 8 wait clocks)
```

Two more rules from the datasheet:

- **Reads above 33 MHz must use Fast Read (`0x0B`),** which adds one dummy byte of wait cycles. Plain Read (`0x03`) is limited to 33 MHz.
- **The memory is organised in 1 KB pages,** and no burst may cross a page boundary.

## A register-level driver, with interrupts off

The first driver (`psram.c`) is polled and simple, and it's built around tCEM:

**Not HAL.** At `-O0`, the build at the time, each `HAL_SPI_Transmit` or `HAL_SPI_Receive` call cost several microseconds of setup before any data moved. Most of the 8 µs would be gone before the first data byte. So the driver drives SPI1's registers directly: wait for TXE, write a byte to `DR`, wait for RXNE, read a byte back.

**Interrupts masked for each burst.** The audio interrupt runs for hundreds of microseconds. If it landed in the middle of a burst, CE# would stay low for all of that time. Masking interrupts for about 5 µs is harmless against a 1.024 ms audio block.

**8-bit access to `DR`.** On the G4's SPI, a 16-bit write to the data register packs and sends *two* bytes. With 8-bit frames, `DR` must be accessed as a byte:

```c
*(__IO uint8_t *)&spi->DR = out;
```

### How big can a burst be? Read the disassembly

The burst size depends on how fast the code moves each byte, and at `-O0` that's slower than you'd think. So instead of guessing, I read the compiled code for the byte loop.

It took about **80 CPU cycles per byte**:

- 32 cycles on the wire (8 bits at 42.5 MHz is 32 cycles of the 170 MHz CPU);
- about 45 cycles of loading and storing local variables on the stack, which is what `-O0` does with every variable ([part 3](03-dsp.md)).

At 80 cycles a byte:

| Data bytes per burst | Bytes including the read header | Estimated CE# low |
|---|---|---|
| 8 | 13 | 6.2 µs |
| 16 | 21 | ~10 µs (over tCEM) |

So the polled driver uses **8 data bytes per burst**.

### Measure the select time on every burst

Here's the uncomfortable fact about tCEM: **at room temperature, the chip doesn't seem to care.** The refresh limit is written for the hot end of the chip's temperature range. At 20 °C the cells hold their charge much longer, so a design that holds CE# low for too long can pass every data test on the bench and still lose data when it's warm.

So the driver times every burst with the CPU's cycle counter (DWT `CYCCNT`), from CE# low to CE# high. The boot screen shows the worst case, **in red if it's over 8 µs**.

| Build | CE# max (measured) |
|---|---|
| `-O0` | 5.0 µs |
| `-O0`, with timeouts (below) | 5.8 µs |
| `-Og` | 4.1 µs |

The 6.2 µs estimate from the disassembly was a little pessimistic, and every build is inside tCEM.

## The hang

To separate the kinds of fault I might hit, I split the boot sequence by speed:

1. **Reset and read the ID at 21 MHz,** well inside every limit.
2. **Run the data tests at the full 42.5 MHz.**

If the ID is right at 21 MHz but the data fails at 42.5 MHz, the driver's logic is fine (the same code works slower), and the fault is electrical: signal integrity, timing, the board. One boot separates the two.

The first boot showed **ID 0D 5D**: the manufacturer ID and the "known good die" code, both correct. Then the board hung.

Suspending it in the debugger showed where: stuck in the loop waiting for **RXNE**, the receive flag, on the very first byte at 42.5 MHz. The byte had gone out, but the SPI never signalled that one had come back in.

The reason is a detail of how the STM32's SPI master works. **Its receiver is clocked by SCK as the signal comes back in through the pin**, not by the internal clock directly. CubeMX generates every SPI pin at `GPIO_SPEED_FREQ_LOW`. The speed setting controls the output driver's slew rate, and a Low-speed pad simply can't toggle at 42.5 MHz. At 21 MHz it managed. At 42.5 MHz the 8 clock edges never arrived at the receiver, so RXNE never set.

Setting the SPI1 pins to **High** speed fixed it.

### What changed because of it

**Every wait loop got a timeout.** A bus fault shouldn't freeze the boot. Each burst gets a 10 µs deadline (1,700 cycles), set once when CE# goes low. The deadline is only checked while actually waiting, so the normal path costs almost nothing extra inside CE#. On a timeout, the driver returns `PSRAM_ERR_TIMEOUT`, and the boot screen shows `SPI timeout SR=…` with the SPI status register, so the flag that never arrived can be decoded from the screen. That's where the 5.0 → 5.8 µs in the table above came from.

**Never open the SPI data register in the debugger's register view.** The debugger reads `DR` to display it, and reading `DR` pops a byte off the receive FIFO, which can clear RXNE under the running code. Looking at the register can cause exactly the hang you're investigating.

## Testing 8 MB properly

A memory test that writes a pattern and reads it back sounds thorough, but it can miss a whole class of faults.

Suppose address line A12 is shorted to A13. Then addresses that differ only in those bits are the same physical cells. Write a pattern across the whole chip and the later writes overwrite the earlier ones. Whether the readback catches it depends entirely on the pattern. Two aliased addresses that happen to receive the same value read back perfectly.

So there are two tests:

**Every boot: a walking-ones address test** (after Michael Barr's classic `memTestAddressBus`). It only writes to address 0 and the 23 power-of-two addresses (1, 2, 4, … 4M), so it takes about 4 ms.

1. Write a marker to every power-of-two address.
2. Write a different value to address 0. Check that no marker changed: if one did, an address bit is **stuck high**.
3. Then, for each power-of-two address in turn, write the different value there, and check that no *other* address changed: if one did, an address bit is **stuck low** or **shorted** to another.

**On demand: the full pattern test.** Hold RECORD at power-up. It writes all 8 MB with an address-derived pattern, reads it back, then does the same with the inverse, so every bit is checked holding both 0 and 1. With the polled driver it takes about 35 seconds.

The result on V2: **ID 0D 5D, address test OK.** The full 8 MB test is still to be run once.

## Why this driver wasn't enough

The polled driver is fine for boot tests, when nothing else is running. It isn't fine for continuous recording:

- **CPU.** Writing each 512-byte block from the audio interrupt would spend about 110 µs per block, 11% of the CPU, spinning on SPI flags.
- **Latency.** Every other interrupt would be locked out for several microseconds at a time, dozens of times per block.
- **It can't wait for the main loop,** which will one day be stuck inside a 250 ms SD write.

So once the boot tests have finished, the polled driver hands SPI1 over to an interrupt- and DMA-driven engine, and never touches it again. Building that engine is [part 7](07-psram-ring-buffer.md), and it's where the 8 µs rule really started to bite.

## What I learned

- **When a device has a maximum select time, the driver's timing is part of meeting the spec.** Design for it explicitly, and work out the burst size from the actual code.
- **Read the generated code and count instructions, then measure.**
- **Don't trust a test that can pass while the design is out of spec.** Measure the spec itself, on every burst.
- **Structure a bring-up so that one boot separates logic faults from electrical ones** (here: ID slow, data fast).
- **Set the GPIO speed of every fast peripheral pin.** CubeMX defaults to Low, and an SPI master's receiver depends on its own SCK pad.
- **Every wait loop needs a timeout,** and the timeout should say what it was waiting for.
- **Test address lines separately from data.** A pattern test can pass on a board with shorted address lines.

---

*Next: [Part 7, the ring buffer](07-psram-ring-buffer.md): recording everything, all the time, and the two mysteries that came with it.*

*Previous: [Part 5, bringing up V2](05-v2-bring-up.md)*
