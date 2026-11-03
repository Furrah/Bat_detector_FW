# Bat Species Classifier — G431 Firmware Design

Oct 29, 2026 · @Joe

## Summary

The classifier becomes a third consumer of the raw 500 ksps ADC stream, alongside the heterodyne mixer and the waterfall. It runs a 256-point FFT every 0.256 ms, cuts the stream into individual calls, measures about a dozen features per call, and votes across the last 5–10 calls. It reports a species when the calls support one and a group label when they don't.

Design principles:

- **Honest labels.** Output is a species, a group ("*Myotis* sp.", "*Nyctalus/Eptesicus*") or "Bat sp.", with a confidence and a call count. Whiskered/Brandt's, the Natterer's complex and the two mouse-eared bats exist only as group entries, so they can never be reported as species.
- **Table-driven.** Species parameters live in a const table in flash, seeded from the European Bat Calls doc and later re-fitted to calls recorded through the V2 front end. Moving to a trained tree or MLP replaces one function per shape group; nothing else changes.
- **Real-time safe.** The per-frame work runs in a low-priority interrupt, so a slow SD write in the main loop can't stall it. If it falls behind it drops blocks and marks the call as gapped, matching the existing drop-rather-than-corrupt policy.
- **Verifiable.** Every stage has a numpy reference model, run on raw samples captured from the device, the same way the FFT was validated.

Out of scope for v1: spectrogram CNNs (no room in 32 KB SRAM), separating two bats calling at once (flagged, not resolved), and social calls.

## Where it sits in the V2 firmware

The classifier reads the same raw half-buffers as the mixer, through its own interrupt. Stages 1–3 run there; stages 4–6 and everything they drive run in the main loop.

&#91;embedded content: V2 firmware with the classifier · 3 execution contexts, 6 new stages\]

Calls cross from the frame interrupt to the main loop through one queue, and everything downstream of the vote reads a single result struct, so the UI, tuner and recorder never touch classifier state directly.

## Timing and resource budget

The per-frame stage dominates: about 23% of the CPU, continuously, with CMSIS-DSP. Everything per call is under 1%. SRAM need is about 5.5 KB, which fits in the 10 KB CCM SRAM if it is free. All cycle counts below are estimates to replace with `DWT->CYCCNT` measurements.

**Frame parameters.** 256-point real FFT, Hann window, hop 128 samples (50% overlap). At 500 ksps that is a 0.512 ms window, 1.95 kHz bins and a new frame every 0.256 ms, so 3,906 frames/s. Only bins 5–72 (10–140 kHz) are analysed, 68 bins in all. The lower edge keeps free-tailed bats and the 14–22 kHz social calls in view.

Why 50% overlap: a 1 ms *Natterer's* sweep still gets about 4 frames and a 3.6 ms *Geoffroy's* call about 14, enough for start, end and slope. The European Bat Calls doc suggested 75%; that doubles the frame cost, so it is a config constant to try once the real load is measured.

### CPU (170 MHz Cortex-M4F)

| Stage | Runs | Cycles each (est.) | Load (est.) |
| --- | --- | --- | --- |
| Frame: int16→float, window, 256-pt rFFT, \|X\|², noise floor, peak pick, segmenter | 3,906 /s, always | 9–10k with CMSIS `arm_rfft_fast_f32` | 21–23% |
| Feature extraction | Once per call, ≤ 40 /s | \~15k (≤ 320 frames, 3 passes) | < 0.4% |
| Shape group + rules (later tree/MLP) + vote | Once per call | \~3k | < 0.1% |
| Sequence close, logging, UI publish | Once per sequence | small | — |

The frame stage runs on every hop whether a bat is present or not, because the noise floor has to keep learning. So 23% is the steady-state cost, not a peak. The two levers if that is too much:

- **Hop 256 (no overlap):** halves it to \~12%, at 0.512 ms time resolution. Fine for CF and QCF calls, marginal for 1–2 ms *Myotis* calls.
- **Feed the waterfall from these frames:** sum 8 consecutive 256-point power spectra per waterfall line instead of running the 1024-point FFT. That removes the existing waterfall FFT entirely, at the cost of 1.95 kHz instead of 488 Hz display bins (about 2 px per bin on a 240 px axis).

To estimate the hand-rolled FFT instead of CMSIS, scale your measured 1024-point cost by N log N: a 256-point transform costs about 0.2× the 1024-point one.

**Deadline.** With a DMA half-buffer of B samples the frame stage gets B/128 frames to do in B/500,000 s. For B = 512 that is 4 frames (\~40k cycles) in 1.024 ms (174k cycles), leaving room for the mixer.

### SRAM

| Buffer | Size | Notes |
| --- | --- | --- |
| Sample history, 256 × int16 | 512 B | Carries the 128-sample overlap between hops |
| FFT in + out, 256 × float each | 2,048 B | `arm_rfft_fast_f32` needs separate buffers; an in-place FFT halves this |
| Noise floor, 68 × float | 272 B | Power spectrum reuses the FFT output buffer |
| Call track, 320 frames × 4 B | 1,280 B | uint16 frequency (10 Hz units), int8 SNR, uint8 flags; 82 ms, longer than any horseshoe call |
| Call queue, 8 × `cls_call_t` | 512 B | Interrupt → main loop |
| Sequence ring, 16 calls | \~512 B | Labels, weights and the features voting needs |
| Event queue, result, state | \~300 B |  |
| **Total** | **\~5.4 KB** | All CPU-only, so it can live in CCM SRAM and stay off the DMA bus |

### Flash

About 15–20 KB of 128 KB: CMSIS rFFT-256 tables built from the source-only library (\~2.2 KB), the Hann window (1 KB), rule and taxon tables (\~2 KB), per-group MLP weights later (\~5 KB), and the code itself.

## Data structures and module interface

The module exposes one function for the frame interrupt and a handful for the main loop. The two sides talk only through single-producer, single-consumer queues, so neither needs a lock.

### Configuration

Lives beside `pipeline_config.h`, so the hop can never drift out of step with `AUDIO_BLOCK_SIZE`.

```c
/* classifier_config.h */
#define CLS_FS_HZ        500000u
#define CLS_FFT_N        256u
#define CLS_HOP          128u                         /* 50% overlap */
#define CLS_BIN_HZ       ((float)CLS_FS_HZ / CLS_FFT_N) /* 1953.125 Hz */
#define CLS_HOP_MS       (1000.0f * CLS_HOP / CLS_FS_HZ) /* 0.256 ms */
#define CLS_K_LO         5u                           /* ~9.8 kHz  */
#define CLS_K_HI         72u                          /* ~140.6 kHz */
#define CLS_TRACK_MAX    320u                         /* 82 ms of frames */
#define CLS_VOTE_N       10u                          /* calls in the voting window */
#define CLS_SEQ_GAP_MS   1000u                        /* silence that ends a sequence */

_Static_assert(AUDIO_BLOCK_SIZE % CLS_HOP == 0, "block must hold whole hops");
```

### Structures

```c
typedef enum { GRP_NONE, GRP_CF, GRP_QCF, GRP_STEEP_FM, GRP_FAINT_FM, GRP_N } cls_group_t;
typedef enum { LVL_SPECIES, LVL_GROUP, LVL_UNKNOWN } cls_level_t;
typedef enum { FEAT_NONE, FEAT_F_PEAK, FEAT_F_END, FEAT_F_CHAR, FEAT_BW, FEAT_DUR } cls_feat_t;

typedef struct {              /* one frame inside a call, 4 B */
    uint16_t f_10hz;          /* interpolated peak frequency, 10 Hz units */
    int8_t   snr_db;          /* peak SNR re noise floor, clamped */
    uint8_t  flags;           /* PT_GAP (block dropped before this frame), PT_CLIP */
} cls_track_pt_t;

typedef struct {              /* features of one call, fits a 64 B queue slot */
    uint32_t frame0;          /* start frame index; wraps after ~12.7 days */
    float dur_ms;
    float f_start, f_end, f_peak, f_char;       /* kHz */
    float bw;                                   /* f_start - f_end */
    float slope_start, slope_mid, slope_end;    /* kHz/ms, positive = falling */
    float flat_frac;          /* share of frames with |slope| < CLS_FLAT_SLOPE */
    float snr_db;             /* max SNR in the call */
    float ipi_ms;             /* start-to-start from previous call, 0 if first */
    uint8_t harmonic;         /* 2nd harmonic present at the max-energy frame */
    uint8_t flags;            /* CALL_TRUNCATED, CALL_GAPPED, CALL_CLIPPED */
} cls_call_t;

typedef struct {              /* one row of the taxon tree, const in flash */
    const char *code;         /* 6-letter code: "PIPPIP", "MYOSP" (display, GUANO) */
    const char *name;         /* "Common pipistrelle" */
    uint8_t     parent;       /* taxon to fall back to; itself for the root "Bat sp." */
    cls_level_t level;
} cls_taxon_t;

typedef struct { uint8_t feat; float lo, hi; } cls_range_t;

typedef struct {              /* one species rule, const in flash */
    uint8_t     taxon;
    cls_group_t group;
    cls_range_t r[2];         /* r[1].feat == FEAT_NONE when one constraint is enough */
    float       dur_mean;     /* ms; soft weight until fitted from own data */
    uint32_t    region_mask;
} cls_rule_t;

typedef struct { cls_group_t group; uint8_t taxon; float score; } cls_label_t;

typedef struct {              /* what the UI, tuner and recorder see */
    uint32_t    seq_id;
    bool        active;       /* sequence still running */
    uint8_t     taxon;        /* species, or the fallback group */
    cls_level_t level;
    float       confidence;   /* 0..1 */
    uint8_t     n_calls;      /* calls behind the verdict */
    float       f_tune_khz;   /* suggested heterodyne target */
    uint8_t     flags;        /* SEQ_ALTERNATING, SEQ_MULTI_BAT, SEQ_GAPPED */
} cls_result_t;
```

### Public API

```c
/* classifier.h */
void CLS_Init(uint32_t region_mask);

/* Frame interrupt only. Consumes one DMA half-buffer of raw ADC samples. */
void CLS_OnBlock(const uint16_t *adc, uint32_t n);

/* Main loop only. */
void CLS_Service(void);                               /* classify, vote, close sequences */
bool CLS_PollEvent(cls_event_t *ev);                  /* CALL_END, SEQ_START, SEQ_END, OVERRUN */
bool CLS_GetResult(cls_result_t *out);                /* true if changed since last poll */
bool CLS_PopCallLog(cls_call_t *c, cls_label_t *l);   /* rows for the SD call log */
void CLS_GetStats(cls_stats_t *s);                    /* frames, drops, worst cycles per block */
```

Queue discipline: the writer fills a slot, issues `__DMB()`, then advances the head index. The reader does the reverse with the tail. A full queue drops the new item and bumps a counter, never overwrites.

## Stage-by-stage design

Six stages, split across two contexts. Stages 1–3 run in the frame interrupt because they touch every frame; stages 4–6 run in the main loop because they touch each call once.

1. **Frame stage** — FFT, whitened peak pick, noise floor.
2. **Segmenter** — turns frames into calls.
3. **Feature extraction** — a dozen numbers per call.
4. **Shape grouping** — CF, FM-QCF, steep FM, faint FM.
5. **Species rules** — table lookup with soft scores, inside one group.
6. **Sequence voting** — 5–10 calls, falls back to a group label.

### 1. Frame stage (interrupt)

The peak is picked on the spectrum divided by the noise floor, not the raw spectrum. That whitens out steady bush-cricket tones and the mic's own slope, so a quiet bat beside a loud insect still wins. The comparison is cross-multiplied, so there are no divides in the bin loop.

```c
/* One call per DMA half-buffer, n = AUDIO_BLOCK_SIZE. */
void CLS_OnBlock(const uint16_t *adc, uint32_t n)
{
    for (uint32_t h = 0; h < n; h += CLS_HOP) {
        if (cls_overrun) { seg_mark_gap(); return; }   /* DMA wrapped onto this half: stop */

        /* 1. slide the 256-sample history one hop, removing DC */
        memmove(hist, hist + CLS_HOP, CLS_HOP * sizeof hist[0]);
        int32_t sum = 0;
        for (i = 0; i < CLS_HOP; i++) {
            int32_t x = adc[h + i];
            sum += x;
            hist[CLS_HOP + i] = (int16_t)(x - dc);
        }
        dc += (sum / (int32_t)CLS_HOP - dc) >> 6;      /* slow DC tracker */

        /* 2. window and transform */
        for (i = 0; i < CLS_FFT_N; i++) fin[i] = hist[i] * hann[i];
        arm_rfft_fast_f32(&rfft, fin, fout, 0);

        /* 3. power in band, peak of P/NF (bins K_LO-1..K_HI+1 so interpolation has neighbours) */
        float *P = fin;                                 /* input buffer is free again */
        uint32_t kb = CLS_K_LO; float pb = 0.0f, nb = 1.0f;
        for (k = CLS_K_LO - 1; k <= CLS_K_HI + 1; k++) {
            P[k] = fout[2*k] * fout[2*k] + fout[2*k+1] * fout[2*k+1];
            if (k >= CLS_K_LO && k <= CLS_K_HI && P[k] * nb > pb * nf[k]) {
                pb = P[k]; nb = nf[k]; kb = k;
            }
        }
        float snr_db = 10.0f * fast_log10(pb / nb);
        float f_khz  = (kb + parabolic_log(P[kb-1], P[kb], P[kb+1])) * CLS_BIN_HZ * 1e-3f;
        bool  harm   = (2*kb <= CLS_K_HI) && (P[2*kb] > H_RATIO * nf[2*kb]);

        /* 4. segment, then learn noise only between calls */
        seg_step(f_khz, snr_db, harm);
        if (seg.state == SEG_IDLE) nf_update(P);
        frame++;
    }
}

static void nf_update(const float *P)
{
    for (k = CLS_K_LO; k <= CLS_K_HI; k++) {
        float a = (P[k] > nf[k]) ? NF_UP : NF_DOWN;  /* 1/2048 (~0.5 s) up, 1/64 (~16 ms) down */
        nf[k] += a * (P[k] - nf[k]);
    }
}
```

`parabolic_log` fits a parabola through the three log-powers and returns the offset in bins (±0.5); on a Hann window that takes peak accuracy well below the 1.95 kHz bin. `fast_log10` is a bit-trick approximation; it is called four times per frame. The segmenter stays muted for the first \~0.5 s after boot while the floor settles.

### 2. Segmenter (interrupt)

A three-state machine: idle, arming (debounces clicks) and in call. A call ends on a run of weak frames, where weak means below an absolute floor or 24 dB below the call's own peak. The relative test stops echoes and reverberation from stretching the call.

```c
/* Starting values; fit them on your own recordings. */
#define T_ON_DB       12.0f   /* SNR to start a call */
#define T_OFF_DB       6.0f   /* weak frame, absolute */
#define END_DROP_DB   24.0f   /* weak frame, relative to the call's max */
#define N_ON           2      /* 0.5 ms above T_ON to start */
#define N_OFF          3      /* 0.77 ms of weak frames to end */
#define RISE_MAX_KHZ   8.0f   /* calls sweep down or hold; a jump up this big = a new call */
#define MIN_CALL_FR    3      /* 0.77 ms of strong frames, else it was a click */

static void seg_step(float f, float snr, bool harm)
{
    switch (seg.state) {
    case SEG_IDLE:
        if (snr >= T_ON_DB) {
            seg.state = SEG_ARMING; seg.n = 1;
            track_reset(frame); track_push(f, snr, harm);
        }
        break;

    case SEG_ARMING:
        if (snr < T_ON_DB) { seg.state = SEG_IDLE; break; }
        track_push(f, snr, harm);
        if (++seg.n >= N_ON) { seg.state = SEG_IN_CALL; seg.off = 0; LED_CALL_ON(); }
        break;

    case SEG_IN_CALL: {
        bool weak = (snr < T_OFF_DB) || (snr < track.max_snr - END_DROP_DB);
        if (!weak && f - track.last_f > RISE_MAX_KHZ) {   /* another bat, or the next call */
            seg_end_call();
            seg.state = SEG_ARMING; seg.n = 1;
            track_reset(frame); track_push(f, snr, harm);
            break;
        }
        track_push(f, snr, harm);                        /* weak tail frames too; trimmed later */
        seg.off = weak ? seg.off + 1 : 0;
        if (seg.off >= N_OFF) { seg_end_call(); seg.state = SEG_IDLE; }
        break;
    }
    }
}

static void seg_end_call(void)
{
    LED_CALL_OFF();                                    /* BSRR write: atomic, safe in an IRQ */
    if (track.n_strong < MIN_CALL_FR) return;
    cls_call_t c;
    extract_features(&track, &c);                      /* ~15k cycles, once per call */
    if (!q_push(&call_q, &c)) stats.call_drops++;
}
```

`track_push` stores up to `CLS_TRACK_MAX` points. Past that it keeps updating the max and the strong-frame count, and sets `CALL_TRUNCATED`; only CF calls ever get that long. A dropped block sets `PT_GAP` on the next point, which becomes `CALL_GAPPED`.

The trigger band starts at \~10 kHz on purpose. Social calls at 14–22 kHz become "calls" here, get logged and recorded, and land in no shape group; they are the deciding evidence for several pipistrelle pairs once you look at them on the PC.

### 3. Feature extraction (interrupt, once per call)

Works straight on the uint16 track with a 3-point median, so it needs no float copy of the track on the interrupt stack.

```c
#define F(i) (median3_clamped(t, i) * 0.01f)            /* kHz, 3-point median of f_10hz */

static void extract_features(const track_t *t, cls_call_t *c)
{
    /* keep only frames within END_DROP_DB of the max */
    uint32_t i0 = first_index(t, t->max_snr - END_DROP_DB);
    uint32_t i1 = last_index (t, t->max_snr - END_DROP_DB);
    uint32_t m  = i1 - i0 + 1;

    c->frame0   = t->frame0 + i0;
    c->dur_ms   = m * CLS_HOP_MS;      /* reads ~one window long; rules are fitted to the same measure */
    c->f_start  = F(i0);
    c->f_end    = F(i1);
    c->f_peak   = F(t->i_max);
    c->bw       = c->f_start - c->f_end;
    c->snr_db   = t->max_snr;
    c->harmonic = t->harm_at_max;
    c->flags    = t->flags;

    /* least-squares slope of F against time over each third; positive = falling */
    if (m >= 6) {
        uint32_t s = m / 3;
        c->slope_start = -lsq_slope(t, i0,         s);
        c->slope_mid   = -lsq_slope(t, i0 + s,     s);
        c->slope_end   = -lsq_slope(t, i1 - s + 1, s);
    } else {
        c->slope_start = c->slope_mid = c->slope_end = c->bw / c->dur_ms;
    }

    /* flatness, and characteristic frequency = flattest point in the last 40% */
    uint32_t flat = 0; float best = INFINITY;
    c->f_char = c->f_end;
    for (i = i0 + 1; i <= i1; i++) {
        float s = fabsf(F(i - 1) - F(i)) / CLS_HOP_MS; /* kHz/ms */
        if (s < CLS_FLAT_SLOPE) flat++;                /* start at 2 kHz/ms */
        if (i >= i0 + (6 * m) / 10 && s < best) { best = s; c->f_char = F(i); }
    }
    c->flat_frac = (m > 1) ? (float)flat / (m - 1) : 0.0f;
}
```

The inter-pulse interval is not set here. It is filled in the main loop after echo rejection, so an echo can't halve it.

### 4. Shape grouping (main loop)

Four tests in order, most specific first. Anything that passes none votes "Bat sp.", which is the safe failure.

```c
static cls_group_t shape_group(const cls_call_t *c)
{
    float flat_ms = c->flat_frac * c->dur_ms;

    if (c->flat_frac >= 0.6f && c->dur_ms >= 15.0f && c->f_peak >= 70.0f)
        return GRP_CF;          /* horseshoes: long constant tone above 70 kHz */
    if (flat_ms >= 2.0f && c->f_char <= 60.0f)
        return GRP_QCF;         /* pipistrelles, Savi's, nyctaloids, free-tailed: flat tail */
    if ((c->harmonic || c->bw < 30.0f) && c->f_start <= 60.0f && flat_ms < 2.0f)
        return GRP_FAINT_FM;    /* long-eared bats, barbastelle: short, low-starting FM */
    if (c->bw >= 30.0f && flat_ms < 2.0f)
        return GRP_STEEP_FM;    /* Myotis */
    return GRP_NONE;
}
```

Known leaks, all of which voting or the fallback absorbs:

- **Pipistrelles in clutter** drop the QCF tail and pass as steep FM. Most calls in a pass keep the tail, so they lose the vote.
- **Distant *Myotis*** lose their high start to air absorption, so `f_start` falls below 60 kHz and they can land in faint FM. They then score low on the long-eared rules and fall back.
- **Pond bat** has a flat middle section and would group as QCF near the Kuhl's/Nathusius' band. Leave its rule out unless it occurs where you record.

### 5. Species rules (main loop)

Each rule is up to two feature ranges plus a soft duration weight. Scores fall off linearly over a margin outside each range rather than cutting hard, so a call 1 kHz outside a band still scores, just lower. When the runner-up scores close to the winner, the call votes for their common parent instead of guessing.

```c
static float range_score(float x, float lo, float hi, float margin)
{
    if (x >= lo && x <= hi) return 1.0f;
    float d = (x < lo) ? lo - x : x - hi;
    return (d >= margin) ? 0.0f : 1.0f - d / margin;
}

#define MARGIN_KHZ  2.0f      /* about one bin */
#define MIN_SCORE   0.3f
#define AMBIG       0.7f      /* runner-up within 70% of the winner = ambiguous */

static cls_label_t classify_call(const cls_call_t *c, cls_group_t g)
{
    cls_label_t best = { g, GROUP_ROOT[g], 0.0f }, second = best;

    for (const cls_rule_t *r = RULES; r < RULES + N_RULES; r++) {
        if (r->group != g || !(r->region_mask & s.region)) continue;
        float sc = dur_weight(c->dur_ms, r->dur_mean);       /* 1.0 within 0.6x-1.6x of the mean */
        for (int j = 0; j < 2; j++)
            if (r->r[j].feat != FEAT_NONE)
                sc *= range_score(feature(c, r->r[j].feat), r->r[j].lo, r->r[j].hi, margin(r->r[j].feat));
        if (sc > best.score)        { second = best; best = (cls_label_t){ g, r->taxon, sc }; }
        else if (sc > second.score) { second = (cls_label_t){ g, r->taxon, sc }; }
    }

    if (best.score < MIN_SCORE)
        return (cls_label_t){ g, GROUP_ROOT[g], 0.5f };     /* shape known, species not */
    if (second.score > AMBIG * best.score)
        return (cls_label_t){ g, common_parent(best.taxon, second.taxon), best.score };
    return best;
}
```

Starting rule table for the Pays de Gex, using the Swiss values in the European Bat Calls doc. Every range is a first guess, to be refitted from calls recorded through your own front end. The region mask decides which rows are live.

| Taxon | Code | Group | Constraint 1 | Constraint 2 | Mean length (ms) | Falls back to |
| --- | --- | --- | --- | --- | --- | --- |
| Greater horseshoe | RHIFER | CF | peak 81–84 kHz | — | — | *Rhinolophus* sp. |
| Lesser horseshoe | RHIHIP | CF | peak 105–112 kHz | — | — | *Rhinolophus* sp. |
| Soprano pipistrelle | PIPPYG | QCF | char 53–58 kHz | — | 6.0 | Pipistrelle group |
| Schreiber's bat | MINSCH | QCF | char 50–52 kHz | — | 6.2 | Pipistrelle group |
| Common pipistrelle | PIPPIP | QCF | char 42–49 kHz | — | 6.3 | Pipistrelle group |
| Kuhl's / Nathusius' pipistrelle | PIPKN | QCF | char 35–42 kHz | — | 6.6 | Pipistrelle group |
| Savi's pipistrelle | HYPSAV | QCF | char 30–35 kHz | — | 7.3 | Bat sp. |
| Northern bat | EPTNIL | QCF | char 28–30 kHz | — | 10.7 | Nyctaloid group |
| Leisler's bat | NYCLEI | QCF | char 22–28 kHz | — | 9.3 | Nyctaloid group |
| Serotine | EPTSER | QCF | char 23–27 kHz | — | 10.9 | Nyctaloid group |
| Parti-coloured bat | VESMUR | QCF | char 24–26 kHz | — | 15.0 | Nyctaloid group |
| Noctule | NYCNOC | QCF | char 18–25 kHz | — | 14.4 | Nyctaloid group |
| Free-tailed bat | TADTEN | QCF | char 9–14 kHz | — | 16.8 | Bat sp. |
| Natterer's complex | MYONAT | Steep FM | end 10–20 kHz | bandwidth ≥ 80 kHz | 4.1 | *Myotis* sp. |
| Geoffroy's bat | MYOEMA | Steep FM | end 36–45 kHz | — | 3.6 | *Myotis* sp. |
| Mouse-eared pair | MYOMYO | Steep FM | end 20–26 kHz | length 5–14 ms | 6.0 | *Myotis* sp. |
| Long-eared bats | PLESP | Faint FM | peak 26–40 kHz | — | 4.0 | Bat sp. |
| Barbastelle | BARBAR | Faint FM | decided in stage 6 | — | 2.5 / 6 | Bat sp. |

Daubenton's, whiskered/Brandt's and Bechstein's get no leaf rule: their end frequencies all sit around 27–28 kHz, so they score as *Myotis* sp. The nyctaloids overlap heavily on frequency, which is why stage 6 adds rhythm and alternation.

**Swapping in a trained model (phase 4).** Replace `classify_call` for one group with a small MLP generated from your labelled calls. Same input struct, same output, same ambiguity rule:

```c
/* cls_model_qcf.h, generated by train.py: 12 features -> 16 hidden -> 8 taxa, ~320 MACs */
static cls_label_t classify_call_mlp(const cls_call_t *c)
{
    float x[NF], h[NH], z[NT];
    pack_features(c, x);                                   /* dur, f_*, bw, slopes, flat_frac, ipi */
    for (i = 0; i < NF; i++) x[i] = (x[i] - MU[i]) * INV_SIG[i];
    for (j = 0; j < NH; j++) h[j] = fmaxf(0.0f, B1[j] + dot(W1[j], x, NF));
    for (t = 0; t < NT; t++) z[t] = B2[t] + dot(W2[t], h, NH);
    softmax(z, NT);
    return top2_with_ambiguity(z, OUT_TAXON, AMBIG);
}
```

Training labels for the whiskered/Brandt's, Natterer's and mouse-eared complexes are collapsed to the complex before training, so the model can't learn a split the calls don't contain.

### 6. Sequence voting (main loop)

Calls arrive from the interrupt queue. The service rejects echoes, fills in the interval, groups and labels each call, then re-votes over the last 10 calls. The verdict starts at the most specific taxon and rolls up to the parent group whenever its share of the weighted vote is too small.

```c
#define SEQ_GAP_FR     (CLS_SEQ_GAP_MS / CLS_HOP_MS)  /* 1 s of silence closes a sequence */
#define SEQ_MIN_CALLS  3                              /* before SEQ_START fires */
#define LEAF_SHARE     0.6f                           /* vote share to name a species */
#define LEAF_MIN_CALLS 5
#define GROUP_SHARE    0.6f
#define ECHO_FR        (10.0f / CLS_HOP_MS)           /* echoes return within ~10 ms */

void CLS_Service(void)
{
    cls_call_t c;
    while (q_pop(&call_q, &c)) {
        if (is_echo(&c, &seq.last)) continue;         /* within ECHO_FR, same band, >= 10 dB weaker */
        if (seq.active && c.frame0 - seq.last.frame0 > SEQ_GAP_FR) close_sequence();
        if (!seq.active) open_sequence();

        c.ipi_ms = seq.n ? (c.frame0 - seq.last.frame0) * CLS_HOP_MS : 0.0f;
        seq.last = c;

        cls_group_t g = shape_group(&c);
        cls_label_t l = (g == GRP_NONE) ? (cls_label_t){ g, TAXON_BAT_SP, 0.3f }
                                        : classify_call(&c, g);
        ring_push(&seq.ring, l, l.score * quality(&c), &c);
        calllog_push(&c, &l);
        if (++seq.n == SEQ_MIN_CALLS) ev_push(EV_SEQ_START);
        vote_and_publish();
    }
    if (seq.active && frame_now() - seq.last.frame0 > SEQ_GAP_FR) close_sequence();
}

static void vote_and_publish(void)
{
    float w[N_TAXA] = {0}, wg[N_TAXA] = {0}, total = 0.0f;
    for (e in last CLS_VOTE_N entries of seq.ring) { w[e.taxon] += e.w; total += e.w; }

    uint8_t t = argmax(w, N_TAXA);
    cls_level_t lvl = TAXA[t].level;
    float share = w[t] / total;

    if (share < LEAF_SHARE || seq.n < LEAF_MIN_CALLS) {      /* roll up one level */
        for (i = 0; i < N_TAXA; i++) wg[TAXA[i].parent] += w[i];
        t = argmax(wg, N_TAXA); share = wg[t] / total; lvl = LVL_GROUP;
        if (share < GROUP_SHARE) { t = TAXON_BAT_SP; lvl = LVL_UNKNOWN; }
    }

    t = sequence_rules(t, &seq.ring);                       /* see below */
    publish(t, lvl, share * mean_score(&seq.ring, t), seq.n, f_tune(&seq.ring, t));
}

static float quality(const cls_call_t *c)
{
    float q = clampf((c->snr_db - 6.0f) / 18.0f, 0.2f, 1.0f);  /* weak calls count less */
    if (c->flags & CALL_GAPPED)  q *= 0.5f;
    if (c->flags & CALL_CLIPPED) q *= 0.7f;
    return q;
}
```

`sequence_rules` applies the cues that only exist across calls, taken from the European Bat Calls doc:

- **Nyctaloid group:** an alternation index (sign changes of successive `f_char` steps larger than 2 kHz). Alternating with some calls at or below 21 kHz gives noctule; alternating with all calls above 22 kHz gives Leisler's; no alternation leaves serotine, parti-coloured or northern bat to the per-call vote.
- **Faint FM group:** two call types alternating in length and band gives barbastelle; otherwise long-eared bats.
- **Pipistrelle group:** interval is logged now; a faster rhythm separates Schreiber's once there are your own recordings to set the threshold.

`f_tune` is the median over the voting window of the frequency that suits the shape: peak for CF, characteristic frequency for QCF, end frequency for steep FM. A median rather than the latest call stops the tuner jumping between the two notes of an alternating noctule.

## Integration with the rest of V2

The classifier changes one existing function (the DMA callback gains three lines) and otherwise only adds calls in the main loop. It never touches SPI1 or SPI2 itself; the display and SD card see it only through the existing owners of those buses.

### Interrupt wiring

The frame stage is too heavy for the DMA interrupt and too time-critical for the main loop, where a busy SD card can block for tens of milliseconds. It gets its own interrupt between the two, pended in software from the DMA callback on any unused peripheral vector.

| Context | Priority | Work |
| --- | --- | --- |
| ADC/DAC DMA half/full complete | Highest | Existing mixer → DAC; hand the fresh half-buffer to the classifier |
| Classifier frame interrupt | Below DMA, above the main loop | `CLS_OnBlock`: frames, segmenter, features |
| Main loop | — | `CLS_Service`, UI, tuner, recorder, FatFS/USB MSC |

```c
volatile const uint16_t *cls_block;
volatile bool cls_inflight, cls_overrun;

void Audio_DMA_HalfOrFull(const uint16_t *half)         /* existing callback */
{
    DSP_ProcessBlock(half);                              /* existing mixer, unchanged */

    if (cls_inflight) {                                  /* last block not finished: DMA is now */
        cls_overrun = true;                              /* refilling the half it is reading    */
        stats.block_drops++;
    } else {
        cls_block = half;
        cls_overrun = false;
        cls_inflight = true;
        NVIC_SetPendingIRQ(CLS_IRQn);
    }
}

void CLS_IRQHandler(void)
{
    uint32_t t0 = DWT->CYCCNT;
    CLS_OnBlock((const uint16_t *)cls_block, AUDIO_BLOCK_SIZE);
    stats.worst_cycles = MAX(stats.worst_cycles, DWT->CYCCNT - t0);
    cls_inflight = false;
}
```

The classifier reads straight from the DMA half-buffer, so it has one half-period to finish. On overrun it abandons the rest of the block at the next hop boundary, marks the open call as gapped and skips the next block, so it never processes samples DMA is overwriting.

### Main loop

```c
while (1) {
    /* ... existing: waterfall, buttons, recorder, FatFS / USB MSC state machine ... */

    CLS_Service();

    cls_event_t ev;
    while (CLS_PollEvent(&ev)) {
        switch (ev.type) {
        case EV_SEQ_START:
            if (rec.mode == REC_AUTO) Recorder_Start();   /* PSRAM pre-trigger supplies the first calls */
            break;
        case EV_SEQ_END:
            Recorder_TagSequence(&last_result);           /* verdict into the file's metadata */
            if (rec.mode == REC_AUTO) Recorder_StopAfter(HANGOVER_MS);
            break;
        case EV_OVERRUN:
            UI_FlagOverrun();
            break;
        }
    }

    cls_result_t r;
    if (CLS_GetResult(&r)) {
        last_result = r;
        UI_RequestLabelRedraw(&r);                        /* drawn by the SPI2 owner, at most 4 Hz */
        if (tune.mode == TUNE_AUTO && r.level != LVL_UNKNOWN && r.confidence >= TUNE_MIN_CONF)
            Tuner_SetTarget(r.f_tune_khz);
    }

    cls_call_t c; cls_label_t l;
    while (CLS_PopCallLog(&c, &l)) CallLog_Append(&c, &l);   /* RAM ring, flushed with WAV writes */
}
```

### Heterodyne tuning

This replaces the spec's "FFT peak detection" auto-tune with the classifier's sequence median.

- **Target.** LO = `f_tune` minus the beat offset, about 1.5 kHz, so the call's steady part lands in the middle of the 1–2 kHz output bandpass.
- **Stability.** Retune only when the verdict changes or `f_tune` moves by more than about 2 kHz, and hold each setting for at least 1 s. The NCO is phase-continuous, so changing its increment does not click.
- **Manual wins.** Touching the tuning control switches to manual until auto is selected again.

### Display

A one-line status bar under the waterfall: label, confidence, call count and tuned frequency, for example "*P. pipistrellus* · 87% · 9 calls · 45.2 kHz". Group-level verdicts get a different colour so a fallback is obvious at a glance. Optionally, a short tick at the waterfall's leading edge per detected call, coloured by shape group.

Because the display shares SPI2 with the SD card, the classifier only requests a redraw; the SPI2 owner draws it between SD transactions.

### LEDs

This takes up the suggested split: one LED for power and battery, one for call activity. The segmenter drives the call LED directly with a `BSRR` write at call start and end, which is atomic and safe from the interrupt. A 5 ms call is barely visible, so stretch each pulse to about 30 ms in the main loop.

### Recording and metadata

- **Auto-record.** `EV_SEQ_START` fires after the third call. With bats calling every 100–150 ms, that is up to \~0.5 s in, so set the PSRAM pre-trigger to at least 1 s. At 1 MB/s that is a small slice of the 8 MB part.
- **WAV metadata.** GUANO is plain UTF-8 `Key: value` lines in a `guan` sub-chunk, which may sit at the end of the RIFF file, so it can be written after recording when the verdict is known ([GUANO spec](https://github.com/riggsd/guano-spec/blob/master/guano_specification.md)). `GUANO|Version` must come first and `Timestamp` is required. `Species Auto ID` is the defined field for the automatic result; vendors add their own fields under a namespace, as Wildlife Acoustics does ([WA namespace](https://wildlifeacoustics.com/SCHEMA/GUANO.html)).
- **Call log.** One CSV row per call (time, all features, group, label, score, sequence id) to the SD card, written through the same FatFS/USB MSC exclusion as WAVs. While USB owns the card, rows wait in a 16-row RAM ring and the oldest are dropped and counted if it fills. This log is the training set for phase 4.

Example metadata block for a recording:

```text
GUANO|Version: 1.0
Timestamp: 2027-06-14T22:41:07+02:00
Samplerate: 500000
Length: 12.4
Model: Bat Detector V2
Firmware Version: 2.0.0
Species Auto ID: PIPPIP
BDV2|Auto ID Level: species
BDV2|Auto ID Confidence: 0.87
BDV2|Auto ID Calls: 9
BDV2|Rule Table: pays-de-gex-v1
```

## Validation plan and build order

Build it in five steps, each useful on its own, and check every step against a numpy model run on the same raw samples. Step 2 already gives a call counter and a training log before any species logic exists.

1. **Measure headroom.** Read SRAM and flash use from the `.map` file. Time the mixer and the waterfall FFT per block with `DWT->CYCCNT`. Confirm `AUDIO_BLOCK_SIZE` and which context `DSP_ProcessBlock` runs in.
2. **Frame stage, segmenter, call LED, call log.** No classification yet. Write the numpy reference for stages 1–3 first, then port it. Pass when device features match numpy on the same capture to within one bin in frequency and one frame in duration.
3. **Shape groups, rule table, voting, status line.** Run the numpy version over your own recorded passes and score it before porting.
4. **Tuner, auto-record, GUANO chunk.** Mostly plumbing into code that already exists.
5. **Trained models from your own call log.** Train per group offline, export weights to a C header, and adopt a model for a group only where it beats the rules on held-out passes.

### Bench signals

Injected from a signal generator into the front end, bypassing the mic, so the DSP is tested separately from the transducer.

| Signal | Imitates | Expect |
| --- | --- | --- |
| 82 kHz tone, 50 ms bursts every 100 ms | Greater horseshoe | CF group, `f_peak` 82 ± 0.5 kHz, duration 50 ± 1 ms |
| Linear chirp 100 → 30 kHz in 3 ms, every 100 ms | *Myotis* | Steep FM, `bw` about 70 kHz, `flat_frac` near 0 |
| Chirp 70 → 47 kHz in 2 ms, then 46 → 45 kHz over 4 ms, every 90 ms | Common pipistrelle | QCF, `f_char` about 45 kHz, label PIPPIP after 5 calls |
| Same, alternating 20 and 24 kHz tails | Noctule | Nyctaloid, alternation flag set |
| Continuous 40 kHz tone plus the pipistrelle chirp | Bush-cricket under a bat | Tone whitened out within \~0.5 s; pipistrelle still detected |
| All of the above while recording to SD and redrawing the display | Worst case | `block_drops` = 0, `worst_cycles` under one half-period |

### Field scoring

Run the device beside its own WAVs: label every recorded pass with a desktop classifier plus your own checks, then compare with the device verdicts. Track two numbers per shape group:

- **Wrong-species rate** — the device named a species and it was wrong. This is the one to drive down.
- **Fallback rate** — the device gave a group where a species was possible. This is the price of honesty.

`LEAF_SHARE`, `AMBIG` and `LEAF_MIN_CALLS` trade one against the other. Tune them per group from these two numbers rather than by eye.

## Open questions

- [ ] Where does `DSP_ProcessBlock` run today, DMA interrupt or main loop, and what is `AUDIO_BLOCK_SIZE`? This fixes the interrupt wiring and must be a multiple of 128.
- [ ] How much CPU and SRAM are left, and is the CCM SRAM free? This decides hop 128 or 256, and CMSIS or the hand-rolled FFT.
- [ ] What does the SPVA1A0LR5H-1 actually deliver at 100–115 kHz? Until it is measured, the lesser horseshoe rule may never fire.
- [ ] Is GPS in V2? It decides whether the region mask comes from a menu or from position, and fills `Loc Position` in GUANO.
- [ ] Should auto-tune be the default, or only switched on by the user?
- [ ] Should auto-record default on or off, given battery and SD capacity?
- [ ] Is a 1.95 kHz waterfall acceptable if it frees the 1024-point FFT's CPU?
- [ ] 50% or 75% frame overlap, once the real load is known?
