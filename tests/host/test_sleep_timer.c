#include <stdio.h>
#include "sleep_timer.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define MIN (60LL * 1000000)

int main(void)
{
    sleep_timer_t t = {0};
    CHECK(!sleep_timer_active(&t) && sleep_timer_level(&t, 5 * MIN) == 1000 && sleep_timer_remaining_min(&t, 0) == 0);

    /* 30 min: fade over the last 10 */
    int64_t s = 1000 * MIN;
    sleep_timer_set(&t, s, 30);
    CHECK(sleep_timer_active(&t) && sleep_timer_remaining_min(&t, s) == 30);
    CHECK(sleep_timer_remaining_min(&t, s + MIN / 2) == 30 && sleep_timer_remaining_min(&t, s + MIN) == 29);
    CHECK(sleep_timer_level(&t, s + 20 * MIN - 1) == 1000);
    CHECK(sleep_timer_level(&t, s + 25 * MIN) == 500);
    CHECK(sleep_timer_level(&t, s + 30 * MIN - 1) == 0);
    CHECK(sleep_timer_level(&t, s + 30 * MIN) == -1 && sleep_timer_remaining_min(&t, s + 31 * MIN) == 0);

    /* 90 min: fade at most 10 min; 2 min: fade at least 1 min */
    sleep_timer_set(&t, s, 90);
    CHECK(sleep_timer_level(&t, s + 80 * MIN - 1) == 1000 && sleep_timer_level(&t, s + 85 * MIN) == 500);
    sleep_timer_set(&t, s, 2);
    CHECK(sleep_timer_level(&t, s + MIN - 1) == 1000 && sleep_timer_level(&t, s + MIN + MIN / 2) == 500);

    /* restart, limit, off */
    sleep_timer_set(&t, s, 1000);
    CHECK(sleep_timer_remaining_min(&t, s) == SLEEP_TIMER_MAX_MIN);
    sleep_timer_set(&t, s, 0);
    CHECK(!sleep_timer_active(&t) && sleep_timer_level(&t, s) == 1000);

    printf("sleep_timer: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
