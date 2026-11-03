# Core/Diag: on-target diagnostics for STM32 firmware

Small, self-contained diagnostics, collected from the bat detector's bring-up so they can be reused in other STM32 projects. Each piece is a header, or a header and a C file. None of them depends on the rest of this project.

## Using it in another project

1. Copy the `Diag` folder into the project's `Core` folder, which CubeIDE already compiles.
2. Add `../Core/Diag` to the compiler's include paths: Project → Properties → C/C++ Build → Settings → MCU GCC Compiler → Include paths. Do it for each build configuration, and for any file with its own include-path list.
3. Change the one line in `diag_port.h` to your device's header (for example `stm32f4xx.h`).

The DWT cycle counter exists on Cortex-M3, M4, M7 and M33 parts, but not on M0/M0+.

## What's here

| File | What it does |
|---|---|
| `diag_port.h` | The device header, and `DIAG_INLINE`: helpers that always compile into their caller, so they're safe in interrupts that run from RAM. |
| `diag_cycles.h` | Timing with the DWT cycle counter: start it, read it, convert cycles to µs or ns, and keep a worst case, best case or running average. |
| `diag_error.h` | Failure records: a count, plus the details of the first failure packed into one word (`DIAG_PACK4`). One word shows whole in a watch window or a stats readout. |
| `diag_stats.h/.c` | A table of named counters (`DIAG_STAT_U32(var)` and so on) for a PC to read: a debugger's watch window, without the debugger. |
| `diag_link.h/.c` | A framed command link over any byte transport (send and connected functions you supply). It has a 20-byte header, a CRC-32 (`zlib.crc32` on the PC), a command queue fed from the receive interrupt, and frames sent in place. `N`, `T` and `Z` serve the stats table; you add your own commands and frames. `Tools/diaglink.py` is the PC side. |
| `diag_spi.h/.c` | Start-up checks that read the registers CubeMX actually set: an SPI's frame size and NSS pulse mode, pins slower than a given speed, and an interrupt alone at the top priority. Plus a receive-FIFO drain for drivers that mix register access with HAL calls. |

## The link in another project

1. Write two transport functions: `send` (start a transfer; return 0 while the previous one is still going) and `connected`.
2. Fill in a `DiagLinkConfig_t`: the magic, a version, the transport functions, your commands, and optionally a stats table and hooks.
3. Call `DiagLink_Init` once, `DiagLink_OnReceive` from the receive callback, and `DiagLink_Poll` from the main loop.
4. On the PC:

   ```python
   from diaglink import FrameReader, open_port, read_stat_table, read_stats

   reader = FrameReader(open_port("COM8"), magic=b"BATD", version=1)
   values, uptime_ms = read_stats(reader, read_stat_table(reader))
   ```

The bat detector's own use is `Core/Src/debug_link.c` (the captures and the USB transport) and `Core/Src/stats_table.c` (its counters).

## Patterns worth keeping

These are the lessons behind the code (bat detector diary, entries 32–43):
- **Measure every time, keep the worst.** A worst case kept on every pass (`Diag_KeepMax`) catches the rare slow event that a one-off measurement misses. Pair it with a way to restart the measurement, so a reading covers only what follows.
- **Keep the first failure, not the last.** Later failures are usually knock-on effects. Clear the record to catch the next one.
- **Compare cycles, convert rarely.** In a hot interrupt, compare raw cycle counts and convert to ns only when a new worst case appears. That saves a division on every pass.
- **Check the disassembly of a RAM-resident interrupt.** It must not call into flash. `DIAG_INLINE` forces inlining, but confirm it with `objdump`. Force-inline its own small helpers too: an unrelated change elsewhere in the file can make the compiler stop inlining them.
- **Reset statistics outside the measured window.** A reset done inside the code being timed is counted in the next worst case.
- **Check what CubeMX generated, at start-up.** Its defaults (NSS pulse mode, 4-bit frames, Low-speed pins, priority 0 for every new interrupt) each cost a debugging session. A check that reads the registers and refuses to run costs a few bytes.
