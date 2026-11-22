#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "mbedtls/sha1.h"
#include "tonies_portal.h"
#include "playback.h"
#include "freshness.h"
#include "tonie_seen.h"
#include "proto/toniebox.pb.taf-header.pb-c.h"

static const char *TAG = "Tonies";
#define CONTENT_DIR "/sdcard/CONTENT"
#define LIST_MAX 300
#define UPLOAD_BUF 16384
/* consecutive receive timeouts before an upload is given up */
#define UPLOAD_TIMEOUTS_MAX 10

static void (*touch_cb)(void);

static void touch(void)
{
    if (touch_cb)
    {
        touch_cb();
    }
}

static esp_err_t send_msg(httpd_req_t *req, bool ok, const char *error, const char *extra_json)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":%s%s%s%s%s%s}", ok ? "true" : "false", error ? ",\"error\":\"" : "",
             error ? error : "", error ? "\"" : "", extra_json ? "," : "", extra_json ? extra_json : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* "?uid=E004035012345678" */
static bool query_uid(httpd_req_t *req, uint64_t *uid)
{
    char query[64] = "", val[20] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "uid", val, sizeof(val)) != ESP_OK || strlen(val) != 16)
    {
        return false;
    }
    char *end;
    *uid = strtoull(val, &end, 16);
    return *end == 0 && *uid != 0;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void variant(char *out, size_t len, const char *base, const char *suffix)
{
    snprintf(out, len, "%s%s", base, suffix);
}

/* ---------------------------------------------------------------- list */

static int collect_uids(uint64_t *uids, int max)
{
    DIR *top = opendir(CONTENT_DIR);
    if (!top)
    {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(top)) && n < max)
    {
        if (strlen(de->d_name) != 8)
        {
            continue;
        }
        char dir_path[32];
        snprintf(dir_path, sizeof(dir_path), CONTENT_DIR "/%.8s", de->d_name);
        DIR *sub = opendir(dir_path);
        if (!sub)
        {
            continue;
        }
        struct dirent *fe;
        while ((fe = readdir(sub)) && n < max)
        {
            char name[9];
            snprintf(name, sizeof(name), "%.8s", fe->d_name);
            uint64_t uid;
            if (!freshness_uid_from_names(de->d_name, name, &uid))
            {
                continue;
            }
            bool dup = false;
            for (int i = 0; i < n && !dup; i++)
            {
                dup = uids[i] == uid;
            }
            if (!dup)
            {
                uids[n++] = uid;
            }
        }
        closedir(sub);
    }
    closedir(top);
    return n;
}

static void entry_json(char *out, size_t len, uint64_t uid)
{
    char *file = pb_build_filename(uid);
    char p[72];
    long size = file_size(file);
    uint32_t audio_id = 0;
    int chapters = 0;
    long long num_bytes = -1;
    bool stream = false;
    if (size >= 0)
    {
        FILE *f = fopen(file, "rb");
        TonieboxAudioFileHeader *taf = f ? pb_toniefile_get_header(f) : NULL;
        if (f)
        {
            fclose(f);
        }
        if (taf)
        {
            audio_id = taf->audio_id;
            chapters = (int)taf->n_track_page_nums;
            num_bytes = (long long)taf->num_bytes;
            stream = PB_IS_STREAM_LENGTH(taf->num_bytes);
            toniebox_audio_file_header__free_unpacked(taf, NULL);
        }
    }
    bool complete = size >= 0 && num_bytes >= 0 && size == num_bytes + TONIEFILE_FRAME_SIZE;
    variant(p, sizeof(p), file, ".tmp");
    long tmp = file_size(p);
    variant(p, sizeof(p), file, ".old");
    bool old = file_size(p) >= 0;
    variant(p, sizeof(p), file, ".bad");
    bool bad = file_size(p) >= 0;
    bool own = pb_file_is_own(file);
    free(file);

    const char *state = complete ? (old ? "ok_old" : "ok") : tmp >= 0 || size >= 0 ? "partial" : bad ? "bad" : old ? "old" : "none";
    snprintf(out, len,
             "{\"uid\":\"%016llX\",\"state\":\"%s\",\"size\":%ld,\"tmp\":%ld,\"audio_id\":%lu,\"chapters\":%d,"
             "\"own\":%s,\"bad\":%s,\"stream\":%s}",
             (unsigned long long)uid, state, size, tmp, (unsigned long)audio_id, chapters, own ? "true" : "false",
             bad ? "true" : "false", stream ? "true" : "false");
}

static esp_err_t list_handler(httpd_req_t *req)
{
    touch();
    uint64_t *uids = malloc(sizeof(uint64_t) * LIST_MAX);
    if (!uids)
    {
        return send_msg(req, false, "Kein Speicher frei", NULL);
    }
    int n = collect_uids(uids, LIST_MAX);
    httpd_resp_set_type(req, "application/json");
    char buf[320];
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info("/sdcard", &total, &free_b);
    snprintf(buf, sizeof(buf), "{\"total_mb\":%llu,\"free_mb\":%llu,\"tonies\":[", (unsigned long long)(total >> 20),
             (unsigned long long)(free_b >> 20));
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    for (int i = 0; i < n; i++)
    {
        if (i)
        {
            httpd_resp_send_chunk(req, ",", 1);
        }
        entry_json(buf, sizeof(buf), uids[i]);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    free(uids);
    httpd_resp_send_chunk(req, "],\"seen\":[", HTTPD_RESP_USE_STRLEN);
    tonie_seen_t seen[TONIE_SEEN_MAX];
    int s = tonie_seen_get(seen);
    for (int i = 0; i < s; i++)
    {
        snprintf(buf, sizeof(buf), "%s{\"uid\":\"%016llX\",\"time\":%lu}", i ? "," : "", (unsigned long long)seen[i].uid,
                 (unsigned long)seen[i].time);
        httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* -------------------------------------------------------------- delete */

/* all variants of a tonie file; keep_own (upload): not .own and .upl */
static void remove_all(const char *file, bool keep_own)
{
    static const char *suffix[] = {"", ".tmp", ".old", ".bad", ".upl", ".own"};
    char p[72];
    for (int i = 0; i < 6; i++)
    {
        /* upload: keep the marker and the new file itself */
        if (keep_own && (!strcmp(suffix[i], ".own") || !strcmp(suffix[i], ".upl")))
        {
            continue;
        }
        variant(p, sizeof(p), file, suffix[i]);
        unlink(p);
    }
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    touch();
    uint64_t uid;
    if (!query_uid(req, &uid))
    {
        return send_msg(req, false, "Ungültige UID", NULL);
    }
    char *file = pb_build_filename(uid);
    remove_all(file, false);
    free(file);
    ESP_LOGI(TAG, "%016llX deleted", (unsigned long long)uid);
    return send_msg(req, true, NULL, NULL);
}

/* ---------------------------------------------------------- protection */

static esp_err_t own_handler(httpd_req_t *req)
{
    touch();
    uint64_t uid;
    char query[64] = "", on[4] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "on", on, sizeof(on));
    if (!query_uid(req, &uid))
    {
        return send_msg(req, false, "Ungültige UID", NULL);
    }
    char *file = pb_build_filename(uid);
    char p[72];
    variant(p, sizeof(p), file, ".own");
    bool ok;
    if (!strcmp(on, "1"))
    {
        FILE *f = file_size(file) >= 0 ? fopen(p, "w") : NULL;
        ok = f != NULL;
        if (f)
        {
            fputs("uploaded or protected on the TeddyBox setup page\n", f);
            fclose(f);
        }
    }
    else
    {
        unlink(p);
        ok = true;
    }
    free(file);
    return send_msg(req, ok, ok ? NULL : "Keine Datei für diesen Tonie", NULL);
}

/* -------------------------------------------------------------- upload */

/* checks the 4 KB TAF header; NULL if fine */
static const char *check_header(const uint8_t *buf, size_t total, uint8_t sha1[20], uint32_t *audio_id, int *chapters)
{
    uint32_t len = (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
    if (len == 0 || len > TONIEFILE_FRAME_SIZE - 4)
    {
        return "Keine TAF-Datei (Header)";
    }
    TonieboxAudioFileHeader *taf = toniebox_audio_file_header__unpack(NULL, len, buf + 4);
    if (!taf)
    {
        return "Keine TAF-Datei (Header nicht lesbar)";
    }
    const char *err = NULL;
    if (PB_IS_STREAM_LENGTH(taf->num_bytes))
    {
        err = "Stream-Dateien können nicht hochgeladen werden";
    }
    else if (taf->num_bytes + TONIEFILE_FRAME_SIZE != total)
    {
        err = "Dateilänge passt nicht zum Header (unvollständig?)";
    }
    else if (taf->sha1_hash.len != 20)
    {
        err = "Header ohne SHA-1";
    }
    else
    {
        memcpy(sha1, taf->sha1_hash.data, 20);
        *audio_id = taf->audio_id;
        *chapters = (int)taf->n_track_page_nums;
    }
    toniebox_audio_file_header__free_unpacked(taf, NULL);
    return err;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    touch();
    uint64_t uid;
    if (!query_uid(req, &uid))
    {
        return send_msg(req, false, "Ungültige UID", NULL);
    }
    size_t total = req->content_len;
    if (total < TONIEFILE_FRAME_SIZE * 2)
    {
        return send_msg(req, false, "Datei zu klein", NULL);
    }
    uint64_t sd_total = 0, sd_free = 0;
    esp_vfs_fat_info("/sdcard", &sd_total, &sd_free);
    if (sd_free < (uint64_t)total + 64 * 1024)
    {
        return send_msg(req, false, "Nicht genug Platz auf der SD-Karte", NULL);
    }

    char *file = pb_build_filename(uid);
    char dir[32], upl[72];
    snprintf(dir, sizeof(dir), "%.24s", file); /* /sdcard/CONTENT/XXXXXXXX */
    mkdir(CONTENT_DIR, 0777);
    mkdir(dir, 0777);
    variant(upl, sizeof(upl), file, ".upl");

    uint8_t *buf = malloc(UPLOAD_BUF);
    FILE *f = buf ? fopen(upl, "wb") : NULL;
    const char *err = !buf ? "Kein Speicher frei" : !f ? "Datei kann nicht angelegt werden" : NULL;
    uint8_t sha1[20], digest[20];
    uint32_t audio_id = 0;
    int chapters = 0;
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    size_t got = 0;
    int timeouts = 0;
    int64_t t0 = esp_timer_get_time();

    while (!err && got < total)
    {
        /* the first block is exactly the 4 KB header */
        size_t want = got == 0 ? TONIEFILE_FRAME_SIZE : (total - got < UPLOAD_BUF ? total - got : UPLOAD_BUF);
        size_t fill = 0;
        while (fill < want && !err)
        {
            int r = httpd_req_recv(req, (char *)buf + fill, want - fill);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
            {
                if (++timeouts > UPLOAD_TIMEOUTS_MAX)
                {
                    err = "Zeitüberschreitung";
                }
                continue;
            }
            if (r <= 0)
            {
                err = "Verbindung abgebrochen";
                break;
            }
            timeouts = 0;
            fill += r;
        }
        if (err)
        {
            break;
        }
        touch();
        if (got == 0)
        {
            err = check_header(buf, total, sha1, &audio_id, &chapters);
        }
        else
        {
            mbedtls_sha1_update(&ctx, buf, fill);
        }
        if (!err && fwrite(buf, 1, fill, f) != fill)
        {
            err = "Schreibfehler auf der SD-Karte";
        }
        got += fill;
    }
    mbedtls_sha1_finish(&ctx, digest);
    mbedtls_sha1_free(&ctx);
    if (f && fclose(f) != 0 && !err)
    {
        err = "Schreibfehler auf der SD-Karte";
    }
    free(buf);
    if (!err && memcmp(digest, sha1, 20))
    {
        err = "SHA-1 stimmt nicht, Datei beschädigt";
    }

    if (err)
    {
        ESP_LOGE(TAG, "Upload %016llX failed: %s", (unsigned long long)uid, err);
        unlink(upl);
        /* drain the rest so the browser gets the answer */
        char tmp[256];
        while (got < total)
        {
            int r = httpd_req_recv(req, tmp, sizeof(tmp));
            if (r <= 0)
            {
                break;
            }
            got += r;
        }
        free(file);
        return send_msg(req, false, err, NULL);
    }

    /* replace whatever was there, mark as own */
    remove_all(file, true);
    char own[72];
    variant(own, sizeof(own), file, ".own");
    bool ok = rename(upl, file) == 0;
    FILE *o = ok ? fopen(own, "w") : NULL;
    if (o)
    {
        fputs("uploaded on the TeddyBox setup page\n", o);
        fclose(o);
    }
    free(file);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "Upload %016llX: %u bytes, audio id %08lX, %d chapters, %d.%d s", (unsigned long long)uid,
             (unsigned)total, (unsigned long)audio_id, chapters, ms / 1000, ms % 1000 / 100);
    char extra[96];
    snprintf(extra, sizeof(extra), "\"audio_id\":%lu,\"chapters\":%d,\"seconds\":%d", (unsigned long)audio_id, chapters,
             ms / 1000);
    return send_msg(req, ok, ok ? NULL : "Umbenennen fehlgeschlagen", extra);
}

esp_err_t tonies_portal_register(httpd_handle_t server, void (*touch_fn)(void))
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    touch_cb = touch_fn;
    const httpd_uri_t uris[] = {
        {.uri = "/api/tonies", .method = HTTP_GET, .handler = list_handler},
        {.uri = "/api/tonie/upload", .method = HTTP_POST, .handler = upload_handler},
        {.uri = "/api/tonie/delete", .method = HTTP_POST, .handler = delete_handler},
        {.uri = "/api/tonie/own", .method = HTTP_POST, .handler = own_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK)
        {
            return e;
        }
    }
    return ESP_OK;
}
