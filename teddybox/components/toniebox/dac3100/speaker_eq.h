#pragma once

/*
 * Equalizer for the built-in speaker, run by the DAC3100 itself (biquads A-E
 * of processing block PRB_P25, no CPU load). Plain C so it can be tested on a PC.
 *
 * Basis: two measurements of a box with the EQ off (flat), phone microphone,
 * third-octave bands relative to 1 kHz (2026-11-21), repeatable within about
 * 1.5 dB: steep bass roll-off below 250 Hz (speaker), +4.6/+4.9 dB at
 * 400/500 Hz (closed foam cube), +3.5 dB at 1.6 kHz, a dip of -3.7 dB at
 * 3.15 kHz, above 10 kHz not reliable (microphone). Only cuts, so nothing
 * can clip.
 */

#include <stdint.h>

typedef enum
{
    SPEAKER_EQ_OFF = 0,    /* flat, like the original firmware */
    SPEAKER_EQ_LOWCUT = 1, /* only the high-pass */
    SPEAKER_EQ_FULL = 2,   /* high-pass plus two gentle cuts */
    SPEAKER_EQ_COUNT
} speaker_eq_t;

#define SPEAKER_EQ_DEFAULT SPEAKER_EQ_FULL
#define SPEAKER_EQ_BIQUADS 5

/* 4th order Butterworth high-pass: the speaker can not reproduce this range,
 * it only adds cone excursion and distortion at high volume */
#define SPEAKER_EQ_HPF_HZ 150.0
/* "boxy"/cardboard sound of the closed cube: measured peak at 400-500 Hz */
#define SPEAKER_EQ_BOX_HZ 470.0
#define SPEAKER_EQ_BOX_DB -5.0
#define SPEAKER_EQ_BOX_Q 1.4
/* measured +3.5 dB at 1.6 kHz (the former cut at 3.2 kHz sat in a dip) */
#define SPEAKER_EQ_PRESENCE_HZ 1650.0
#define SPEAKER_EQ_PRESENCE_DB -3.0
#define SPEAKER_EQ_PRESENCE_Q 1.8

/* DAC3100 coefficient format (1.15):
 * H(z) = (N0 + 2*N1 z^-1 + N2 z^-2) / (2^15 - 2*D1 z^-1 - D2 z^-2) */
typedef struct
{
    int16_t n0, n1, n2, d1, d2;
} dac3100_biquad_t;

/* fills all SPEAKER_EQ_BIQUADS entries, unused ones are pass-through */
void speaker_eq_design(speaker_eq_t mode, double fs, dac3100_biquad_t bq[SPEAKER_EQ_BIQUADS]);

/* "off", "lowcut", "full"; parse returns -1 if unknown (also accepts "0".."2") */
const char *speaker_eq_name(speaker_eq_t mode);
int speaker_eq_parse(const char *s);
