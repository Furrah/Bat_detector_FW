"""
analyse_capture.py - analyse captures recorded by Tools/capture.py.

    python -I Tools/analyse_capture.py Captures/<file>.npz [more files] [--show]

raw     The averaged spectrum in dBFS and its strongest lines, each with its
        possible alias (fs - f) and any harmonic relation. Noise above 10 kHz,
        DC offset and clipping. The block energy the detector should see,
        computed from the raw samples with the firmware's own 10 kHz high-pass,
        so it can be compared with batEnergyDb / batFloorDb.

energy  Energy statistics, periodicities in the energy (main-loop activity
        and mains show up as lines), and the loud events. The detector is
        replayed offline, checked against the board's own floor and pulse
        counts, then swept over thresholds to give false pulses per minute.

A PNG is written next to each capture. Needs: python -m pip install numpy matplotlib
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np


# 12-bit ADC codes about mid-scale: a full-scale (0 dBFS) sine has amplitude 2048
ADC_MIDSCALE = 2048
ADC_FULL_SCALE = 2048.0
ADC_MAX_CODE = 4095

# dsp.c: Biquad_InitHighPass(&high_pass, 10000.0f, SAMPLE_RATE)
HIGH_PASS_HZ = 10000.0

# bat_detect.c parameters: keep these in step with the firmware
DETECTOR = {
    "threshold_db": 10.0,
    "pulse_max_ms": 100,
    "noise_run_ms": 150,
    "trigger_window_ms": 600,
    "hold_ms": 2000,
    "floor_fall_ms": 250.0,
    "floor_rise_ms": 4000.0,
}

SWEEP_THRESHOLDS_DB = [6, 8, 10, 12, 15, 20]

# Periodic activity that could show up in the block energy
SUSPECTS = [
    (1.0, "battery text, every 1 s"),
    (25.0, "waterfall row, every 40 ms"),
    (50.0, "mains"),
    (100.0, "10 ms main-loop tick (ADC2 scan, LEDs), or mains x2"),
    (150.0, "mains x3"),
]

# Plot inks and series colours (light theme)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SERIES_1 = "#2a78d6"
SERIES_2 = "#eb6834"
SERIES_3 = "#1baf7a"


def to_db(energy):
    """The firmware's dB scale for block energies (BatDetect_ToDb)."""
    return 10.0 * np.log10(np.asarray(energy, dtype=np.float64) + 1.0e-6)


def running_median(values, half_width):
    padded = np.pad(values, half_width, mode="edge")
    windows = np.lib.stride_tricks.sliding_window_view(padded, 2 * half_width + 1)
    return np.median(windows, axis=1)


def chunk_median(values, chunk):
    """Median in consecutive chunks, stretched back to full length (cheap floor estimate)."""
    chunk = max(1, chunk)
    count = math.ceil(len(values) / chunk)
    padded = np.pad(values, (0, count * chunk - len(values)), mode="edge")
    return np.repeat(np.median(padded.reshape(count, chunk), axis=1), chunk)[:len(values)]


def find_peaks(freqs, level, floor, min_freq, min_above_db, max_count, exclusion):
    """Strongest local maxima, refined by parabolic interpolation. exclusion is in Hz."""
    k = np.arange(1, len(level) - 1)
    candidate = ((level[k] > level[k - 1]) & (level[k] >= level[k + 1])
                 & (level[k] - floor[k] >= min_above_db) & (freqs[k] >= min_freq))
    order = k[candidate][np.argsort(level[k[candidate]])[::-1]]

    bin_hz = freqs[1] - freqs[0]
    peaks = []

    for i in order:
        # skip window sidelobes and shoulders of a stronger line
        if any(abs(freqs[i] - p["freq"]) < exclusion for p in peaks):
            continue

        a, b, c = level[i - 1], level[i], level[i + 1]
        curve = a - 2.0 * b + c
        offset = 0.5 * (a - c) / curve if curve != 0 else 0.0

        peaks.append({
            "bin": int(i),
            "freq": freqs[i] + offset * bin_hz,
            "level": b - 0.25 * (a - c) * offset,
            "above": b - floor[i],
        })

        if len(peaks) == max_count:
            break

    return peaks


def harmonic_of(peak, peaks, tolerance, sample_rate=None):
    """'n x f0' if peak sits on a harmonic of a stronger peak (folded about Nyquist if a rate is given)."""
    for other in peaks:
        if other is peak or other["level"] <= peak["level"]:
            continue
        for n in range(2, 11):
            harmonic = n * other["freq"]
            folded = harmonic
            if sample_rate is not None:
                folded = abs(((harmonic + sample_rate / 2) % sample_rate) - sample_rate / 2)
            if abs(folded - peak["freq"]) <= tolerance:
                unit = (f"{other['freq'] / 1e3:.2f} kHz" if sample_rate is not None else f"{other['freq']:.2f} Hz")
                folds = sample_rate is not None and harmonic > sample_rate / 2
                return f"{n} x {unit}" + (" (folded)" if folds else "")
    return ""


def label_peaks(ax, peaks, x_of, y_of, text_of, max_labels, min_spacing):
    """Direct labels for the strongest peaks, skipping any that would crowd a stronger one."""
    placed = []
    for p in peaks:
        if len(placed) == max_labels:
            break
        if any(abs(x_of(p) - x) < min_spacing for x in placed):
            continue
        ax.annotate(text_of(p), (x_of(p), y_of(p)), textcoords="offset points", xytext=(4, 5),
                    fontsize=8, color=INK_2)
        placed.append(x_of(p))


def style_axes(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=11)
    ax.set_xlabel(xlabel, color=INK_2)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors=INK_2, labelsize=9)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)


def new_figure(rows, title):
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(rows, 1, figsize=(11, 3.4 * rows), facecolor=SURFACE)
    fig.suptitle(title, x=0.01, ha="left", color=INK, fontsize=12)
    return fig, np.atleast_1d(axes)


def finish_figure(fig, path, show):
    import matplotlib.pyplot as plt

    fig.tight_layout()
    png = path.with_suffix(".png")
    fig.savefig(png, dpi=110, facecolor=SURFACE)
    print(f"\nPlot: {png}")
    if show:
        plt.show()
    plt.close(fig)


# ---------------------------------------------------------------- raw snapshots


def high_pass_block_energy(samples, sample_rate, block_size):
    """
    Mirror dsp.c: (code - 2048) through the 10 kHz Butterworth high-pass
    (dsp_filters.c Biquad_InitHighPass, direct form I), squared and summed per
    block. The first block of each snapshot is dropped while the filter settles.
    """
    k = math.tan(math.pi * HIGH_PASS_HZ / sample_rate)
    norm = 1.0 / (1.0 + math.sqrt(2.0) * k + k * k)
    b0, b1, b2 = norm, -2.0 * norm, norm
    a1 = 2.0 * (k * k - 1.0) * norm
    a2 = (1.0 - math.sqrt(2.0) * k + k * k) * norm

    energies = []

    for snapshot in samples:
        x = snapshot.astype(np.float64) - ADC_MIDSCALE
        y = np.empty_like(x)
        x1 = x2 = y1 = y2 = 0.0

        for n, xn in enumerate(x):
            yn = b0 * xn + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, xn, y1, yn
            y[n] = yn

        blocks = (y ** 2).reshape(-1, block_size).sum(axis=1)
        energies.append(blocks[1:])

    return np.concatenate(energies)


def analyse_raw(data, path, show):
    codes = data["samples"]
    sample_rate = float(data["sample_rate"])
    block_size = int(data["block_size"])
    count, length = codes.shape
    samples = codes.astype(np.float64)

    print(f"{count} snapshots of {length} samples ({1e3 * length / sample_rate:.1f} ms) at {sample_rate / 1e3:g} kHz")

    # DC offset and clipping
    means = samples.mean(axis=1)
    clipped = int(np.count_nonzero((codes == 0) | (codes == ADC_MAX_CODE)))
    print(f"Mean code {means.mean():.1f} (mid-scale {ADC_MIDSCALE}, spread {means.min():.1f}-{means.max():.1f}), "
          f"range {codes.min()}-{codes.max()}, clipped samples: {clipped}")

    # Averaged spectrum, calibrated so a sine reads its amplitude in dBFS
    x = samples - means[:, None]
    window = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(length) / length)   # periodic Hann
    spectra = np.abs(np.fft.rfft(x * window, axis=1)) ** 2
    power = spectra.mean(axis=0)
    freqs = np.fft.rfftfreq(length, 1.0 / sample_rate)
    bin_hz = freqs[1]
    level = 20.0 * np.log10(2.0 * np.sqrt(power) / window.sum() / ADC_FULL_SCALE + 1e-12)
    floor = running_median(level, 64)

    def band_rms(low, high):
        band = (freqs >= low) & (freqs < high)
        return math.sqrt(2.0 * power[band].sum() / (length * np.sum(window ** 2)))

    def rms_dbfs(rms):
        return 20.0 * math.log10(rms / (ADC_FULL_SCALE / math.sqrt(2.0)) + 1e-12)

    print(f"\nNoise (RMS in ADC codes, and dBFS against a full-scale sine), {bin_hz:.0f} Hz bins:")
    for low, high in [(1e3, 10e3), (10e3, sample_rate / 2), (20e3, 120e3)]:
        rms = band_rms(low, high)
        print(f"  {low / 1e3:5.0f}-{high / 1e3:5.0f} kHz: {rms:6.2f} codes rms ({rms_dbfs(rms):6.1f} dBFS)")
    print(f"  median floor 20-120 kHz: {np.median(level[(freqs >= 20e3) & (freqs < 120e3)]):.1f} dBFS per bin")

    # Strongest lines
    peaks = find_peaks(freqs, level, floor, min_freq=1e3, min_above_db=10.0, max_count=15,
                       exclusion=4 * bin_hz)

    print("\nStrongest lines (10 dB or more above the local floor):")
    print("   freq kHz   dBFS  above floor  per-snapshot dBFS (min/max)  if above Nyquist  relation")

    nyquist = sample_rate / 2
    for p in peaks:
        lo, hi = max(0, p["bin"] - 2), p["bin"] + 3
        per_snapshot = 20.0 * np.log10(2.0 * np.sqrt(spectra[:, lo:hi].max(axis=1)) / window.sum() / ADC_FULL_SCALE + 1e-12)

        relation = harmonic_of(p, peaks, 2 * bin_hz, sample_rate)

        print(f"  {p['freq'] / 1e3:8.2f}  {p['level']:6.1f}  {p['above']:8.1f} dB   "
              f"{per_snapshot.min():6.1f} / {per_snapshot.max():6.1f}            "
              f"{(sample_rate - p['freq']) / 1e3:7.2f} kHz       {relation}")

    if not peaks:
        print("  none")

    # What the detector should see
    hp_energy = high_pass_block_energy(codes, sample_rate, block_size)
    hp_db = to_db(hp_energy)
    print(f"\nDetector input from these samples (10 kHz high-pass energy per block, firmware dB scale):")
    print(f"  median {np.median(hp_db):.2f} dB, range {hp_db.min():.2f}-{hp_db.max():.2f} dB, "
          f"{len(hp_db)} blocks: compare batEnergyDb / batFloorDb")

    # Plot: spectrum, then the loudest snapshot's waveform
    fig, (ax_spec, ax_wave) = new_figure(2, f"{path.name}: raw ADC")

    khz = freqs / 1e3
    ax_spec.plot(khz, level, color=SERIES_1, linewidth=0.9, label=f"spectrum, mean of {count}")
    ax_spec.plot(khz, floor, color=INK_2, linewidth=1.0, linestyle="--", label="local floor (median)")
    for p in peaks[:6]:
        ax_spec.plot(p["freq"] / 1e3, p["level"], "o", color=SERIES_1, markersize=6,
                     markeredgecolor=SURFACE, markeredgewidth=1.5)
    label_peaks(ax_spec, peaks, lambda p: p["freq"] / 1e3, lambda p: p["level"],
                lambda p: f"{p['freq'] / 1e3:.1f} kHz", max_labels=6, min_spacing=nyquist / 1e3 * 0.04)
    ax_spec.set_xlim(0, nyquist / 1e3)
    style_axes(ax_spec, f"Averaged spectrum ({bin_hz:.0f} Hz bins, Hann)", "frequency (kHz)", "level (dBFS)")
    ax_spec.legend(loc="upper right", frameon=False, fontsize=9, labelcolor=INK_2)

    loudest = int(np.argmax(x.std(axis=1)))
    t_ms = 1e3 * np.arange(length) / sample_rate
    ax_wave.plot(t_ms, codes[loudest], color=SERIES_1, linewidth=0.6)
    ax_wave.set_xlim(0, t_ms[-1])
    style_axes(ax_wave, f"Snapshot {loudest + 1} (the loudest), raw codes", "time (ms)", "ADC code")

    finish_figure(fig, path, show)


# ---------------------------------------------------------------- energy stream


def ms_to_blocks(ms, block_us):
    """bat_detect.c MS_TO_BLOCKS: whole blocks, rounded up."""
    return (int(ms) * 1000 + block_us - 1) // block_us


def replay_detector(energy, floor_start, block_us, threshold_db):
    """
    bat_detect.c BatDetect_ProcessBlock, block by block in float32. Starts
    after warm-up, from a known floor, outside any pulse and with no bat
    present. Returns the floor after each block, the loud runs and counts.
    """
    f32 = np.float32
    threshold = f32(10.0) ** (f32(threshold_db) / f32(10.0))
    fall = f32(block_us) / (f32(DETECTOR["floor_fall_ms"]) * f32(1000.0))
    rise = f32(block_us) / (f32(DETECTOR["floor_rise_ms"]) * f32(1000.0))
    pulse_max = ms_to_blocks(DETECTOR["pulse_max_ms"], block_us)
    noise_run = ms_to_blocks(DETECTOR["noise_run_ms"], block_us)
    window = ms_to_blocks(DETECTOR["trigger_window_ms"], block_us)
    hold_blocks = ms_to_blocks(DETECTOR["hold_ms"], block_us)

    floor = f32(floor_start)
    floors = np.empty(len(energy), dtype=np.float32)
    runs = []
    pulses = triggers = noise_runs = 0
    run = since = hold = 0
    have_pulse = present = False

    for i, e in enumerate(energy):
        if e > floor * threshold:
            run += 1
            if run == noise_run + 1:
                noise_runs += 1
            if run > noise_run:
                floor = f32(floor + (e - floor) * rise)
        else:
            if run > 0:
                runs.append((i - run, run))
                if run <= pulse_max:
                    pulses += 1
                    if present:
                        hold = hold_blocks
                    elif have_pulse and since <= window:
                        present = True
                        hold = hold_blocks
                        triggers += 1
                    have_pulse = True
                    since = 0
                run = 0

            floor = f32(floor + (e - floor) * (fall if e < floor else rise))

        floors[i] = floor

        if since < window + 1:
            since += 1

        if present and hold > 0:
            hold -= 1
            if hold == 0:
                present = False

    return {
        "floors": floors,
        "runs": runs,
        "pulses": pulses,
        "triggers": triggers,
        "noise_runs": noise_runs,
        "pulse_max": pulse_max,
        "noise_run": noise_run,
    }


def analyse_energy(data, path, show):
    sample_rate = float(data["sample_rate"])
    block_size = int(data["block_size"])
    block_s = block_size / sample_rate
    block_us = round(block_s * 1e6)

    blocks = data["block"].astype(np.int64)
    energy = data["energy"].astype(np.float32)
    total = data["total"].astype(np.float64)
    floor_db = data["floor_db"].astype(np.float64)
    count = len(energy)
    seconds = count * block_s
    minutes = seconds / 60.0

    if count < 100:
        sys.exit(f"{path.name}: only {count} records, too short to analyse")

    print(f"{count} blocks = {seconds:.1f} s; {int(data['dropped'])} dropped on the board, "
          f"{int(data['gaps'])} gaps in the block numbers")

    e_db = to_db(energy)
    above = e_db - floor_db
    ultrasonic_db = 10.0 * np.log10((energy + 1e-6) / (total + 1e-6))

    print(f"\nBlock energy above 10 kHz: median {np.median(e_db):.2f} dB, "
          f"1% / 99% {np.percentile(e_db, 1):.2f} / {np.percentile(e_db, 99):.2f} dB, max {e_db.max():.2f} dB")
    print(f"Noise floor: {floor_db.min():.2f}-{floor_db.max():.2f} dB")
    print(f"Energy above floor: median {np.median(above):+.2f} dB, 99.9% {np.percentile(above, 99.9):+.2f} dB; "
          f"blocks over +{DETECTOR['threshold_db']:g} dB: {np.count_nonzero(above > DETECTOR['threshold_db'])}")
    print(f"Share of the energy above 10 kHz: median {np.median(ultrasonic_db):.1f} dB")

    # The board's own counts over the capture
    board_pulses = int(np.sum(np.diff(data["pulses"].astype(np.int64)) % 65536))
    board_noise = int(np.sum(np.diff(data["noise_runs"].astype(np.int64)) % 256))
    present = (data["flags"] & 1).astype(np.int8)
    board_triggers = int(np.count_nonzero(np.diff(present) == 1))
    print(f"\nBoard counted: {board_pulses} pulses ({board_pulses / minutes:.1f}/min), "
          f"{board_triggers} triggers, {board_noise} noise runs")

    # Replay from the board's floor after the first record, then compare
    floor_start = np.float32(10.0 ** (floor_db[0] / 10.0) - 1e-6)
    replay = replay_detector(energy[1:], floor_start, block_us, DETECTOR["threshold_db"])
    floor_error = np.abs(to_db(replay["floors"]) - floor_db[1:]).max()
    print(f"Replay counted: {replay['pulses']} pulses, {replay['triggers']} triggers, "
          f"{replay['noise_runs']} noise runs; floor within {floor_error:.3f} dB of the board's")
    if replay["pulses"] != board_pulses or floor_error > 0.1:
        print("  The replay disagrees with the board: check DETECTOR against bat_detect.c "
              "(a pulse already under way at the start can account for one)")

    # Loud events
    pulse_max, noise_run = replay["pulse_max"], replay["noise_run"]
    print(f"\nLoud runs (over +{DETECTOR['threshold_db']:g} dB), bat-like up to {pulse_max} blocks, "
          f"continuous over {noise_run}:")
    print("   time s   length ms   peak above floor   share above 10 kHz   class")

    classes = {"bat-like": 0, "too long": 0, "continuous": 0}
    for start, length in replay["runs"]:
        i = start + 1   # replay started at record 1
        segment = slice(i, i + length)
        peak = i + int(np.argmax(e_db[segment]))
        reference = floor_db[i - 1]
        kind = "bat-like" if length <= pulse_max else ("too long" if length <= noise_run else "continuous")
        classes[kind] += 1
        if sum(classes.values()) <= 25:
            print(f"  {(blocks[i] - blocks[0]) * block_s:7.2f}   {length * block_s * 1e3:8.1f}   "
                  f"{e_db[peak] - reference:12.1f} dB    {ultrasonic_db[peak]:12.1f} dB       {kind}")
    if sum(classes.values()) > 25:
        print(f"  ... {sum(classes.values()) - 25} more")
    print("  per minute: " + ", ".join(f"{n / minutes:.1f} {kind}" for kind, n in classes.items()))

    # Threshold sweep
    print("\nThreshold sweep (replayed):")
    print("  threshold   pulses/min   triggers/min   noise runs/min")
    for threshold in SWEEP_THRESHOLDS_DB:
        result = replay_detector(energy[1:], floor_start, block_us, threshold)
        print(f"   +{threshold:2d} dB    {result['pulses'] / minutes:9.1f}   {result['triggers'] / minutes:11.1f}"
              f"   {result['noise_runs'] / minutes:13.1f}")

    # Periodicities in the energy
    series = e_db - e_db.mean()
    taper = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(count) / count)
    spectrum = 20.0 * np.log10(np.abs(np.fft.rfft(series * taper)) + 1e-12)
    freqs = np.fft.rfftfreq(count, block_s)
    resolution = freqs[1]
    spectrum_floor = chunk_median(spectrum, int(round(2.0 / resolution)))
    relative = spectrum - np.median(spectrum_floor)
    lines = find_peaks(freqs, spectrum, spectrum_floor, min_freq=0.5, min_above_db=10.0, max_count=8,
                       exclusion=max(0.5, 3 * resolution))

    print(f"\nPeriodic components in the energy ({resolution:.3f} Hz resolution):")
    tolerance = max(0.1, 2 * resolution)
    for p in lines:
        relation = harmonic_of(p, lines, tolerance)
        suspect = next((name for f, name in SUSPECTS if abs(p["freq"] - f) <= tolerance), "")
        note = f"harmonic {relation}" if relation else suspect
        print(f"  {p['freq']:8.2f} Hz   {p['above']:5.1f} dB above the floor   {note}")
    if not lines:
        print("  none stand out")

    # Plot: energy over time, histogram above floor, periodicities
    fig, (ax_time, ax_hist, ax_period) = new_figure(3, f"{path.name}: block energy")

    t = (blocks - blocks[0]) * block_s
    ax_time.plot(t, e_db, color=SERIES_1, linewidth=0.5, label="block energy above 10 kHz")
    ax_time.plot(t, floor_db, color=SERIES_2, linewidth=1.4, label="noise floor")
    ax_time.plot(t, floor_db + DETECTOR["threshold_db"], color=INK_2, linewidth=1.0, linestyle="--",
                 label=f"threshold (+{DETECTOR['threshold_db']:g} dB)")
    bat_like = [start + 1 + int(np.argmax(e_db[start + 1:start + 1 + length]))
                for start, length in replay["runs"] if length <= pulse_max]
    if bat_like:
        ax_time.plot(t[bat_like], e_db[bat_like], "v", color=SERIES_3, markersize=6,
                     markeredgecolor=SURFACE, markeredgewidth=1.0, label="bat-like pulse")
    ax_time.set_xlim(0, t[-1])
    style_axes(ax_time, "Detector input over time", "time (s)", "energy (dB)")
    ax_time.legend(loc="lower right", bbox_to_anchor=(1.0, 1.0), frameon=False, fontsize=9,
                   labelcolor=INK_2, ncol=4, borderaxespad=0.2)

    ax_hist.hist(above, bins=np.arange(math.floor(above.min()), math.ceil(above.max()) + 0.5, 0.5),
                 color=SERIES_1, edgecolor=SURFACE, linewidth=0.5)
    ax_hist.set_yscale("log")
    ax_hist.axvline(DETECTOR["threshold_db"], color=INK_2, linewidth=1.0, linestyle="--")
    ax_hist.text(DETECTOR["threshold_db"], 0.95, f" threshold +{DETECTOR['threshold_db']:g} dB",
                 transform=ax_hist.get_xaxis_transform(), fontsize=8, color=INK_2, va="top")
    style_axes(ax_hist, "Energy above the noise floor", "dB above floor", "blocks (log scale)")

    ax_period.plot(freqs, relative, color=SERIES_1, linewidth=0.6)
    median_level = np.median(spectrum_floor)
    label_peaks(ax_period, lines, lambda p: p["freq"], lambda p: p["level"] - median_level,
                lambda p: f"{p['freq']:.1f} Hz", max_labels=5, min_spacing=freqs[-1] * 0.04)
    ax_period.set_xlim(0, freqs[-1])
    style_axes(ax_period, "Periodicities in the energy", "frequency (Hz)", "dB above median")

    finish_figure(fig, path, show)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", type=Path, help=".npz captures from capture.py")
    parser.add_argument("--show", action="store_true", help="open the plots as well as saving them")
    args = parser.parse_args()

    import matplotlib
    if not args.show:
        matplotlib.use("Agg")

    for path in args.files:
        with np.load(path, allow_pickle=False) as capture:
            data = {name: capture[name] for name in capture.files}

        kind = str(data["kind"])
        label = str(data["label"])
        print(f"=== {path.name} ({kind}{', ' + label if label else ''}; build {data['build']})")

        if kind == "raw":
            analyse_raw(data, path, args.show)
        elif kind == "energy":
            analyse_energy(data, path, args.show)
        else:
            print(f"Unknown capture kind '{kind}'")
        print()


if __name__ == "__main__":
    main()
