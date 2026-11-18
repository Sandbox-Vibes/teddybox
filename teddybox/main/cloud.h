#pragma once

#include "cloud_http.h"

typedef struct
{
    const char *host;
    uint16_t port;
    const char *path;
    const char *auth;
    uint32_t range_start;
    esp_err_t (*data_received_cbr)(void *ctx, uint8_t *data, size_t length);
    void *data_received_ctx;
    esp_err_t (*connection_closed_cbr)(void *ctx);
    void *connection_closed_ctx;
} cloud_req_t;

typedef enum
{
    CC_STATE_INIT,
    CC_STATE_CONNECTING,
    CC_STATE_CONNECTED,
    CC_STATE_RECEIVING,
    CC_STATE_ABORTED,
    CC_STATE_FINISHED,
    CC_STATE_ERROR
} cloud_content_state_t;


typedef struct
{
    /* input fields filled by requester */
    uint64_t nfc_uid;
    bool abort;

    /* working variables for the download handler */
    char *location;
    char *filename;
    char *auth;
    SemaphoreHandle_t update_sem;

    cloud_content_state_t state;
    uint32_t status_code;
    uint32_t content_length;
    uint32_t received;
    SemaphoreHandle_t file_sem;
    FILE *handle;
} cloud_content_req_t;

void cloud_init(void);
esp_err_t cloud_set_time(void);
cloud_content_req_t *cloud_content_download(uint64_t nfc_uid, const uint8_t *nfc_token);
cloud_content_state_t cloud_content_get_state(cloud_content_req_t *req);
void cloud_content_cleanup(cloud_content_req_t *req);
