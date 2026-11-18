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
    /* set by the cloud task as its very last access, only then the requester may free it */
    volatile bool done;

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
/* POST from the cloud task context, response body must fit into resp */
esp_err_t cloud_post(const char *path, const uint8_t *body, size_t body_len, uint8_t *resp, size_t resp_size, size_t *resp_len);
esp_err_t cloud_set_time(void);
cloud_content_req_t *cloud_content_download(uint64_t nfc_uid, const uint8_t *nfc_token);
cloud_content_state_t cloud_content_get_state(cloud_content_req_t *req);
void cloud_content_cleanup(cloud_content_req_t *req);
