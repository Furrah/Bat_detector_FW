# Bat detector firmware (STM32G431, V2 board)

Firmware for a handheld ultrasonic bat detector covering 10–120 kHz. 

## What it does

- **Heterodyne listening.** The microphone is sampled at 250 kHz and mixed digitally with a local oscillator (LO) anywhere from 0 to 120 kHz.
  - **Manual tuning:** the LO knob sets the frequency.
  - **Auto-tune:** the LO follows the bats being heard.
  - Hold PLAYBACK for 0.8 s to switch between them.
- **Live spectrogram.** A waterfall from 0 to 125 kHz on the 2.4" display.
- **Rolling buffer.** The last 16.8 s of raw audio are always kept in an 8 MB PSRAM.
- **Time-expansion playback.** PLAYBACK replays the last 2 s ten times slower, so calls become audible.
- **Recording to SD.** RECORD saves a WAV file that starts 5 s before the press.
  - Files are `BATnnnnn.WAV`: mono, 16-bit, 250 kHz. They open in bat-analysis software.
  - A second press stops it; recordings also stop at 10 minutes.
- **Bat-pass detector.** LED0 lights during a bat pass, and LED1 flashes for each call in it.
- **USB debug link.** It feeds the PC tools in `Tools/`.

## Controls

| Control | Action |
|---|---|
| RECORD | Start or stop a recording. Hold it at power-up for the full 8 MB PSRAM test (~35 s). |
| PLAYBACK, short press | Start or stop time-expansion playback (acts on release). Hold it at power-up for the SD speed test. |
| PLAYBACK, hold 0.8 s | Switch between manual and auto tuning. |
| LO knob | The LO frequency in manual mode (ignored in auto). |
| Volume knob | Volume. |
| S1 | Power. |

## SD card

Use an SDHC card of 32 GB or less, formatted FAT32 with the SD Association's SD Card Formatter. exFAT, which larger cards ship with, isn't supported. Use a card that fits the slot natively; a microSD card in an adapter was unreliable.

## Hardware

- **MCU:** STM32G431CBT6.
- **Front end:** SPVA1A0LR5H-1 MEMS microphone into one OPA365 stage at a gain of 140.
- **Memory and display:** APS6404L 8 MB PSRAM on SPI1; ILI9341 display and SD slot sharing SPI2.
- **Audio out:** TS4990 amplifier.

The schematic is `bat_detector_circuit.pdf`.

## Building

1. In STM32CubeIDE 2.1.1, choose File → Import → Existing Projects into Workspace, and select this folder.
2. Build the Debug configuration (`-Og`; don't use `-O0`, which is too slow for the 250 kHz audio interrupt).
3. Flash with an ST-Link.

## PC tools

Install the Python dependencies with `python -m pip install pyserial numpy matplotlib`, then run the tools as `python -I Tools/<tool>.py`.

| Tool | What it does |
|---|---|
| `capture.py` | Reads the board over USB: raw snapshots, detector energies, the stats table, and the FFT and PSRAM checks. |
| `analyse_capture.py` | Analyses captures: spectra, noise, and a replay of the detector. |
| `check_raw.py` | Checks recordings on the PC. |
| `classify.py`, `bat_signals.py` | A model of a bat species classifier (deferred on the device) and a generator of synthetic bat calls. |
| `gen_dsp_tables.py` | Generates the DSP lookup tables in `Core/DSP/Tables/`. |

## Documentation

- `Docs/engineering_diary.md`: every design decision, with the measurements and the lessons.
- `CLAUDE.md`: developer notes, covering the firmware's structure, its limits and the hard-won gotchas.
- `Docs/timer_waveform_output.md`, and the bat call and species classifier notes in `Docs/`.

## Known limitations (v2.0.0)

- **Detector false triggers.** Loud broadband ultrasound, such as rubbing fingers or clothing, or typing nearby, triggers it. Auto-tune follows those sounds too. A test of the sound's shape is planned.
- **Auto-tune accuracy** has been checked on synthetic calls and a 40 kHz transducer, not yet on real bats.
- **File dates.** No 32.768 kHz crystal is fitted, so the clock doesn't keep time and files are dated 1980.
- **Not tested:** a full 10-minute recording, a power cut during a recording (it should lose at most the last 2 s), and recording straight after a playback.
