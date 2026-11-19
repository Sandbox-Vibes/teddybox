/* virtual time + a simulated Tonie (NXP SLIX-L in privacy mode, Boxine password) */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "trf7962a.h"
#include "freertos/FreeRTOS.h"

#define TONIE_PASS 0x7FFD6E5B
int sim_verbose = 0;
static double now_ms = 0, field_on_since = -1, field_ms = 0;
static bool field = false;
static jmp_buf sim_end; static double sim_end_ms;
double sim_now(void) { return now_ms / 1000.0; }

/* scenario: tonie A from 5 s to 35 s, tonie B from 35.2 s to 50 s, nothing until 60 s */
static uint64_t tag_at(double t) { if (t >= 5000 && t < 35000) return 0xE004035000112233ULL; if (t >= 35200 && t < 50000) return 0xE004035044556677ULL; return 0; }
static bool tag_powered = false, tag_unlocked = false; static uint8_t rnd[2];
static uint64_t last_tag = 0;

static void advance(double ms)
{
    now_ms += ms;
    if (now_ms >= sim_end_ms) { if (field) field_ms += now_ms - field_on_since; longjmp(sim_end, 1); }
}
static void tag_update(void)
{
    uint64_t t = tag_at(now_ms);
    if (t != last_tag || !field) { tag_powered = false; tag_unlocked = false; }  /* no energy -> privacy mode again */
    last_tag = t;
    if (t && field) tag_powered = true;
}
void vTaskDelay(TickType_t ms) { tag_update(); advance(ms); tag_update(); }
void trf7962a_field(trf7962a_t c, bool on)
{
    if (on && !field) field_on_since = now_ms;
    if (!on && field) field_ms += now_ms - field_on_since;
    field = on; tag_update();
}
esp_err_t trf7962a_reset(trf7962a_t c) { trf7962a_field(c, false); field = false; advance(2); return ESP_OK; }
esp_err_t trf7962a_xmit(trf7962a_t c, uint8_t *tx, uint8_t txl, uint8_t *rx, uint8_t *rxl)
{
    advance(4); tag_update();                 /* one ISO15693 exchange ~4 ms */
    uint64_t t = tag_at(now_ms);
    if (!t || !tag_powered) return ESP_FAIL;  /* timeout, nobody answered */
    switch (tx[1]) {
    case 0xB2: rnd[0] = (uint8_t)now_ms; rnd[1] = (uint8_t)(now_ms / 7); rx[0]=0; rx[1]=rnd[0]; rx[2]=rnd[1]; rx[3]=rx[4]=0; *rxl=5; return ESP_OK;
    case 0xB3: { uint32_t p = (tx[4]^rnd[0]) | (tx[5]^rnd[1])<<8 | (tx[6]^rnd[0])<<16 | (uint32_t)(tx[7]^rnd[1])<<24;
                 if (p != TONIE_PASS) return ESP_FAIL; tag_unlocked = true; rx[0]=0; rx[1]=rx[2]=0; *rxl=3; return ESP_OK; }
    case 0x01: if (!tag_unlocked) return ESP_FAIL; rx[0]=0; rx[1]=0; for (int i=0;i<8;i++) rx[2+i]=t>>(8*i); rx[10]=rx[11]=0; *rxl=12; return ESP_OK;
    case 0x23: if (!tag_unlocked) return ESP_FAIL; rx[0]=0; memset(rx+1, 0x42, 32); *rxl=35; return ESP_OK;
    }
    return ESP_FAIL;
}
trf7962a_t audio_board_get_trf(void) { return (trf7962a_t)1; }
static double play_events[8]; static int n_play = 0; static double stop_at[8]; static int n_stop = 0;
void pb_play_content(uint64_t uid) { if (n_play < 8) play_events[n_play++] = now_ms; if (sim_verbose) printf("%8.3fs  >> play %016llX\n", sim_now(), (unsigned long long)uid); }
void pb_play_content_token(uint64_t uid, uint8_t *tok) {}
void pb_stop(void) { if (n_stop < 8) stop_at[n_stop++] = now_ms; if (sim_verbose) printf("%8.3fs  >> stop\n", sim_now()); }

void nfc_mainthread(void *arg);
int main(int argc, char **argv)
{
    sim_verbose = argc > 1;
    struct { const char *name; double from, to; } phase[] = {{"ohne Tonie", 0, 5000}, {"Tonie liegt auf", 5000, 35000}, {"ohne Tonie", 50000, 60000}};
    /* run once per phase end, measuring the field share inside the phase */
    double prev_field = 0, prev_t = 0;
    for (int i = 0; i < 3; i++) {
        now_ms = 0; field = false; field_ms = 0; n_play = n_stop = 0; last_tag = 0; tag_powered = tag_unlocked = false;
        sim_end_ms = phase[i].from; double f0 = 0;
        if (phase[i].from > 0) { if (!setjmp(sim_end)) nfc_mainthread((void *)1); f0 = field_ms; }
        now_ms = 0; field = false; field_ms = 0; n_play = n_stop = 0; last_tag = 0; tag_powered = tag_unlocked = false;
        sim_end_ms = phase[i].to;
        int v = sim_verbose; sim_verbose = v && i == 2;
        if (!setjmp(sim_end)) nfc_mainthread((void *)1);
        sim_verbose = v;
        printf("  %-16s Feld an: %5.1f %%\n", phase[i].name, 100.0 * (field_ms - f0) / (phase[i].to - phase[i].from));
        if (i == 2) {
            printf("  Ereignisse: Start A bei %.2f s, Start B bei %.2f s, Stopp nach Entfernen bei %.2f s (Tonie weg bei 50.00 s)\n",
                   n_play > 0 ? play_events[0]/1000 : -1, n_play > 1 ? play_events[1]/1000 : -1, n_stop > 0 ? stop_at[n_stop-1]/1000 : -1);
            printf("  Anzahl Starts: %d (erwartet 2), Stopps: %d\n", n_play, n_stop);
        }
    }
    return 0;
}
