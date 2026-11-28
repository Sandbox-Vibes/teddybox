#pragma once
/* internals shared by cloud.c, cloud_download.c and cloud_claim.c; not for
   other modules (they use cloud.h) */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cloud.h"
#include "cloud_http.h"

/*
 * Up to two backends: teddyCloud (preferred) and, only if boxine_fallback
 * is enabled, the Boxine cloud with the original CA from the assets
 * partition. The cloud task picks the first that answers /v1/time.
 */
typedef struct
{
    const char *name;
    const char *host;
    uint16_t port;
    uint8_t *ca;
    size_t ca_len;
    /* server name for SNI and certificate check, NULL = neither (teddyCloud's
       certificate is issued to "TeddyCloud Server", not to a host name) */
    const char *tls_name;
} cloud_backend_t;

#define BACKEND_TEDDYCLOUD 0
#define BACKEND_BOXINE 1
/* while on Boxine, check every few minutes whether teddyCloud is back */
#ifndef BACKEND_RETRY_PREFERRED_MS
#define BACKEND_RETRY_PREFERRED_MS (5 * 60 * 1000)
#endif

extern cloud_backend_t backends[2];
extern volatile int active_backend;
#define CLOUD_HOST (backends[active_backend].host)
#define CLOUD_PORT (backends[active_backend].port)

/* download write block = TAF block, and how often an SD access is retried */
#define CLOUD_WRITE_BLOCK 4096
/* while a tonie plays the download is limited, the SD card and the CPU are
   shared with the playback (about 12 KiB/s): at full speed (250 KiB/s with
   IDF 5, ESP-ADF decoder at 160 MHz) the sound stuttered. Limit from
   teddybox.ini "download_playing" (default 0 = unlimited, about 300 KiB/s
   with CELT at 160 MHz: decoder 40.7 % of one core, clean sound; decoder =
   adf: at most 96). Without
   playback (nightly update) full speed. */
#define CLOUD_SYNC_BYTES (128 * 1024)

/* Buffers of the cloud task. All requests run in it, one after the other
   (also the automatic resume), so one set is enough. Allocated once at the
   first request and kept: before, every request and every download took
   and gave back these pieces around the TLS handshake, which left the heap
   cut into small blocks while a tonie played. */
typedef struct
{
    uint8_t rx[HTTP_RECEIVE_SIZE];
    uint8_t header[MAX_HTTP_HEADER_SIZE + 1];
    char request[768];
    char url[160];
    uint8_t wbuf[CLOUD_WRITE_BLOCK];
} cloud_bufs_t;
/* buffers of the cloud task (allocated at the first request, kept) */
cloud_bufs_t *cloud_bufs(void);
uint8_t *cloud_header_buffer(void);

/* one request over TLS to the active backend (cloud task only) */
esp_err_t cloud_request(cloud_req_t *req);

/* content download requests, filled by cloud_content_download() */
extern QueueHandle_t cloud_request_queue;
/* task watchdog entry of the cloud task (a download checks in per block) */
extern int cloud_taskwatch;

/* cloud_download.c: answers one queued download request (always, also when
   the cloud is not reachable); true if the connection failed */
bool cloud_download_serve(cloud_content_req_t *req, bool cloud_available);

/* cloud_claim.c: sends a pending claim when its time has come */
void cloud_claim_step(bool cloud_available);
