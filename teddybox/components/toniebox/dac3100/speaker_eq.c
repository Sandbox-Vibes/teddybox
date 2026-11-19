#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "speaker_eq.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* normalized biquad, a0 = 1 */
typedef struct
{
    double b0, b1, b2, a1, a2;
} bq_t;

static const bq_t bq_flat = {1, 0, 0, 0, 0};

/* RBJ audio EQ cookbook */
static bq_t bq_highpass(double fs, double f, double q)
{
    double w = 2 * M_PI * f / fs, c = cos(w), alpha = sin(w) / (2 * q), a0 = 1 + alpha;
    bq_t r = {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0};
    return r;
}

static bq_t bq_peak(double fs, double f, double db, double q)
{
    double A = pow(10, db / 40), w = 2 * M_PI * f / fs, c = cos(w), alpha = sin(w) / (2 * q), a0 = 1 + alpha / A;
    bq_t r = {(1 + alpha * A) / a0, -2 * c / a0, (1 - alpha * A) / a0, -2 * c / a0, (1 - alpha / A) / a0};
    return r;
}

static int16_t q15(double v)
{
    long r = lround(v * 32768.0);
    return r > 32767 ? 32767 : (r < -32768 ? -32768 : (int16_t)r);
}

static dac3100_biquad_t to_dac(bq_t b)
{
    dac3100_biquad_t r = {q15(b.b0), q15(b.b1 / 2), q15(b.b2), q15(-b.a1 / 2), q15(-b.a2)};
    return r;
}

void speaker_eq_design(speaker_eq_t mode, double fs, dac3100_biquad_t bq[SPEAKER_EQ_BIQUADS])
{
    bq_t f[SPEAKER_EQ_BIQUADS];
    for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
    {
        f[i] = bq_flat;
    }
    if (mode == SPEAKER_EQ_LOWCUT || mode == SPEAKER_EQ_FULL)
    {
        /* 4th order Butterworth = two 2nd order sections with these Q values */
        f[0] = bq_highpass(fs, SPEAKER_EQ_HPF_HZ, 0.54119610);
        f[1] = bq_highpass(fs, SPEAKER_EQ_HPF_HZ, 1.30656296);
    }
    if (mode == SPEAKER_EQ_FULL)
    {
        f[2] = bq_peak(fs, SPEAKER_EQ_BOX_HZ, SPEAKER_EQ_BOX_DB, SPEAKER_EQ_BOX_Q);
        f[3] = bq_peak(fs, SPEAKER_EQ_HARSH_HZ, SPEAKER_EQ_HARSH_DB, SPEAKER_EQ_HARSH_Q);
    }
    for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
    {
        bq[i] = to_dac(f[i]);
    }
}

static const char *const names[SPEAKER_EQ_COUNT] = {"off", "lowcut", "full"};

const char *speaker_eq_name(speaker_eq_t mode)
{
    return (unsigned)mode < SPEAKER_EQ_COUNT ? names[mode] : "?";
}

int speaker_eq_parse(const char *s)
{
    for (int i = 0; i < SPEAKER_EQ_COUNT; i++)
    {
        if (!strcmp(s, names[i]))
        {
            return i;
        }
    }
    if (s[0] >= '0' && s[0] < '0' + SPEAKER_EQ_COUNT && !s[1])
    {
        return s[0] - '0';
    }
    return -1;
}
