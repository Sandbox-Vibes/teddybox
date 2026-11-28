/*
 * teddyCloud patch of the original firmware, done on the box (setup page,
 * System tab), the same as teddyCloud's ESP32 patch of a flash image:
 *
 *  - the CA in flash (/spiflash/cert/ca.der) becomes teddyCloud's CA; the
 *    original Boxine CA is kept on the SD card first (CLOUD_BOXINE_CA_FILE,
 *    also used by TeddyBox's Boxine fallback from then on)
 *  - in the original app (slot found by bootsel), the server names
 *    "prod.de.tbs.toys" and "rtnl.bxcl.de" are replaced by the teddyCloud
 *    host (zero padded). A host of 13 to 16 characters fits only the first
 *    one: the live log (RTNL) of the original firmware then still goes to
 *    rtnl.bxcl.de and fails there (teddyCloud's CA). The image checksum and
 *    its SHA-256 are fixed, the image is verified
 *
 * TeddyBox itself does not need any of this (its CA and host are in
 * teddybox.ini). Every changed position is stored in NVS, so the patch can
 * be undone byte for byte. A power loss while a flash sector is rewritten
 * can break the original app: the setup page asks for the flash backup
 * first.
 */
#include "ofw_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "spi_flash_mmap.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "bootsel.h"
#include "cloud.h"
#include "settings.h"

static const char *TAG = "OFW-Patch";

#define FLASH_CA "/spiflash/cert/ca.der"
#define FLASH_CA_NEW "/spiflash/cert/ca.new"
#define SD_TC_CA "/sdcard/teddybox/ca.der"
#define NVS_NS "ofwpatch"
#define NVS_KEY "rec"

static const char *const names[OFW_NAME_KINDS] = {"prod.de.tbs.toys", "rtnl.bxcl.de"};

typedef struct
{
    char host[OFW_HOST_MAX + 1];
    char label[17];   /* slot of the original app at patch time */
    uint32_t img_len; /* its image length, hash included */
    uint8_t hash;     /* SHA-256 appended */
    uint8_t n;
    struct
    {
        uint32_t off;
        uint8_t kind;
    } e[OFW_MAX_HITS];
} patch_rec_t;

static void set_err(char *err, size_t size, const char *msg)
{
    if (err && size)
    {
        snprintf(err, size, "%s", msg);
    }
    ESP_LOGE(TAG, "%s", msg);
}

bool ofw_patch_host_valid(const char *host)
{
    size_t n = host ? strlen(host) : 0;
    if (n == 0 || n > OFW_HOST_MAX)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!isalnum((unsigned char)host[i]) && host[i] != '.' && host[i] != '-')
        {
            return false;
        }
    }
    return true;
}

/* verified image: length and whether a SHA-256 is appended */
static esp_err_t image_info(const esp_partition_t *part, uint32_t *len, bool *hash)
{
    esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    esp_image_metadata_t md;
    esp_err_t r = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md);
    if (r != ESP_OK)
    {
        return r;
    }
    *len = md.image_len;
    *hash = md.image.hash_appended;
    return ESP_OK;
}

/* changes bytes in flash: read the sector, change, erase, write back */
static esp_err_t flash_patch(const esp_partition_t *part, uint32_t off, const uint8_t *data, size_t len)
{
    uint8_t *sec = malloc(SPI_FLASH_SEC_SIZE);
    if (!sec)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = ESP_OK;
    while (len && r == ESP_OK)
    {
        uint32_t base = off & ~(SPI_FLASH_SEC_SIZE - 1);
        size_t in = off - base;
        size_t k = SPI_FLASH_SEC_SIZE - in < len ? SPI_FLASH_SEC_SIZE - in : len;
        r = esp_partition_read(part, base, sec, SPI_FLASH_SEC_SIZE);
        if (r == ESP_OK && memcmp(sec + in, data, k) != 0)
        {
            memcpy(sec + in, data, k);
            r = esp_partition_erase_range(part, base, SPI_FLASH_SEC_SIZE);
            if (r == ESP_OK)
            {
                r = esp_partition_write(part, base, sec, SPI_FLASH_SEC_SIZE);
            }
        }
        off += k;
        data += k;
        len -= k;
    }
    free(sec);
    return r;
}

/* finds the strings (with their NUL) in the image, hash and checksum excluded */
static int find_all(const esp_partition_t *part, uint32_t len, const char *const *pats, int npats,
                    uint32_t *offs, uint8_t *kinds, int max)
{
    enum
    {
        CHUNK = 2048,
        OVER = 32
    };
    uint8_t *buf = malloc(CHUNK + OVER);
    if (!buf)
    {
        return -1;
    }
    int n = 0;
    for (uint32_t pos = 0; pos < len; pos += CHUNK)
    {
        size_t k = len - pos < CHUNK + OVER ? len - pos : CHUNK + OVER;
        if (esp_partition_read(part, pos, buf, k) != ESP_OK)
        {
            n = -1;
            break;
        }
        for (int p = 0; p < npats; p++)
        {
            size_t pl = strlen(pats[p]) + 1;
            for (size_t i = 0; i + pl <= k && i < CHUNK; i++)
            {
                if (buf[i] == (uint8_t)pats[p][0] && !memcmp(buf + i, pats[p], pl))
                {
                    if (n < max)
                    {
                        offs[n] = pos + i;
                        kinds[n] = p;
                    }
                    n++;
                }
            }
        }
    }
    free(buf);
    return n;
}

/* checksum byte (0xEF xor all segment data) and SHA-256, computed anew from
   flash, so also right after an interrupted run */
static esp_err_t fix_image(const esp_partition_t *part, uint32_t len, bool hash)
{
    esp_image_header_t hdr;
    esp_err_t r = esp_partition_read(part, 0, &hdr, sizeof(hdr));
    if (r != ESP_OK || hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *buf = malloc(2048);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }
    uint8_t chk = 0xEF; /* as the ROM: 0xEF xor every data byte */
    uint32_t pos = sizeof(hdr);
    for (int sgi = 0; sgi < hdr.segment_count && r == ESP_OK; sgi++)
    {
        esp_image_segment_header_t sh;
        r = esp_partition_read(part, pos, &sh, sizeof(sh));
        pos += sizeof(sh);
        if (r == ESP_OK && pos + sh.data_len > len)
        {
            r = ESP_ERR_INVALID_SIZE;
        }
        for (uint32_t done = 0; r == ESP_OK && done < sh.data_len;)
        {
            size_t k = sh.data_len - done < 2048 ? sh.data_len - done : 2048;
            r = esp_partition_read(part, pos + done, buf, k);
            for (size_t i = 0; i < k; i++)
            {
                chk ^= buf[i];
            }
            done += k;
        }
        pos += sh.data_len;
    }
    /* padded to 16 bytes, the checksum is the last byte */
    uint32_t chk_off = ((pos + 1 + 15) & ~15u) - 1;
    if (r == ESP_OK && chk_off != len - 1 - (hash ? 32 : 0))
    {
        r = ESP_ERR_INVALID_SIZE;
    }
    if (r == ESP_OK)
    {
        r = flash_patch(part, chk_off, &chk, 1);
    }
    if (r == ESP_OK && hash)
    {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        for (uint32_t p = 0; p < len - 32 && r == ESP_OK; p += 2048)
        {
            size_t k = len - 32 - p < 2048 ? len - 32 - p : 2048;
            r = esp_partition_read(part, p, buf, k);
            mbedtls_sha256_update(&ctx, buf, k);
        }
        uint8_t digest[32];
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
        if (r == ESP_OK)
        {
            r = flash_patch(part, len - 32, digest, 32);
        }
    }
    free(buf);
    return r;
}

/* the bytes of a recorded position: the original name or the host, zero padded */
static size_t name_bytes(const patch_rec_t *rec, int i, bool host, uint8_t out[20])
{
    const char *orig = names[rec->e[i].kind];
    size_t pl = strlen(orig) + 1;
    memset(out, 0, 20);
    memcpy(out, host ? rec->host : orig, host ? strlen(rec->host) : pl);
    return pl;
}

/* writes the strings at the recorded positions: to the host (patch) or back
   to the original names (undo), then fixes checksum and SHA-256 and verifies.
   Every position must hold one of the two (also after an interrupted run),
   otherwise nothing is written */
static esp_err_t write_names(const esp_partition_t *part, const patch_rec_t *rec, bool to_host)
{
    if (strcmp(part->label, rec->label) || rec->img_len < 64 || rec->img_len > part->size)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t now[20], a[20], b[20];
    for (int i = 0; i < rec->n; i++)
    {
        size_t pl = name_bytes(rec, i, false, a);
        name_bytes(rec, i, true, b);
        if (esp_partition_read(part, rec->e[i].off, now, pl) != ESP_OK)
        {
            return ESP_FAIL;
        }
        if (memcmp(now, a, pl) && memcmp(now, b, pl))
        {
            ESP_LOGE(TAG, "Unexpected bytes at 0x%06lx, not touched", (unsigned long)rec->e[i].off);
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_err_t r = ESP_OK;
    for (int i = 0; i < rec->n && r == ESP_OK; i++)
    {
        size_t pl = name_bytes(rec, i, to_host, b);
        r = flash_patch(part, rec->e[i].off, b, pl);
    }
    if (r == ESP_OK)
    {
        r = fix_image(part, rec->img_len, rec->hash);
    }
    uint32_t len;
    bool hash;
    if (r == ESP_OK)
    {
        r = image_info(part, &len, &hash); /* must verify again */
    }
    return r;
}

static bool rec_load(patch_rec_t *rec)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return false;
    }
    size_t sz = sizeof(*rec);
    bool ok = nvs_get_blob(h, NVS_KEY, rec, &sz) == ESP_OK && sz == sizeof(*rec) && rec->n <= OFW_MAX_HITS;
    nvs_close(h);
    return ok;
}

static esp_err_t rec_store(const patch_rec_t *rec)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK)
    {
        return r;
    }
    r = rec ? nvs_set_blob(h, NVS_KEY, rec, sizeof(*rec)) : nvs_erase_key(h, NVS_KEY);
    if (r == ESP_ERR_NVS_NOT_FOUND)
    {
        r = ESP_OK;
    }
    if (r == ESP_OK)
    {
        r = nvs_commit(h);
    }
    nvs_close(h);
    return r;
}

static uint8_t *file_load(const char *path, size_t *len)
{
    uint8_t *p = NULL;
    return cloud_load_cert(path, &p, len) == ESP_OK ? p : NULL;
}

static bool file_write(const char *path, const uint8_t *d, size_t len)
{
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(d, len, 1, f) == 1;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    return ok;
}

/* replaces the CA in flash: new file first, then swapped in */
static bool flash_ca_write(const uint8_t *d, size_t len)
{
    unlink(FLASH_CA_NEW);
    if (!file_write(FLASH_CA_NEW, d, len))
    {
        unlink(FLASH_CA_NEW);
        return false;
    }
    unlink(FLASH_CA);
    return rename(FLASH_CA_NEW, FLASH_CA) == 0;
}

esp_err_t ofw_patch_status(ofw_patch_status_t *st)
{
    memset(st, 0, sizeof(*st));
    size_t len = 0;
    uint8_t *ca = file_load(FLASH_CA, &len);
    st->flash_ca_teddycloud = ca && cloud_ca_is_teddycloud(ca, len);
    free(ca);
    struct stat sb;
    st->boxine_ca_saved = stat(CLOUD_BOXINE_CA_FILE, &sb) == 0;

    patch_rec_t rec;
    if (rec_load(&rec))
    {
        st->patched = true;
        snprintf(st->host, sizeof(st->host), "%s", rec.host);
    }
    const esp_partition_t *part = bootsel_get_original();
    if (!part)
    {
        return ESP_OK;
    }
    st->ofw_found = true;
    uint32_t img;
    bool hash;
    if (image_info(part, &img, &hash) != ESP_OK)
    {
        return ESP_OK;
    }
    st->ofw_valid = true;
    uint32_t offs[OFW_MAX_HITS];
    uint8_t kinds[OFW_MAX_HITS];
    int n = find_all(part, img - (hash ? 32 : 0), names, OFW_NAME_KINDS, offs, kinds, OFW_MAX_HITS);
    for (int i = 0; i < n && i < OFW_MAX_HITS; i++)
    {
        st->names_found[kinds[i]]++;
    }
    return ESP_OK;
}

esp_err_t ofw_patch_apply(const char *host, char *err, size_t errlen)
{
    if (!ofw_patch_host_valid(host))
    {
        set_err(err, errlen, "Address: 1 to 16 characters, letters, digits, '.' and '-'");
        return ESP_ERR_INVALID_ARG;
    }
    const settings_t *s = settings_get();
    if (s->cloud_port != 443)
    {
        /* the original firmware connects to port 443; teddyCloud patches the
           port in the code, which is not done here */
        set_err(err, errlen, "teddyCloud must be reachable on port 443 for the original firmware");
        return ESP_ERR_INVALID_STATE;
    }
    patch_rec_t old;
    if (rec_load(&old))
    {
        set_err(err, errlen, "Already patched, undo first");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = bootsel_get_original();
    if (!part)
    {
        set_err(err, errlen, "No original firmware found");
        return ESP_ERR_NOT_FOUND;
    }

    /* the teddyCloud CA TeddyBox uses */
    size_t tc_len = 0;
    uint8_t *tc = file_load(s->ca_path, &tc_len);
    if (!tc)
    {
        set_err(err, errlen, "No teddyCloud CA: upload it on the Connection tab first");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t img;
    bool hash;
    if (image_info(part, &img, &hash) != ESP_OK)
    {
        free(tc);
        set_err(err, errlen, "The original firmware does not verify, not touched");
        return ESP_ERR_INVALID_STATE;
    }
    patch_rec_t rec = {0};
    snprintf(rec.host, sizeof(rec.host), "%s", host);
    snprintf(rec.label, sizeof(rec.label), "%s", part->label);
    rec.img_len = img;
    rec.hash = hash;
    uint32_t offs[OFW_MAX_HITS];
    uint8_t kinds[OFW_MAX_HITS];
    int n = find_all(part, img - (hash ? 32 : 0), names, OFW_NAME_KINDS, offs, kinds, OFW_MAX_HITS);
    if (n <= 0 || n > OFW_MAX_HITS)
    {
        free(tc);
        set_err(err, errlen, n == 0 ? "Server names not found in the original firmware (patched before?)"
                                    : "Server names found too often, not touched");
        return ESP_ERR_NOT_FOUND;
    }
    bool rtnl_fits = strlen(host) <= OFW_HOST_RTNL_MAX;
    for (int i = 0; i < n; i++)
    {
        if (kinds[i] == 1 && !rtnl_fits)
        {
            continue;
        }
        rec.e[rec.n].off = offs[i];
        rec.e[rec.n].kind = kinds[i];
        rec.n++;
    }
    if (rec.n == 0)
    {
        free(tc);
        set_err(err, errlen, "Server name not found in the original firmware (patched before?)");
        return ESP_ERR_NOT_FOUND;
    }
    if (!rtnl_fits)
    {
        ESP_LOGW(TAG, "'%s' is longer than 12 characters: rtnl.bxcl.de stays", host);
    }

    /* 1. the Boxine CA in flash to the SD card, unless it is teddyCloud's */
    size_t fl_len = 0;
    uint8_t *fl = file_load(FLASH_CA, &fl_len);
    bool fl_is_tc = fl && (cloud_ca_is_teddycloud(fl, fl_len) || (fl_len == tc_len && !memcmp(fl, tc, tc_len)));
    if (fl && !fl_is_tc)
    {
        mkdir("/sdcard/teddybox", S_IRWXU);
        if (!file_write(CLOUD_BOXINE_CA_FILE, fl, fl_len))
        {
            free(fl);
            free(tc);
            set_err(err, errlen, "SD card not writable, nothing changed");
            return ESP_FAIL;
        }
        settings_t *ms = settings_get_mutable();
        snprintf(ms->boxine_ca, sizeof(ms->boxine_ca), "%s", CLOUD_BOXINE_CA_FILE);
        ESP_LOGI(TAG, "Boxine CA kept in '%s'", CLOUD_BOXINE_CA_FILE);
    }
    free(fl);

    /* 2. names in the original app; record first, so an interrupted patch can be undone */
    esp_err_t r = rec_store(&rec);
    if (r == ESP_OK)
    {
        ESP_LOGW(TAG, "Patching %d server name(s) in '%s' to '%s'", rec.n, part->label, host);
        r = write_names(part, &rec, true);
        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "Patch failed (%s), restoring", esp_err_to_name(r));
            if (write_names(part, &rec, false) == ESP_OK)
            {
                rec_store(NULL);
            }
        }
    }
    if (r != ESP_OK)
    {
        free(tc);
        set_err(err, errlen, "Writing the original firmware failed, see the log");
        return r;
    }

    /* 3. teddyCloud's CA into flash */
    if (!fl_is_tc && !flash_ca_write(tc, tc_len))
    {
        free(tc);
        set_err(err, errlen, "Names patched, but the CA in flash could not be written");
        return ESP_FAIL;
    }
    free(tc);
    ESP_LOGW(TAG, "Original firmware patched for teddyCloud at '%s'", host);
    return ESP_OK;
}

esp_err_t ofw_patch_undo(char *err, size_t errlen)
{
    patch_rec_t rec;
    if (!rec_load(&rec))
    {
        set_err(err, errlen, "Not patched by TeddyBox");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = bootsel_get_original();
    if (!part)
    {
        set_err(err, errlen, "No original firmware found");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t r = write_names(part, &rec, false);
    if (r != ESP_OK)
    {
        set_err(err, errlen, "Restoring the original firmware failed, see the log");
        return r;
    }
    rec_store(NULL);
    ESP_LOGW(TAG, "Server names in '%s' restored", part->label);

    /* the Boxine CA back into flash, if it was kept */
    size_t bx_len = 0;
    uint8_t *bx = file_load(CLOUD_BOXINE_CA_FILE, &bx_len);
    if (!bx || cloud_ca_is_teddycloud(bx, bx_len))
    {
        free(bx);
        set_err(err, errlen, "Names restored; no original CA on the SD card, the CA in flash stays teddyCloud's");
        return ESP_ERR_NOT_FOUND;
    }
    settings_t *ms = settings_get_mutable();
    if (!strcmp(ms->ca_path, FLASH_CA))
    {
        /* TeddyBox took teddyCloud's CA from flash: keep it on the SD card */
        size_t fl_len = 0;
        uint8_t *fl = file_load(FLASH_CA, &fl_len);
        mkdir("/sdcard/teddybox", S_IRWXU);
        bool ok = fl && file_write(SD_TC_CA, fl, fl_len);
        free(fl);
        if (!ok)
        {
            free(bx);
            set_err(err, errlen, "Names restored; teddyCloud's CA could not be saved to the SD card, the CA in flash stays");
            return ESP_FAIL;
        }
        snprintf(ms->ca_path, sizeof(ms->ca_path), "%s", SD_TC_CA);
    }
    bool ok = flash_ca_write(bx, bx_len);
    free(bx);
    if (!ok)
    {
        set_err(err, errlen, "Names restored; the CA in flash could not be written");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "Original CA back in flash");
    return ESP_OK;
}
