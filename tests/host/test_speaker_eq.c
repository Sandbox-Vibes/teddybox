/* frequency response of the speaker EQ as the DAC3100 computes it, from the
 * rounded 16 bit coefficients:  H(z) = (N0 + 2 N1 z^-1 + N2 z^-2) / (2^15 - 2 D1 z^-1 - D2 z^-2)
 * ./test_speaker_eq csv   prints the curves */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include "speaker_eq.h"

static double response_db(const dac3100_biquad_t *bq, double fs, double f)
{
    double complex z1 = cexp(-I * 2 * M_PI * f / fs), h = 1;
    for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
    {
        double complex num = bq[i].n0 + 2.0 * bq[i].n1 * z1 + bq[i].n2 * z1 * z1;
        double complex den = 32768.0 - 2.0 * bq[i].d1 * z1 - bq[i].d2 * z1 * z1;
        h *= num / den;
    }
    return 20 * log10(cabs(h));
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    const double fs = 48000;
    dac3100_biquad_t eq[SPEAKER_EQ_COUNT][SPEAKER_EQ_BIQUADS];
    for (int m = 0; m < SPEAKER_EQ_COUNT; m++)
    {
        speaker_eq_design(m, fs, eq[m]);
    }

    if (argc > 1 && !strcmp(argv[1], "csv"))
    {
        printf("hz,off,lowcut,full\n");
        for (double f = 20; f <= 20000; f *= pow(2, 1.0 / 24))
        {
            printf("%.1f,%.2f,%.2f,%.2f\n", f, response_db(eq[0], fs, f), response_db(eq[1], fs, f), response_db(eq[2], fs, f));
        }
        return 0;
    }

    for (int m = 0; m < SPEAKER_EQ_COUNT; m++)
    {
        for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
        {
            const dac3100_biquad_t *b = &eq[m][i];
            /* denominator 1 - a1 z^-1 - a2 z^-2 with a1 = D1/2^14, a2 = D2/2^15 */
            double a1 = b->d1 / 16384.0, a2 = b->d2 / 32768.0;
            CHECK(fabs(a2) < 1 && fabs(a1) < 1 - a2, "mode %d biquad %d unstable", m, i);
        }
        /* never louder than the input anywhere: no clipping from the EQ */
        double max = -100;
        for (double f = 20; f <= 20000; f *= 1.01)
        {
            double r = response_db(eq[m], fs, f);
            max = r > max ? r : max;
        }
        CHECK(max < 0.05, "mode %d has %.2f dB gain", m, max);
    }

    /* off: flat (default coefficients, 0x7FFF = 1 - 2^-15) */
    CHECK(fabs(response_db(eq[0], fs, 100)) < 0.01 && fabs(response_db(eq[0], fs, 10000)) < 0.01, "off is flat");
    CHECK(eq[0][0].n0 == 0x7FFF && eq[0][0].n1 == 0 && eq[0][0].d1 == 0, "off uses the DAC defaults");

    /* high-pass: 4th order Butterworth, -3 dB at the corner, 24 dB/octave */
    CHECK(fabs(response_db(eq[1], fs, SPEAKER_EQ_HPF_HZ) + 3.01) < 0.2, "-3 dB at %.0f Hz: %.2f", SPEAKER_EQ_HPF_HZ, response_db(eq[1], fs, SPEAKER_EQ_HPF_HZ));
    CHECK(fabs(response_db(eq[1], fs, SPEAKER_EQ_HPF_HZ / 2) + 24.1) < 0.6, "one octave below: %.2f", response_db(eq[1], fs, SPEAKER_EQ_HPF_HZ / 2));
    CHECK(fabs(response_db(eq[1], fs, 1000)) < 0.05, "lowcut flat at 1 kHz: %.3f", response_db(eq[1], fs, 1000));
    CHECK(fabs(response_db(eq[1], fs, 15000)) < 0.05, "lowcut flat at 15 kHz");

    /* full: the two cuts, close to their nominal depth (the high-pass adds a little near 400 Hz) */
    double box = response_db(eq[2], fs, SPEAKER_EQ_BOX_HZ) - response_db(eq[1], fs, SPEAKER_EQ_BOX_HZ);
    double harsh = response_db(eq[2], fs, SPEAKER_EQ_PRESENCE_HZ) - response_db(eq[1], fs, SPEAKER_EQ_PRESENCE_HZ);
    CHECK(fabs(box - SPEAKER_EQ_BOX_DB) < 0.15, "470 Hz cut %.2f", box);
    /* the box cut adds a little at 1.65 kHz */
    CHECK(fabs(harsh - SPEAKER_EQ_PRESENCE_DB) < 0.5, "1.65 kHz cut %.2f", harsh);
    /* the measured dip at 3.15 kHz is left alone */
    CHECK(response_db(eq[2], fs, 3150) > -1.0, "3.15 kHz nearly untouched: %.2f", response_db(eq[2], fs, 3150));
    CHECK(fabs(response_db(eq[2], fs, SPEAKER_EQ_SIBILANCE_HZ) - SPEAKER_EQ_SIBILANCE_DB) < 0.3, "6.3 kHz cut %.2f", response_db(eq[2], fs, SPEAKER_EQ_SIBILANCE_HZ));
    CHECK(fabs(response_db(eq[2], fs, 12000)) < 0.3, "12 kHz nearly untouched: %.2f", response_db(eq[2], fs, 12000));

    CHECK(speaker_eq_parse("full") == SPEAKER_EQ_FULL && speaker_eq_parse("1") == SPEAKER_EQ_LOWCUT && speaker_eq_parse("x") == -1, "parse");

    printf("speaker_eq: lowcut %.1f dB @75 Hz, %.1f dB @150 Hz, %.2f dB @1 kHz; full %.1f dB @470 Hz, %.1f dB @1.65 kHz\n",
           response_db(eq[1], fs, 75), response_db(eq[1], fs, 150), response_db(eq[1], fs, 1000),
           response_db(eq[2], fs, 470), response_db(eq[2], fs, 1650));
    printf("speaker_eq: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
