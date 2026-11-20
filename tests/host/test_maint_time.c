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
    check("right at 03:10 -> next day", maint_next_time(local(2026, 11, 21, 3, 10), 3, 10, 0, 12 * 3600), local(2026, 11, 22, 3, 10));
    check("month end", maint_next_time(local(2026, 11, 30, 22, 0), 3, 0, 0, 12 * 3600), local(2026, 12, 1, 3, 0));
    check("year end", maint_next_time(local(2026, 12, 31, 23, 0), 3, 0, 0, 12 * 3600), local(2027, 1, 1, 3, 0));
    /* 29.3.2026: 02:00 -> 03:00 (02:xx does not exist); 25.10.2026: 03:00 -> 02:00 */
    check("spring DST, 03:05 exists", maint_next_time(local(2026, 3, 28, 22, 0), 3, 5, 0, 12 * 3600), local(2026, 3, 29, 3, 5));
    check("autumn DST, 03:05 once", maint_next_time(local(2026, 10, 24, 22, 0), 3, 5, 0, 12 * 3600), local(2026, 10, 25, 3, 5));
    time_t a = maint_next_time(local(2026, 10, 24, 22, 0), 3, 5, 0, 12 * 3600);
    check("after autumn DST run -> next day 03:05", maint_next_time(a + 60, 3, 5, a, 12 * 3600), local(2026, 10, 26, 3, 5));
    check("spring DST at 02:20 (missing hour)", maint_next_time(local(2026, 3, 28, 22, 0), 2, 20, 0, 12 * 3600) > local(2026, 3, 28, 22, 0), 1);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails;
}
