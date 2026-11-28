#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "freshness.h"
#include "cloud.h"
#include "playback.h"
#include "maint.h"
#include "toniebox.pb.freshness-check.fc-request.pb-c.h"

static const char *TAG = "Freshness";

#define CONTENT_DIR "/sdcard/CONTENT"
/* ~40 bytes per entry while the check runs */
#define FC_MAX_TONIES 128
#define FC_RESP_MAX 4096
/* every tonie UID ends with E0 04 03, stored byte reversed in the file name */
#define TONIE_FILE_SUFFIX "0304E0"

static bool parse_hex8(const char *s, uint8_t bytes[4])
{
    if (strlen(s) != 8)
    {
        return false;
    }
    for (int i = 0; i < 8; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return false;
        }
    }
    for (int i = 0; i < 4; i++)
    {
        char h[3] = {s[2 * i], s[2 * i + 1], 0};
        bytes[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    return true;
}

bool freshness_uid_from_names(const char *dir, const char *file, uint64_t *uid)
{
    uint8_t lo[4], hi[4];
    if (!parse_hex8(dir, lo) || !parse_hex8(file, hi) || strcasecmp(file + 2, TONIE_FILE_SUFFIX))
    {
        return false;
    }
    /* same layout as pb_build_filename(): byte 0 first */
    uint64_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        v |= (uint64_t)lo[i] << (8 * i);
        v |= (uint64_t)hi[i] << (8 * (i + 4));
    }
    *uid = v;
    return true;
}

static bool read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7)
    {
        if (*pos >= len)
        {
            return false;
        }
        uint8_t b = buf[(*pos)++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
        {
            *out = v;
            return true;
        }
    }
    return false;
}

static uint64_t read_fixed64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

/* teddyCloud: max_vol 0=25 %, 1=50 %, 2=75 %, 3=100 %; led 0=on, 1=off, 2=dimmed;
   slap_dir 1 = back on the left side (the box's slap_reverse) */
static int fc_vol_percent(int v) { return v >= 0 && v <= 3 ? 25 * (v + 1) : -1; }

static int fc_led_brightness(int v) { return v == 0 ? 3 : v == 1 ? 0 : v == 2 ? 1 : -1; }

int freshness_box_changes(const freshness_settings_t *now, const freshness_settings_t *last, int cur_slap_level,
                          freshness_box_changes_t *out)
{
    *out = (freshness_box_changes_t){-1, -1, -1, -1, -1};
    if (!last)
    {
        return 0;
    }
#define FC_CHANGED(f) (now->f >= 0 && now->f != last->f)
    int n = 0;
    if (FC_CHANGED(max_vol_spk) && (out->volume_max_spk = fc_vol_percent(now->max_vol_spk)) >= 0)
    {
        n++;
    }
    if (FC_CHANGED(max_vol_hdp) && (out->volume_max_hdp = fc_vol_percent(now->max_vol_hdp)) >= 0)
    {
        n++;
    }
    if (FC_CHANGED(led) && (out->led_brightness = fc_led_brightness(now->led)) >= 0)
    {
        n++;
    }
    if (FC_CHANGED(slap_en) && (now->slap_en == 0 || now->slap_en == 1))
    {
        out->slap_level = now->slap_en ? (cur_slap_level > 0 ? cur_slap_level : 3) : 0;
        n++;
    }
    if (FC_CHANGED(slap_dir) && (now->slap_dir == 0 || now->slap_dir == 1))
    {
        out->slap_reverse = now->slap_dir;
        n++;
    }
#undef FC_CHANGED
    return n;
}

int freshness_parse_response(const uint8_t *buf, size_t len, uint64_t *marked, int max_marked, freshness_settings_t *settings)
{
    /* parsed by hand: all fields are "required" in the .proto, a strict
       unpacker would reject a response that misses one of them */
    freshness_settings_t s = {-1, -1, -1, -1, -1};
    int n = 0;
    size_t pos = 0;

    while (pos < len)
    {
        uint64_t key;
        if (!read_varint(buf, len, &pos, &key))
        {
            return -1;
        }
        uint32_t field = key >> 3, type = key & 7;

        if (field == 1 && type == 1) /* repeated fixed64, not packed */
        {
            if (pos + 8 > len)
            {
                return -1;
            }
            if (n < max_marked)
            {
                marked[n++] = read_fixed64(buf + pos);
            }
            pos += 8;
        }
        else if (field == 1 && type == 2) /* packed variant */
        {
            uint64_t l;
            if (!read_varint(buf, len, &pos, &l) || l > len - pos || l % 8)
            {
                return -1;
            }
            for (size_t i = 0; i < l; i += 8)
            {
                if (n < max_marked)
                {
                    marked[n++] = read_fixed64(buf + pos + i);
                }
            }
            pos += l;
        }
        else if (type == 0)
        {
            uint64_t v;
            if (!read_varint(buf, len, &pos, &v))
            {
                return -1;
            }
            int32_t iv = (int32_t)v;
            switch (field)
            {
            case 3: s.max_vol_spk = iv; break;
            case 4: s.slap_en = iv; break;
            case 5: s.slap_dir = iv; break;
            case 7: s.max_vol_hdp = iv; break;
            case 8: s.led = iv; break;
            default: break;
            }
        }
        else if (type == 2)
        {
            uint64_t l;
            if (!read_varint(buf, len, &pos, &l) || l > len - pos)
            {
                return -1;
            }
            pos += l;
        }
        else if (type == 1)
        {
            pos += 8;
        }
        else if (type == 5)
        {
            pos += 4;
        }
        else
        {
            return -1;
        }
        if (pos > len)
        {
            return -1;
        }
    }

    if (settings)
    {
        *settings = s;
    }
    return n;
}

/* collects UID and audio_id of every tonie file, removes .old leftovers whose new version is complete */
static int collect_tonies(TonieFCInfo *infos, int max, int *unreadable)
{
    int skipped_custom = 0;
    *unreadable = 0;
    DIR *top = opendir(CONTENT_DIR);
    if (!top)
    {
        ESP_LOGW(TAG, "No %s on the SD card", CONTENT_DIR);
        return 0;
    }

    int n = 0;
    struct dirent *de;
    while ((de = readdir(top)) && n < max)
    {
        uint8_t tmp[4];
        if (!parse_hex8(de->d_name, tmp))
        {
            continue;
        }
        char dir_path[48];
        snprintf(dir_path, sizeof(dir_path), CONTENT_DIR "/%.8s", de->d_name);
        char dir_name[9];
        snprintf(dir_name, sizeof(dir_name), "%.8s", de->d_name);

        DIR *sub = opendir(dir_path);
        if (!sub)
        {
            continue;
        }
        struct dirent *fe;
        while ((fe = readdir(sub)) && n < max)
        {
            uint64_t uid;
            if (!freshness_uid_from_names(dir_name, fe->d_name, &uid))
            {
                continue;
            }
            char path[64];
            snprintf(path, sizeof(path), "%s/%.8s", dir_path, fe->d_name);
            if (pb_file_is_own(path))
            {
                /* uploaded on the setup page: not teddyCloud's business */
                continue;
            }
            if (cloud_backend_is_boxine())
            {
                /* the Boxine cloud only hears about original tonies: by UID and
                   by the token saved when it was placed (none saved: unknown,
                   left out too) */
                uint8_t token[32];
                if (cloud_tag_is_custom(uid, NULL) || !maint_token_load(uid, token) || cloud_tag_is_custom(uid, token))
                {
                    skipped_custom++;
                    continue;
                }
            }

            FILE *f = fopen(path, "rb");
            if (!f)
            {
                continue;
            }
            TonieboxAudioFileHeader *taf = pb_toniefile_get_header(f);
            struct stat st;
            bool complete = taf && fstat(fileno(f), &st) == 0 && (uint64_t)st.st_size == taf->num_bytes + TONIEFILE_FRAME_SIZE;
            fclose(f);
            if (!taf)
            {
                (*unreadable)++;
                continue;
            }

            tonie_fcinfo__init(&infos[n]);
            infos[n].uid = uid;
            infos[n].audio_id = taf->audio_id;
            toniebox_audio_file_header__free_unpacked(taf, NULL);
            n++;

            if (complete)
            {
                char old[72];
                snprintf(old, sizeof(old), "%s.old", path);
                unlink(old); /* fails silently if there is none */
            }
        }
        closedir(sub);
    }
    closedir(top);
    if (skipped_custom)
    {
        ESP_LOGI(TAG, "%d tag(s) that are not original tonies left out (Boxine cloud)", skipped_custom);
    }
    return n;
}

static void park_outdated(uint64_t uid)
{
    if (pb_is_playing() && pb_get_current_uid() == uid)
    {
        ESP_LOGI(TAG, "  %016llX is playing right now, checking again next time", (unsigned long long)uid);
        return;
    }

    char *file = pb_build_filename(uid);
    struct stat st;
    if (pb_file_is_own(file))
    {
        ESP_LOGI(TAG, "  %016llX was uploaded on the setup page, kept", (unsigned long long)uid);
        free(file);
        return;
    }
    if (stat(file, &st) != 0)
    {
        /* teddyCloud may report tonies it knows for this box that were never played here */
        ESP_LOGI(TAG, "  %016llX not on this box, nothing to do", (unsigned long long)uid);
        free(file);
        return;
    }

    if (pb_check_file(file) == PB_ERR_GOOD_FILE)
    {
        /* keep it as fallback until the new version is complete */
        char old[72];
        snprintf(old, sizeof(old), "%s.old", file);
        unlink(old);
        if (rename(file, old) == 0)
        {
            ESP_LOGI(TAG, "  %016llX outdated, new version is loaded next time it is placed", (unsigned long long)uid);
        }
        /* an unfinished download is of the old version too: must not be resumed */
        char tmp[72];
        snprintf(tmp, sizeof(tmp), "%s.tmp", file);
        unlink(tmp);
    }
    else
    {
        /* a partial download of the old version must not be resumed */
        unlink(file);
        ESP_LOGI(TAG, "  %016llX outdated, partial download removed", (unsigned long long)uid);
    }
    free(file);
}

static volatile int runs_done = 0;

int freshness_runs(void)
{
    return runs_done;
}

esp_err_t freshness_run(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    TonieFCInfo *infos = calloc(FC_MAX_TONIES, sizeof(TonieFCInfo));
    TonieFCInfo **ptrs = calloc(FC_MAX_TONIES, sizeof(TonieFCInfo *));
    uint8_t *resp = malloc(FC_RESP_MAX);
    uint64_t *marked = calloc(FC_MAX_TONIES, sizeof(uint64_t));
    uint8_t *req_buf = NULL;
    esp_err_t ret = ESP_FAIL;

    if (!infos || !ptrs || !resp || !marked)
    {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }

    int unreadable;
    int n = collect_tonies(infos, FC_MAX_TONIES, &unreadable);
    if (unreadable)
    {
        ESP_LOGW(TAG, "%d tonie header(s) could not be read", unreadable);
    }
    if (n == FC_MAX_TONIES)
    {
        ESP_LOGW(TAG, "More than %d tonies on the SD card, only the first %d are checked", FC_MAX_TONIES, FC_MAX_TONIES);
    }
    for (int i = 0; i < n; i++)
    {
        ptrs[i] = &infos[i];
    }

    if (n == 0 && unreadable)
    {
        /* most likely no memory just now (28 Nov): try again later
           instead of reporting an empty card */
        ret = ESP_FAIL;
        goto out;
    }
    if (n == 0)
    {
        /* nothing to ask for; teddyCloud would also wait for a body that never comes */
        ESP_LOGI(TAG, "No tonies on the SD card, nothing to check");
        ret = ESP_OK;
        goto out;
    }

    TonieFreshnessCheckRequest req = TONIE_FRESHNESS_CHECK_REQUEST__INIT;
    req.n_tonie_infos = n;
    req.tonie_infos = ptrs;
    size_t req_len = tonie_freshness_check_request__get_packed_size(&req);
    /* an empty request is valid, but the body must not be NULL to make it a POST */
    req_buf = malloc(req_len ? req_len : 1);
    if (!req_buf)
    {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    tonie_freshness_check_request__pack(&req, req_buf);

    ESP_LOGI(TAG, "Checking %d tonie(s)", n);
    size_t resp_len = 0;
    if (cloud_post("/v1/freshness-check", req_buf, req_len, resp, FC_RESP_MAX, &resp_len) != ESP_OK)
    {
        goto out;
    }

    freshness_settings_t settings;
    int marked_count = freshness_parse_response(resp, resp_len, marked, FC_MAX_TONIES, &settings);
    if (marked_count < 0)
    {
        ESP_LOGE(TAG, "Invalid response (%u bytes)", (unsigned)resp_len);
        goto out;
    }

    ESP_LOGI(TAG, "%d of %d tonie(s) have new content", marked_count, n);
    for (int i = 0; i < marked_count; i++)
    {
        park_outdated(marked[i]);
    }
    ESP_LOGI(TAG, "teddyCloud box settings: max volume speaker %d, headphones %d, slap %d/%d, led %d",
             settings.max_vol_spk, settings.max_vol_hdp, settings.slap_en, settings.slap_dir, settings.led);
    freshness_box_settings_received(&settings);
    ret = ESP_OK;

out:
    if (ret == ESP_OK)
    {
        runs_done++;
    }
    free(infos);
    free(ptrs);
    free(resp);
    free(marked);
    free(req_buf);
    return ret;
}
