/* maint_next_time(): next local hh:mm after now, across days and the DST changes (Europe/Berlin) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "maint.h"

static int fails;

static time_t local(int y, int mo, int d, int h, int mi)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1};
    return mktime(&tm);
}

static void check(const char *what, time_t got, time_t want)
{
    char g[32], w[32];
    struct tm tg, tw;
    localtime_r(&got, &tg);
    localtime_r(&want, &tw);
    strftime(g, sizeof g, "%Y-%m-%d %H:%M %Z", &tg);
    strftime(w, sizeof w, "%Y-%m-%d %H:%M %Z", &tw);
    int ok = got == want;
    fails += !ok;
    printf("%s %-46s %s (want %s)\n", ok ? "ok  " : "FAIL", what, g, w);
}

int main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    check("evening -> 03:10 next day", maint_next_time(local(2026, 11, 20, 20, 15), 3, 10, 0, 12 * 3600), local(2026, 11, 21, 3, 10));
    check("01:00 -> 03:10 same night", maint_next_time(local(2026, 11, 21, 1, 0), 3, 10, 0, 12 * 3600), local(2026, 11, 21, 3, 10));
    check("01:00, ran at 00:30 -> next night (12 h gap)", maint_next_time(local(2026, 11, 21, 1, 0), 3, 10, local(2026, 11, 21, 0, 30), 12 * 3600), local(2026, 11, 22, 3, 10));
    check("hour changed to 13 at 12:56, last run 07:44 -> 13:10 the same day (1 h gap)",
          maint_next_time(local(2026, 11, 23, 12, 56), 13, 10, local(2026, 11, 23, 7, 44), 3600), local(2026, 11, 23, 13, 10));
    check("ran at 13:05, now 13:50 -> next day (1 h gap)",
          maint_next_time(local(2026, 11, 23, 13, 50), 13, 0, local(2026, 11, 23, 13, 5), 3600), local(2026, 11, 24, 13, 0));
    check("right at 03:10 -> next day", maint_next_time(local(2026, 11, 21, 3, 10), 3, 10, 0, 12 * 3600), local(2026, 11, 22, 3, 10));
    check("month end", maint_next_time(local(2026, 11, 30, 22, 0), 3, 0, 0, 12 * 3600), local(2026, 12, 1, 3, 0));
    check("year end", maint_next_time(local(2026, 12, 31, 23, 0), 3, 0, 0, 12 * 3600), local(2027, 1, 1, 3, 0));
    /* 29.3.2026: 02:00 -> 03:00 (02:xx does not exist); 25.10.2026: 03:00 -> 02:00 */
    check("spring DST, 03:05 exists", maint_next_time(local(2026, 3, 28, 22, 0), 3, 5, 0, 12 * 3600), local(2026, 3, 29, 3, 5));
    check("autumn DST, 03:05 once", maint_next_time(local(2026, 10, 24, 22, 0), 3, 5, 0, 12 * 3600), local(2026, 10, 25, 3, 5));
    time_t a = maint_next_time(local(2026, 10, 24, 22, 0), 3, 5, 0, 12 * 3600);
    check("after autumn DST run -> next day 03:05", maint_next_time(a + 60, 3, 5, a, 12 * 3600), local(2026, 10, 26, 3, 5));
    check("spring DST at 02:20 (missing hour)", maint_next_time(local(2026, 3, 28, 22, 0), 2, 20, 0, 12 * 3600) > local(2026, 3, 28, 22, 0), 1);
    /* token names: same as the content file of the tonie from the box log */
    char dir[9], file[9];
    maint_token_names(0xE00403500EEA5646ULL, dir, file);
    int ok = !strcmp(dir, "4656EA0E") && !strcmp(file, "500304E0");
    fails += !ok;
    printf("%s token E00403500EEA5646 -> TOKENS/%s/%s\n", ok ? "ok  " : "FAIL", dir, file);
    /* deep sleep drift: the case seen (14.5 h, 9 min fast) and the conversions */
    {
        int64_t real = (14LL * 3600 + 30 * 60) * 1000000LL, box = real + 9LL * 60 * 1000000LL;
        int32_t ppm = maint_drift_ppm(box, real);
        int ok = ppm > 10300 && ppm < 10400;
        fails += !ok;
        printf("%s drift 9 min in 14.5 h = %ld ppm\n", ok ? "ok  " : "FAIL", (long)ppm);
        int64_t back = maint_drift_box_to_real(box, ppm);
        ok = llabs(back - real) < 2000000; /* ppm is rounded down */
        fails += !ok;
        printf("%s box -> real: %lld s off\n", ok ? "ok  " : "FAIL", (long long)((back - real) / 1000000));
        int64_t sleep = maint_drift_real_to_box(real, ppm);
        ok = llabs(sleep - box) < 2000000;
        fails += !ok;
        printf("%s real -> box (sleep timer): %lld s off\n", ok ? "ok  " : "FAIL", (long long)((sleep - box) / 1000000));
        ok = maint_drift_ppm(real, real) == 0 && maint_drift_ppm(real - 36000000LL, real) < 0 &&
             maint_drift_real_to_box(real, 0) == real && maint_drift_box_to_real(real, 0) == real &&
             maint_drift_box_to_real(maint_drift_real_to_box(5000000000LL, -20000), -20000) - 5000000000LL < 2;
        fails += !ok;
        printf("%s no drift, slow clock, round trip\n", ok ? "ok  " : "FAIL");
    }
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails;
}
