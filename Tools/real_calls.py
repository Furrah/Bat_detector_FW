"""
real_calls.py - scores the classifier model (classify.py + species.py) on
real recordings: the Zenodo "Bat recordings split data set" (record
3247097, CC-BY 4.0, M. Bertran Ferrer), common and soprano pipistrelles
recorded at 256 kHz.

The dataset cut each 400 ms stretch of its recordings into alternating
pieces, call / gap / call ..., one WAV each, named by recording, fragment
and piece number. Put back in order the pieces cover 97-99% of each
fragment (a few ms are missing at the joins), and with the whole silent
fragments they rebuild the original recordings. Each unbroken run of
fragments is scored as one recording: real audio, nothing synthetic.

  - The joins are cross-faded over 0.1 ms, so the missing samples don't click.
  - The classifier learns its noise floor over the first 0.52 s. Each run is
    preceded by a copy of its own first 0.6 s (real background, the same
    recording), and nothing found in that copy is scored.
  - Runs are resampled to 250 kHz and scaled to 12-bit codes, as classify.py
    does with any 16-bit WAV.

    python -I Tools/real_calls.py <Bat_recordings.zip> [--min-seconds 1.2] [--write-wavs DIR]
    python -I Tools/real_calls.py <Bat_recordings.zip> --validate

--validate fits the one number the textbook rules get wrong here, the
common/soprano boundary on the characteristic frequency, on half the nights,
and scores it on the other half against the textbook rules, both ways round.
Splitting by night, not by recording, keeps the same bat out of both halves.

The zip isn't extracted: members are read from it directly. --write-wavs
saves each rebuilt run as a 256 kHz WAV, to listen to or to run
classify.py --plot on.

Needs: python -m pip install numpy
"""

import argparse
import collections
import copy
import io
import re
import sys
import wave
import zipfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))    # -I leaves the script's folder off the path

import classify  # noqa: E402
import species  # noqa: E402

RATE = 256_000                  # the dataset
PIECE = re.compile(r"(PIPI|PIPY)_(\d{8})_(\d{6})-(\d+)_(\d+)_(\d+)_([01])_(\d+)_B_\.wav$")
WHOLE = re.compile(r"S-(PIPI|PIPY)_(\d{8})_(\d{6})-(\d+)\.wav$")
TRUTH = {"PIPI": "PIPPIP", "PIPY": "PIPPYG"}
JOIN_FADE = int(0.1e-3 * RATE)
WARMUP_S = 0.6


def read_wav(z, name):
    with wave.open(io.BytesIO(z.read(name))) as w:
        if w.getframerate() != RATE or w.getsampwidth() != 2 or w.getnchannels() != 1:
            raise ValueError(f"{name}: unexpected format")
        return np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64)


def join(parts):
    """Concatenate, cross-fading each join over JOIN_FADE samples"""
    out = parts[0].copy()
    for p in parts[1:]:
        n = min(JOIN_FADE, out.size, p.size)
        if n == 0:
            out = np.concatenate([out, p])
            continue
        w = 0.5 * (1 - np.cos(np.pi * np.arange(n) / n))
        out[-n:] = out[-n:] * (1 - w) + p[:n] * w
        out = np.concatenate([out, p[n:]])
    return out


def index(z):
    """{(species, day, time): {fragment: [(piece number, member name)]}}"""
    recordings = collections.defaultdict(lambda: collections.defaultdict(list))
    for info in z.infolist():
        name = info.filename
        if "__MACOSX" in name or not name.lower().endswith(".wav"):
            continue
        base = name.rsplit("/", 1)[-1]
        m = PIECE.search(base)
        if m:
            sp, day, t, frag, _start, _power, _is_call, idx = m.groups()
            recordings[(sp, day, t)][int(frag)].append((int(idx), name))
            continue
        m = WHOLE.search(base)
        if m:
            sp, day, t, frag = m.groups()
            recordings[(sp, day, t)][int(frag)].append((0, name))
    return recordings


def runs(fragments):
    """Unbroken runs of consecutive fragment numbers"""
    numbers = sorted(fragments)
    run = [numbers[0]]
    for f in numbers[1:]:
        if f == run[-1] + 1:
            run.append(f)
        else:
            yield run
            run = [f]
    yield run


def to_codes(x):
    y = classify.resample(x, RATE)
    return np.clip(np.round(y / 16) + classify.ADC_MIDSCALE, 0, classify.ADC_MAX).astype(np.int64)


def write_wav(path, x):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.clip(np.round(x), -32768, 32767).astype("<i2").tobytes())


def best_split(a, b):
    """The single threshold that best separates a (below) from b (above): (balanced accuracy, kHz)"""
    best = (0.0, None)
    for t in np.arange(40.0, 60.01, 0.1):
        acc = (np.mean(a < t) + np.mean(b >= t)) / 2
        if acc > best[0]:
            best = (acc, t)
    return best


# Two halves by night, each with both species (see the docstring)
HALVES = {"A": {"20160607", "20160609", "20160820", "20160920"},
          "B": {"20160821", "20160825", "20160919"}}


def analysed_runs(z, recordings, min_seconds):
    """Stages 1-3 for every run, once: [(species, night, calls)]"""
    items = []
    for key in sorted(recordings):
        for run in runs(recordings[key]):
            if len(run) * 0.4 < min_seconds - 1e-9:
                continue
            x = join([join([read_wav(z, n) for _, n in sorted(recordings[key][f])]) for f in run])
            x -= x.mean()
            warm = int(WARMUP_S * RATE)
            res = classify.analyse(to_codes(join([x[:warm], x])), classify.PARAMS)
            items.append((key[0], key[1], [c for c in res.calls if c["t_s"] >= WARMUP_S]))
    return items


def with_boundary(rules, boundary):
    """The rule table with the common/soprano boundary moved to boundary kHz"""
    out = []
    for taxon, group, ranges, dur_mean, regions in rules:
        if taxon == "PIPPIP":
            ranges = [("f_char", ranges[0][1], boundary)]
        elif taxon == "PIPPYG":
            ranges = [("f_char", boundary, ranges[0][2])]
        out.append((taxon, group, ranges, dur_mean, regions))
    return out


def score(items, rules):
    """Stages 4-6 with a rule table: per species, Counter of verdicts"""
    saved = species.RULES
    species.RULES = rules
    try:
        verdicts = {sp: collections.Counter() for sp in TRUTH}
        for sp, _night, calls in items:
            seqs = [s for s in species.identify(copy.deepcopy(calls)) if s.reported]
            verdicts[sp][max(seqs, key=lambda s: len(s.calls)).taxon if seqs else "nothing"] += 1
        return verdicts
    finally:
        species.RULES = saved


def fit_boundary(items):
    """The f_char threshold that best splits the two species' QCF calls"""
    f = {sp: [] for sp in TRUTH}
    for sp, _night, calls in items:
        tagged = copy.deepcopy(calls)
        species.identify(tagged)
        f[sp] += [c["f_char"] for c in tagged if not c.get("echo") and c.get("group") == species.QCF
                  and 35.0 < c["f_char"] < 60.0]
    return best_split(np.array(f["PIPI"]), np.array(f["PIPY"]))


def summary(verdicts):
    lines = []
    for sp, truth in TRUTH.items():
        v = verdicts[sp]
        total = sum(v.values())
        right = v[truth]
        wrong = sum(n for k, n in v.items() if k != truth and k in species.TAXA and species.TAXA[k][2] == species.SPECIES)
        group = sum(n for k, n in v.items() if k in species.TAXA and species.TAXA[k][2] == species.GROUP)
        other = total - right - wrong - group
        lines.append(f"{species.TAXA[truth][0]:<20} {total:4d}   right {right:3d} ({100 * right / max(total, 1):3.0f}%)   "
                     f"wrong species {wrong:3d} ({100 * wrong / max(total, 1):3.0f}%)   group {group:3d}   Bat sp./nothing {other:3d}")
    return lines


def validate(z, recordings, min_seconds):
    print("Analysing every run (stages 1-3, once)...")
    items = analysed_runs(z, recordings, min_seconds)
    halves = {h: [it for it in items if it[1] in nights] for h, nights in HALVES.items()}
    for h, its in halves.items():
        n = collections.Counter(sp for sp, _, _ in its)
        print(f"  half {h}: nights {', '.join(sorted(HALVES[h]))}: {n['PIPI']} common, {n['PIPY']} soprano")
    print()
    for train, test in (("A", "B"), ("B", "A")):
        acc, boundary = fit_boundary(halves[train])
        print(f"Fit on {train}: boundary {boundary:.1f} kHz ({100 * acc:.0f}% of {train}'s calls on the right side, balanced). "
              f"Tested on {test}:")
        print("  textbook rules (common 42-49, soprano 53-58 kHz):")
        for line in summary(score(halves[test], species.RULES)):
            print("    " + line)
        print(f"  fitted (common 42-{boundary:.1f}, soprano {boundary:.1f}-58 kHz):")
        for line in summary(score(halves[test], with_boundary(species.RULES, boundary))):
            print("    " + line)
        print()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("zip", type=Path)
    parser.add_argument("--min-seconds", type=float, default=1.2, help="skip shorter runs")
    parser.add_argument("--write-wavs", type=Path, help="save each rebuilt run here")
    parser.add_argument("--validate", action="store_true", help="fit the boundary on half the nights, test on the rest")
    args = parser.parse_args()
    if args.validate:
        z = zipfile.ZipFile(args.zip)
        validate(z, index(z), args.min_seconds)
        return 0
    if args.write_wavs:
        args.write_wavs.mkdir(parents=True, exist_ok=True)

    z = zipfile.ZipFile(args.zip)
    recordings = index(z)

    verdicts = {sp: collections.Counter() for sp in TRUTH}
    labels = {sp: collections.Counter() for sp in TRUTH}
    per_call = {sp: collections.defaultdict(list) for sp in TRUTH}
    per_run = {sp: collections.defaultdict(list) for sp in TRUTH}
    seconds = collections.Counter()
    n_runs = collections.Counter()

    for key in sorted(recordings):
        sp = key[0]
        for run in runs(recordings[key]):
            if len(run) * 0.4 < args.min_seconds - 1e-9:
                continue
            fragments = [join([read_wav(z, n) for _, n in sorted(recordings[key][f])]) for f in run]
            x = join(fragments)
            x -= x.mean()
            if args.write_wavs:
                write_wav(args.write_wavs / f"{sp}_{key[1]}_{key[2]}_{run[0]:03d}-{run[-1]:03d}.wav", x)
            warm = int(WARMUP_S * RATE)
            res = classify.analyse(to_codes(join([x[:warm], x])), classify.PARAMS)
            calls = [c for c in res.calls if c["t_s"] >= WARMUP_S]
            sequences = [s for s in species.identify(calls) if s.reported]

            n_runs[sp] += 1
            seconds[sp] += x.size / RATE
            verdicts[sp][max(sequences, key=lambda s: len(s.calls)).taxon if sequences else "nothing"] += 1
            qcf = []
            for c in calls:
                if c.get("echo") or c.get("label") is None:
                    continue
                labels[sp][c["label"]] += 1
                if c["group"] == species.QCF and c["f_char"] > 35.0:
                    qcf.append(c)
            for feat in ("f_char", "f_peak", "f_end"):
                values = [c[feat] for c in qcf]
                per_call[sp][feat] += values
                if len(values) >= 3:
                    per_run[sp][feat].append(float(np.median(values)))

    print(f"Zenodo 3247097, rebuilt recordings (runs of {args.min_seconds:.1f} s or more): "
          f"{n_runs['PIPI']} common ({seconds['PIPI']:.0f} s), {n_runs['PIPY']} soprano ({seconds['PIPY']:.0f} s)\n")
    for sp, truth in TRUTH.items():
        total = sum(verdicts[sp].values())
        if not total:
            continue
        right = verdicts[sp][truth]
        wrong_species = sum(v for k, v in verdicts[sp].items()
                            if k != truth and k in species.TAXA and species.TAXA[k][2] == species.SPECIES)
        print(f"{species.TAXA[truth][0]} ({sp}): {total} recordings")
        print(f"  verdict: {right}/{total} right ({100 * right / total:.0f}%), "
              f"{wrong_species} wrong species ({100 * wrong_species / total:.0f}%)   "
              + ", ".join(f"{k} {v}" for k, v in verdicts[sp].most_common()))
        n = sum(labels[sp].values())
        print(f"  per-call labels ({n} calls): " + ", ".join(f"{k} {100 * v / n:.0f}%" for k, v in labels[sp].most_common(7)))
        print()

    print("Separating the two by one frequency (QCF calls above 35 kHz):")
    for level, data in (("per call", per_call), ("per recording (median of its calls)", per_run)):
        print(f"  {level}:")
        for feat in ("f_char", "f_peak", "f_end"):
            a, b = np.array(data["PIPI"][feat]), np.array(data["PIPY"][feat])
            if a.size < 3 or b.size < 3:
                continue
            acc, t = best_split(a, b)
            qa, qb = np.percentile(a, [10, 50, 90]), np.percentile(b, [10, 50, 90])
            print(f"    {feat:<7} common {qa[0]:5.1f} /{qa[1]:5.1f} /{qa[2]:5.1f}   soprano {qb[0]:5.1f} /{qb[1]:5.1f} /{qb[2]:5.1f} kHz"
                  f" (10/50/90%)   best split {t:.1f} kHz: {100 * acc:.0f}% balanced accuracy (n {a.size}/{b.size})")


if __name__ == "__main__":
    sys.exit(main())
