#pragma once

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cloud_http.h"

typedef struct
{
    const char *host;
    uint16_t port;
    const char *path;
    const char *auth;
    uint32_t range_start;
    /* optional request body, turns the request into a POST */
    const uint8_t *body;
    size_t body_len;
    esp_err_t (*data_received_cbr)(void *ctx, uint8_t *data, size_t length);
    void *data_received_ctx;
    esp_err_t (*connection_closed_cbr)(void *ctx);
    void *connection_closed_ctx;
} cloud_req_t;

/* report a placed tonie to teddyCloud (/v1/claim), sent a few seconds later */
void cloud_claim(uint64_t uid, const uint8_t token[32]);
void cloud_claim_path(uint64_t uid, char *path, size_t size);

typedef enum
{
    CC_STATE_INIT,
    CC_STATE_CONNECTING,
    CC_STATE_CONNECTED,
    CC_STATE_RECEIVING,
    /* connection lost midway: the cloud task resumes it with Range (still running) */
    CC_STATE_RESUMING,
    CC_STATE_ABORTED,
    CC_STATE_FINISHED,
    CC_STATE_ERROR
} cloud_content_state_t;


typedef struct
{
    /* input fields filled by requester */
    uint64_t nfc_uid;
    bool abort;
    /* set by the cloud task as its very last access, only then the requester may free it */
    volatile bool done;

    /* working variables for the download handler */
    char *location;
    char *filename;
    /* the download goes here and is renamed to filename when complete (as the
       original firmware does), so a partial download never looks like a tonie */
    char *tmpname;
    char *auth;
    SemaphoreHandle_t update_sem;

    cloud_content_state_t state;
    uint32_t status_code;
    uint32_t content_length;
    uint32_t received;
    /* for the speed in the log: when this connection started and at which byte */
    int64_t speed_start_us;
    uint32_t speed_start_bytes;
    /* data is written in 4 KB blocks aligned to the file (= TAF blocks):
       fewer SD accesses, no partial sectors, the file lock is held less often.
       "received" only counts what is in the file; whatever is still in wbuf
       when a download is cut off is fetched again on resume */
    uint8_t *wbuf;
    uint32_t wbuf_len;
    int64_t sd_write_us; /* time spent writing to SD since the last log line */
    int64_t next_block_us; /* rate limit while playing: next block not before */
    /* FATFS only writes the file size to the directory entry on fsync/fclose.
       A file opened again sees that size, so it is synced regularly; after an
       SD error the download never seeks beyond it (that would extend the file
       with whatever is in the new clusters) */
    uint32_t synced;
    /* automatic resume after a lost connection: number of the attempt, and
       bytes to drop when the server ignored the Range and sends from 0 */
    int resume_attempt;
    /* teddyCloud stream (Content-Length no multiple of 4096): never resumed
       with Range, the .tmp is deleted afterwards */
    bool is_stream;
    int64_t end_us; /* when the transfer ended (esp_timer), 0 while running */
    uint32_t skip_left;
    SemaphoreHandle_t file_sem;
    FILE *handle;
} cloud_content_req_t;

void cloud_init(void);
/* TLS connection to teddyCloud's API port with the box certificate, for
   RTNL; NULL while teddyCloud is not the backend in use (never Boxine) */
struct esp_tls;
struct esp_tls *cloud_connect_teddycloud(void);
/* POST from the cloud task context, response body must fit into resp */
esp_err_t cloud_post(const char *path, const uint8_t *body, size_t body_len, uint8_t *resp, size_t resp_size, size_t *resp_len);
esp_err_t cloud_set_time(void);
cloud_content_req_t *cloud_content_download(uint64_t nfc_uid, const uint8_t *nfc_token);
/* downloads configured at all (teddyCloud or the Boxine fallback) */
bool cloud_is_enabled(void);
/* WiFi connected and a backend reachable: a download can start now */
bool cloud_is_ready(void);
/* a content download is queued or running: background work that needs RAM
   (file check, RTNL connection) waits, the download and the decoder next to
   it need nearly all of it */
bool cloud_download_busy(void);
cloud_content_state_t cloud_content_get_state(cloud_content_req_t *req);
void cloud_content_cleanup(cloud_content_req_t *req);
