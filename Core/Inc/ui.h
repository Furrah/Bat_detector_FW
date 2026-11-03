/*
 * ui.h
 *
 * The front panel: the two buttons, the two knobs, the LEDs and the display's
 * header line, plus the waterfall's rows (with auto-tune) and the battery
 * readout.
 *
 *   RECORD            start or stop a recording (recorder.h)
 *   PLAYBACK, short   start or stop time-expansion playback (on release)
 *   PLAYBACK, 0.8 s   the next tuning mode: manual (the LO knob) or auto
 *   LO knob           the heterodyne LO, 0-120 kHz, in manual mode
 *   Volume knob       the volume
 *   LED0              a bat pass is in progress (bat_detect.h)
 *   LED1              a flash for each call during a pass
 *
 * The header line shows the LO ("LO: 45.0 kHz" or "AUTO 43.5kHz"), the
 * playback countdown, the recording time, or a message for 2 s.
 */

#ifndef UI_H_
#define UI_H_

/* Main loop, once the audio and the waterfall have started */
void UI_Init(void);

/* Main loop: every 10 ms, the knobs, buttons, header line and LEDs */
void UI_Poll(void);

/* Main loop, after FFT_Process: the waterfall's rows and the battery readout */
void UI_PollDisplay(void);

#endif /* UI_H_ */
