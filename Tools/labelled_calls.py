"""
labelled_calls.py - score the classifier model (classify.py, species.py) on
recordings labelled with their species in the file name (diary 50):

  xeno-canto            "XC1030145 - Soprano Pipistrelle - Pipistrellus pygmaeus.wav"
                        (the original WAVs, 192 kHz, downloaded by browser)
  Biodiversity Ireland  "FullSpectrum_Pipistrellus_pygmaeus.wav" (256-312.5 kHz)

Keep them under Captures/ (gitignored). Each file is resampled to 250 kHz and
scaled to 12-bit codes (classify.load_codes), with its own first 0.6 s
copied in front so the noise floor settles; that copy isn't scored. The
verdict is the taxon of the file's longest reported sequence. Byte-identical
copies are skipped.

    python -I Tools/labelled_calls.py Captures/Xeno_canto Captures/biodiversotyireland [--plot DIR]

A verdict is "right" if it's the species (or the best the rules can name: a
Myotis with no species rule is right as "Myotis sp."), "group" if it's a
group the species belongs to, "wrong" if it names something else, and
"Bat sp." if no more than a bat.
"""

import argparse
import collections
import hashlib
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))    # -I leaves the script's folder off the path

import classify  # noqa: E402
import species  # noqa: E402

WARMUP_S = 0.6

# Latin name -> the taxon a right answer names
TRUTH = {
    "Pipistrellus pipistrellus": "PIPPIP",
    "Pipistrellus pygmaeus": "PIPPYG",
    "Pipistrellus nathusii": "PIPKN",
    "Pipistrellus kuhlii": "PIPKN",
    "Eptesicus serotinus": "EPTSER",
    "Nyctalus noctula": "NYCNOC",
    "Nyctalus leisleri": "NYCLEI",
    "Myotis daubentonii": "MYOSP",      # no species rule: Myotis sp. is the best possible
    "Myotis mystacinus": "MYOSP",
    "Myotis brandtii": "MYOSP",
    "Myotis nattereri": "MYONAT",
    "Plecotus auritus": "PLESP",
    "Plecotus austriacus": "PLESP",
    "Barbastella barbastellus": "BARBAR",
    "Rhinolophus hipposideros": "RHIHI",
    "Rhinolophus ferrumequinum": "RHIFER",
}


def latin(path):
    m = re.match(r"XC\d+ - .+ - ([A-Z][a-z]+ [a-z]+)", path.stem)
    if m:
        return m.group(1)
    m = re.match(r"FullSpectrum_([A-Z][a-z]+)_([a-z]+)$", path.stem)
    return f"{m.group(1)} {m.group(2)}" if m else None


def outcome(truth, verdict):
    if verdict is None:
        return "nothing"
    if verdict == truth:
        return "right"
    if verdict == "BATSP":
        return "Bat sp."
    return "group" if verdict in species.ancestors(truth) else "wrong"


def median(values):
    return float(np.median(values)) if len(values) else float("nan")


def score_file(path, plot_dir=None):
    codes, note = classify.load_codes(path)
    warm = int(WARMUP_S * classify.SAMPLE_RATE_HZ)
    res = classify.analyse(np.concatenate([codes[:warm], codes]), classify.PARAMS)
    calls = [c for c in res.calls if c["t_s"] >= WARMUP_S]
    sequences = [s for s in species.identify(calls) if s.reported]
    verdict = max(sequences, key=lambda s: len(s.calls)).taxon if sequences else None
    if plot_dir:
        classify.plot(res, classify.PARAMS, plot_dir / (path.stem[:40].replace(" ", "_") + ".png"), path.stem[:60])
    return note, res, calls, sequences, verdict


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="+", type=Path, help="folders or WAVs")
    parser.add_argument("--plot", type=Path, help="write a PNG per file here")
    args = parser.parse_args()
    if args.plot:
        args.plot.mkdir(parents=True, exist_ok=True)

    files = [f for p in args.paths for f in (sorted(p.glob("*.wav")) if p.is_dir() else [p])]
    seen, rows, band = set(), [], []
    for path in files:
        name = latin(path)
        if name not in TRUTH:
            print(f"skip {path.name}: no known species in the name")
            continue
        digest = hashlib.md5(path.read_bytes()).hexdigest()
        if digest in seen:
            print(f"skip {path.name}: a copy")
            continue
        seen.add(digest)
        truth = TRUTH[name]
        note, res, calls, sequences, verdict = score_file(path, args.plot)
        result = outcome(truth, verdict)
        rows.append((name, result, verdict))

        real = [c for c in calls if not c.get("echo") and c.get("label")]
        labels = collections.Counter(c["label"] for c in real)
        pip = [c["f_char"] for c in real if c["group"] == species.QCF and 35.0 <= c["f_char"] <= 65.0]
        if name.startswith("Pipistrellus"):
            band.append((truth, path.stem, pip))
        print(f"\n{path.name}  ({note}; truth {truth})")
        print(f"  {len(real)} calls, {res.stats['broadband']} broadband, {res.stats['clicks']} clicks; labels "
              + ", ".join(f"{k} {100 * v / len(real):.0f}%" for k, v in labels.most_common(5)))
        for s in sequences:
            print(f"    {s.start_s - WARMUP_S:7.2f}-{s.end_s - WARMUP_S:7.2f} s {len(s.calls):4d} calls  "
                  f"{s.taxon:<6} {100 * s.confidence:3.0f}%  {s.name}")
        print(f"  verdict {verdict or '-'}: {result}")

    print("\nPipistrelle-band calls (QCF, f_char 35-65 kHz), median per file:")
    for truth, stem, f in sorted(band, key=lambda b: median(b[2]) if b[2] else 0):
        print(f"  {truth:<6} {median(f):5.1f} kHz  ({len(f):4d} calls)  {stem[:60]}")

    print("\nSummary (verdict = the longest reported sequence)")
    kinds = ("right", "group", "Bat sp.", "wrong", "nothing")
    print(f"  {'species':<26} {'files':>5} " + " ".join(f"{k:>8}" for k in kinds))
    by_species = collections.defaultdict(collections.Counter)
    for name, result, _ in rows:
        by_species[name][result] += 1
    total = collections.Counter()
    for name, c in by_species.items():
        total += c
        print(f"  {name:<26} {sum(c.values()):5d} " + " ".join(f"{c[k]:8d}" for k in kinds))
    print(f"  {'all':<26} {sum(total.values()):5d} " + " ".join(f"{total[k]:8d}" for k in kinds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
