

#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "trf7962a.h"

#include "playback.h"
#include "rtnl.h"
#include "tonie_seen.h"
#include "taskwatch.h"
#include "board.h"
#include "nfc.h"

#define COUNT(x) (sizeof(x) / sizeof(x[0]))

static uint8_t slix_get_rand[] = {0x02, 0xB2, 0x04};
static uint8_t slix_get_inventory[] = {0x26, 0x01, 0x00};
static uint8_t slix_set_pass[] = {0x02, 0xB3, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00};
/* ISO15693 READ MULTIPLE BLOCKS: high data rate, first block 0, 8 blocks (value is count - 1) */
/* READ SINGLE BLOCK. The ICODE SLIX-L in a tonie does not implement READ
   MULTIPLE BLOCKS and stays silent on it in non-addressed mode (NXP data
   sheet, 9.5.2), which is why every token read failed on the box with 0 bytes.
   One block answers with flags + 4 data + CRC = 7 bytes. */
#define NFC_TOKEN_BLOCKS_PER_READ 1
static uint8_t iso_read_token[] = {0x02, 0x20, 0x00};

static const uint32_t passes[] = {0x0F0F0F0F, 0x7FFD6E5B, 0x00000000};
static uint8_t received_data[256];
static uint8_t received_length;

static char dump_buf[129];

/* the tag on the box: written by the NFC task, read by the playback task
   (download wait, stream restart); under nfc_lock so the 8 byte UID and the
   32 byte token are never read half written */
static portMUX_TYPE nfc_lock = portMUX_INITIALIZER_UNLOCKED;
static bool nfc_valid = false;
static uint8_t nfc_current_uid_rev[8];
static uint8_t nfc_current_token[32];
static int nfc_retry = 0;

static const char *TAG = "[NFC]";

typedef enum
{
    STATE_SEARCHING,
    STATE_TAG,
    STATE_SYSINFO
} nfc_state_t;

uint64_t nfc_get_current_uid()
{
    uint64_t uid = NFC_UID_INVALID;
    taskENTER_CRITICAL(&nfc_lock);
    if (nfc_valid)
    {
        uid = 0;
        for (int i = 7; i >= 0; i--)
        {
            uid = uid << 8 | nfc_current_uid_rev[i];
        }
    }
    taskEXIT_CRITICAL(&nfc_lock);
    return uid;
}

bool nfc_get_current_token(uint8_t token[32])
{
    taskENTER_CRITICAL(&nfc_lock);
    bool valid = nfc_valid;
    memcpy(token, nfc_current_token, 32);
    taskEXIT_CRITICAL(&nfc_lock);
    return valid;
}

static void nfc_set_valid(bool valid)
{
    taskENTER_CRITICAL(&nfc_lock);
    nfc_valid = valid;
    taskEXIT_CRITICAL(&nfc_lock);
}

void nfc_dump(char *buffer, uint8_t *data, uint8_t length)
{
    int pos = 0;

    buffer[pos] = 0;

    for (int pos = 0; pos < length; pos++)
    {
        sprintf(&buffer[pos * 3], "%02X ", data[pos]);
    }
}

void nfc_log_dump(const char *title, uint8_t *data, uint8_t length)
{
    nfc_dump(dump_buf, data, length);
    ESP_LOGD(TAG, "%s: %s", title, dump_buf);
}

esp_err_t nfc_get_rand(trf7962a_t trf, uint8_t *rand)
{
    if (trf7962a_xmit(trf, slix_get_rand, sizeof(slix_get_rand), received_data, &received_length) != ESP_OK)
    {
        return ESP_FAIL;
    }
    nfc_log_dump("GET RANDOM", received_data, received_length);
    if (received_length != 5 || received_data[0] != 0)
    {
        /* also a tag without the NXP commands answering with an error */
        ESP_LOGD(TAG, "received RAND with %d bytes, status %d", received_length, received_data[0]);
        return ESP_FAIL;
    }

    rand[0] = received_data[1];
    rand[1] = received_data[2];

    return ESP_OK;
}

esp_err_t nfc_reset(trf7962a_t trf)
{
    ESP_LOGD(TAG, "NFC Reset");
    if (trf7962a_reset(trf) != ESP_OK)
    {
        return ESP_FAIL;
    }
    trf7962a_field(trf, false);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    trf7962a_field(trf, true);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    return ESP_OK;
}

/* reads the 32 byte tag memory the cloud expects as "Authorization: BD <hex>" */
static esp_err_t nfc_read_token(trf7962a_t trf)
{
    /* read into a local buffer (SPI, retries), publish it at the end */
    uint8_t token[32] = {0};
    esp_err_t ret = ESP_OK;
    const int chunk = NFC_TOKEN_BLOCKS_PER_READ * 4;
    for (int block = 0; block * 4 < (int)sizeof(token); block += NFC_TOKEN_BLOCKS_PER_READ)
    {
        bool ok = false;
        for (int retry = 0; retry < 3 && !ok; retry++)
        {
            iso_read_token[2] = block;
            ok = trf7962a_xmit(trf, iso_read_token, sizeof(iso_read_token), received_data, &received_length) == ESP_OK &&
                 received_length >= 1 + chunk && received_data[0] == 0;
        }
        if (!ok)
        {
            ESP_LOGW(TAG, "Could not read tag memory block %d (%d bytes, status %d)", block, received_length,
                     received_length ? received_data[0] : -1);
            memset(token, 0, sizeof(token));
            ret = ESP_FAIL;
            break;
        }
        memcpy(&token[block * 4], &received_data[1], chunk);
    }
    taskENTER_CRITICAL(&nfc_lock);
    memcpy(nfc_current_token, token, sizeof(token));
    taskEXIT_CRITICAL(&nfc_lock);
    return ret;
}

/* when token was detected, try to start playback using UID */
static void nfc_play()
{
    pb_play_content(nfc_get_current_uid());
}

/* later, when memory was read, call pb handler so it will be able to download the file */
static void nfc_play_token()
{
    uint8_t token[32];
    nfc_get_current_token(token);
    pb_play_content_token(nfc_get_current_uid(), token);
}

static void nfc_stop()
{
    pb_stop();
}

/*
 * Power: with the RF field on the TRF7962A draws 70-130 mA. The field is only
 * switched on for the short moments a tag is queried and off in between.
 * A Tonie loses power with the field and comes back in privacy mode, so the
 * presence check unlocks it again with the password that worked before.
 */
static int nfc_unlock_pass = -1; /* index into passes[], -1: tag needs no password */
static int nfc_search_failures = 0;

static void nfc_field_on(trf7962a_t trf)
{
    trf7962a_field(trf, true);
    /* tags need a moment to power up */
    vTaskDelay(pdMS_TO_TICKS(NFC_FIELD_SETTLE_MS));
}

static void nfc_field_off(trf7962a_t trf)
{
    trf7962a_field(trf, false);
}

/* INVENTORY, true if exactly one tag answered; UID in received_data[2..9] */
static bool nfc_inventory(trf7962a_t trf)
{
    if (trf7962a_xmit(trf, slix_get_inventory, sizeof(slix_get_inventory), received_data, &received_length) != ESP_OK)
    {
        return false;
    }
    nfc_log_dump("INVENTORY", received_data, received_length);
    if (received_length != 12 || received_data[0] != 0)
    {
        return false;
    }
    /* every ISO 15693 UID starts with E0 (sent LSB first, so it is the last
       UID byte); 27 Nov: interference (collision/framing errors for a minute)
       let a garbled answer through as UID 1FFFFCFFF01F81F6, the box stopped
       the tonie and tried to download that one */
    if (received_data[9] != 0xE0)
    {
        ESP_LOGW(TAG, "Inventory answer with an invalid UID ignored");
        return false;
    }
    return true;
}

/* privacy password via GET RANDOM + SET PASSWORD */
static bool nfc_unlock(trf7962a_t trf, int pass)
{
    uint8_t rand[2];
    if (nfc_get_rand(trf, rand) != ESP_OK)
    {
        return false;
    }
    slix_set_pass[4] = (passes[pass] >> 0) ^ rand[0];
    slix_set_pass[5] = (passes[pass] >> 8) ^ rand[1];
    slix_set_pass[6] = (passes[pass] >> 16) ^ rand[0];
    slix_set_pass[7] = (passes[pass] >> 24) ^ rand[1];
    nfc_log_dump("SET PASS", slix_set_pass, sizeof(slix_set_pass));
    if (trf7962a_xmit(trf, slix_set_pass, sizeof(slix_set_pass), received_data, &received_length) != ESP_OK)
    {
        return false;
    }
    return received_length == 3 && received_data[0] == 0;
}

static void nfc_new_tag(trf7962a_t trf, const char *what)
{
    /* UID and "valid" together; the token of the previous tag is cleared
       until this one's is read */
    taskENTER_CRITICAL(&nfc_lock);
    memcpy(nfc_current_uid_rev, &received_data[2], 8);
    memset(nfc_current_token, 0, sizeof(nfc_current_token));
    nfc_valid = true;
    taskEXIT_CRITICAL(&nfc_lock);
    ESP_LOGI(TAG, "Tag %s: %llX", what, nfc_get_current_uid());
    /* before the playback starts, teddyCloud expects the tag first */
    rtnl_tag(nfc_get_current_uid(), true);
    tonie_seen_add(nfc_get_current_uid());
    nfc_play();
    /* field is still on and the tag unlocked; received_data is reused, the UID is copied */
    nfc_read_token(trf);
    nfc_field_off(trf);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    nfc_play_token();
}

void nfc_mainthread(void *arg)
{
    nfc_state_t state = STATE_SEARCHING;
    trf7962a_t trf = (trf7962a_t)arg;

    nfc_field_off(trf);
    int tw = taskwatch_add("NFC", 30000);

    while (true)
    {
        taskwatch_feed(tw);
        vTaskDelay(10 / portTICK_PERIOD_MS);

        switch (state)
        {
        case STATE_TAG:
        {
            nfc_field_on(trf);
            bool present = nfc_inventory(trf);
            if (!present && nfc_unlock_pass >= 0)
            {
                /* back in privacy mode after the field was off */
                present = nfc_unlock(trf, nfc_unlock_pass) && nfc_inventory(trf);
            }

            if (!present)
            {
                nfc_field_off(trf);
                if (nfc_retry++ > NFC_RETRIES)
                {
                    state = STATE_SEARCHING;
                }
                else
                {
                    /* check again soon, so removing a tonie is noticed quickly */
                    vTaskDelay(pdMS_TO_TICKS(NFC_RETRY_INTERVAL_MS));
                }
                break;
            }
            nfc_retry = 0;

            if (!nfc_valid)
            {
                nfc_new_tag(trf, "entered");
            }
            else if (memcmp(nfc_current_uid_rev, &received_data[2], 8))
            {
                /* a different UID only counts when a second read agrees, a
                   single disturbed answer must not stop the tonie */
                uint8_t first[8];
                memcpy(first, &received_data[2], 8);
                if (nfc_inventory(trf) && !memcmp(first, &received_data[2], 8))
                {
                    nfc_new_tag(trf, "changed");
                }
                else
                {
                    ESP_LOGW(TAG, "Other UID read once, not confirmed: ignored");
                    nfc_field_off(trf);
                }
            }
            else
            {
                nfc_field_off(trf);
            }
            vTaskDelay(pdMS_TO_TICKS(NFC_TAG_CHECK_INTERVAL_MS));
            break;
        }

        case STATE_SEARCHING:
        default:
        {
            uint8_t rand[2];

            if (nfc_valid)
            {
                uint64_t gone = nfc_get_current_uid();
                nfc_set_valid(false);
                nfc_dump(dump_buf, nfc_current_uid_rev, 8);
                ESP_LOGI(TAG, "Tag disappeared: %s", dump_buf);
                rtnl_tag_removed(gone);
                nfc_stop();
            }

            nfc_field_on(trf);
            if (nfc_get_rand(trf, rand) != ESP_OK)
            {
                /* tags without the NXP commands (other makers, no privacy mode,
                   e.g. self-made tonies) answer a plain INVENTORY; a tonie in
                   privacy mode stays silent there. Only every second search,
                   the extra exchange keeps the field on longer. */
                if ((nfc_search_failures & 1) && nfc_inventory(trf))
                {
                    ESP_LOGI(TAG, "Tag without privacy mode found");
                    nfc_search_failures = 0;
                    nfc_unlock_pass = -1;
                    nfc_retry = 0;
                    state = STATE_TAG;
                    break;
                }
                /* nothing there; every few seconds reinitialize the chip in case it got stuck */
                if (++nfc_search_failures >= NFC_SEARCH_RESET_EVERY)
                {
                    nfc_search_failures = 0;
                    nfc_reset(trf);
                }
                nfc_field_off(trf);
                vTaskDelay(pdMS_TO_TICKS(NFC_SEARCH_INTERVAL_MS));
                break;
            }
            nfc_search_failures = 0;

            if (nfc_inventory(trf))
            {
                ESP_LOGI(TAG, "Unlocked tag found");
                nfc_unlock_pass = -1;
                nfc_retry = 0;
                state = STATE_TAG;
                break;
            }
            ESP_LOGI(TAG, "Locked tag detected");

            bool unlocked = false;
            for (int pass = 0; pass < COUNT(passes) && !unlocked; pass++)
            {
                ESP_LOGI(TAG, "Test pass 0x%08X", passes[pass]);
                nfc_reset(trf);
                if (nfc_unlock(trf, pass))
                {
                    unlocked = true;
                    nfc_unlock_pass = pass;
                }
                else
                {
                    ESP_LOGE(TAG, "  Password incorrect");
                }
            }

            if (unlocked)
            {
                ESP_LOGI(TAG, "Unlocked tag");
                nfc_retry = 0;
                state = STATE_TAG;
            }
            else
            {
                nfc_field_off(trf);
                vTaskDelay(pdMS_TO_TICKS(NFC_SEARCH_INTERVAL_MS));
            }
            break;
        }
        }
    }
}

void nfc_init()
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    trf7962a_t trf = audio_board_get_trf();

    if (nfc_reset(trf) != ESP_OK)
    {
        ESP_LOGE(TAG, "NFC chip not detected. Exiting.");
        return;
    }

    xTaskCreatePinnedToCore(nfc_mainthread, "[TB] NFC", 4608 /* 2.1 KB used */, trf, NFC_TASK_PRIO, NULL, tskNO_AFFINITY);
}
