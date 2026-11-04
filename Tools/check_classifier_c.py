"""
check_classifier_c.py - checks the firmware's classifier (Core/DSP/
classifier_calls.c, classifier_species.c) against the PC model
(classify.py, species.py) on the same audio, call by call.

The C is built for the PC with gcc (MSYS2's is found at
C:/msys64/ucrt64/bin if it isn't on PATH) and run through
Tools/classifier_host/host_main.c, which prints the board's call-log CSV.
Each WAV is turned into 12-bit codes exactly as classify.py does, with its
first 0.6 s copied in front (as labelled_calls.py), and both see those codes.

    python -I Tools/check_classifier_c.py Captures/Xeno_canto Captures/biodiversotyireland
    python -I Tools/check_classifier_c.py --bench
    python -I Tools/check_classifier_c.py --device D:\\CALLS.CSV D:\\BAT00001.WAV

The C runs in float32 and the model in float64, so a call at a threshold
can go either way; the check reports how often that happens.

--device checks the board itself: its call log against the model on a
recording it made of the same pass (CLASSIFIER_PLAN.md, step 1). The board
must log every call for this: CALL_LOG_CALLS 1 in Core/Inc/call_log.h. The log
gives ring positions and the WAV doesn't, so they're lined up by the most
common offset between calls of the same frequency. The board's session
started up to 2 s before the WAV, with its own noise-floor history, so
expect most calls to match rather than all.
"""

import argparse
import collections
import csv
import io
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import classify  # noqa: E402
import species  # noqa: E402

WARMUP_S = 0.6
SOURCES = ["Core/DSP/classifier_calls.c", "Core/DSP/classifier_species.c", "Core/DSP/Tables/dsp_tables.c",
           "Tools/classifier_host/host_main.c"]
FEATURES = ["dur_ms", "f_start", "f_end", "f_peak", "f_char", "snr_db"]


def find_gcc(given):
    for candidate in (given, shutil.which("gcc"), "C:/msys64/ucrt64/bin/gcc.exe"):
        if candidate and Path(candidate).exists():
            return str(candidate)
    raise SystemExit("gcc not found: install it (MSYS2) or pass --gcc")


def build(gcc, out_dir):
    exe = out_dir / "classifier_host.exe"
    cmd = [gcc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-I", str(ROOT / "Core/DSP"), "-I", str(ROOT / "Core/Inc")]
    cmd += [str(ROOT / s) for s in SOURCES] + ["-lm", "-o", str(exe)]
    env = None
    if Path(gcc).parent.name == "bin":          # MSYS2's gcc needs its own DLLs on PATH
        import os
        env = dict(os.environ, PATH=str(Path(gcc).parent) + os.pathsep + os.environ.get("PATH", ""))
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0 or result.stderr.strip():
        print(result.stderr)
        if result.returncode != 0:
            raise SystemExit("build failed")
    return exe, env


def run_c(exe, env, codes, tmp):
    path = tmp / "codes.u16"
    codes.astype("<u2").tofile(path)
    out = subprocess.run([str(exe), str(path)], capture_output=True, text=True, env=env, check=True).stdout
    rows = [r for r in csv.reader(io.StringIO(out)) if r][1:]     # Windows text mode doubles the \r
    calls = [{"hop": int(r[1]), "dur_ms": float(r[2]), "f_start": float(r[3]), "f_end": float(r[4]),
              "f_peak": float(r[5]), "f_char": float(r[6]), "snr_db": float(r[12]), "group": r[15],
              "label": r[16] or None, "echo": r[18] == "1"} for r in rows if r[0] == "C"]
    seqs = [{"first": int(r[1]), "last": int(r[2]), "calls": int(r[3]), "taxon": r[5], "confidence": float(r[6]),
             "reported": r[7] == "1"} for r in rows if r[0] == "P"]
    return calls, seqs


def run_model(codes):
    res = classify.analyse(codes, classify.PARAMS)
    sequences = species.identify(res.calls)
    calls = [{"hop": c["frame0"], **{k: c[k] for k in FEATURES}, "group": c["group"],
              "label": c.get("label"), "echo": bool(c.get("echo"))} for c in res.calls]
    seqs = [{"first": s.calls[0]["frame0"], "last": s.calls[-1]["frame0"], "calls": len(s.calls), "taxon": s.taxon,
             "confidence": s.confidence, "reported": s.reported} for s in sequences]
    return calls, seqs


def compare(model_calls, c_calls, model_seqs, c_seqs, stats):
    by_hop = {}
    for c in c_calls:
        by_hop.setdefault(c["hop"], c)
    used = set()
    for m in model_calls:
        match = next((by_hop[h] for h in (m["hop"], m["hop"] - 1, m["hop"] + 1)
                      if h in by_hop and id(by_hop[h]) not in used), None)
        if match is None:
            stats["model only"] += 1
            continue
        used.add(id(match))
        stats["matched"] += 1
        for k in FEATURES:
            stats["diff " + k].append(abs(m[k] - match[k]))
        stats["same group"] += m["group"] == match["group"]
        stats["same label"] += (m["label"] or "") == (match["label"] or "") and m["echo"] == match["echo"]
    stats["C only"] += len(c_calls) - len(used)

    reported_model = [s for s in model_seqs if s["reported"]]
    reported_c = [s for s in c_seqs if s["reported"]]
    stats["sequences"] += len(reported_model)
    for m in reported_model:
        match = next((c for c in reported_c if abs(c["first"] - m["first"]) <= 1), None)
        if match is not None and match["taxon"] == m["taxon"] and match["calls"] == m["calls"]:
            stats["same verdict"] += 1
            stats["diff confidence"].append(abs(match["confidence"] - m["confidence"]))
    final_m = max(reported_model, key=lambda s: s["calls"])["taxon"] if reported_model else "-"
    final_c = max(reported_c, key=lambda s: s["calls"])["taxon"] if reported_c else "-"
    return final_m, final_c


def read_log(path):
    """The board's CALLS.CSV: (calls, sequences) as run_c gives them"""
    rows = [r for r in csv.reader(path.read_text(encoding="ascii", errors="replace").splitlines()) if r]
    calls = [{"hop": int(r[1]), "dur_ms": float(r[2]), "f_start": float(r[3]), "f_end": float(r[4]),
              "f_peak": float(r[5]), "f_char": float(r[6]), "snr_db": float(r[12]), "group": r[15],
              "label": r[16] or None, "echo": r[18] == "1"} for r in rows if r[0] == "C" and len(r) >= 19]
    seqs = [{"first": int(r[1]), "last": int(r[2]), "calls": int(r[3]), "taxon": r[5], "confidence": float(r[6]),
             "reported": r[7] == "1"} for r in rows if r[0] == "P" and len(r) >= 8]
    return calls, seqs


def device_check(log_path, wav_path):
    board_calls, board_seqs = read_log(log_path)
    codes, note = classify.load_codes(wav_path)
    warm = int(WARMUP_S * classify.SAMPLE_RATE_HZ)
    warm_hops = warm // classify.HOP
    model_calls, model_seqs = run_model(np.concatenate([codes[:warm], codes]))
    model_calls = [c for c in model_calls if c["hop"] >= warm_hops]
    if not board_calls or not model_calls:
        raise SystemExit(f"no calls to compare: {len(board_calls)} in the log, {len(model_calls)} in the model")

    # the offset most calls of the same frequency agree on
    hb = np.array([c["hop"] for c in board_calls], dtype=np.int64)
    fb = np.array([c["f_char"] for c in board_calls])
    offsets = collections.Counter()
    for m in model_calls:
        near = np.abs(fb - m["f_char"]) < 0.5
        offsets.update((hb[near] - m["hop"]).tolist())
    offset = offsets.most_common(1)[0][0]

    span = (model_calls[0]["hop"] + offset - 2, model_calls[-1]["hop"] + offset + 2)
    board = [dict(c, hop=c["hop"] - offset) for c in board_calls if span[0] <= c["hop"] <= span[1]]

    stats = collections.defaultdict(int)
    for k in FEATURES + ["confidence"]:
        stats["diff " + k] = []
    compare(model_calls, board, [], [], stats)

    # sequences: the two sessions started at different points, so pair them by overlap, not by first call
    model_rep = [s for s in model_seqs if s["reported"] and s["last"] >= warm_hops]
    board_rep = [dict(s, first=s["first"] - offset, last=s["last"] - offset) for s in board_seqs if s["reported"]]
    for m in model_rep:
        b = next((b for b in board_rep if b["first"] <= m["last"] and b["last"] >= m["first"]), None)
        stats["sequences"] += 1
        stats["same verdict"] += b is not None and b["taxon"] == m["taxon"]
    final_m = max(model_rep, key=lambda s: s["calls"])["taxon"] if model_rep else "-"
    overlapping = [b for b in board_rep if b["last"] >= span[0] - offset and b["first"] <= span[1] - offset]
    final_b = max(overlapping, key=lambda s: s["calls"])["taxon"] if overlapping else "-"
    total = stats["matched"] + stats["model only"]
    print(f"{wav_path.name} ({note}) against {log_path.name}: ring offset {offset} hops")
    print(f"calls: {stats['matched']} matched, {stats['model only']} only in the model, {stats['C only']} only on "
          f"the board ({100 * stats['matched'] / max(total, 1):.1f}% of the model's)")
    print(f"same label {100 * stats['same label'] / max(stats['matched'], 1):.1f}% of matched calls")
    for k in FEATURES:
        d = np.array(stats["diff " + k])
        if d.size:
            print(f"  {k:<8} |board - model|: median {np.median(d):.3f}, max {d.max():.3f}")
    print(f"reported sequences: {stats['same verdict']}/{stats['sequences']} the same; "
          f"longest: model {final_m}, board {final_b}")
    return 0


def inputs(args):
    if args.bench:
        import bat_signals
        noise = bat_signals.NoiseModel(bat_signals.DEFAULT_NOISE)
        for s in bat_signals.bench_signals(noise):
            yield f"bench {s.name}", s.codes()
    for p in args.paths:
        for path in (sorted(p.glob("*.wav")) if p.is_dir() else [p]):
            codes, _ = classify.load_codes(path)
            warm = int(WARMUP_S * classify.SAMPLE_RATE_HZ)
            yield path.name, np.concatenate([codes[:warm], codes])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", type=Path, help="WAVs or folders of WAVs")
    parser.add_argument("--bench", action="store_true", help="also the self-test's bench signals")
    parser.add_argument("--gcc", help="path to gcc")
    parser.add_argument("--device", type=Path, metavar="CALLS.CSV", help="the board's log, against one WAV it recorded")
    args = parser.parse_args()
    if args.device:
        if len(args.paths) != 1:
            parser.error("--device takes the log and one WAV")
        return device_check(args.device, args.paths[0])
    if not args.paths and not args.bench:
        parser.error("give WAVs or folders, or --bench")

    stats = collections.defaultdict(int)
    for k in FEATURES + ["confidence"]:
        stats["diff " + k] = []
    files = verdicts_same = 0
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        exe, env = build(find_gcc(args.gcc), tmp)
        seen = set()
        for name, codes in inputs(args):
            key = hash(codes.tobytes())
            if key in seen:
                continue
            seen.add(key)
            model_calls, model_seqs = run_model(codes)
            c_calls, c_seqs = run_c(exe, env, codes, tmp)
            before = dict(matched=stats["matched"], same=stats["same label"])
            final_m, final_c = compare(model_calls, c_calls, model_seqs, c_seqs, stats)
            files += 1
            verdicts_same += final_m == final_c
            n = stats["matched"] - before["matched"]
            print(f"{name[:58]:<58} calls {len(model_calls):5d} model / {len(c_calls):5d} C, "
                  f"labels same {stats['same label'] - before['same']:5d}/{n:<5d} verdict {final_m:<6} / {final_c:<6}"
                  f"{'' if final_m == final_c else '  DIFFERENT'}")

    total = stats["matched"] + stats["model only"]
    print(f"\n{files} recordings")
    print(f"calls: {stats['matched']} matched within a frame, {stats['model only']} only in the model, "
          f"{stats['C only']} only in the C ({100 * stats['matched'] / max(total, 1):.2f}% of the model's)")
    print(f"same shape group {100 * stats['same group'] / max(stats['matched'], 1):.2f}%, "
          f"same label {100 * stats['same label'] / max(stats['matched'], 1):.2f}% of matched calls")
    for k in FEATURES:
        d = np.array(stats["diff " + k])
        if d.size:
            print(f"  {k:<8} |C - model|: median {np.median(d):.3f}, 99% {np.percentile(d, 99):.3f}, max {d.max():.3f}")
    print(f"reported sequences: {stats['same verdict']}/{stats['sequences']} with the same taxon and call count")
    print(f"file verdicts (longest sequence): {verdicts_same}/{files} the same")
    return 0


if __name__ == "__main__":
    sys.exit(main())
