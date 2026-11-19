/* runs the real dac3100.c against a register model and checks it against the
 * values the original firmware writes (read from the flash dump) */
#include <string.h>
#include <assert.h>
#include "i2c_bus.h"
#include "dac3100.h"

static uint8_t regs[256][128], page;
static int headset;
static int writes_to_coef_pages, coef_writes_while_dac_on;
i2c_bus_handle_t i2c_bus_create(int port, i2c_config_t *c) { return (void *)1; }
esp_err_t i2c_bus_write_bytes(i2c_bus_handle_t h, int addr, uint8_t *reg, int rl, uint8_t *d, int dl)
{
    assert(addr == 0x18 || addr == (0x18 << 1) || addr == 0x30);
    if (*reg == 0) page = *d; else regs[page][*reg & 0x7f] = *d;
    if (page >= 8 && page <= 9 && *reg) { writes_to_coef_pages++; if (regs[0][63] & 0xC0) coef_writes_while_dac_on++; }
    return ESP_OK;
}
esp_err_t i2c_bus_read_bytes(i2c_bus_handle_t h, int addr, uint8_t *reg, int rl, uint8_t *d, int dl)
{
    *d = (page == 0 && *reg == 67) ? (headset ? 0x20 : 0) : regs[page][*reg & 0x7f];
    return ESP_OK;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    audio_hal_codec_config_t cfg = {0};
    dac3100_init(&cfg);
    CHECK(dac3100_get_output() == DAC3100_OUT_SPEAKER, "speaker after init");
    CHECK(regs[0][60] == 0x19, "PRB_P25");
    CHECK(regs[0][63] == 0xFD, "speaker mono mix 0x%02x", regs[0][63]);
    CHECK(regs[1][32] == 0x86, "class-D on");
    CHECK(regs[1][31] == 0x00, "HP drivers off, got 0x%02x", regs[1][31]);
    CHECK(regs[1][38] == 0x92, "speaker analog -9 dB");
    CHECK(regs[1][42] == 0x00, "muted after init");
    dac3100_set_mute(false);
    CHECK(regs[1][42] == 0x04, "speaker driver 6 dB unmuted, got 0x%02x", regs[1][42]);

    /* the 16 steps of the original: walk them like the ears do */
    static const int8_t spk[16] = {-40, -37, -34, -31, -28, -25, -22, -18, -15, -12, -9, -6, -3, 0, 3, 5};
    static const int8_t hp[16] = {-40, -36, -32, -28, -26, -24, -22, -20, -18, -16, -14, -12, -10, -8, -6, -4};
    int v = 0, steps = 0;
    dac3100_set_volume(v);
    CHECK((int8_t)regs[0][65] == -40, "min -20 dB");
    while ((v = dac3100_volume_step(v, 1)) >= 0)
    {
        steps++;
        dac3100_set_volume(v);
        CHECK((int8_t)regs[0][65] == spk[steps] && regs[0][66] == regs[0][65], "step %d: %d %% -> %d", steps, v, (int8_t)regs[0][65]);
        if (steps == 15) CHECK(v == 100, "top step is 100 %%, got %d", v);
    }
    CHECK(steps == 15, "15 steps up, got %d", steps);
    CHECK(dac3100_volume_step(100, 1) == -1 && dac3100_volume_step(0, -1) == -1, "limits");
    for (int p = 0; p <= 100; p++) CHECK(dac3100_volume_index(dac3100_volume_percent(dac3100_volume_index(p))) == dac3100_volume_index(p), "round trip %d", p);
    int down = 100, n = 0; while ((down = dac3100_volume_step(down, -1)) >= 0) n++;
    CHECK(n == 15, "15 steps down");
    /* a HA value between steps moves to the neighbouring step */
    CHECK(dac3100_volume_step(55, 1) == dac3100_volume_percent(9), "55 %% up -> step 9");

    /* headphones: other path off, stereo, own table, mute state kept */
    dac3100_set_volume(100);
    dac3100_set_output(DAC3100_OUT_HEADPHONE);
    CHECK(regs[0][63] == 0xD5, "HP stereo");
    CHECK(regs[1][32] == 0x00 && regs[1][42] == 0x00, "speaker off");
    CHECK(regs[1][31] == 0xC2 && regs[1][36] == 0x92 && regs[1][37] == 0x92, "HP on");
    CHECK(regs[1][40] == 0x06 && regs[1][41] == 0x06, "HP unmuted (was unmuted)");
    CHECK((int8_t)regs[0][65] == hp[15], "HP max -2 dB, got %d", (int8_t)regs[0][65]);
    dac3100_set_mute(true);
    CHECK(regs[1][40] == 0x02 && regs[1][41] == 0x02 && regs[1][42] == 0, "HP muted");
    dac3100_set_output(DAC3100_OUT_SPEAKER);
    CHECK(regs[1][42] == 0x00 && regs[1][32] == 0x86 && regs[1][31] == 0, "back to speaker, still muted");
    CHECK((int8_t)regs[0][65] == spk[15], "speaker table again");

    /* beep: right follows left, level follows volume */
    dac3100_beep(0, 0x140);
    CHECK(regs[0][72] == 0x80 && (regs[0][71] & 0x80), "beep start");
    CHECK((regs[0][71] & 0x3F) == 0, "beep at +2.5 dB volume -> level 0");
    dac3100_set_volume(0); dac3100_beep(0, 0x140);
    CHECK((regs[0][71] & 0x3F) == 42, "beep at -20 dB -> 42, got %d", regs[0][71] & 0x3F);

    /* headset at boot */
    memset(regs, 0, sizeof(regs)); headset = 1;
    dac3100_init(&cfg);
    CHECK(dac3100_get_output() == DAC3100_OUT_HEADPHONE && regs[0][63] == 0xD5 && regs[1][32] == 0, "headset at boot");

    CHECK(coef_writes_while_dac_on == 0, "coefficients only written with the DAC off (%d)", coef_writes_while_dac_on);

    /* EQ: speaker gets the designed biquads (left = right), headphones the flat defaults */
    headset = 0; memset(regs, 0, sizeof(regs)); dac3100_init(&cfg);
    dac3100_biquad_t bq[SPEAKER_EQ_BIQUADS];
    speaker_eq_design(SPEAKER_EQ_DEFAULT, 48000, bq);
    CHECK(dac3100_get_speaker_eq() == SPEAKER_EQ_DEFAULT, "default EQ");
    CHECK((int16_t)(regs[8][2] << 8 | regs[8][3]) == bq[0].n0 && (int16_t)(regs[8][10] << 8 | regs[8][11]) == bq[0].d2 && (int16_t)(regs[8][8] << 8 | regs[8][9]) == bq[0].d1, "biquad A left");
    CHECK((int16_t)(regs[8][66] << 8 | regs[8][67]) == bq[0].n0 && (int16_t)(regs[8][104] << 8 | regs[8][105]) == bq[3].d2, "right = left");
    CHECK(regs[0][63] == 0xFD, "DAC back on after EQ");
    dac3100_set_output(DAC3100_OUT_HEADPHONE);
    CHECK(regs[8][2] == 0x7F && regs[8][3] == 0xFF && regs[8][4] == 0 && regs[8][12] == 0x7F, "headphones flat");
    dac3100_set_output(DAC3100_OUT_SPEAKER);
    dac3100_set_mute(false);
    dac3100_set_speaker_eq(SPEAKER_EQ_OFF);
    CHECK(regs[8][2] == 0x7F && regs[8][3] == 0xFF, "EQ off on the fly");
    CHECK(regs[0][63] == 0xFD && regs[1][42] == 0x04, "playing again, unmuted");
    dac3100_set_speaker_eq(SPEAKER_EQ_LOWCUT);
    speaker_eq_design(SPEAKER_EQ_LOWCUT, 48000, bq);
    CHECK((int16_t)(regs[8][12] << 8 | regs[8][13]) == bq[1].n0 && regs[8][22] == 0x7F, "lowcut: A,B set, C flat");
    CHECK(coef_writes_while_dac_on == 0, "still never written with the DAC on");
    CHECK(dac3100_set_speaker_eq(7) != ESP_OK, "invalid mode");
    printf("dac3100: %s\n", fails ? "FAILED" : "all tests ok");
    return fails != 0;
}
