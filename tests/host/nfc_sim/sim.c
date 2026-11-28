/* virtual time + a simulated Tonie (NXP SLIX-L in privacy mode, Boxine password).
   "./sim plain": tonie B is a plain ISO15693 tag of another maker (no GET
   RANDOM / SET PASSWORD, no privacy mode, answers with an error flag) */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "trf7962a.h"
#include "freertos/FreeRTOS.h"

#define TONIE_PASS 0x7FFD6E5B
int sim_verbose = 0;
static bool plain_b = false;
static double now_ms = 0, field_on_since = -1, field_ms = 0;
static bool field = false;
static jmp_buf sim_end; static double sim_end_ms;
double sim_now(void) { return now_ms / 1000.0; }

/* scenario: tonie A from 5 s to 35 s, tonie B from 35.2 s to 50 s, nothing until 60 s */
static uint64_t tag_at(double t) { if (t >= 5000 && t < 35000) return 0xE004035000112233ULL; if (t >= 35200 && t < 50000) return 0xE004035044556677ULL; return 0; }
static int unsupported = 0, fifo_overflows = 0, tokens_ok = 0, tokens_bad = 0;
static uint8_t tag_mem(uint64_t uid, int i) { return (uint8_t)(uid >> (8 * (i % 8))) ^ (uint8_t)(0x11 * i); }
static bool tag_powered = false, tag_unlocked = false; static uint8_t rnd[2];
static uint64_t last_tag = 0;
static int garbled = 0, other = 0; /* disturbed answers given (per run) */

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
    if (plain_b && t == 0xE004035044556677ULL) {
        if (tx[1] == 0xB2 || tx[1] == 0xB3) { rx[0] = 1; rx[1] = 1; rx[2] = rx[3] = 0; *rxl = 4; return ESP_OK; } /* error: not supported */
        tag_unlocked = true;                  /* no privacy mode */
    }
    switch (tx[1]) {
    case 0xB2: rnd[0] = (uint8_t)now_ms; rnd[1] = (uint8_t)(now_ms / 7); rx[0]=0; rx[1]=rnd[0]; rx[2]=rnd[1]; rx[3]=rx[4]=0; *rxl=5; return ESP_OK;
    case 0xB3: { uint32_t p = (tx[4]^rnd[0]) | (tx[5]^rnd[1])<<8 | (tx[6]^rnd[0])<<16 | (uint32_t)(tx[7]^rnd[1])<<24;
                 if (p != TONIE_PASS) return ESP_FAIL; tag_unlocked = true; rx[0]=0; rx[1]=rx[2]=0; *rxl=3; return ESP_OK; }
    case 0x01: { if (!tag_unlocked) return ESP_FAIL;
        /* interference while tonie A lies there (27 Nov): one garbled answer
           without E0 at 15 s, one single answer with another E0 UID at 20 s;
           neither may stop A */
        uint64_t u = t;
        if (t == 0xE004035000112233ULL && now_ms >= 15000 && !garbled) { garbled = 1; u = 0x1FFFFCFFF01F81F6ULL; }
        else if (t == 0xE004035000112233ULL && now_ms >= 20000 && !other) { other = 1; u = 0xE0040350DEADBEEFULL; }
        rx[0]=0; rx[1]=0; for (int i=0;i<8;i++) rx[2+i]=u>>(8*i); rx[10]=rx[11]=0; *rxl=12; return ESP_OK; }
    case 0x23: /* READ MULTIPLE BLOCKS: not implemented by the SLIX-L, silent in non-addressed mode */
        unsupported++; return ESP_FAIL;
    case 0x20: { /* READ SINGLE BLOCK: 8 blocks of 4 bytes */
        if (!tag_unlocked) return ESP_FAIL;
        int first = tx[2], len = 1 + 4 + 2;
        if (txl != 3 || first > 7) return ESP_FAIL;
        /* the TRF7962A FIFO holds 12 bytes, longer answers fail with this driver */
        if (len > 12) { fifo_overflows++; return ESP_FAIL; }
        rx[0] = 0;
        for (int i = 0; i < 4; i++) rx[1 + i] = tag_mem(t, 4 * first + i);
        rx[5] = rx[6] = 0xCC; /* CRC, the chip passes it on */
        *rxl = len; return ESP_OK; }
    }
    return ESP_FAIL;
}
trf7962a_t audio_board_get_trf(void) { return (trf7962a_t)1; }
static double play_events[8]; static int n_play = 0; static double stop_at[8]; static int n_stop = 0;
void pb_play_content(uint64_t uid) { if (n_play < 8) play_events[n_play++] = now_ms; if (sim_verbose) printf("%8.3fs  >> play %016llX\n", sim_now(), (unsigned long long)uid); }
void pb_play_content_token(uint64_t uid, uint8_t *tok)
{
    bool ok = true;
    for (int i = 0; i < 32; i++) ok &= tok[i] == tag_mem(uid, i);
    if (ok) tokens_ok++; else tokens_bad++;
}
void pb_stop(void) { if (n_stop < 8) stop_at[n_stop++] = now_ms; if (sim_verbose) printf("%8.3fs  >> stop\n", sim_now()); }

void nfc_mainthread(void *arg);
int main(int argc, char **argv)
{
    plain_b = argc > 1 && !strcmp(argv[1], "plain");
    sim_verbose = argc > 1 && !plain_b;
    if (plain_b) printf("  (tonie B: tag without NXP commands and privacy mode)\n");
    struct { const char *name; double from, to; } phase[] = {{"no tonie", 0, 5000}, {"tonie on the box", 5000, 35000}, {"no tonie", 50000, 60000}};
    /* run once per phase end, measuring the field share inside the phase */
    double prev_field = 0, prev_t = 0;
    for (int i = 0; i < 3; i++) {
        now_ms = 0; field = false; field_ms = 0; n_play = n_stop = 0; last_tag = 0; tag_powered = tag_unlocked = false;
        sim_end_ms = phase[i].from; double f0 = 0;
        if (phase[i].from > 0) { if (!setjmp(sim_end)) nfc_mainthread((void *)1); f0 = field_ms; }
        garbled = other = 0;
        now_ms = 0; field = false; field_ms = 0; n_play = n_stop = 0; tokens_ok = tokens_bad = fifo_overflows = unsupported = 0; last_tag = 0; tag_powered = tag_unlocked = false;
        sim_end_ms = phase[i].to;
        int v = sim_verbose; sim_verbose = v && i == 2;
        if (!setjmp(sim_end)) nfc_mainthread((void *)1);
        sim_verbose = v;
        printf("  %-16s field on: %5.1f %%\n", phase[i].name, 100.0 * (field_ms - f0) / (phase[i].to - phase[i].from));
        if (i == 2) {
            printf("  events: start A at %.2f s, start B at %.2f s, stop after removal at %.2f s (tonie gone at 50.00 s)\n",
                   n_play > 0 ? play_events[0]/1000 : -1, n_play > 1 ? play_events[1]/1000 : -1, n_stop > 0 ? stop_at[n_stop-1]/1000 : -1);
            printf("  starts: %d (want 2), stops: %d\n", n_play, n_stop);
            printf("  tokens: %d read correctly, %d wrong (want 2 / 0), FIFO overflows: %d, unsupported commands: %d\n", tokens_ok, tokens_bad, fifo_overflows, unsupported);
            if (tokens_ok != 2 || tokens_bad || fifo_overflows || unsupported) { printf("  FAIL token\n"); return 1; }
        }
    }
    return 0;
}
