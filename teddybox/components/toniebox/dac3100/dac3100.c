/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include "esp_log.h"
#include "driver/i2c.h"
#include "i2c_bus.h"
#include "driver/gpio.h"
#include "board.h"

#include "dac3100.h"
#include "speaker_eq.h"

static const char *TAG = "DAC3100";

static bool codec_init_flag;
static i2c_bus_handle_t i2c_handle;
static uint8_t reg_cache[14][256];
static uint8_t reg_page = 0;

/*
 * Output and volume handling as in the original firmware (main/audio/CO3100.c
 * of v5.235.0, read from the flash dump): 16 volume steps per output, set as
 * DAC digital volume in 0.5 dB units, speaker gets (L+R)/2, the unused output
 * is powered down. No biquads and no DRC are programmed there either.
 */
static const int8_t vol_table_spk[DAC3100_VOL_STEPS] = {-40, -37, -34, -31, -28, -25, -22, -18, -15, -12, -9, -6, -3, 0, 3, 5};
static const int8_t vol_table_hp[DAC3100_VOL_STEPS] = {-40, -36, -32, -28, -26, -24, -22, -20, -18, -16, -14, -12, -10, -8, -6, -4};
/* lowest step in 0.5 dB: -40 (-20 dB) as in the original, lower for a
   quieter start, DAC3100_VOL_MIN_MUTE for silence. Then the lower half of
   the steps (up to the middle step, the usual default volume) is spread
   evenly in dB between it and the middle step; the upper half stays. */
static int vol_min_hdb = DAC3100_VOL_MIN_DEFAULT;
#define VOL_MID_STEP (DAC3100_VOL_STEPS / 2)
static dac3100_output_t cur_output = DAC3100_OUT_NONE;
static int cur_volume = 50;
static bool cur_muted = true;
static uint8_t beep_level = 0x3F;
static speaker_eq_t cur_eq = SPEAKER_EQ_DEFAULT;
static uint8_t cur_data_path = 0x14; /* DACs off */

/* Opus always decodes to 48 kHz, that is all the box ever plays */
#define SPEAKER_EQ_FS 48000.0

uint16_t ofwButtonFreqTable[5][4][2] = {
    {
        // 16000
        {0x278A, 0x79BD}, //+
        {0x30F9, 0x763F}, //++
        {0x18F5, 0x7D87}, //-
        {0x0F0A, 0x7F1A}  //--
    },
    {
        // 22050
        {0x1CEA, 0x7CB1}, //+
        {0x23F9, 0x7AD5}, //++
        {0x122A, 0x7EB2}, //-
        {0x0AED, 0x7F87}  //--
    },
    {
        // 32000
        {0x1404, 0x7E6D}, //+
        {0x18F7, 0x7D8A}, //++
        {0x0C8A, 0x7F61}, //-
        {0x0788, 0x7FC7}  //--
    },
    {
        // 44100
        {0x0E8D, 0x7F2B}, //+
        {0x122C, 0x7EB4}, //++
        {0x091B, 0x7FAC}, //-
        {0x0578, 0x7FE2}  //--
    },
    {
        // 48000
        {0x0D60, 0x7F4D}, //+
        {0x10B4, 0x7EE7}, //++
        {0x085E, 0x7FB9}, //-
        {0x0506, 0x7FE6}  //--
    }};

audio_hal_func_t AUDIO_CODEC_DAC3100_DEFAULT_HANDLE = {
    .audio_codec_initialize = dac3100_init,
    .audio_codec_deinitialize = dac3100_deinit,
    .audio_codec_ctrl = dac3100_ctrl_state,
    .audio_codec_config_iface = dac3100_config_i2s,
    .audio_codec_set_mute = dac3100_set_mute,
    .audio_codec_set_volume = dac3100_set_volume,
    .audio_codec_get_volume = dac3100_get_volume,
};

static int i2c_init()
{
    int res;
    i2c_config_t dac3100_i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000};
    res = get_i2c_pins(I2C_NUM_0, &dac3100_i2c_cfg);
    i2c_handle = i2c_bus_create(I2C_NUM_0, &dac3100_i2c_cfg);

    if (!i2c_handle)
    {
        ESP_LOGE(TAG, "I2C init failed");
    }
    return res;
}

static esp_err_t dac3100_write_reg(uint8_t reg_add, uint8_t data)
{
    if (reg_add == 0)
    {
        reg_page = data;
    }
    else
    {
        reg_cache[reg_page][reg_add] = data;
    }
    return i2c_bus_write_bytes(i2c_handle, DAC3100_ADDR, &reg_add, sizeof(reg_add), &data, sizeof(data));
}

static esp_err_t dac3100_read_reg(uint8_t reg_add, uint8_t *p_data)
{
    esp_err_t err = i2c_bus_read_bytes(i2c_handle, DAC3100_ADDR, &reg_add, sizeof(reg_add), p_data, 1);

    if (err == ESP_OK)
    {
        reg_cache[reg_page][reg_add] = *p_data;
    }

    return err;
}

bool dac3100_initialized()
{
    return codec_init_flag;
}

void dac3100_read_all()
{
    for (int i = 0; i < 50; i++)
    {
        uint8_t reg = 0;
        dac3100_read_reg(i, &reg);
        ESP_LOGW(TAG, "  %x: %x", i, reg);
    }
}

esp_err_t dac3100_dump_reg(enum PAGE page, uint8_t reg)
{
    uint8_t val = 0;
    dac3100_write_reg(PAGE_CONTROL, page);
    esp_err_t ret = dac3100_read_reg(reg, &val);
    ESP_LOGW(TAG, "  %x: %x", reg, val);

    return ret;
}

uint8_t dac3100_headset_detected()
{
    uint8_t val = 0;
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    /* reset flags  */
    dac3100_read_reg(DAC_INTR_FLAGS, &val);
    dac3100_read_reg(HEADSET_DETECT, &val);

    return (val >> 5) & 0x03;
}

esp_err_t dac3100_beep_generate(uint16_t sin, uint16_t cos, uint32_t length)
{
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);

    dac3100_write_reg(BEEP_LEN_MSB, length >> 16);
    dac3100_write_reg(BEEP_LEN_MID, length >> 8);
    dac3100_write_reg(BEEP_LEN_LSB, length);

    dac3100_write_reg(BEEP_SIN_MSB, sin >> 8);
    dac3100_write_reg(BEEP_SIN_LSB, sin);

    dac3100_write_reg(BEEP_COS_MSB, cos >> 8);
    dac3100_write_reg(BEEP_COS_LSB, cos);

    dac3100_write_reg(BEEP_R_GEN, 0x80);              /* right beep volume follows left */
    dac3100_write_reg(BEEP_L_GEN, 0x80 | beep_level); /* start */

    return ESP_OK;
}

esp_err_t dac3100_beep(uint16_t index, uint32_t length)
{
    uint16_t(*pBeep)[2] = ofwButtonFreqTable[4];

    return dac3100_beep_generate(pBeep[index][0], pBeep[index][1], length);
}

esp_err_t dac3100_init(audio_hal_codec_config_t *cfg)
{
    ESP_LOGI(TAG, "dac3100 init");

    i2c_init();

    /* from datasheet */

    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(SOFTWARE_RESET, 0x01);
    dac3100_write_reg(CLOCKGEN_MUX, 0x07);
    // dac3100_write_reg(PLL_J_VAL, 0x08);
    // dac3100_write_reg(PLL_D_VAL_MSB, 0x00);
    // dac3100_write_reg(PLL_D_VAL_LSB, 0x00);
    // dac3100_write_reg(PLL_P_R_VAL, 0x91);
    // dac3100_write_reg(DAC_NDAC_VAL, 0x88);
    // dac3100_write_reg(DAC_MDAC_VAL, 0x82);
    // dac3100_write_reg(DAC_DOSR_VAL_MSB, 0x00);
    // dac3100_write_reg(DAC_DOSR_VAL_LSB, 0x80);
    // dac3100_write_reg(CODEC_IF_CTRL1, 0x00);
    // dac3100_write_reg(DAC_PROC_BLOCK_SEL, 0x0B);

    // dac3100_write_reg(PAGE_CONTROL, DAC_FILTER_DRC_COE_1A);
    // dac3100_write_reg(0x01, 0x04);

    // dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    // dac3100_write_reg(VOL_MICDET_SAR_ADC, 0x00);


    // dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    // dac3100_write_reg(DAC_DATA_PATH_SETUP, 0xD4);
    // dac3100_write_reg(DAC_VOL_CTRL, 0x00);

    /* https://github.com/toniebox-reverse-engineering/RvX_TLV320DAC3100/blob/master/RvX_TLV320DAC3100.cpp */
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    // dac3100_write_reg(SOFTWARE_RESET, 0x01);
    dac3100_write_reg(CLOCKGEN_MUX, 0x07);     // 0000:reserved, 01:PLL_CLKIN=BCLK, 11:CODEC_CLKIN=PLL_CLK
    /* BCLK = 32 x fs (16 bit stereo): 1.536 MHz at 48 kHz, 1.4112 MHz at 44.1 kHz.
       The former values (R=6, J=32, NDAC 4, MDAC 6, DOSR 256) gave the right
       fs but ran far outside the data sheet limits: PLL_CLK 295 MHz (max
       110), DAC_CLK 74 MHz (max 49.152), DAC_MOD_CLK 12.3 MHz (max 6.758);
       high tones came out with strong spurs below them. Now at 48 kHz:
       PLL_CLK = 1.536 MHz x R 2 x J 32 / P 1 = 98.304 MHz,
       DAC_CLK = / NDAC 2 = 49.152 MHz, DAC_MOD_CLK = / MDAC 8 = 6.144 MHz,
       fs = / DOSR 128 = 48 kHz (44.1 kHz input: 90.3 / 45.2 / 5.6 MHz).
       PRB_P25 needs MDAC x DOSR / 32 = 32 >= its resource class. */
    dac3100_write_reg(PLL_J_VAL, 0x20);        // J = 32
    dac3100_write_reg(PLL_D_VAL_MSB, 0x00);    // D = 0
    dac3100_write_reg(PLL_D_VAL_LSB, 0x00);
    dac3100_write_reg(PLL_P_R_VAL, 0x92);      // 1: PLL powered up, P = 1, R = 2
    dac3100_write_reg(DAC_NDAC_VAL, 0x82);     // NDAC powered up, divider 2
    dac3100_write_reg(DAC_MDAC_VAL, 0x88);     // MDAC powered up, divider 8
    dac3100_write_reg(DAC_DOSR_VAL_MSB, 0x00); // DOSR = 128
    dac3100_write_reg(DAC_DOSR_VAL_LSB, 0x80);

    vTaskDelay(10 / portTICK_RATE_MS);

    dac3100_write_reg(CODEC_IF_CTRL1, 0x00); // 00:Codec IF=I2S, 00: Codec IF WL=16 bits, 0:BCLK=Input, 0:WCKL=Output, 0:reserved        // w IF statt INT

    dac3100_write_reg(DAC_PROC_BLOCK_SEL, 0x19); // 000:reserved, 11001:DAC signal-processing block PRB_P25

    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL);
    dac3100_write_reg(HP_OUT_POP_REM_SET, 0x4E);         // 0:simultan.DAC/HP/SP, 1001:power-on-time=1.22s*,11:drv.ramp-up=3.9ms,0:CM voltage
    dac3100_write_reg(OUT_PGA_RAMP_DOWN_PER_CTRL, 0x70); // 0:reserved, 111=30.5ms*, 0000:reserved *8.2MHz
    dac3100_write_reg(DAC_LR_OUT_MIX_ROUTING, 0x44);     // 01:DAC_L to MixAmp_L,00:AIN1/2 not routed, 01:DAC_R to MIxAmp_R, 00:AIN1/2 not routed
    dac3100_write_reg(MICBIAS, 0x0B);                    // 0:SwPowDwn not enabled, 000:reserved, 1:MICBIAS powered up, 0:reserved, 11:MICBIAS=AVDD
    dac3100_write_reg(HP_DRIVER_CTRL, 0xE0);             // 000:Debounce Time=0us, 01:DAC perform.increased, 1:HPL output=lineout, 1:HPR output=lineout, 0:reserved ??? LINE

    dac3100_write_reg(PAGE_CONTROL, MCLK_DIVIDER);
    dac3100_write_reg(TIMER_CLK_MCLK_DIV, 0x01); // 0:Internal oscillator for delay timer, 0000001: MCLK divider=1

    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(HEADSET_DETECT, 0x8C);   // 1:Headset detection enabled, RR, 011:Debounce Prog.Glitch=128ms, 00:Debounce Prog.Glitch=0ms
    dac3100_write_reg(INT1_CTRL_REG, 0x80);    // 1:Headset-insertion detect IRQ INT1, 0:Button-press detect, ...., 0=INT1 is only one pulse 2ms
    dac3100_write_reg(GPIO1_INOUT_CTRL, 0x14); // XX:reserved, 0101:GPIO1=INT1 output, X=GPIO1 input buffer value, GPIO1 Output=X

#if 0
    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL); // MUTE ALL
    dac3100_write_reg(L_VOL_TO_HPL, 0x7F);        // HPL Vol -oo
    dac3100_write_reg(R_VOL_TO_HPR, 0x7F);        // HPL Vol -oo
    dac3100_write_reg(L_VOL_TO_SPK, 0x7F);        // SPK Vol -oo
    vTaskDelay(50 / portTICK_RATE_MS);

    // MUTE HP Driver AND SPK Driver
    dac3100_write_reg(HPL_DRIVER, 0x02); // HPL driver is muted ??? must 1
    dac3100_write_reg(HPR_DRIVER, 0x02); // HPR driver is muted ??? must 1
    dac3100_write_reg(SPK_DRIVER, 0x00); // SPK driver is muted

    // PAUSE 50ms
    vTaskDelay(50 / portTICK_RATE_MS);

    // AMPS Power Down
    dac3100_write_reg(HP_DRIVERS, 0x00); // HPL HPR Driver Power Down  ??? must 1
    dac3100_write_reg(SPK_AMP, 0x06); // SPK Amp Power Down ??? must 000011

    // PAUSE 50ms
    vTaskDelay(50 / portTICK_RATE_MS);

    dac3100_write_reg(HPL_DRIVER, 0x06); // HPL driver 0dB, not muted
    dac3100_write_reg(HPR_DRIVER, 0x06); // HPR drvier 0dB, not muted
    dac3100_write_reg(HP_DRIVERS, 0xC4);   // HPL HPR is power up, 1,35V, Shortcut=Error ??? must 1
    dac3100_write_reg(L_VOL_TO_HPL, 0x92); // ??? Aux to HP ???
    dac3100_write_reg(R_VOL_TO_HPR, 0x92); // ??? Aux to HP ???

    dac3100_write_reg(L_VOL_TO_SPK, 0x00); // !!! FEHLTE !!!
    dac3100_write_reg(SPK_AMP, 0x86);      // !!! FEHLTE !!! SPK Amp Power Up

    // PAUSE 50ms
    vTaskDelay(50 / portTICK_RATE_MS);

#endif
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(DAC_VOL_L_CTRL, 0xDC);
    dac3100_write_reg(DAC_VOL_R_CTRL, 0xDC);
    dac3100_write_reg(DAC_VOL_CTRL, 0x00);

    cur_output = DAC3100_OUT_NONE;
    cur_muted = true;
    dac3100_set_output(dac3100_headset_detected() ? DAC3100_OUT_HEADPHONE : DAC3100_OUT_SPEAKER);

    return ESP_OK;
}

esp_err_t dac3100_deinit(void)
{
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(SOFTWARE_RESET, 0x01);
    return ESP_OK;
}

esp_err_t dac3100_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    return ESP_OK;
}

esp_err_t dac3100_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    return ESP_OK;
}

/* mutes the driver of the active output */
esp_err_t dac3100_set_mute(bool mute)
{
    cur_muted = mute;
    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL);
    if (cur_output == DAC3100_OUT_SPEAKER)
    {
        dac3100_write_reg(SPK_DRIVER, mute ? 0x00 : 0x04); /* 6 dB, unmuted */
    }
    else if (cur_output == DAC3100_OUT_HEADPHONE)
    {
        dac3100_write_reg(HPL_DRIVER, mute ? 0x02 : 0x06); /* 0 dB, unmuted */
        dac3100_write_reg(HPR_DRIVER, mute ? 0x02 : 0x06);
    }
    return ESP_OK;
}

int dac3100_volume_index(int percent)
{
    if (percent < 0)
    {
        percent = 0;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    return (percent * (DAC3100_VOL_STEPS - 1) + 50) / 100;
}

int dac3100_volume_percent(int index)
{
    return (index * 100 + (DAC3100_VOL_STEPS - 1) / 2) / (DAC3100_VOL_STEPS - 1);
}

int dac3100_volume_step(int percent, int dir)
{
    int idx = dac3100_volume_index(percent) + (dir > 0 ? 1 : -1);
    if (idx < 0 || idx >= DAC3100_VOL_STEPS)
    {
        return -1;
    }
    return dac3100_volume_percent(idx);
}

static int step_half_db(const int8_t *table, int idx)
{
    if (vol_min_hdb == DAC3100_VOL_MIN_DEFAULT || idx >= VOL_MID_STEP)
    {
        return table[idx];
    }
    /* -63.5 dB is the lowest the DAC volume goes; below that: muted */
    int lo = vol_min_hdb <= DAC3100_VOL_MIN_MUTE ? -127 : vol_min_hdb;
    return lo + (table[VOL_MID_STEP] - lo) * idx / VOL_MID_STEP;
}

static bool step_muted(int idx)
{
    return idx == 0 && vol_min_hdb <= DAC3100_VOL_MIN_MUTE;
}

int dac3100_volume_half_db(int percent)
{
    const int8_t *table = cur_output == DAC3100_OUT_HEADPHONE ? vol_table_hp : vol_table_spk;
    return step_half_db(table, dac3100_volume_index(percent));
}

static void dac3100_apply_volume(void)
{
    const int8_t *table = cur_output == DAC3100_OUT_HEADPHONE ? vol_table_hp : vol_table_spk;
    int idx = dac3100_volume_index(cur_volume);
    int8_t v = step_half_db(table, idx);

    /* beep level follows the volume, as in the original (1 dB units, 2 = 0 dB) */
    beep_level = v < -43 ? 45 : (v >= 1 ? 0 : 2 - v);

    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(DAC_VOL_L_CTRL, (uint8_t)v);
    dac3100_write_reg(DAC_VOL_R_CTRL, (uint8_t)v);
    /* lowest step "silent": both DAC channels muted (beeps too) */
    dac3100_write_reg(DAC_VOL_CTRL, step_muted(idx) ? 0x0C : 0x00);
}

void dac3100_set_volume_min(int half_db)
{
    if (half_db > DAC3100_VOL_MIN_DEFAULT)
    {
        half_db = DAC3100_VOL_MIN_DEFAULT;
    }
    if (half_db < DAC3100_VOL_MIN_MUTE)
    {
        half_db = DAC3100_VOL_MIN_MUTE;
    }
    vol_min_hdb = half_db;
    if (cur_output != DAC3100_OUT_NONE)
    {
        dac3100_apply_volume();
    }
}

/* DAC channels off; filter coefficients can only be written in this state */
static void dac3100_dac_off(void)
{
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(DAC_DATA_PATH_SETUP, cur_data_path & 0x3F);
    for (int i = 0; i < 20; i++)
    {
        uint8_t flags = 0;
        dac3100_read_reg(DAC_FLAG_REG1, &flags);
        if (!(flags & 0x88)) /* left and right DAC powered down */
        {
            break;
        }
        vTaskDelay(5 / portTICK_RATE_MS);
    }
}

/* biquads A-E for both channels; the speaker gets the EQ, headphones stay flat */
static void dac3100_write_eq(bool speaker)
{
    dac3100_biquad_t bq[SPEAKER_EQ_BIQUADS];
    speaker_eq_design(speaker ? cur_eq : SPEAKER_EQ_OFF, SPEAKER_EQ_FS, bq);

    dac3100_write_reg(PAGE_CONTROL, DAC_FILTER_DRC_COE_1A);
    for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
    {
        const int16_t c[5] = {bq[i].n0, bq[i].n1, bq[i].n2, bq[i].d1, bq[i].d2};
        for (int k = 0; k < 5; k++)
        {
            uint8_t reg_l = 2 + 10 * i + 2 * k;   /* left: page 8, registers 2-51 */
            uint8_t reg_r = 66 + 10 * i + 2 * k;  /* right: page 8, registers 66-115 */
            dac3100_write_reg(reg_l, (uint16_t)c[k] >> 8);
            dac3100_write_reg(reg_l + 1, c[k] & 0xFF);
            dac3100_write_reg(reg_r, (uint16_t)c[k] >> 8);
            dac3100_write_reg(reg_r + 1, c[k] & 0xFF);
        }
    }
    /* read back: shows whether the DAC took the coefficients */
    int bad = 0;
    for (int i = 0; i < SPEAKER_EQ_BIQUADS; i++)
    {
        const int16_t c[5] = {bq[i].n0, bq[i].n1, bq[i].n2, bq[i].d1, bq[i].d2};
        for (int k = 0; k < 5; k++)
        {
            uint8_t reg_l = 2 + 10 * i + 2 * k, hi = 0, lo = 0;
            dac3100_read_reg(reg_l, &hi);
            dac3100_read_reg(reg_l + 1, &lo);
            bad += (uint16_t)(hi << 8 | lo) != (uint16_t)c[k];
        }
    }
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (bad)
    {
        ESP_LOGE(TAG, "EQ: %s, %d coefficient(s) not taken by the DAC", speaker ? speaker_eq_name(cur_eq) : "flat (headphones)", bad);
    }
    else
    {
        ESP_LOGI(TAG, "EQ: %s (coefficients checked)", speaker ? speaker_eq_name(cur_eq) : "flat (headphones)");
    }
}

static void dac3100_dac_on(void)
{
    dac3100_write_reg(PAGE_CONTROL, SERIAL_IO);
    dac3100_write_reg(DAC_DATA_PATH_SETUP, cur_data_path);
}

esp_err_t dac3100_set_speaker_eq(speaker_eq_t mode)
{
    if ((unsigned)mode >= SPEAKER_EQ_COUNT)
    {
        return ESP_FAIL;
    }
    if (mode == cur_eq)
    {
        return ESP_OK;
    }
    cur_eq = mode;
    if (cur_output == DAC3100_OUT_SPEAKER)
    {
        bool muted = cur_muted;
        dac3100_set_mute(true);
        dac3100_dac_off();
        dac3100_write_eq(true);
        dac3100_dac_on();
        dac3100_set_mute(muted);
    }
    return ESP_OK;
}

speaker_eq_t dac3100_get_speaker_eq(void)
{
    return cur_eq;
}

/* same sequence as the original firmware: everything off, then only the wanted output on */
esp_err_t dac3100_set_output(dac3100_output_t output)
{
    if (output == cur_output)
    {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "output: %s", output == DAC3100_OUT_HEADPHONE ? "headphones" : "speaker");

    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL);
    dac3100_write_reg(L_VOL_TO_HPL, 0x7F);
    dac3100_write_reg(R_VOL_TO_HPR, 0x7F);
    dac3100_write_reg(L_VOL_TO_SPK, 0x7F);
    vTaskDelay(50 / portTICK_RATE_MS);
    dac3100_write_reg(HPL_DRIVER, 0x00);
    dac3100_write_reg(HPR_DRIVER, 0x00);
    dac3100_write_reg(SPK_DRIVER, 0x00);
    vTaskDelay(50 / portTICK_RATE_MS);
    dac3100_dac_off();
    dac3100_write_eq(output == DAC3100_OUT_SPEAKER);
    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL);
    dac3100_write_reg(HP_DRIVERS, 0x00);
    dac3100_write_reg(SPK_AMP, 0x00);
    vTaskDelay(50 / portTICK_RATE_MS);

    cur_output = output;
    if (output == DAC3100_OUT_HEADPHONE)
    {
        dac3100_write_reg(HP_DRIVERS, 0xC2);   /* HPL/HPR powered up */
        dac3100_write_reg(L_VOL_TO_HPL, 0x92); /* routed, -9 dB */
        dac3100_write_reg(R_VOL_TO_HPR, 0x92);
        cur_data_path = 0xD5; /* both DACs on, L=left, R=right */
    }
    else
    {
        dac3100_write_reg(SPK_AMP, 0x86);      /* class-D powered up */
        dac3100_write_reg(L_VOL_TO_SPK, 0x92); /* routed, -9 dB */
        cur_data_path = 0xFD; /* both DACs get (L+R)/2, the speaker is fed from the left one */
    }
    dac3100_set_mute(cur_muted);
    vTaskDelay(50 / portTICK_RATE_MS);
    dac3100_dac_on();
    dac3100_apply_volume();
    return ESP_OK;
}

dac3100_output_t dac3100_get_output(void)
{
    return cur_output;
}

esp_err_t dac3100_set_gain(int gain)
{
    uint8_t reg_val = (reg_cache[1][0x2A] & ~0x18) | (gain << 3);

    dac3100_write_reg(PAGE_CONTROL, DAC_OUT_VOL);
    dac3100_write_reg(SPK_DRIVER, reg_val);
    return ESP_OK;
}

/* volume in percent, mapped to the 16 steps of the original firmware */
esp_err_t dac3100_set_volume(int volume)
{
    cur_volume = volume;
    dac3100_apply_volume();
    return ESP_OK;
}

esp_err_t dac3100_get_volume(int *volume)
{
    return ESP_OK;
}
