#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
/* only the estimator from battery.c */
static bool playing_now;
bool pb_is_playing(void) { return playing_now; }
#define static_assert _Static_assert
#define battery_task battery_task_unused
#define battery_init battery_init_unused
int adc1_get_raw(int c) { return 0; }
unsigned esp_adc_cal_raw_to_voltage(int r, void *c) { return 0; }
#include "battery_copy.c"
int fails;
static uint32_t filt;
/* like battery_task: IIR (3 old + 1 new) / 4 */
static void step(bool play, uint32_t mv, int n) { playing_now = play; for (int i = 0; i < n; i++) { filt = filt ? (filt * 3 + mv) / 4 : mv; update_rest(filt); } }
static void expect(const char *what, uint32_t want)
{
    int ok = (int)rest_mv >= (int)want - 3 && (int)rest_mv <= (int)want + 3;
    fails += !ok;
    printf("%s %-50s rest %u (want %u)\n", ok ? "ok  " : "FAIL", what, rest_mv, want);
}
int main(void)
{
    /* box starts playing at once: no sag known yet -> loaded value */
    step(true, 3700, 20);  expect("boot while playing, sag unknown", 3700);
    step(false, 3760, 2);  expect("idle, still settling: last estimate", 3700);
    step(false, 3760, 20); expect("idle settled (sag measured ~60)", 3760);
    step(true, 3700, 2);   expect("playing again, settling: keep idle value", 3760);
    step(true, 3700, 20);  expect("playing, sag added back", 3760);
    step(true, 3680, 30);  expect("playing, battery drains 20 mV", 3740);
    charging = true;  step(true, 4600, 5);  expect("charging -> unknown", 0);
    charging = false; filt = 0; step(true, 4040, 30); expect("after the charger, playing: old sag used", 4100);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails;
}
const settings_t *settings_get(void){static settings_t s={3600,3670,3700};return &s;}
