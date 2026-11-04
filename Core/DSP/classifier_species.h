/*
 * classifier_species.h
 *
 * Stages 4-6 of the species classifier: calls in (classifier_calls.h),
 * a verdict per sequence out.
 *   4. Shape group: CF, QCF, steep FM, faint FM; rising calls don't vote.
 *   5. Species rules: each call against the European ranges of its group.
 *   6. Sequences: calls less than 1 s apart; the last 10 vote.
 * A port of Tools/species.py, with the same taxa, rules and tie-breaks. No
 * HAL: Tools/check_classifier_c.py builds it on the PC.
 */

#ifndef DSP_CLASSIFIER_SPECIES_H_
#define DSP_CLASSIFIER_SPECIES_H_

#include <stddef.h>
#include <stdint.h>

#include "classifier_calls.h"


/* Shape groups (stage 4) */
typedef enum
{
    CLS_GROUP_NONE = 0,
    CLS_GROUP_CF,
    CLS_GROUP_QCF,
    CLS_GROUP_STEEP_FM,
    CLS_GROUP_FAINT_FM,
    CLS_GROUP_RISING
} ClsGroup_t;

/* Regions whose rules apply (species.py: island species are off by default) */
#define CLS_REGION_MAINLAND     1U
#define CLS_REGION_CRETE        2U
#define CLS_REGION_MADEIRA      4U

/* A finished sequence */
typedef struct
{
    uint8_t taxon;          /* index into the taxa (ClsSpecies_Code) */
    uint8_t reported;       /* CLS_SEQ_MIN_CALLS or more: a bat */
    uint16_t calls;
    uint16_t echoes;
    float confidence;       /* 0-1 */
    uint32_t firstFrame;    /* frame0 of its first and last calls */
    uint32_t lastFrame;
} ClsVerdict_t;


void ClsSpecies_Reset(void);

/* CLS_REGION_... bits; mainland by default */
void ClsSpecies_SetRegions(uint8_t regions);

/* Stages 4-5 for one call, in time order: fills its group, label, score and
 * echo flag. Returns 1 and fills *finished if this call started a new
 * sequence and so closed the one before. */
uint8_t ClsSpecies_AddCall(ClsCall_t *call, ClsVerdict_t *finished);

/* End of the audio: close the open sequence. Returns 1 if there was one. */
uint8_t ClsSpecies_Flush(ClsVerdict_t *finished);

/* 1 if the taxon is the faint-sweep group or in it: "FAINT", "PLESP", "BARBAR" */
uint8_t ClsSpecies_IsFaint(uint8_t taxon);

/* The taxon's 6-letter code ("PIPPIP"), and a group's name ("QCF") */
const char *ClsSpecies_Code(uint8_t taxon);
const char *ClsSpecies_GroupName(uint8_t group);

/* The header line's 13 characters: "PIPPIP 87%  9" (out: 14 bytes) */
void ClsSpecies_HeaderText(const ClsVerdict_t *v, char *out);

/* One CSV line each, ending in "\r\n" (the PC model writes the same):
 *   C,hop,dur_ms,f_start,f_end,f_peak,f_char,bw,slope_start,slope_mid,slope_end,flat_frac,snr_db,harmonic,flags,group,label,score,echo
 *   P,first_hop,last_hop,calls,echoes,taxon,confidence,reported
 * hopOffset is added to the frame numbers (e.g. the ring position of frame 0).
 * Return the length, or 0 if it didn't fit. */
size_t ClsSpecies_FormatCall(const ClsCall_t *c, uint32_t hopOffset, char *out, size_t size);
size_t ClsSpecies_FormatVerdict(const ClsVerdict_t *v, uint32_t hopOffset, char *out, size_t size);

/* The CSV's first line: with call lines (their columns; the P lines follow
 * the format above), or with pass lines only */
extern const char clsCsvHeader[];
extern const char clsCsvPassHeader[];

#endif /* DSP_CLASSIFIER_SPECIES_H_ */
