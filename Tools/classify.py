"""
classify.py - numpy reference model of the bat species classifier
(Docs/Bat Species Classifier.md, Docs/CLASSIFIER_PLAN.md). Stages 1-3, the
frame stage, the segmenter and the per-call features, are here; stages 4-6,
shape groups, species rules and sequence voting, are in species.py. It
mirrors the C design step by step so the board can later be checked against
it on the same samples.

At 250 kHz the doc's frames (designed for 500 ksps) become a 128-point FFT
with a 64-sample hop: the same 0.512 ms window, 1.95 kHz bins and a frame
every 0.256 ms, four frames per 256-sample audio block.

    python -I Tools/classify.py D:\\BAT00001.WAV [more files] [--plot] [--doc-params]
    python -I Tools/classify.py --selftest [--plot]

Files: recorder WAVs, other 16- or 24-bit PCM WAVs (resampled to 250 kHz and
scaled to 12-bit codes, as the board would see them) or .RAW ADC codes.
--selftest runs the doc's bench signals (Tools/bat_signals.py), synthesised
in the board's measured noise, and checks the features against the truth.

Needs: python -m pip install numpy matplotlib
"""

import argparse
import glob
import math
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))    # -I leaves the script's folder off the path

import species  # noqa: E402


SAMPLE_RATE_HZ = 250_000
ADC_MIDSCALE = 2048
ADC_MAX = 4095

# classifier_config.h, at 250 kHz
FFT_N = 128
HOP = 64
BIN_KHZ = SAMPLE_RATE_HZ / FFT_N / 1000         # 1.953 kHz
HOP_MS = 1000 * HOP / SAMPLE_RATE_HZ            # 0.256 ms
K_LO = 5                                        # 9.8 kHz
K_HI = 61                                       # 119.1 kHz (Nyquist is bin 64)
TRACK_MAX = 320                                 # 82 ms of frames

# Parameters as the doc gives them. Values that differ from the doc are
# chosen in PARAMS below, with the reason.
DOC_PARAMS = {
    "nf_up": 1 / 2048,          # floor rises over ~0.5 s ...
    "nf_down": 1 / 64,          # ... and falls over ~16 ms
    "nf_bias_db": 0.0,          # SNR is measured against the floor as tracked
    "settle_frames": 2048,      # segmenter muted while the floor settles (0.52 s)
    "t_on_db": 12.0,            # SNR to start a call
    "t_off_db": 6.0,            # weak frame, absolute
    "end_drop_db": 24.0,        # weak frame, relative to the call's max
    "n_on": 2,                  # frames above t_on to start
    "n_off": 3,                 # weak frames to end
    "rise_max_khz": 8.0,        # a jump up this big starts a new call
    "min_call_frames": 3,       # strong frames, else it was a click
    "harm_db": 10.0,            # 2nd harmonic counts if this far above its floor (not in the doc)
    "flat_slope": 2.0,          # kHz/ms: flatter than this counts as flat
    "t_broad_db": None,         # broadband (rub) test: off in the doc; see PARAMS
    "t_near_db": 10.0,
    "t_raised_db": None,
    "drop_max_khz": None,       # in a call, how far the peak may fall per frame: no limit in the doc
}
PARAMS = dict(DOC_PARAMS)
PARAMS.update({
    # The floor tracker rises 32x slower than it falls, so it settles below
    # the mean noise power: -6.1 dB measured on the board's noise (-7 dB in
    # theory for Gaussian noise). The peak of 57 noise bins is typically
    # +6.4 dB re the mean, so with the doc's values pure noise read 11.4 dB
    # (median) against t_on 12 dB: 29,000 false calls a minute. Measuring
    # against the mean noise makes the thresholds mean what they say.
    "nf_bias_db": 6.0,
    # Noise frames peak at +6.4 dB (median) re the mean, so a 6 dB t_off
    # called half of them strong and weak calls never ended. 2% reach 9 dB.
    "t_off_db": 9.0,
    # A frame whose median bin is more than this above the mean noise is
    # broadband (a rub, cloth, typing), not part of a call. The bench calls
    # reach 2.1 dB at most, the synthetic rubs 14.6 dB at least.
    "t_broad_db": 6.0,
    "t_near_db": 10.0,
    # A call most of whose frames have their median bin more than this above
    # the mean noise is broadband: the edges of a rub. Bench calls stay at
    # 2.1 dB or below in every frame; the rub fragments that get past the
    # frame tests sit at 2-7 dB.
    "t_raised_db": 3.0,
    # In a call, the peak is searched for only down to this far below the
    # track's last point (rises are left to rise_max_khz). The doc searches the
    # whole band every frame, so a call jumped to whatever else was loudest:
    # a horseshoe's 108 kHz call to its own 54 kHz fundamental, soprano calls
    # to an insect at 11 kHz (diary 50). The steepest European sweep, Natterer's
    # (~120 to 20 kHz in 2-3 ms), falls ~13 kHz per 0.256 ms frame.
    "drop_max_khz": 16.0,
})

# per-point and per-call flags (doc: PT_GAP, PT_CLIP; CALL_TRUNCATED, CALL_GAPPED, CALL_CLIPPED)
PT_CLIP = 2
PT_RAISED = 4                                   # the frame's median bin is raised (not in the doc)
CALL_TRUNCATED, CALL_GAPPED, CALL_CLIPPED = 1, 2, 4

IDLE, ARMING, IN_CALL = 0, 1, 2

# Plot inks (as analyse_capture.py)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SERIES_1 = "#2a78d6"
SERIES_2 = "#eb6834"
SERIES_3 = "#1baf7a"


# ---------------------------------------------------------------- input


def read_chunks(raw):
    """RIFF chunks after the 'WAVE' id: {id: (offset of its data, its size)}"""
    chunks = {}
    offset = 12
    while offset + 8 <= len(raw):
        chunk_id, size = struct.unpack_from("<4sI", raw, offset)
        chunks.setdefault(chunk_id, (offset + 8, size))
        offset += 8 + size + (size & 1)
        if chunk_id == b"data":
            break
    return chunks


def resample(x, rate):
    """FFT resampling to 250 kHz (drops everything above the new Nyquist)"""
    count = int(round(x.size * SAMPLE_RATE_HZ / rate))
    spectrum = np.fft.rfft(x)
    keep = count // 2 + 1
    spectrum = spectrum[:keep] if keep <= spectrum.size else np.pad(spectrum, (0, keep - spectrum.size))
    return np.fft.irfft(spectrum, count) * (count / x.size)


def load_codes(path):
    """12-bit ADC codes at 250 kHz, and a description of where they came from"""
    raw = path.read_bytes()
    if raw[:4] != b"RIFF":
        return np.frombuffer(raw[:len(raw) // 2 * 2], dtype="<u2").astype(np.int64), "raw ADC codes"

    chunks = read_chunks(raw)
    if b"fmt " not in chunks or b"data" not in chunks:
        raise ValueError("not a WAV file")
    fmt_offset, _ = chunks[b"fmt "]
    audio_format, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", raw, fmt_offset)
    if audio_format not in (1, 0xFFFE) or bits not in (16, 24):
        raise ValueError(f"only 16- and 24-bit PCM WAVs are read (format {audio_format}, {bits}-bit)")
    data_offset, data_bytes = chunks[b"data"]
    data_bytes = min(data_bytes, len(raw) - data_offset)
    width = bits // 8
    frames = data_bytes // width // channels
    b = np.frombuffer(raw, np.uint8, count=frames * channels * width,
                      offset=data_offset).reshape(frames, channels, width)[:, 0, :]
    if bits == 16:
        samples = b.copy().view("<i2").reshape(-1)
        if rate == SAMPLE_RATE_HZ and not np.any(samples & 0xF):
            return (samples.astype(np.int64) >> 4) + ADC_MIDSCALE, "recorder WAV"
        x = samples.astype(np.float64)
    else:
        v = b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)
        x = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 256    # to the 16-bit scale

    note = f"{rate} Hz {bits}-bit WAV"
    if rate != SAMPLE_RATE_HZ:
        x = resample(x, rate)
        note += ", resampled to 250 kHz"
    return np.clip(np.round(x / 16) + ADC_MIDSCALE, 0, ADC_MAX).astype(np.int64), note + ", scaled to 12 bits"


# ---------------------------------------------------------------- stage 1: frames


def hann():
    """Symmetric Hann, as gen_dsp_tables.py makes fftHannWindow"""
    n = np.arange(FFT_N)
    return 0.5 * (1 - np.cos(2 * np.pi * n / (FFT_N - 1)))


def dc_removed_hops(codes):
    """
    The doc's slow DC tracker, in integers as the C does it: each hop has the
    tracker's value before the hop subtracted, then the tracker moves 1/64 of
    the way to the hop's mean.
    """
    count = codes.size // HOP
    hops = codes[:count * HOP].reshape(count, HOP)
    dc = np.empty(count, dtype=np.int64)
    d = ADC_MIDSCALE
    for h, total in enumerate(hops.sum(axis=1).tolist()):
        dc[h] = d
        d += (total // HOP - d) >> 6
    clipped = np.any((hops == 0) | (hops == ADC_MAX), axis=1)
    return (hops - dc[:, None]).astype(np.float64), clipped


def power_frames(hops_z, start, count, window):
    """|X|^2 of frames start..start+count-1; frame j is hops j-1 and j (hops_z has a zero hop first)"""
    frames = np.concatenate([hops_z[start:start + count], hops_z[start + 1:start + count + 1]], axis=1)
    spectrum = np.fft.rfft(frames * window, axis=1)
    return spectrum.real ** 2 + spectrum.imag ** 2


# ---------------------------------------------------------------- stage 2: segmenter


class Track:
    """Points of one call: frequency in 10 Hz units, SNR as int8, flags (cls_track_pt_t)"""

    def reset(self, frame0):
        self.frame0 = frame0
        self.f_10hz, self.snr, self.flags = [], [], []
        self.max_snr = -math.inf
        self.i_max = 0
        self.harm_at_max = False
        self.n_strong = 0
        self.last_f = 0.0
        self.call_flags = 0

    def push(self, f, snr, harm, flags, strong, weak):
        if len(self.f_10hz) < TRACK_MAX:
            self.f_10hz.append(int(round(f * 100)))
            self.snr.append(int(min(max(round(snr), -128), 127)))
            self.flags.append(flags)
            if snr > self.max_snr:
                self.i_max, self.harm_at_max = len(self.f_10hz) - 1, harm
        else:
            self.call_flags |= CALL_TRUNCATED
        self.max_snr = max(self.max_snr, snr)
        if strong:
            self.n_strong += 1
        if not weak:
            self.last_f = f
        if flags & PT_CLIP:
            self.call_flags |= CALL_CLIPPED


class Segmenter:
    """The doc's three-state machine: idle, arming (debounces clicks), in call"""

    def __init__(self, p):
        self.p = p
        self.state = IDLE
        self.n = self.off = 0
        self.track = Track()
        self.calls = []
        self.stats = {"arms": 0, "aborted_arms": 0, "clicks": 0, "rise_splits": 0, "broadband": 0}

    def start(self, frame, f, snr, harm, flags):
        self.state, self.n = ARMING, 1
        self.stats["arms"] += 1
        self.track.reset(frame)
        self.track.push(f, snr, harm, flags, snr >= self.p["t_on_db"], False)

    def step(self, frame, f, snr, harm, flags):
        p = self.p
        if self.state == IDLE:
            if snr >= p["t_on_db"]:
                self.start(frame, f, snr, harm, flags)

        elif self.state == ARMING:
            if snr < p["t_on_db"]:
                self.state = IDLE
                self.stats["aborted_arms"] += 1
                return
            if p["drop_max_khz"] is not None and f - self.track.last_f > p["rise_max_khz"]:
                # Started on something lower, a harmonic's fundamental say, and
                # jumped up: start again here (as rise_max does in a call).
                # Not in the doc; goes with drop_max_khz.
                self.stats["aborted_arms"] += 1
                self.start(frame, f, snr, harm, flags)
                return
            self.track.push(f, snr, harm, flags, True, False)
            self.n += 1
            if self.n >= p["n_on"]:
                self.state, self.off = IN_CALL, 0

        else:
            t = self.track
            weak = snr < p["t_off_db"] or snr < t.max_snr - p["end_drop_db"]
            if not weak and f - t.last_f > p["rise_max_khz"]:     # another bat, or the next call
                self.end_call()
                self.stats["rise_splits"] += 1
                self.start(frame, f, snr, harm, flags)
                return
            t.push(f, snr, harm, flags, snr >= p["t_on_db"], weak)
            self.off = self.off + 1 if weak else 0
            if self.off >= p["n_off"]:
                self.end_call()
                self.state = IDLE

    def end_call(self):
        if self.track.n_strong < self.p["min_call_frames"]:
            self.stats["clicks"] += 1
            return
        if self.track.flags and sum(1 for f in self.track.flags if f & PT_RAISED) * 2 >= len(self.track.flags):
            self.stats["broadband"] += 1        # not in the doc: mostly raised frames, a rub
            return
        call = extract_features(self.track, self.p)
        if call is not None:
            self.calls.append(call)


# ---------------------------------------------------------------- stage 3: features


def extract_features(t, p):
    """
    The doc's extract_features. One change: the call is measured over the
    frames at or above max(t_on, max - end_drop). The doc uses the relative
    test only, which keeps the noise frames at the tail of a call less than
    30 dB above the floor; at t_off, 2% of noise frames still count, and one
    at the end of a call becomes its end frequency. Under 1e-5 reach t_on.
    """
    snr = np.array(t.snr)
    valid = np.flatnonzero(snr >= max(p["t_on_db"], t.max_snr - p["end_drop_db"]))
    if valid.size == 0:
        return None
    i0, i1 = int(valid[0]), int(valid[-1])
    m = i1 - i0 + 1
    f = np.array(t.f_10hz, dtype=np.int64)

    def F10(i):
        """10 Hz units: 3-point median of the track, indices clamped to the call"""
        a, b, c = f[max(i - 1, i0)], f[i], f[min(i + 1, i1)]
        return int(sorted((a, b, c))[1])

    def F(i):
        """kHz"""
        return float(F10(i)) * 0.01

    def lsq_slope(start, count):
        """kHz/ms, least squares over count frames"""
        y = np.array([F(i) for i in range(start, start + count)])
        x = np.arange(count) * HOP_MS
        x -= x.mean()
        return float(np.dot(x, y - y.mean()) / np.dot(x, x))

    c = {
        "frame0": t.frame0 + i0,
        "t_s": (t.frame0 + i0) * HOP / SAMPLE_RATE_HZ,
        "dur_ms": m * HOP_MS,
        "f_start": F(i0),
        "f_end": F(i1),
        "f_peak": F(t.i_max) if i0 <= t.i_max <= i1 else F(i0),
        "snr_db": float(t.max_snr),
        "harmonic": bool(t.harm_at_max),
        "flags": t.call_flags,
    }
    c["bw"] = c["f_start"] - c["f_end"]

    # slope of each third, positive = falling
    if m >= 6:
        s = m // 3
        c["slope_start"] = -lsq_slope(i0, s)
        c["slope_mid"] = -lsq_slope(i0 + s, s)
        c["slope_end"] = -lsq_slope(i1 - s + 1, s)
    else:
        c["slope_start"] = c["slope_mid"] = c["slope_end"] = c["bw"] / c["dur_ms"]

    # flatness: share of frame-to-frame steps flatter than flat_slope
    flat = sum(abs(F(i - 1) - F(i)) / HOP_MS < p["flat_slope"] for i in range(i0 + 1, i1 + 1))
    c["flat_frac"] = flat / (m - 1) if m > 1 else 0.0

    # Characteristic frequency: the end of the flattest part of the last 40%.
    # Changed from the doc, which takes the flattest single step: on a QCF
    # tail every step is about as flat, so frame noise picked a random point
    # in it (24.0-25.1 kHz on a 25 -> 24 kHz tail). Here the slope is fitted
    # over 4 frames (0.77 ms) and the latest minimum wins. The fit is in
    # whole 10 Hz units (x2, so |slope| = this * 0.01 / (10 * HOP_MS)): equal
    # slopes then tie exactly. In floats, rounding broke ties differently in
    # the C (float32) and here (float64), up to 5.5 kHz apart (diary 51).
    best = math.inf
    c["f_char"] = c["f_end"]
    for i in range(max(i0 + 3, i0 + (6 * m) // 10), i1 + 1):
        slope = abs(-3 * F10(i - 3) - F10(i - 2) + F10(i - 1) + 3 * F10(i))
        if slope <= best:
            best, c["f_char"] = slope, F(i)

    # the track, for plotting
    c["track_t"] = (t.frame0 + np.arange(i0, i1 + 1)) * HOP / SAMPLE_RATE_HZ
    c["track_f"] = np.array([F(i) for i in range(i0, i1 + 1)])
    return c


# ---------------------------------------------------------------- the model


class Result:
    pass


def analyse(codes, p=PARAMS, max_columns=3000):
    """Run stages 1-3 over a recording. Returns calls, statistics and traces for plotting."""
    hops, clipped = dc_removed_hops(codes)
    n_frames = hops.shape[0]
    hops_z = np.vstack([np.zeros((1, HOP)), hops])
    window = hann()

    seg = Segmenter(p)
    nf = None
    bias = 10 ** (p["nf_bias_db"] / 10)
    up, down = p["nf_up"], p["nf_down"]
    harm_ratio = 10 ** (p["harm_db"] / 10)
    t_on = p["t_on_db"]
    broad = 10 ** (p["t_broad_db"] / 10) if p["t_broad_db"] is not None else math.inf
    near = 10 ** (-p["t_near_db"] / 10)
    raised_level = 10 ** (p["t_raised_db"] / 10) if p["t_raised_db"] is not None else math.inf

    snr_trace = np.zeros(n_frames, dtype=np.float32)
    median_trace = np.zeros(n_frames, dtype=np.float32)
    state_trace = np.zeros(n_frames, dtype=np.int8)
    idle_before = np.zeros(n_frames, dtype=bool)
    group = max(1, math.ceil(n_frames / max_columns))      # frames per spectrogram column
    columns = []
    idle_power = np.zeros(K_HI - K_LO + 1)
    idle_frames = 0
    nf_sum = np.zeros(K_HI - K_LO + 1)

    chunk = group * max(1, 4096 // group)
    for start in range(0, n_frames, chunk):
        count = min(chunk, n_frames - start)
        P = power_frames(hops_z, start, count, window)
        band = P[:, K_LO:K_HI + 1]
        logP = np.log(np.maximum(P, 1e-30))
        for g in range(0, count, group):
            columns.append(10 * np.log10(np.maximum(P[g:g + group].max(axis=0), 1e-30)))

        for r in range(count):
            j = start + r
            pb = band[r]
            if nf is None:
                nf = np.maximum(pb.copy(), 1e-30)
            if j < p["settle_frames"]:
                nf += np.where(pb > nf, up, down) * (pb - nf)
                continue

            ratio = pb / (nf * bias)
            kg = int(np.argmax(ratio))              # the whole band: starts a call, and the broadband test
            kr = kg
            if seg.state != IDLE and p["drop_max_khz"] is not None:
                # in a call: no further down than drop_max below its last point
                lo = max(0, math.ceil((seg.track.last_f - p["drop_max_khz"]) / BIN_KHZ) - K_LO)
                if kg < lo:
                    kr = lo + int(np.argmax(ratio[lo:]))
            kb = kr + K_LO
            snr = 10 * math.log10(ratio[kr]) if ratio[kr] > 0 else -99.0
            # Broadband test (not in the doc; Docs/CLASSIFIER_PLAN.md). A
            # tone or a sweep lifts a few bins of the whitened spectrum; a rub
            # lifts them all. Two counts, each one pass over the bins in C:
            #   - more than half the bins above t_broad (the median is up):
            #     the bench calls reach 2.1 dB, the synthetic rubs 14.6 dB+;
            #   - more than half the bins within t_near of the peak: catches
            #     a rub's rising and falling edges, where the median is still
            #     low but the spectrum is as flat as noise.
            median_trace[j] = 10 * math.log10(max(float(np.median(ratio)), 1e-30))
            raised = p["t_raised_db"] is not None and np.count_nonzero(ratio > raised_level) * 2 > ratio.size
            if p["t_broad_db"] is not None and (np.count_nonzero(ratio > broad) * 2 > ratio.size
                                                or np.count_nonzero(ratio > ratio[kg] * near) * 2 > ratio.size):
                snr = min(snr, p["t_off_db"] - 0.01)    # broadband: can't start or carry a call
            # The bin is picked on the whitened spectrum, but the frequency
            # is the call's own: step to a neighbour with more raw power, or
            # the parabola's offset clamps at +-0.5 bin (sticks at half bins).
            # Not in the doc.
            if kb < K_HI and P[r, kb + 1] > P[r, kb]:
                kb += 1
            elif kb > K_LO and P[r, kb - 1] > P[r, kb]:
                kb -= 1
            a, b, c = logP[r, kb - 1], logP[r, kb], logP[r, kb + 1]
            curve = a - 2 * b + c
            offset = min(max(0.5 * (a - c) / curve, -0.5), 0.5) if curve < 0 else 0.0
            f = (kb + offset) * BIN_KHZ
            harm = 2 * kb <= K_HI and P[r, 2 * kb] > harm_ratio * bias * nf[2 * kb - K_LO]

            idle_before[j] = seg.state == IDLE
            seg.step(j, f, snr, harm, (PT_CLIP if clipped[j] else 0) | (PT_RAISED if raised else 0))
            snr_trace[j] = snr
            state_trace[j] = seg.state

            if seg.state == IDLE:
                nf += np.where(pb > nf, up, down) * (pb - nf)
                idle_power += pb
                nf_sum += nf
                idle_frames += 1

    if seg.state == IN_CALL:
        seg.end_call()

    calls = seg.calls
    for prev, call in zip([None] + calls[:-1], calls):
        call["ipi_ms"] = (call["frame0"] - prev["frame0"]) * HOP_MS if prev else 0.0

    res = Result()
    res.calls = calls
    res.stats = dict(seg.stats)
    res.stats.update(frames=n_frames, seconds=codes.size / SAMPLE_RATE_HZ, idle_frames=idle_frames)
    idle = idle_before
    res.stats["idle_snr_median"] = float(np.median(snr_trace[idle])) if idle.any() else math.nan
    res.stats["idle_over_t_on"] = float(np.mean(snr_trace[idle] >= t_on)) if idle.any() else math.nan
    # where the floor sits against the mean noise power, while idle
    res.stats["floor_vs_mean_db"] = (float(np.median(10 * np.log10(nf_sum / idle_power)))
                                     if idle_frames else math.nan)
    res.snr_trace, res.state_trace, res.median_trace = snr_trace, state_trace, median_trace
    res.spectrogram = np.array(columns).T
    res.column_s = group * HOP / SAMPLE_RATE_HZ
    res.nf = nf
    res.clipped_hops = int(clipped.sum())
    return res


# ---------------------------------------------------------------- report


FIELDS = [
    ("t_s", "t (s)", "{:8.3f}"), ("dur_ms", "dur", "{:5.1f}"), ("f_start", "start", "{:6.1f}"),
    ("f_end", "end", "{:6.1f}"), ("f_peak", "peak", "{:6.1f}"), ("f_char", "char", "{:6.1f}"),
    ("bw", "bw", "{:5.1f}"), ("slope_start", "s1", "{:5.1f}"), ("slope_mid", "s2", "{:5.1f}"),
    ("slope_end", "s3", "{:5.1f}"), ("flat_frac", "flat", "{:4.2f}"), ("snr_db", "snr", "{:4.0f}"),
    ("ipi_ms", "ipi", "{:6.1f}"),
]


def call_table(calls, limit=40):
    lines = ["  " + " ".join(f"{h:>{len(fmt.format(0))}}" for _, h, fmt in FIELDS) + "  h flags  group     label"]
    for c in calls[:limit]:
        flags = "".join(ch for bit, ch in ((CALL_TRUNCATED, "T"), (CALL_GAPPED, "G"), (CALL_CLIPPED, "C"))
                        if c["flags"] & bit)
        label = "echo" if c.get("echo") else f"{c.get('label') or '':<6} {c.get('score', 0.0):.2f}"
        lines.append("  " + " ".join(fmt.format(c[k]) for k, _, fmt in FIELDS)
                     + f"  {'y' if c['harmonic'] else '-'} {flags:<5}  {c.get('group', ''):<9} {label}")
    if len(calls) > limit:
        lines.append(f"  ... {len(calls) - limit} more")
    return "\n".join(lines)


def print_summary(res, p):
    s = res.stats
    minutes = max(s["seconds"] - p["settle_frames"] * HOP / SAMPLE_RATE_HZ, 1e-9) / 60
    print(f"  {s['seconds']:.2f} s, {s['frames']:,} frames; {len(res.calls)} calls ({len(res.calls) / minutes:.1f}/min), "
          f"{s['clicks']} clicks, {s['broadband']} broadband, {s['aborted_arms']} aborted starts, {s['rise_splits']} rise splits"
          + (f", {res.clipped_hops} clipped hops" if res.clipped_hops else ""))
    print(f"  while idle: frame SNR median {s['idle_snr_median']:.1f} dB, {100 * s['idle_over_t_on']:.2f}% of frames "
          f"at or above t_on ({p['t_on_db']:.0f} dB); floor {s['floor_vs_mean_db']:+.1f} dB re the mean noise power")


def sequence_table(sequences):
    lines = []
    for s in sequences:
        if not s.reported:
            continue
        lines.append(f"  {s.start_s:7.2f}-{s.end_s:7.2f} s  {len(s.calls):3d} calls ({s.echoes} echoes)  "
                     f"{s.taxon:<6} {s.name} ({s.level}), confidence {100 * s.confidence:.0f}%, "
                     f"tune {s.tune_khz:.1f} kHz   header \"{species.header_text(s)}\"")
    short = sum(1 for s in sequences if not s.reported)
    if short:
        lines.append(f"  {short} run(s) of fewer than {species.SEQ_MIN_CALLS} calls: not reported (no bat)")
    return "\n".join(lines) if lines else "  no sequences"


# ---------------------------------------------------------------- plot


def style_axes(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=11)
    ax.set_xlabel(xlabel, color=INK_2)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.tick_params(colors=INK_2, labelsize=9)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)


def plot(res, p, path, title, truth=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), facecolor=SURFACE, sharex=True,
                                   gridspec_kw={"height_ratios": [2, 1]})
    fig.suptitle(title, x=0.01, ha="left", color=INK, fontsize=12)

    spec = res.spectrogram
    top = np.percentile(spec[K_LO:K_HI + 1], 99.9)
    seconds = spec.shape[1] * res.column_s
    ax1.imshow(spec, origin="lower", aspect="auto", cmap="Greys", vmin=top - 60, vmax=top,
               extent=(0, seconds, -BIN_KHZ / 2, (spec.shape[0] - 0.5) * BIN_KHZ))
    for c in res.calls:
        ax1.plot(c["track_t"], c["track_f"], color=SERIES_2, linewidth=1.4)
    if truth:
        for t in truth:
            ax1.plot([t["t_s"], t["t_s"]], [t["f_end"] - 3, t["f_start"] + 3], color=SERIES_1,
                     linewidth=0.6, alpha=0.6)
    style_axes(ax1, "128-point frames (dB), detected call tracks in orange"
               + (", true call starts in blue" if truth else ""), "", "kHz")
    ax1.set_ylim(0, 125)

    t = np.arange(res.snr_trace.size) * HOP / SAMPLE_RATE_HZ
    step = max(1, res.snr_trace.size // 20000)
    trace = res.snr_trace[:t.size // step * step].reshape(-1, step).max(axis=1)
    ax2.plot(t[::step][:trace.size], trace, color=INK_2, linewidth=0.6)
    for level, label in ((p["t_on_db"], "t_on"), (p["t_off_db"], "t_off")):
        ax2.axhline(level, color=SERIES_1, linewidth=0.8, linestyle="--")
        ax2.annotate(label, (t[-1], level), xytext=(4, -3), textcoords="offset points", fontsize=8, color=SERIES_1)
    for c in res.calls:
        ax2.axvspan(c["t_s"], c["t_s"] + c["dur_ms"] / 1000, color=SERIES_2, alpha=0.25, linewidth=0)
    style_axes(ax2, "frame SNR: peak of P / floor (dB); calls shaded", "s", "dB")
    ax2.grid(True, color=GRID, linewidth=0.8)

    fig.tight_layout()
    fig.savefig(path, dpi=110, facecolor=SURFACE)
    plt.close(fig)
    print(f"  plot: {path}")


# ---------------------------------------------------------------- self-test


def match_calls(calls, truth):
    """Pair each true call with the first detected call that starts inside it (+-1 ms)"""
    pairs, used = [], set()
    for t in truth:
        for i, c in enumerate(calls):
            if i not in used and t["t_s"] - 1e-3 <= c["t_s"] <= t["t_s"] + t["dur_ms"] / 1000 + 1e-3:
                pairs.append((t, c))
                used.add(i)
                break
    inside = sum(1 for i, c in enumerate(calls) if i not in used and any(
        t["t_s"] - 1e-3 <= c["t_s"] <= t["t_s"] + t["dur_ms"] / 1000 + 1e-3 for t in truth))
    return pairs, len(calls) - len(used) - inside, inside


def median_error(pairs, key):
    return float(np.median([c[key] - t[key] for t, c in pairs])) if pairs else math.nan


# the doc's expectations, per bench signal: (feature, target, tolerance)
EXPECT = {
    "cf82": [("f_peak", 82.0, 0.5), ("dur_ms", 50.0, 1.0)],
    # bw reads 57-63, not the doc's "about 70": a frame's frequency is the
    # centre of the part of the call inside its window, which for the first
    # and last frames is 0.25-0.35 ms inside the call, 6-8 kHz at each end at
    # 23 kHz/ms. Rules on bw must be fitted to this measure.
    "fm100-30": [("bw", 70.0, 2 * 0.35 * 70.0 / 3.0), ("flat_frac", 0.0, 0.2)],
    "pip45": [("f_char", 45.0, 1.0)],
    "noctule": [("f_char", None, 1.0)],         # 20 / 24 alternating: checked against the truth
    "cricket-pip": [("f_char", 45.0, 1.0)],
    "pip45-echo": [("f_char", 45.0, 1.0)],
    "soprano55": [("f_char", 55.0, 1.0)],
    # Sloping tails: f_char is the flattest point of the last 40% of the call,
    # and on a tail of constant slope every point there is equally flat, so it
    # can land anywhere in it. Tolerance: 1 kHz plus the tail's change over
    # that 40% (Leisler's 0.8 kHz, serotine 1.3 kHz). The rules' soft margins
    # are 2 kHz.
    "leisler": [("f_char", None, 1.0 + 0.8)],
    "serotine": [("f_char", 25.0, 1.0 + 1.3)],
    "natterer": [],
}

# stages 4-6: the acceptable verdicts per bench signal; an empty set means
# nothing may be reported (no bat)
EXPECT_ID = {
    "cf82": {"RHIFER"},
    "fm100-30": {"MYOSP"},          # ends near 30 kHz: no Myotis species has a rule there
    "pip45": {"PIPPIP"},
    "noctule": {"NYCNOC"},
    "cricket-pip": {"PIPPIP"},
    "pip45-echo": {"PIPPIP"},
    "noise": set(),
    "soprano55": {"PIPPYG"},
    "leisler": {"NYCLEI"},
    "serotine": {"EPTSER", "NYCGRP"},   # the doc: often group only
    "natterer": {"MYONAT", "MYOSP"},
    "rub-flat": set(),
    "rub-tilt": set(),
}


def selftest(p, plot_dir=None):
    import bat_signals

    noise = bat_signals.NoiseModel(bat_signals.DEFAULT_NOISE)
    print(f"Noise: {noise.source}, {noise.rms_above(10_000):.1f} codes rms above 10 kHz; calls at +20 dB re that\n")

    print(f"  {'signal':<12} {'calls':>5} {'found':>5} {'extra':>5} {'split':>5}   median error: "
          f"{'dur':>5} {'start':>6} {'end':>6} {'char':>6} {'peak':>6}   check")
    all_passed = True
    id_lines = []
    for s in bat_signals.bench_signals(noise):
        res = analyse(s.codes(), p)
        sequences = species.identify(res.calls)
        pairs, extra, split = match_calls(res.calls, s.calls)
        verdicts = []
        for key, target, tol in EXPECT.get(s.name, []):
            values = np.array([c[key] for _, c in pairs])
            targets = np.array([t[key] for t, _ in pairs]) if target is None else target
            ok = values.size > 0 and bool(np.all(np.abs(values - targets) <= tol))
            verdicts.append(f"{key} {'ok' if ok else 'FAIL'}")
            all_passed &= ok
        if s.calls:
            found_all = len(pairs) == len(s.calls)
            verdicts.insert(0, "all found" if found_all else "MISSED")
            all_passed &= found_all
        minutes = (s.x.size / bat_signals.SAMPLE_RATE_HZ - p["settle_frames"] * HOP / SAMPLE_RATE_HZ) / 60
        if not s.calls:
            verdicts.append(f"{extra / minutes:.1f} false calls/min")
        print(f"  {s.name:<12} {len(s.calls):5d} {len(pairs):5d} {extra:5d} {split:5d}   {'':14}"
              + " ".join(f"{median_error(pairs, k):+6.2f}" for k in ("dur_ms", "f_start", "f_end", "f_char", "f_peak"))
              + "   " + ", ".join(verdicts))
        if plot_dir:
            plot(res, p, plot_dir / f"selftest_{s.name}.png", f"{s.name}: {s.description}", s.calls)

        reported = [q for q in sequences if q.reported]
        verdicts_id = sorted({q.taxon for q in reported})
        expected = EXPECT_ID.get(s.name)
        if expected is not None:
            ok = (not reported) if not expected else (len(reported) == 1 and reported[0].taxon in expected)
            all_passed &= ok
            got = ", ".join(f"{q.taxon} {100 * q.confidence:.0f}% ({len(q.calls)} calls, {q.echoes} echoes)"
                            for q in reported) or "nothing reported"
            want = " or ".join(sorted(expected)) or "nothing"
            id_lines.append(f"  {s.name:<12} {got:<44} want {want:<16} {'ok' if ok else 'FAIL'}")

    print("\n  Species (stages 4-6):")
    print("\n".join(id_lines))

    print("\n  pip45 at lower levels (dB re noise rms above 10 kHz):")
    for s in bat_signals.level_sweep(noise, [-10, -5, 0, 5, 10]):
        res = analyse(s.codes(), p)
        pairs, extra, split = match_calls(res.calls, s.calls)
        char_ok = sum(abs(c["f_char"] - 45.0) <= 1.0 for _, c in pairs)
        print(f"    {s.name:<14} found {len(pairs):2d}/{len(s.calls)}, f_char within 1 kHz {char_ok:2d}, "
              f"extra {extra}, median snr {np.median([c['snr_db'] for _, c in pairs]) if pairs else math.nan:4.1f} dB")

    print("\n  PASS" if all_passed else "\n  FAIL")
    return all_passed


# ---------------------------------------------------------------- main


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", type=Path)
    parser.add_argument("--selftest", action="store_true", help="run the bench signals")
    parser.add_argument("--plot", action="store_true", help="write a PNG next to each file")
    parser.add_argument("--doc-params", action="store_true", help="the doc's parameters exactly")
    parser.add_argument("--all-calls", action="store_true", help="print every call, not the first 40")
    args = parser.parse_args()

    p = DOC_PARAMS if args.doc_params else PARAMS
    if args.selftest:
        plot_dir = None
        if args.plot:
            plot_dir = Path("Captures/synth")
            plot_dir.mkdir(parents=True, exist_ok=True)
        return 0 if selftest(p, plot_dir) else 1

    if not args.files:
        parser.error("give files, or --selftest")
    # Windows shells pass wildcards through: expand D:\BAT*.WAV here
    files = [Path(f) for arg in args.files
             for f in (sorted(glob.glob(str(arg))) if glob.has_magic(str(arg)) else [arg])]
    for path in files:
        codes, note = load_codes(path)
        print(f"{path}: {note}")
        res = analyse(codes, p)
        sequences = species.identify(res.calls)
        print_summary(res, p)
        if res.calls:
            print(call_table(res.calls, limit=10**9 if args.all_calls else 40))
        print("  sequences:")
        print(sequence_table(sequences))
        if args.plot:
            plot(res, p, path.with_suffix(".png"), path.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
