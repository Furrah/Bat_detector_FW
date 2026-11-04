/*
 * classifier.h
 *
 * The bat species classifier on the board (Docs/CLASSIFIER_PLAN.md). When
 * the detector starts a pass, it reads the pass back from the PSRAM ring in
 * the main loop, a moment behind the live audio, from 2 s before the pass
 * began: stages 1-3 (classifier_calls.c) find and measure the calls, stages
 * 4-6 (classifier_species.c) name the bat. Each finished sequence's verdict
 * goes to the header line ("PIPPIP 87%  9") and, with every call, to
 * CALLS.CSV on the SD card (call_log.c).
 *
 * It stands aside while a recording is open or saving and during playback,
 * and it never reads audio about to be overwritten: more than 12 s behind,
 * it ends the session and starts again on newer audio.
 */

#ifndef DSP_CLASSIFIER_H_
#define DSP_CLASSIFIER_H_

#include <stdint.h>

/* 1 reports passes named in the faint-sweep group: "FAINT" (long-eared
 * bat/barbastelle), "PLESP", "BARBAR". Off: indoor noise and taps are named
 * this way too easily, so they're neither shown nor logged (the user, Nov 4). */
#define CLASSIFIER_REPORT_FAINT 0

void Classifier_Init(void);

/* Main loop: a few blocks of the pass at a time */
void Classifier_Poll(void);

/* The latest reported verdict's header text (14 bytes), once: 1 if there's a new one */
uint8_t Classifier_TakeVerdict(char *text);

/* For Live Expressions: the most CPU cycles one ring block (4 frames) has
 * taken, and how many times the classifier fell behind and skipped audio */
extern volatile uint32_t classifierBlockCyclesMax;
extern volatile uint32_t classifierSkips;

#endif /* DSP_CLASSIFIER_H_ */
