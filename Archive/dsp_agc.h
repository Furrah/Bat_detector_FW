/*
 * dsp_agc.h
 *
 *  Created on: 6 Aug 2026
 *      Author: Joe Spencer
 */

#ifndef DSP_DSP_AGC_H_
#define DSP_DSP_AGC_H_

#define AGC_TARGET      800.0f
#define AGC_MAX_GAIN    8.0f
#define AGC_MIN_GAIN    0.25f

#define AGC_ATTACK      0.05f
#define AGC_RELEASE     0.001f



float AGC_Process(float input);

#endif /* DSP_DSP_AGC_H_ */
