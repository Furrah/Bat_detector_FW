"""
species.py - stages 4-6 of the classifier model: shape groups, the species
rules and sequence voting (Docs/CLASSIFIER_PLAN.md). Stages 1-3 (frames,
segmenter, features) are in classify.py; this takes its calls.

The design follows Docs/Bat Species Classifier.md. The species table is
Europe-wide, from "Docs/European Bat Calls — Species, Frequencies &
Acoustic ID.md":

  - Species that can't be told apart by their calls exist only as group
    labels (whiskered/Brandt's, the mouse-eared and Natterer's complexes,
    the long-eared bats), so they can never be reported as a species.
  - Pairs that only range separates on the mainland are merged too
    (serotine/isabelline serotine; lesser/Mediterranean/Mehely's horseshoe).
  - Island-only species (Crete and Madeira pipistrelles) are behind a
    region setting, off by default: their bands overlap the common
    pipistrelle's, and leaving them on would turn every common pipistrelle
    into "Pipistrelle sp.".

Every range is a literature value, to be refitted to calls recorded through
the V2 front end.
"""

import math

import numpy as np


# ---------------------------------------------------------------- stage 4: shape groups

NONE, CF, QCF, STEEP_FM, FAINT_FM, RISING = "none", "CF", "QCF", "steep FM", "faint FM", "rising"

# Stage 3 measures a sweep's ends a quarter of a millisecond inside the call:
# a frame's frequency is the centre of the part of the call inside its
# 0.512 ms window (diary 43). Extrapolating each end along its own slope by
# that much gives back the true start and end, which is what the literature
# ranges describe. Flat calls are unchanged.
END_EXTRAP_MS = 0.25


EXTRAP_MIN_MS = 1.5    # 6 frames: below this stage 3's slopes are just bw / dur
BAND_KHZ = (9.8, 119.1)  # the analysed bins (classify.K_LO .. K_HI)


def derived(c):
    """Features computed from stage 3's: the extrapolated sweep ends"""
    if c["dur_ms"] >= EXTRAP_MIN_MS:
        c["f_start_x"] = min(c["f_start"] + max(c["slope_start"], 0.0) * END_EXTRAP_MS, BAND_KHZ[1])
        c["f_end_x"] = max(c["f_end"] - max(c["slope_end"], 0.0) * END_EXTRAP_MS, BAND_KHZ[0])
    else:
        c["f_start_x"], c["f_end_x"] = c["f_start"], c["f_end"]
    c["bw_x"] = c["f_start_x"] - c["f_end_x"]
    c["flat_ms"] = c["flat_frac"] * c["dur_ms"]


RISING_KHZ = 5.0       # echolocation calls fall or hold; one rising more than this isn't one
NARROW_KHZ = 8.0       # narrower than this is a constant-frequency tail (QCF), not a faint sweep
# A call peaking in the lowest analysed bin (9.8 kHz) is energy from below the
# band leaking in (wind, handling, insects), not a call. On real recordings
# (Zenodo 3247097) 5,188 such "calls", median 1.0 ms at 9.9 kHz, were labelled
# free-tailed bat. Half a bin above the bin's centre.
BAND_EDGE_KHZ = BAND_KHZ[0] + 1.0


def shape_group(c):
    """The doc's four tests, most specific first. None of them: 'Bat sp.'"""
    if c["bw_x"] < -RISING_KHZ or c["f_peak"] < BAND_EDGE_KHZ:
        return RISING       # not echolocation (a rub fragment, a social call, energy from below the band): no vote
    if c["flat_frac"] >= 0.6 and c["dur_ms"] >= 15.0 and c["f_peak"] >= 70.0:
        return CF           # horseshoes: a long constant tone above 70 kHz
    if (c["flat_ms"] >= 2.0 or c["bw_x"] < NARROW_KHZ) and c["f_char"] <= 60.0:
        # pipistrelles, Savi's, nyctaloids, free-tailed: a flat tail. Narrow
        # calls too: on real pipistrelles (Zenodo 3247097) 30% of calls were
        # 1-3 ms tails without their sweep (clutter, or cut fragments), and
        # with the doc's 2 ms of flat tail alone they fell through to faint FM.
        return QCF
    if (c["harmonic"] or c["bw_x"] < 30.0) and c["bw_x"] >= NARROW_KHZ and c["f_start_x"] <= 60.0 and c["flat_ms"] < 2.0:
        return FAINT_FM     # long-eared bats, barbastelle: short, low-starting sweeps of 8 kHz or more
    if c["bw_x"] >= 30.0 and c["flat_ms"] < 2.0:
        return STEEP_FM     # Myotis
    return NONE


# ---------------------------------------------------------------- taxa

SPECIES, GROUP, UNKNOWN = "species", "group", "unknown"

# code: (name, parent, level). Codes are the 6-letter ones bat software uses
# where one exists; groups have made-up codes.
TAXA = {
    "BATSP":  ("Bat sp.", None, UNKNOWN),

    "RHISP":  ("Rhinolophus sp.", "BATSP", GROUP),
    "RHIFER": ("Greater horseshoe", "RHISP", SPECIES),
    "RHIBLA": ("Blasius's horseshoe", "RHISP", SPECIES),
    "RHIHI":  ("Lesser/Mediterranean/Mehely's horseshoe", "RHISP", GROUP),

    "PIPGRP": ("Pipistrelle group", "BATSP", GROUP),
    "PIPPYG": ("Soprano pipistrelle", "PIPGRP", SPECIES),
    "MINSCH": ("Schreiber's bat", "PIPGRP", SPECIES),
    "PIPPIP": ("Common pipistrelle", "PIPGRP", SPECIES),
    "PIPCRE": ("Crete pipistrelle", "PIPGRP", SPECIES),
    "PIPMAD": ("Madeira pipistrelle", "PIPGRP", SPECIES),
    "PIPKN":  ("Kuhl's/Nathusius' pipistrelle", "PIPGRP", GROUP),
    "HYPSAV": ("Savi's pipistrelle", "BATSP", SPECIES),

    "NYCGRP": ("Nyctaloid group", "BATSP", GROUP),
    "EPTNIL": ("Northern bat", "NYCGRP", SPECIES),
    "NYCLEI": ("Leisler's bat", "NYCGRP", SPECIES),
    "EPTSER": ("Serotine/isabelline serotine", "NYCGRP", GROUP),
    "VESMUR": ("Parti-coloured bat", "NYCGRP", SPECIES),
    "NYCNOC": ("Noctule", "NYCGRP", SPECIES),
    "NYCLAS": ("Greater noctule", "NYCGRP", SPECIES),
    "TADTEN": ("European free-tailed bat", "BATSP", SPECIES),

    "MYOSP":  ("Myotis sp.", "BATSP", GROUP),
    "MYONAT": ("Natterer's/Escalera's/cryptic myotis", "MYOSP", GROUP),
    "MYOEMA": ("Geoffroy's/Alcathoe myotis", "MYOSP", GROUP),
    "MYOCAP": ("Long-fingered bat", "MYOSP", SPECIES),
    "MYOMYO": ("Mouse-eared bats", "MYOSP", GROUP),

    "FAINT":  ("Long-eared bat/barbastelle", "BATSP", GROUP),
    "PLESP":  ("Plecotus sp.", "FAINT", GROUP),
    "BARBAR": ("Barbastelle", "FAINT", SPECIES),
}

GROUP_ROOT = {NONE: "BATSP", CF: "RHISP", QCF: "BATSP", STEEP_FM: "MYOSP", FAINT_FM: "FAINT", RISING: "BATSP"}


def ancestors(code):
    """code, its parent, ... up to BATSP"""
    chain = []
    while code is not None:
        chain.append(code)
        code = TAXA[code][1]
    return chain


def common_parent(a, b):
    up = ancestors(b)
    return next(t for t in ancestors(a) if t in up)


def depth(code):
    return len(ancestors(code))


# ---------------------------------------------------------------- stage 5: rules

MAINLAND = "mainland"
DEFAULT_REGIONS = frozenset({MAINLAND})

# Soft margins outside each range: the score falls from 1 to 0 over this much
MARGIN = {"f_peak": 2.5, "f_char": 2.0, "f_end_x": 2.0, "bw_x": 5.0, "dur_ms": 1.0}

# (taxon, group, [(feature, lo, hi)], mean length in ms or None, regions)
# hi = inf for "at least". Values: the European Bat Calls doc.
RULES = [
    # horseshoes: the constant frequency settles it (+-2-3 kHz soft edges)
    ("RHIFER", CF, [("f_peak", 81.0, 84.0)], None, {MAINLAND}),
    ("RHIBLA", CF, [("f_peak", 92.0, 98.0)], None, {MAINLAND}),
    ("RHIHI",  CF, [("f_peak", 102.0, 112.0)], None, {MAINLAND}),

    # flat tails: the characteristic frequency
    ("PIPPYG", QCF, [("f_char", 53.0, 58.0)], 6.0, {MAINLAND}),
    # Schreiber's bat (50-52 kHz) has no frequency rule: the reference doc's
    # "frequency alone?" is No. It sits on the common/soprano boundary; on real
    # pipistrelles its band took 19% of common and 10% of soprano calls. It
    # needs its faster rhythm and social calls, which stage 6 doesn't test yet.
    ("PIPPIP", QCF, [("f_char", 42.0, 49.0)], 6.3, {MAINLAND}),
    ("PIPCRE", QCF, [("f_char", 44.0, 55.0)], 4.3, {"crete"}),
    ("PIPMAD", QCF, [("f_char", 44.0, 47.0)], 6.0, {"madeira"}),
    ("PIPKN",  QCF, [("f_char", 35.0, 42.0)], 6.6, {MAINLAND}),
    ("HYPSAV", QCF, [("f_char", 30.0, 35.0)], 7.3, {MAINLAND}),
    ("EPTNIL", QCF, [("f_char", 28.0, 30.0)], 10.7, {MAINLAND}),
    ("NYCLEI", QCF, [("f_char", 22.0, 28.0)], 9.3, {MAINLAND}),
    ("EPTSER", QCF, [("f_char", 23.0, 29.0)], 10.9, {MAINLAND}),
    ("VESMUR", QCF, [("f_char", 24.0, 26.0)], 15.0, {MAINLAND}),
    ("NYCNOC", QCF, [("f_char", 18.0, 25.0)], 14.4, {MAINLAND}),
    # the two lowest callers make long calls (21-30 ms and ~17 ms): a short
    # blip down there is something else
    ("NYCLAS", QCF, [("f_char", 12.0, 16.0), ("dur_ms", 12.0, math.inf)], 25.0, {MAINLAND}),
    ("TADTEN", QCF, [("f_char", 9.0, 14.0), ("dur_ms", 8.0, math.inf)], 16.8, {MAINLAND}),

    # Myotis: the end of the sweep (extrapolated), then bandwidth and length.
    # Daubenton's, whiskered/Brandt's and Bechstein's all end near 27-28 kHz
    # and get no rule: they score as Myotis sp.
    ("MYONAT", STEEP_FM, [("f_end_x", 10.0, 20.0), ("bw_x", 80.0, math.inf)], 4.1, {MAINLAND}),
    ("MYOEMA", STEEP_FM, [("f_end_x", 36.0, 45.0)], 3.6, {MAINLAND}),
    ("MYOCAP", STEEP_FM, [("f_end_x", 32.0, 35.0), ("dur_ms", 5.0, 10.0)], 5.2, {MAINLAND}),
    ("MYOMYO", STEEP_FM, [("f_end_x", 20.0, 26.0), ("dur_ms", 5.0, 14.0)], 6.0, {MAINLAND}),

    # faint sweeps: long-eared bats by peak; the barbastelle is decided in stage 6
    ("PLESP", FAINT_FM, [("f_peak", 26.0, 40.0)], 4.0, {MAINLAND}),
]

MIN_SCORE = 0.3         # below this the call is only its shape group, or the group its band falls in

# When no species rule fits, a QCF call still says which family by its band:
# a flat tail at 33-60 kHz is a pipistrelle (or Schreiber's, or Savi's), at
# 17-31 kHz a nyctaloid. On real pipistrelles, calls between the textbook
# bands (e.g. a common pipistrelle at 50.5 kHz) otherwise fell back to
# "Bat sp." and outvoted the rest.
BAND_GROUPS = {QCF: [("PIPGRP", 33.0, 60.0), ("NYCGRP", 17.0, 31.0)]}
AMBIG = 0.7             # runner-up within 70% of the winner: vote for their common parent


def range_score(x, lo, hi, margin):
    if lo <= x <= hi:
        return 1.0
    d = lo - x if x < lo else x - hi
    return 0.0 if d >= margin else 1.0 - d / margin


def dur_weight(dur, mean):
    """1 within 0.6-1.6x of the mean length, falling to 0.5 at half or double"""
    if mean is None:
        return 1.0
    r = dur / mean
    if 0.6 <= r <= 1.6:
        return 1.0
    return max(0.5, 1.0 - 0.5 * ((0.6 - r) / 0.1 if r < 0.6 else (r - 1.6) / 0.4))


def classify_call(c, group, regions=DEFAULT_REGIONS):
    """(taxon, score) for one call within its shape group"""
    scored = []
    for taxon, g, ranges, dur_mean, rule_regions in RULES:
        if g != group or not (rule_regions & regions):
            continue
        s = dur_weight(c["dur_ms"], dur_mean)
        for feat, lo, hi in ranges:
            s *= range_score(c[feat], lo, hi, MARGIN[feat])
        scored.append((s, taxon))
    scored.sort(reverse=True)

    root = GROUP_ROOT[group]
    for taxon, lo, hi in BAND_GROUPS.get(group, []):
        if lo <= c["f_char"] <= hi:
            root = taxon
            break
    if not scored or scored[0][0] < MIN_SCORE:
        return root, 0.5                       # shape (and band) known, species not
    best_s, best = scored[0]
    if len(scored) > 1 and scored[1][0] > AMBIG * best_s:
        return common_parent(best, scored[1][1]), best_s
    return best, best_s


# ---------------------------------------------------------------- stage 6: sequences

SEQ_GAP_MS = 1000.0     # silence that ends a sequence
SEQ_MIN_CALLS = 3       # fewer is not reported: "no bat"
VOTE_N = 10             # calls in the voting window
LEAF_SHARE = 0.6        # share of the vote to name a taxon
LEAF_MIN_CALLS = 5      # calls before a species is named
ECHO_MS = 10.0          # echoes come back within ~10 ms ...
ECHO_DB = 10.0          # ... at least this much weaker ...
ECHO_KHZ = 3.0          # ... in the same band
ALT_STEP_KHZ = 2.0      # an f_char step this big counts towards alternation


def quality(c):
    """Weak calls count less"""
    q = min(max((c["snr_db"] - 6.0) / 18.0, 0.2), 1.0)
    return q * (0.5 if c["flags"] & 2 else 1.0) * (0.7 if c["flags"] & 4 else 1.0)


def tune_frequency(c, group):
    """The frequency the heterodyne should sit near, by call shape"""
    return {CF: c["f_peak"], QCF: c["f_char"], STEEP_FM: c["f_end_x"]}.get(group, c["f_peak"])


def alternating(values, step=ALT_STEP_KHZ):
    """Successive steps bigger than step that keep changing sign, in at least 60% of the steps"""
    steps = [b - a for a, b in zip(values, values[1:]) if abs(b - a) > step]
    if len(steps) < 3:
        return False
    changes = sum(1 for a, b in zip(steps, steps[1:]) if (a > 0) != (b > 0))
    return changes >= 0.6 * (len(steps) - 1)


def sequence_rules(taxon, window):
    """Cues that exist only across calls (the European doc's 'rhythm beats frequency')"""
    if "NYCGRP" in ancestors(taxon):
        chars = [c["f_char"] for c in window if c["group"] == QCF]
        if alternating(chars):
            if max(chars) < 17.0:
                return "NYCLAS"             # greater noctule: "plip-plop" pairs, very low
            if min(chars) <= 21.0:
                return "NYCNOC"             # some calls at or below ~21 kHz
            if min(chars) > 22.0:
                return "NYCLEI"             # alternates, but stays above ~22 kHz
    if "FAINT" in ancestors(taxon):
        durs = [c["dur_ms"] for c in window if c["group"] == FAINT_FM]
        ratios = [max(a, b) / max(min(a, b), 1e-3) for a, b in zip(durs, durs[1:])]
        if len(ratios) >= 3 and np.median(ratios) >= 1.6:
            return "BARBAR"                 # two call types alternating in length
    return taxon


def vote(window):
    """The most specific taxon with LEAF_SHARE of the weighted vote (species need LEAF_MIN_CALLS)"""
    total = sum(c["weight"] for c in window)
    support = {}
    for c in window:
        for t in ancestors(c["label"]):
            support[t] = support.get(t, 0.0) + c["weight"]
    for t in sorted(support, key=lambda t: (-depth(t), -support[t])):
        share = support[t] / total
        if share < LEAF_SHARE:
            continue
        if TAXA[t][2] == SPECIES and len(window) < LEAF_MIN_CALLS:
            continue
        return t, share
    return "BATSP", 1.0


class Sequence:
    def __init__(self, first):
        self.calls = [first]
        self.echoes = 0

    @property
    def start_s(self):
        return self.calls[0]["t_s"]

    @property
    def end_s(self):
        return self.calls[-1]["t_s"] + self.calls[-1]["dur_ms"] / 1000


def is_echo(c, prev):
    return (prev is not None and (c["t_s"] - prev["t_s"]) * 1000 <= ECHO_MS
            and abs(c["f_char"] - prev["f_char"]) <= ECHO_KHZ and c["snr_db"] <= prev["snr_db"] - ECHO_DB)


def identify(calls, regions=DEFAULT_REGIONS):
    """
    Stages 4-6 over a recording's calls (classify.analyse). Labels each call
    in place and returns the sequences with their verdicts.
    """
    sequences = []
    seq, prev = None, None
    for c in calls:
        derived(c)
        c["group"] = shape_group(c)
        if is_echo(c, prev):
            c["label"], c["score"], c["echo"] = None, 0.0, True
            if seq:
                seq.echoes += 1
            continue
        c["echo"] = False
        if c["group"] == RISING:
            c["label"], c["score"] = None, 0.0
            continue
        if c["group"] == NONE:
            c["label"], c["score"] = "BATSP", 0.3
        else:
            c["label"], c["score"] = classify_call(c, c["group"], regions)
        c["weight"] = c["score"] * quality(c)
        if seq is None or (c["t_s"] - seq.calls[-1]["t_s"]) * 1000 > SEQ_GAP_MS:
            seq = Sequence(c)
            sequences.append(seq)
        else:
            seq.calls.append(c)
        prev = c

    for s in sequences:
        window = s.calls[-VOTE_N:]
        voted, share = vote(window)
        taxon = sequence_rules(voted, window)
        s.taxon = taxon
        s.name, _, s.level = TAXA[taxon]
        # confidence from the vote; a sequence rule refines the label, not the evidence
        mean_score = float(np.mean([c["score"] for c in window if voted in ancestors(c["label"])] or [0.0]))
        s.confidence = share * mean_score
        s.tune_khz = float(np.median([tune_frequency(c, c["group"]) for c in window]))
        s.reported = len(s.calls) >= SEQ_MIN_CALLS
    return sequences


def header_text(s):
    """The display's 13-character header message, e.g. 'PIPPIP 87%  9'"""
    return f"{s.taxon:<6} {min(99, round(100 * s.confidence)):2d}% {min(len(s.calls), 99):2d}"[:13]
