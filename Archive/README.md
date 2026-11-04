# Archive

Source files that are no longer built, kept for reference and reuse. Nothing in this folder is compiled, because CubeIDE builds only `Core`, `Drivers`, `FATFS` and `Middlewares`. Tag `v2.0.0` is the last version that built and ran the first four; the `v2` branch still builds the debug link, stats and `Diag` (diary 47).

| Files | What it is | Why it's here |
|---|---|---|
| `sd_bench.c/.h` | Sustained SD write test under the full recording load. It copied the PSRAM ring's live audio to `SBnnX.RAW` with 1–16 sectors per write and reported whether the writes kept up. Its PC side was `capture.py sdtest` (in `v2.0.0`). | Its job is done: it chose the recorder's 8-sector polled writes (diary 38–40). |
| `tim3_lo.c/.h` | V1's local oscillator: a TIM3 square wave for the analogue mixer. | V2's heterodyne is digital (`dsp.c`). The technique is written up in `Docs/timer_waveform_output.md`. |
| `ili9341_touch.c/.h` | Touch-controller driver for the ILI9341 display module (afiskon's library). | V2 wires no touch controller, and the driver hardcodes `hspi1`, PB4 and PA10, which are now the PSRAM bus. |
| `dsp_agc.c/.h` | An envelope-following automatic gain control. | It was never connected to the DSP chain. Gain switching or AGC is still on the list (CLAUDE.md, "Next up"). |
| `debug_link.c/.h` | The USB CDC debug capture: raw snapshots, the energy stream, PSRAM read-back, FFT capture, the stats commands. PC side: `Tools/capture.py`. | The `essential` branch has no USB and no instrumentation (diary 47). Built and running on `v2`. |
| `stats_table.c/.h` | The table of every timing and health counter, read by `capture.py stats`. | As above. |
| `Diag/` | The reusable diagnostics library: DWT cycle timing, first-failure records, SPI/NVIC start-up checks, the stats table and the framed link. Its README says how to use it in another project. | As above; kept for future projects. |
| `Tools/capture.py`, `analyse_capture.py`, `diaglink.py` | The PC side of the debug link: captures, analysis plots, the detector replay. | They need the `v2` firmware. |

To use a file again, move it back under `Core/` and build once in CubeIDE, which regenerates the makefiles.
