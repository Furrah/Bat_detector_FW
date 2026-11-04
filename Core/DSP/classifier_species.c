/*
 * classifier_species.c
 *
 * Stages 4-6 of the species classifier; see classifier_species.h. Each step
 * follows Tools/species.py, where the reasons for the rules and ranges are
 * given. The ranges are literature values, to be refitted in spring.
 */

#include "classifier_species.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


/* ------------------------------------------------------------------ taxa */

typedef enum { LEVEL_UNKNOWN = 0, LEVEL_GROUP, LEVEL_SPECIES } Level_t;

typedef enum
{
    T_BATSP = 0,
    T_RHISP, T_RHIFER, T_RHIBLA, T_RHIHI,
    T_PIPGRP, T_PIPPYG, T_MINSCH, T_PIPPIP, T_PIPCRE, T_PIPMAD, T_PIPKN, T_HYPSAV,
    T_NYCGRP, T_EPTNIL, T_NYCLEI, T_EPTSER, T_VESMUR, T_NYCNOC, T_NYCLAS, T_TADTEN,
    T_MYOSP, T_MYONAT, T_MYOEMA, T_MYOCAP, T_MYOMYO,
    T_FAINT, T_PLESP, T_BARBAR,
    T_COUNT
} Taxon_t;

#define T_NONE  0xFFU

typedef struct
{
    char code[7];
    uint8_t parent;     /* T_NONE for the root */
    uint8_t level;
} Taxon_s;

/* In species.py's order: the vote's ties go to the taxon seen first, and
 * the rules' ties to the later code, as there */
static const Taxon_s taxa[T_COUNT] =
{
    [T_BATSP]  = { "BATSP",  T_NONE,   LEVEL_UNKNOWN },
    [T_RHISP]  = { "RHISP",  T_BATSP,  LEVEL_GROUP },
    [T_RHIFER] = { "RHIFER", T_RHISP,  LEVEL_SPECIES },
    [T_RHIBLA] = { "RHIBLA", T_RHISP,  LEVEL_SPECIES },
    [T_RHIHI]  = { "RHIHI",  T_RHISP,  LEVEL_GROUP },
    [T_PIPGRP] = { "PIPGRP", T_BATSP,  LEVEL_GROUP },
    [T_PIPPYG] = { "PIPPYG", T_PIPGRP, LEVEL_SPECIES },
    [T_MINSCH] = { "MINSCH", T_PIPGRP, LEVEL_SPECIES },
    [T_PIPPIP] = { "PIPPIP", T_PIPGRP, LEVEL_SPECIES },
    [T_PIPCRE] = { "PIPCRE", T_PIPGRP, LEVEL_SPECIES },
    [T_PIPMAD] = { "PIPMAD", T_PIPGRP, LEVEL_SPECIES },
    [T_PIPKN]  = { "PIPKN",  T_PIPGRP, LEVEL_GROUP },
    [T_HYPSAV] = { "HYPSAV", T_BATSP,  LEVEL_SPECIES },
    [T_NYCGRP] = { "NYCGRP", T_BATSP,  LEVEL_GROUP },
    [T_EPTNIL] = { "EPTNIL", T_NYCGRP, LEVEL_SPECIES },
    [T_NYCLEI] = { "NYCLEI", T_NYCGRP, LEVEL_SPECIES },
    [T_EPTSER] = { "EPTSER", T_NYCGRP, LEVEL_GROUP },
    [T_VESMUR] = { "VESMUR", T_NYCGRP, LEVEL_SPECIES },
    [T_NYCNOC] = { "NYCNOC", T_NYCGRP, LEVEL_SPECIES },
    [T_NYCLAS] = { "NYCLAS", T_NYCGRP, LEVEL_SPECIES },
    [T_TADTEN] = { "TADTEN", T_BATSP,  LEVEL_SPECIES },
    [T_MYOSP]  = { "MYOSP",  T_BATSP,  LEVEL_GROUP },
    [T_MYONAT] = { "MYONAT", T_MYOSP,  LEVEL_GROUP },
    [T_MYOEMA] = { "MYOEMA", T_MYOSP,  LEVEL_GROUP },
    [T_MYOCAP] = { "MYOCAP", T_MYOSP,  LEVEL_SPECIES },
    [T_MYOMYO] = { "MYOMYO", T_MYOSP,  LEVEL_GROUP },
    [T_FAINT]  = { "FAINT",  T_BATSP,  LEVEL_GROUP },
    [T_PLESP]  = { "PLESP",  T_FAINT,  LEVEL_GROUP },
    [T_BARBAR] = { "BARBAR", T_FAINT,  LEVEL_SPECIES },
};

static const char *const groupNames[] = { "none", "CF", "QCF", "steep FM", "faint FM", "rising" };

/* The group a call of each shape falls back to when no species fits */
static const uint8_t groupRoot[] = { T_BATSP, T_RHISP, T_BATSP, T_MYOSP, T_FAINT, T_BATSP };


static uint8_t Taxon_Depth(uint8_t t)
{
    uint8_t depth = 0U;

    for(; t != T_NONE; t = taxa[t].parent)
    {
        depth++;
    }
    return depth;
}

/* t is a or one of its ancestors */
static uint8_t Taxon_Within(uint8_t a, uint8_t t)
{
    for(; a != T_NONE; a = taxa[a].parent)
    {
        if(a == t)
        {
            return 1U;
        }
    }
    return 0U;
}

static uint8_t Taxon_CommonParent(uint8_t a, uint8_t b)
{
    for(; a != T_NONE; a = taxa[a].parent)
    {
        if(Taxon_Within(b, a))
        {
            return a;
        }
    }
    return T_BATSP;
}


/* ------------------------------------------------------------------ rules */

typedef enum { F_PEAK = 0, F_CHAR, F_END_X, F_BW_X, F_DUR } Feature_t;

static const float margin[] = { 2.5f, 2.0f, 2.0f, 5.0f, 1.0f };     /* per Feature_t */

typedef struct
{
    uint8_t feature;
    float lo;
    float hi;
} Range_t;

typedef struct
{
    uint8_t taxon;
    uint8_t group;
    uint8_t ranges;
    Range_t range[2];
    float durMean;      /* ms; 0: no length weighting */
    uint8_t regions;
} Rule_t;

#define MAIN    CLS_REGION_MAINLAND

static const Rule_t rules[] =
{
    /* horseshoes: the constant frequency */
    { T_RHIFER, CLS_GROUP_CF, 1, { { F_PEAK, 81.0f, 84.0f } }, 0.0f, MAIN },
    { T_RHIBLA, CLS_GROUP_CF, 1, { { F_PEAK, 92.0f, 98.0f } }, 0.0f, MAIN },
    { T_RHIHI,  CLS_GROUP_CF, 1, { { F_PEAK, 102.0f, 112.0f } }, 0.0f, MAIN },

    /* flat tails: the characteristic frequency (no rule for Schreiber's bat: species.py) */
    { T_PIPPYG, CLS_GROUP_QCF, 1, { { F_CHAR, 53.0f, 58.0f } }, 6.0f, MAIN },
    { T_PIPPIP, CLS_GROUP_QCF, 1, { { F_CHAR, 42.0f, 49.0f } }, 6.3f, MAIN },
    { T_PIPCRE, CLS_GROUP_QCF, 1, { { F_CHAR, 44.0f, 55.0f } }, 4.3f, CLS_REGION_CRETE },
    { T_PIPMAD, CLS_GROUP_QCF, 1, { { F_CHAR, 44.0f, 47.0f } }, 6.0f, CLS_REGION_MADEIRA },
    { T_PIPKN,  CLS_GROUP_QCF, 1, { { F_CHAR, 35.0f, 42.0f } }, 6.6f, MAIN },
    { T_HYPSAV, CLS_GROUP_QCF, 1, { { F_CHAR, 30.0f, 35.0f } }, 7.3f, MAIN },
    { T_EPTNIL, CLS_GROUP_QCF, 1, { { F_CHAR, 28.0f, 30.0f } }, 10.7f, MAIN },
    { T_NYCLEI, CLS_GROUP_QCF, 1, { { F_CHAR, 22.0f, 28.0f } }, 9.3f, MAIN },
    { T_EPTSER, CLS_GROUP_QCF, 1, { { F_CHAR, 23.0f, 29.0f } }, 10.9f, MAIN },
    { T_VESMUR, CLS_GROUP_QCF, 1, { { F_CHAR, 24.0f, 26.0f } }, 15.0f, MAIN },
    { T_NYCNOC, CLS_GROUP_QCF, 1, { { F_CHAR, 18.0f, 25.0f } }, 14.4f, MAIN },
    { T_NYCLAS, CLS_GROUP_QCF, 2, { { F_CHAR, 12.0f, 16.0f }, { F_DUR, 12.0f, INFINITY } }, 25.0f, MAIN },
    { T_TADTEN, CLS_GROUP_QCF, 2, { { F_CHAR, 9.0f, 14.0f }, { F_DUR, 8.0f, INFINITY } }, 16.8f, MAIN },

    /* Myotis: the end of the sweep, then bandwidth and length */
    { T_MYONAT, CLS_GROUP_STEEP_FM, 2, { { F_END_X, 10.0f, 20.0f }, { F_BW_X, 80.0f, INFINITY } }, 4.1f, MAIN },
    { T_MYOEMA, CLS_GROUP_STEEP_FM, 1, { { F_END_X, 36.0f, 45.0f } }, 3.6f, MAIN },
    { T_MYOCAP, CLS_GROUP_STEEP_FM, 2, { { F_END_X, 32.0f, 35.0f }, { F_DUR, 5.0f, 10.0f } }, 5.2f, MAIN },
    { T_MYOMYO, CLS_GROUP_STEEP_FM, 2, { { F_END_X, 20.0f, 26.0f }, { F_DUR, 5.0f, 14.0f } }, 6.0f, MAIN },

    /* faint sweeps: long-eared bats by peak; the barbastelle comes from stage 6 */
    { T_PLESP,  CLS_GROUP_FAINT_FM, 1, { { F_PEAK, 26.0f, 40.0f } }, 4.0f, MAIN },
};

#define RULE_COUNT  (sizeof(rules) / sizeof(rules[0]))


static float Feature(const ClsCall_t *c, uint8_t feature)
{
    switch(feature)
    {
        case F_PEAK:  return c->f_peak;
        case F_CHAR:  return c->f_char;
        case F_END_X: return c->f_end_x;
        case F_BW_X:  return c->bw_x;
        default:      return c->dur_ms;
    }
}

static float RangeScore(float x, float lo, float hi, float m)
{
    if((x >= lo) && (x <= hi))
    {
        return 1.0f;
    }
    const float d = (x < lo) ? (lo - x) : (x - hi);
    return (d >= m) ? 0.0f : (1.0f - d / m);
}

/* 1 within 0.6-1.6x of the mean length, falling to 0.5 at half or double */
static float DurWeight(float dur, float mean)
{
    if(mean <= 0.0f)
    {
        return 1.0f;
    }
    const float r = dur / mean;
    if((r >= 0.6f) && (r <= 1.6f))
    {
        return 1.0f;
    }
    return fmaxf(0.5f, 1.0f - 0.5f * ((r < 0.6f) ? ((0.6f - r) / 0.1f) : ((r - 1.6f) / 0.4f)));
}

/* species.py sorts (score, code) high to low: equal scores go to the later code */
static uint8_t Ranks_Above(float sa, uint8_t ta, float sb, uint8_t tb)
{
    return (sa > sb) || ((sa == sb) && (strcmp(taxa[ta].code, taxa[tb].code) > 0));
}


/* ------------------------------------------------------------------ stages 4-5 */

static uint8_t regions = CLS_REGION_MAINLAND;

static void Derived(ClsCall_t *c)
{
    if(c->dur_ms >= CLS_EXTRAP_MIN_MS)
    {
        c->f_start_x = fminf(c->f_start + fmaxf(c->slope_start, 0.0f) * CLS_END_EXTRAP_MS, CLS_BAND_HI_KHZ);
        c->f_end_x = fmaxf(c->f_end - fmaxf(c->slope_end, 0.0f) * CLS_END_EXTRAP_MS, CLS_BAND_LO_KHZ);
    }
    else
    {
        c->f_start_x = c->f_start;
        c->f_end_x = c->f_end;
    }
    c->bw_x = c->f_start_x - c->f_end_x;
    c->flat_ms = c->flat_frac * c->dur_ms;
}

static uint8_t ShapeGroup(const ClsCall_t *c)
{
    if((c->bw_x < -CLS_RISING_KHZ) || (c->f_peak < CLS_BAND_EDGE_KHZ))
    {
        return CLS_GROUP_RISING;
    }
    if((c->flat_frac >= 0.6f) && (c->dur_ms >= 15.0f) && (c->f_peak >= 70.0f))
    {
        return CLS_GROUP_CF;
    }
    if(((c->flat_ms >= 2.0f) || (c->bw_x < CLS_NARROW_KHZ)) && (c->f_char <= 60.0f))
    {
        return CLS_GROUP_QCF;
    }
    if((c->harmonic || (c->bw_x < 30.0f)) && (c->bw_x >= CLS_NARROW_KHZ) && (c->f_start_x <= 60.0f)
            && (c->flat_ms < 2.0f))
    {
        return CLS_GROUP_FAINT_FM;
    }
    if((c->bw_x >= 30.0f) && (c->flat_ms < 2.0f))
    {
        return CLS_GROUP_STEEP_FM;
    }
    return CLS_GROUP_NONE;
}

/* The call's label and score within its group */
static void ClassifyCall(ClsCall_t *c)
{
    float bestS = 0.0f, secondS = 0.0f;
    uint8_t best = T_NONE, second = T_NONE;

    for(uint32_t r = 0; r < RULE_COUNT; r++)
    {
        const Rule_t *rule = &rules[r];

        if((rule->group != c->group) || !(rule->regions & regions))
        {
            continue;
        }

        float s = DurWeight(c->dur_ms, rule->durMean);

        for(uint32_t i = 0; i < rule->ranges; i++)
        {
            s *= RangeScore(Feature(c, rule->range[i].feature), rule->range[i].lo, rule->range[i].hi,
                            margin[rule->range[i].feature]);
        }

        if((best == T_NONE) || Ranks_Above(s, rule->taxon, bestS, best))
        {
            second = best;
            secondS = bestS;
            best = rule->taxon;
            bestS = s;
        }
        else if((second == T_NONE) || Ranks_Above(s, rule->taxon, secondS, second))
        {
            second = rule->taxon;
            secondS = s;
        }
    }

    /* no species: the group of the shape, or of the band a flat tail is in */
    uint8_t root = groupRoot[c->group];

    if(c->group == CLS_GROUP_QCF)
    {
        if((c->f_char >= 33.0f) && (c->f_char <= 60.0f))
        {
            root = T_PIPGRP;
        }
        else if((c->f_char >= 17.0f) && (c->f_char <= 31.0f))
        {
            root = T_NYCGRP;
        }
    }

    if((best == T_NONE) || (bestS < CLS_MIN_SCORE))
    {
        c->label = root;
        c->score = 0.5f;
    }
    else if((second != T_NONE) && (secondS > CLS_AMBIG * bestS))
    {
        c->label = Taxon_CommonParent(best, second);
        c->score = bestS;
    }
    else
    {
        c->label = best;
        c->score = bestS;
    }
}

static float Quality(const ClsCall_t *c)
{
    float q = fminf(fmaxf((c->snr_db - 6.0f) / 18.0f, 0.2f), 1.0f);

    if(c->flags & CLS_CALL_GAPPED)
    {
        q *= 0.5f;
    }
    if(c->flags & CLS_CALL_CLIPPED)
    {
        q *= 0.7f;
    }
    return q;
}


/* ------------------------------------------------------------------ stage 6 */

/* What the vote needs of each call in the window */
typedef struct
{
    uint8_t label;
    uint8_t group;
    float score;
    float weight;
    float f_char;
    float dur_ms;
} Voter_t;

static Voter_t window[CLS_VOTE_N];      /* the sequence's last calls, a ring */
static uint32_t seqCalls;               /* calls in the open sequence; 0: none open */
static uint32_t seqEchoes;
static uint32_t seqFirstFrame;
static uint32_t seqLastFrame;
static float seqLastT;

static uint8_t prevValid;               /* the last voting call, for the echo test */
static float prevT;
static float prevFChar;
static float prevSnr;


/* f_char steps bigger than ALT_STEP that keep changing sign, in 60% of the steps */
static uint8_t Alternating(const float *values, uint32_t n)
{
    float steps[CLS_VOTE_N];
    uint32_t count = 0U;
    uint32_t changes = 0U;

    for(uint32_t i = 1; i < n; i++)
    {
        const float d = values[i] - values[i - 1U];
        if(fabsf(d) > CLS_ALT_STEP_KHZ)
        {
            steps[count++] = d;
        }
    }
    if(count < 3U)
    {
        return 0U;
    }
    for(uint32_t i = 1; i < count; i++)
    {
        changes += ((steps[i] > 0.0f) != (steps[i - 1U] > 0.0f)) ? 1U : 0U;
    }
    return (float)changes >= 0.6f * (float)(count - 1U);
}

static float Median(float *v, uint32_t n)
{
    for(uint32_t i = 1; i < n; i++)         /* insertion sort: n < 10 */
    {
        const float x = v[i];
        uint32_t j = i;
        for(; (j > 0U) && (v[j - 1U] > x); j--)
        {
            v[j] = v[j - 1U];
        }
        v[j] = x;
    }
    return (n & 1U) ? v[n / 2U] : (0.5f * (v[n / 2U - 1U] + v[n / 2U]));
}

/* Cues that exist only across calls */
static uint8_t SequenceRules(uint8_t taxon, const Voter_t *w, uint32_t n)
{
    float values[CLS_VOTE_N];
    uint32_t count = 0U;

    if(Taxon_Within(taxon, T_NYCGRP))
    {
        float lo = INFINITY, hi = -INFINITY;

        for(uint32_t i = 0; i < n; i++)
        {
            if(w[i].group == CLS_GROUP_QCF)
            {
                values[count++] = w[i].f_char;
                lo = fminf(lo, w[i].f_char);
                hi = fmaxf(hi, w[i].f_char);
            }
        }
        if(Alternating(values, count))
        {
            if(hi < 17.0f)
            {
                return T_NYCLAS;
            }
            if(lo <= 21.0f)
            {
                return T_NYCNOC;
            }
            if(lo > 22.0f)
            {
                return T_NYCLEI;
            }
        }
    }

    if(Taxon_Within(taxon, T_FAINT))
    {
        float durs[CLS_VOTE_N];
        uint32_t nd = 0U;

        for(uint32_t i = 0; i < n; i++)
        {
            if(w[i].group == CLS_GROUP_FAINT_FM)
            {
                durs[nd++] = w[i].dur_ms;
            }
        }
        count = 0U;
        for(uint32_t i = 1; i < nd; i++)
        {
            const float a = durs[i - 1U], b = durs[i];
            values[count++] = fmaxf(a, b) / fmaxf(fminf(a, b), 1e-3f);
        }
        if((count >= 3U) && (Median(values, count) >= 1.6f))
        {
            return T_BARBAR;
        }
    }

    return taxon;
}

/* The most specific taxon with LEAF_SHARE of the weighted vote */
static uint8_t Vote(const Voter_t *w, uint32_t n, float *share)
{
    float support[T_COUNT] = { 0 };
    uint8_t order[T_COUNT];
    uint8_t seen[T_COUNT] = { 0 };
    uint32_t orderCount = 0U;
    float total = 0.0f;

    for(uint32_t i = 0; i < n; i++)
    {
        total += w[i].weight;
        for(uint8_t t = w[i].label; t != T_NONE; t = taxa[t].parent)
        {
            if(!seen[t])
            {
                seen[t] = 1U;
                order[orderCount++] = t;
            }
            support[t] += w[i].weight;
        }
    }

    uint8_t best = T_NONE;
    uint8_t bestDepth = 0U;

    for(uint32_t i = 0; i < orderCount; i++)
    {
        const uint8_t t = order[i];
        const uint8_t depth = Taxon_Depth(t);

        if((total <= 0.0f) || ((support[t] / total) < CLS_LEAF_SHARE))
        {
            continue;
        }
        if((taxa[t].level == LEVEL_SPECIES) && (n < CLS_LEAF_MIN_CALLS))
        {
            continue;
        }
        if((best == T_NONE) || (depth > bestDepth) || ((depth == bestDepth) && (support[t] > support[best])))
        {
            best = t;
            bestDepth = depth;
        }
    }

    if(best == T_NONE)
    {
        *share = 1.0f;
        return T_BATSP;
    }
    *share = support[best] / total;
    return best;
}

static void Sequence_Finish(ClsVerdict_t *v)
{
    Voter_t w[CLS_VOTE_N];
    const uint32_t n = (seqCalls < CLS_VOTE_N) ? seqCalls : CLS_VOTE_N;
    float share;
    float scoreSum = 0.0f;
    uint32_t scoreCount = 0U;

    /* the window, oldest first */
    for(uint32_t i = 0; i < n; i++)
    {
        w[i] = window[(seqCalls - n + i) % CLS_VOTE_N];
    }

    const uint8_t voted = Vote(w, n, &share);

    for(uint32_t i = 0; i < n; i++)
    {
        if(Taxon_Within(w[i].label, voted))
        {
            scoreSum += w[i].score;
            scoreCount++;
        }
    }

    v->taxon = SequenceRules(voted, w, n);
    v->confidence = share * ((scoreCount > 0U) ? (scoreSum / (float)scoreCount) : 0.0f);
    v->calls = (uint16_t)((seqCalls > 0xFFFFU) ? 0xFFFFU : seqCalls);
    v->echoes = (uint16_t)((seqEchoes > 0xFFFFU) ? 0xFFFFU : seqEchoes);
    v->reported = seqCalls >= CLS_SEQ_MIN_CALLS;
    v->firstFrame = seqFirstFrame;
    v->lastFrame = seqLastFrame;

    seqCalls = 0U;
    seqEchoes = 0U;
}


/* ------------------------------------------------------------------ API */

void ClsSpecies_Reset(void)
{
    seqCalls = 0U;
    seqEchoes = 0U;
    prevValid = 0U;
}

void ClsSpecies_SetRegions(uint8_t r)
{
    regions = r;
}

uint8_t ClsSpecies_AddCall(ClsCall_t *c, ClsVerdict_t *finished)
{
    uint8_t closed = 0U;

    Derived(c);
    c->group = ShapeGroup(c);
    c->echo = 0U;
    c->label = CLS_NO_LABEL;
    c->score = 0.0f;
    c->weight = 0.0f;

    /* an echo: within 10 ms of the last call, in its band, 10 dB weaker */
    if(prevValid && ((c->t_s - prevT) <= CLS_ECHO_S) && (fabsf(c->f_char - prevFChar) <= CLS_ECHO_KHZ)
            && (c->snr_db <= (prevSnr - CLS_ECHO_DB)))
    {
        c->echo = 1U;
        if(seqCalls > 0U)
        {
            seqEchoes++;
        }
        return 0U;
    }

    if(c->group == CLS_GROUP_RISING)
    {
        return 0U;      /* not echolocation: no vote */
    }

    if(c->group == CLS_GROUP_NONE)
    {
        c->label = T_BATSP;
        c->score = 0.3f;
    }
    else
    {
        ClassifyCall(c);
    }
    c->weight = c->score * Quality(c);

    if((seqCalls > 0U) && ((c->t_s - seqLastT) > CLS_SEQ_GAP_S))
    {
        Sequence_Finish(finished);
        closed = 1U;
    }
    if(seqCalls == 0U)
    {
        seqFirstFrame = c->frame0;
    }

    window[seqCalls % CLS_VOTE_N] = (Voter_t){ c->label, c->group, c->score, c->weight, c->f_char, c->dur_ms };
    seqCalls++;
    seqLastFrame = c->frame0;
    seqLastT = c->t_s;

    prevValid = 1U;
    prevT = c->t_s;
    prevFChar = c->f_char;
    prevSnr = c->snr_db;

    return closed;
}

uint8_t ClsSpecies_Flush(ClsVerdict_t *finished)
{
    if(seqCalls == 0U)
    {
        return 0U;
    }
    Sequence_Finish(finished);
    return 1U;
}

uint8_t ClsSpecies_IsFaint(uint8_t taxon)
{
    return (taxon < T_COUNT) && Taxon_Within(taxon, T_FAINT);
}

const char *ClsSpecies_Code(uint8_t taxon)
{
    return (taxon < T_COUNT) ? taxa[taxon].code : "";
}

const char *ClsSpecies_GroupName(uint8_t group)
{
    return (group < (sizeof(groupNames) / sizeof(groupNames[0]))) ? groupNames[group] : "";
}

void ClsSpecies_HeaderText(const ClsVerdict_t *v, char *out)
{
    long percent = lrintf(100.0f * v->confidence);

    percent = (percent > 99) ? 99 : ((percent < 0) ? 0 : percent);
    snprintf(out, 14, "%-6s %2ld%% %2u", ClsSpecies_Code(v->taxon), percent,
             (unsigned)((v->calls > 99U) ? 99U : v->calls));
}


/* ------------------------------------------------------------------ CSV */

const char clsCsvHeader[] =
    "type,hop,dur_ms,f_start,f_end,f_peak,f_char,bw,slope_start,slope_mid,slope_end,flat_frac,snr_db,"
    "harmonic,flags,group,label,score,echo\r\n";

const char clsCsvPassHeader[] = "type,first_hop,last_hop,calls,echoes,taxon,confidence,reported\r\n";

/* A line built into a fixed buffer: no float printf on the board (newlib-nano) */
typedef struct
{
    char *p;
    char *end;
    uint8_t ok;
} Writer_t;

static void W_Str(Writer_t *w, const char *s)
{
    while(*s)
    {
        if(w->p >= w->end)
        {
            w->ok = 0U;
            return;
        }
        *w->p++ = *s++;
    }
}

static void W_UInt(Writer_t *w, uint32_t v)
{
    char digits[11];
    uint32_t n = 0U;

    do
    {
        digits[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while(v > 0U);

    while(n > 0U)
    {
        char s[2] = { digits[--n], 0 };
        W_Str(w, s);
    }
}

/* v with the given decimals, rounded, as "-12.34" */
static void W_Fixed(Writer_t *w, float v, uint32_t decimals)
{
    uint32_t scale = 1U;

    for(uint32_t i = 0; i < decimals; i++)
    {
        scale *= 10U;
    }

    const long n = lrintf(v * (float)scale);
    const uint32_t mag = (uint32_t)((n < 0) ? -n : n);
    uint32_t frac = mag % scale;
    char digits[10];

    if(n < 0)
    {
        W_Str(w, "-");
    }
    W_UInt(w, mag / scale);
    if(decimals > 0U)
    {
        W_Str(w, ".");
        for(uint32_t i = decimals; i > 0U; i--)
        {
            digits[i - 1U] = (char)('0' + (frac % 10U));
            frac /= 10U;
        }
        digits[decimals] = 0;
        W_Str(w, digits);
    }
}

static size_t W_Done(Writer_t *w, char *out)
{
    if(w->ok && (w->p < w->end))
    {
        *w->p = 0;
        return (size_t)(w->p - out);
    }
    out[0] = 0;
    return 0U;
}

size_t ClsSpecies_FormatCall(const ClsCall_t *c, uint32_t hopOffset, char *out, size_t size)
{
    Writer_t w = { out, out + size, 1U };

    W_Str(&w, "C,");            W_UInt(&w, hopOffset + c->frame0);
    W_Str(&w, ",");             W_Fixed(&w, c->dur_ms, 3U);
    W_Str(&w, ",");             W_Fixed(&w, c->f_start, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->f_end, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->f_peak, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->f_char, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->bw, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->slope_start, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->slope_mid, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->slope_end, 2U);
    W_Str(&w, ",");             W_Fixed(&w, c->flat_frac, 3U);
    W_Str(&w, ",");             W_Fixed(&w, c->snr_db, 1U);
    W_Str(&w, ",");             W_UInt(&w, c->harmonic);
    W_Str(&w, ",");             W_UInt(&w, c->flags);
    W_Str(&w, ",");             W_Str(&w, ClsSpecies_GroupName(c->group));
    W_Str(&w, ",");             W_Str(&w, (c->label == CLS_NO_LABEL) ? "" : ClsSpecies_Code(c->label));
    W_Str(&w, ",");             W_Fixed(&w, c->score, 3U);
    W_Str(&w, ",");             W_UInt(&w, c->echo);
    W_Str(&w, "\r\n");

    return W_Done(&w, out);
}

size_t ClsSpecies_FormatVerdict(const ClsVerdict_t *v, uint32_t hopOffset, char *out, size_t size)
{
    Writer_t w = { out, out + size, 1U };

    W_Str(&w, "P,");            W_UInt(&w, hopOffset + v->firstFrame);
    W_Str(&w, ",");             W_UInt(&w, hopOffset + v->lastFrame);
    W_Str(&w, ",");             W_UInt(&w, v->calls);
    W_Str(&w, ",");             W_UInt(&w, v->echoes);
    W_Str(&w, ",");             W_Str(&w, ClsSpecies_Code(v->taxon));
    W_Str(&w, ",");             W_Fixed(&w, v->confidence, 3U);
    W_Str(&w, ",");             W_UInt(&w, v->reported);
    W_Str(&w, "\r\n");

    return W_Done(&w, out);
}
