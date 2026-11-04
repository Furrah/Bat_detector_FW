"""
bat_signals.py - synthetic bat calls for testing the classifier model
(Tools/classify.py) before there are real recordings.

Each call follows a frequency track made of linear segments, with a
raised-cosine envelope and an optional second harmonic, and repeats at the
species' pulse interval. The noise is shaped like the board's own: its
spectrum comes from a raw capture (capture.py raw), and the steady lines in
it (e.g. the 24.95 kHz one) are added back as pure tones, as they are on the
board. Samples are rounded to 12-bit ADC codes and clipped, with the
measured DC offset.

    python -I Tools/bat_signals.py [--out Captures/synth] [--noise Captures/<raw>.npz]

writes the bench signals of Docs/Bat Species Classifier.md ("Bench signals")
as recorder-format WAVs (BATnnnnn.WAV layout: 512-byte header, (code - 2048)
x 16), each with a .json of the true call parameters.

Needs: python -m pip install numpy
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np


SAMPLE_RATE_HZ = 250_000
ADC_MIDSCALE = 2048
ADC_MAX = 4095
DC_OFFSET_CODES = -48           # measured on BAT00001-6 (Nov 2)
WAV_SAMPLE_SHIFT = 4            # recorder.c: (code - 2048) << 4

DEFAULT_NOISE = Path(__file__).resolve().parent.parent / "Captures" / "20261030_143507_raw_quiet-room.npz"

LINE_ABOVE_DB = 10.0            # a bin this far above its neighbourhood is a steady line
LINE_MIN_HZ = 5000.0


# ---------------------------------------------------------------- noise


class NoiseModel:
    """
    Noise with the spectrum of a raw capture: a smooth part (shaped Gaussian
    noise) plus the capture's steady lines as sinusoids.
    """

    SEGMENT = 1024

    def __init__(self, path=None):
        self.lines = []                     # (frequency Hz, amplitude in codes)
        if path is None or not Path(path).exists():
            # white noise at roughly the measured level above 10 kHz
            self.freqs = np.linspace(0, SAMPLE_RATE_HZ / 2, self.SEGMENT // 2 + 1)
            self.density = np.full(self.freqs.size, 6.0 ** 2 / self.freqs.size)
            self.source = "white, 6 codes rms"
            return

        data = np.load(path)
        snapshots = data["samples"].astype(np.float64)
        snapshots -= snapshots.mean(axis=1, keepdims=True)

        n = self.SEGMENT
        window = np.hanning(n)
        s2 = np.sum(window ** 2)
        powers = []
        for x in snapshots:
            for start in range(0, x.size - n + 1, n // 2):
                powers.append(np.abs(np.fft.rfft(x[start:start + n] * window)) ** 2)
        # variance per bin: sums to the signal's variance
        density = np.mean(powers, axis=0) / (s2 * n / 2)

        self.freqs = np.fft.rfftfreq(n, 1.0 / SAMPLE_RATE_HZ)
        smooth = running_median(density, 7)
        excess_db = 10 * np.log10(density / smooth)

        # steady lines: local maxima well above their neighbourhood; the
        # excess power over the main lobe is the line's A^2 / 2
        k = np.arange(2, density.size - 2)
        peaks = k[(excess_db[k] > LINE_ABOVE_DB) & (density[k] >= density[k - 1])
                  & (density[k] >= density[k + 1]) & (self.freqs[k] >= LINE_MIN_HZ)]
        for i in peaks:
            excess = np.sum(np.clip(density[i - 2:i + 3] - smooth[i - 2:i + 3], 0, None))
            a, b, c = np.log(density[i - 1:i + 2])
            offset = 0.5 * (a - c) / (a - 2 * b + c)
            self.lines.append((float(self.freqs[i] + offset * self.freqs[1]), math.sqrt(2 * excess)))

        self.density = smooth
        self.source = Path(path).name

    def rms_above(self, hz):
        """rms of the smooth part above a frequency, in codes"""
        return math.sqrt(np.sum(self.density[self.freqs >= hz]))

    def generate(self, count, rng):
        spectrum = np.fft.rfft(rng.standard_normal(count))
        freqs = np.fft.rfftfreq(count, 1.0 / SAMPLE_RATE_HZ)
        # white noise of unit variance has density 1 / (count / 2) per bin of
        # this length; shape it to the model's density per 1024-point bin
        shape = np.sqrt(np.interp(freqs, self.freqs, self.density) * (self.SEGMENT / 2))
        x = np.fft.irfft(spectrum * shape, count)
        t = np.arange(count) / SAMPLE_RATE_HZ
        for freq, amplitude in self.lines:
            x += amplitude * np.sin(2 * np.pi * freq * t + rng.uniform(0, 2 * np.pi))
        return x


def running_median(values, half_width):
    padded = np.pad(values, half_width, mode="edge")
    return np.median(np.lib.stride_tricks.sliding_window_view(padded, 2 * half_width + 1), axis=1)


# ---------------------------------------------------------------- calls


def call_waveform(track, amplitude, harmonic_db=None, rng=None):
    """
    One call. track is [(ms, kHz), ...]: linear segments, starting at 0 ms.
    Returns the samples and the call's true parameters.
    """
    times = np.array([p[0] for p in track]) * 1e-3
    freqs = np.array([p[1] for p in track]) * 1e3
    duration = times[-1]
    count = int(round(duration * SAMPLE_RATE_HZ))
    t = np.arange(count) / SAMPLE_RATE_HZ
    f = np.interp(t, times, freqs)
    phase = 2 * np.pi * np.cumsum(f) / SAMPLE_RATE_HZ
    if rng is not None:
        phase += rng.uniform(0, 2 * np.pi)

    # raised-cosine attack and decay, 15% of the call each (0.2-2 ms)
    taper = min(max(0.15 * duration, 0.2e-3), 2e-3, duration / 2)
    envelope = np.ones(count)
    ramp = int(taper * SAMPLE_RATE_HZ)
    if ramp > 0:
        rise = 0.5 * (1 - np.cos(np.pi * np.arange(ramp) / ramp))
        envelope[:ramp] = rise
        envelope[-ramp:] = rise[::-1]

    x = np.sin(phase)
    if harmonic_db is not None:
        x += 10 ** (harmonic_db / 20) * np.sin(2 * phase)
    x *= amplitude * envelope

    # truth, in the classifier's terms (kHz, ms; slopes positive = falling)
    slopes = np.abs(np.diff(freqs)) / np.maximum(np.diff(times) * 1e3, 1e-9) / 1e3
    last = [i for i, (t0, t1) in enumerate(zip(times[:-1], times[1:])) if t1 > 0.6 * duration]
    flattest = min(last, key=lambda i: slopes[i])
    truth = {
        "dur_ms": duration * 1e3,
        "f_start": freqs[0] / 1e3,
        "f_end": freqs[-1] / 1e3,
        "f_peak": float(np.interp(duration / 2, times, freqs)) / 1e3,
        "f_char": float(freqs[flattest + 1]) / 1e3,
        "bw": (freqs[0] - freqs[-1]) / 1e3,
        "flat": bool(slopes[flattest] < 2.0),
    }
    return x, truth


class Signal:
    """A signal under construction: noise plus calls, in ADC codes"""

    def __init__(self, name, seconds, noise, rng, description):
        self.name = name
        self.description = description
        self.rng = rng
        self.x = noise.generate(int(seconds * SAMPLE_RATE_HZ), rng)
        self.calls = []
        self.noise_rms = noise.rms_above(10_000)

    def level(self, db):
        """amplitude in codes for a level in dB re the noise rms above 10 kHz"""
        return self.noise_rms * 10 ** (db / 20)

    def add_call(self, at_s, track, level_db, harmonic_db=None, echo=None):
        wave, truth = call_waveform(track, self.level(level_db), harmonic_db, self.rng)
        start = int(round(at_s * SAMPLE_RATE_HZ))
        end = min(start + wave.size, self.x.size)
        self.x[start:end] += wave[:end - start]
        truth.update({"t_s": at_s, "level_db": level_db})
        self.calls.append(truth)
        if echo is not None:
            delay_ms, echo_db = echo
            e0 = start + int(delay_ms * 1e-3 * SAMPLE_RATE_HZ)
            e1 = min(e0 + wave.size, self.x.size)
            if e0 < self.x.size:
                self.x[e0:e1] += 10 ** (echo_db / 20) * wave[:e1 - e0]

    def add_sequence(self, first_s, last_s, interval_ms, tracks, level_db, **kw):
        """calls every interval_ms; tracks cycle (for alternating calls)"""
        at, i = first_s, 0
        while at < last_s:
            self.add_call(at, tracks[i % len(tracks)], level_db, **kw)
            at += interval_ms * 1e-3
            i += 1

    def add_tone(self, from_s, freq_khz, level_db):
        start = int(from_s * SAMPLE_RATE_HZ)
        t = np.arange(self.x.size - start) / SAMPLE_RATE_HZ
        self.x[start:] += self.level(level_db) * np.sin(2 * np.pi * freq_khz * 1e3 * t)

    def add_burst(self, at_s, dur_ms, level_db, lo_khz=15.0, hi_khz=110.0, tilt_db_per_octave=0.0):
        """
        A burst of broadband noise, a stand-in for a finger or cloth rub (not
        a call: it adds no truth). level_db is its rms over the band, re the
        noise rms above 10 kHz. tilt < 0 puts more energy low down.
        """
        count = int(dur_ms * 1e-3 * SAMPLE_RATE_HZ)
        spectrum = np.fft.rfft(self.rng.standard_normal(count))
        f = np.fft.rfftfreq(count, 1.0 / SAMPLE_RATE_HZ) / 1e3
        band = (f >= lo_khz) & (f <= hi_khz)
        shape = np.where(band, (np.maximum(f, lo_khz) / lo_khz) ** (tilt_db_per_octave / 6.02), 0.0)
        burst = np.fft.irfft(spectrum * shape, count)
        burst *= self.level(level_db) / max(np.std(burst), 1e-12)
        ramp = min(count // 4, int(2e-3 * SAMPLE_RATE_HZ))
        if ramp > 0:
            rise = 0.5 * (1 - np.cos(np.pi * np.arange(ramp) / ramp))
            burst[:ramp] *= rise
            burst[-ramp:] *= rise[::-1]
        start = int(round(at_s * SAMPLE_RATE_HZ))
        end = min(start + count, self.x.size)
        self.x[start:end] += burst[:end - start]

    def add_rubbing(self, first_s, last_s, level_db, **kw):
        """Bursts of 5-60 ms at irregular 40-250 ms intervals, like rubbing fingers"""
        at = first_s
        while at < last_s:
            dur = self.rng.uniform(5.0, 60.0)
            self.add_burst(at, dur, level_db + self.rng.uniform(-6.0, 6.0), **kw)
            at += (dur + self.rng.uniform(40.0, 250.0)) * 1e-3

    def codes(self):
        return np.clip(np.round(self.x + ADC_MIDSCALE + DC_OFFSET_CODES), 0, ADC_MAX).astype(np.uint16)


# Doc "Bench signals", at 250 kHz. Calls start after the classifier's 0.5 s
# noise-floor settling time.
PIPISTRELLE = [(0, 70), (2, 46), (6, 45)]
MYOTIS = [(0, 100), (3, 30)]
HORSESHOE = [(0, 82), (50, 82)]
NOCTULE_LOW = [(0, 30), (3, 22), (15, 20)]
NOCTULE_HIGH = [(0, 34), (3, 26), (15, 24)]
# Added for the species stages (European Bat Calls doc values)
SOPRANO = [(0, 80), (2, 56), (6, 55)]
LEISLER_LOW = [(0, 36), (2, 26), (9, 24.5)]
LEISLER_HIGH = [(0, 39), (2, 29), (9, 27.5)]
SEROTINE = [(0, 40), (4, 27), (11, 25)]
NATTERER = [(0, 110), (1.5, 15)]        # the widest Myotis sweep: ~100 kHz in about 1 ms


def bench_signals(noise, seed=1, level_db=20.0):
    rng = np.random.default_rng(seed)
    signals = []

    s = Signal("cf82", 3.0, noise, rng, "82 kHz tone, 50 ms every 100 ms (greater horseshoe)")
    s.add_sequence(0.7, 2.9, 100, [HORSESHOE], level_db)
    signals.append(s)

    s = Signal("fm100-30", 3.0, noise, rng, "chirp 100 -> 30 kHz in 3 ms every 100 ms (Myotis)")
    s.add_sequence(0.7, 2.9, 100, [MYOTIS], level_db)
    signals.append(s)

    s = Signal("pip45", 3.0, noise, rng, "70 -> 46 kHz in 2 ms, 46 -> 45 kHz in 4 ms, every 90 ms (common pipistrelle)")
    s.add_sequence(0.7, 2.9, 90, [PIPISTRELLE], level_db)
    signals.append(s)

    s = Signal("noctule", 4.0, noise, rng, "15 ms QCF calls, tails alternating 20 / 24 kHz, every 250 ms (noctule)")
    s.add_sequence(0.7, 3.9, 250, [NOCTULE_LOW, NOCTULE_HIGH], level_db)
    signals.append(s)

    s = Signal("cricket-pip", 3.0, noise, rng, "40 kHz tone throughout, 10 dB above the calls, plus pip45 from 1 s")
    s.add_tone(0.0, 40.0, level_db + 10)
    s.add_sequence(1.0, 2.9, 90, [PIPISTRELLE], level_db)
    signals.append(s)

    s = Signal("pip45-echo", 3.0, noise, rng, "pip45 with an echo 8 ms later, 15 dB down")
    s.add_sequence(0.7, 2.9, 90, [PIPISTRELLE], level_db, echo=(8.0, -15.0))
    signals.append(s)

    s = Signal("noise", 60.0, noise, rng, "noise only, for false calls per minute")
    signals.append(s)

    # Added for the species stages
    s = Signal("soprano55", 3.0, noise, rng, "80 -> 56 kHz in 2 ms, 56 -> 55 kHz in 4 ms, every 80 ms (soprano pipistrelle)")
    s.add_sequence(0.7, 2.9, 80, [SOPRANO], level_db)
    signals.append(s)

    s = Signal("leisler", 4.0, noise, rng, "9 ms QCF calls, tails alternating 24.5 / 27.5 kHz, every 200 ms (Leisler's)")
    s.add_sequence(0.7, 3.9, 200, [LEISLER_LOW, LEISLER_HIGH], level_db)
    signals.append(s)

    s = Signal("serotine", 4.0, noise, rng, "11 ms calls ending at 25 kHz, every 150-250 ms, no alternation (serotine)")
    at = 0.7
    while at < 3.9:
        s.add_call(at, SEROTINE, level_db)
        at += rng.uniform(0.15, 0.25)
    signals.append(s)

    s = Signal("natterer", 3.0, noise, rng, "110 -> 15 kHz in 1.5 ms every 80 ms (Natterer's: the widest sweep)")
    s.add_sequence(0.7, 2.9, 80, [NATTERER], level_db)
    signals.append(s)

    s = Signal("rub-flat", 6.0, noise, rng, "broadband bursts 15-110 kHz, 5-60 ms, irregular (a rub, flat spectrum)")
    s.add_rubbing(0.7, 5.9, level_db + 5)
    signals.append(s)

    s = Signal("rub-tilt", 6.0, noise, rng, "the same, falling 6 dB per octave (a rub, more energy low down)")
    s.add_rubbing(0.7, 5.9, level_db + 5, tilt_db_per_octave=-6.0)
    signals.append(s)

    return signals


def level_sweep(noise, levels_db, seed=2):
    """pip45 at a range of levels, 20 calls each"""
    rng = np.random.default_rng(seed)
    signals = []
    for db in levels_db:
        s = Signal(f"pip45-{db:+.0f}dB", 2.6, noise, rng, f"pip45 at {db:+.0f} dB re noise rms")
        s.add_sequence(0.7, 2.5, 90, [PIPISTRELLE], db)
        signals.append(s)
    return signals


# ---------------------------------------------------------------- WAV files


def write_recorder_wav(path, codes):
    """BATnnnnn.WAV layout (recorder.c): RIFF, fmt, 460-byte JUNK pad, data at 512"""
    samples = ((codes.astype(np.int32) - ADC_MIDSCALE) << WAV_SAMPLE_SHIFT).astype("<i2")
    data_bytes = samples.size * 2
    header = b"RIFF" + struct.pack("<I", 504 + data_bytes) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE_HZ, SAMPLE_RATE_HZ * 2, 2, 16)
    header += b"JUNK" + struct.pack("<I", 460) + bytes(460)
    header += b"data" + struct.pack("<I", data_bytes)
    assert len(header) == 512
    path.write_bytes(header + samples.tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=Path("Captures/synth"))
    parser.add_argument("--noise", type=Path, default=DEFAULT_NOISE, help="raw capture whose noise to copy")
    parser.add_argument("--level", type=float, default=20.0, help="call level, dB re noise rms above 10 kHz")
    args = parser.parse_args()

    noise = NoiseModel(args.noise)
    print(f"Noise: {noise.source}, {noise.rms_above(10_000):.1f} codes rms above 10 kHz, "
          f"{len(noise.lines)} line(s): " + ", ".join(f"{f / 1e3:.2f} kHz {a:.1f} codes" for f, a in noise.lines))

    args.out.mkdir(parents=True, exist_ok=True)
    for s in bench_signals(noise, level_db=args.level):
        wav = args.out / f"{s.name}.wav"
        write_recorder_wav(wav, s.codes())
        wav.with_suffix(".json").write_text(json.dumps({"description": s.description, "calls": s.calls}, indent=1))
        print(f"  {wav}: {len(s.calls)} calls, {s.description}")


if __name__ == "__main__":
    sys.exit(main())
