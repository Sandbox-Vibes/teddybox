#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX_HTTP_HEADER_SIZE 1024
#define HTTP_RECEIVE_SIZE 1600

typedef enum
{
    CBR_CLOSE_OK = 0x1000
} cloud_cbr_err_t;

/*
 * Minimal HTTP/1.1 response parser for the cloud client. Handles responses
 * with Content-Length (what Boxine and teddyCloud send) and responses that
 * end when the connection closes. Chunked encoding is not supported and
 * rejected.
 */
typedef struct
{
    esp_err_t (*http_status_cbr)(void *ctx, int status_code);
    esp_err_t (*http_data_cbr)(void *ctx, uint8_t *data, size_t length);
    esp_err_t (*http_end_cbr)(void *ctx);
    /* range_start: first byte of this response within the whole file (Content-Range), else 0
       total_length: size of the whole file, range_start + body length if unknown */
    esp_err_t (*content_length_cbr)(void *ctx, size_t range_start, size_t total_length);
    void *ctx;

    /* MAX_HTTP_HEADER_SIZE + 1 bytes, allocated by the caller */
    uint8_t *header_buffer;

    /* state, zero initialize */
    size_t header_size;
    bool header_parsed;
    bool has_content_length;
    bool finished;
    size_t content_length;
    size_t received_data_length;
} http_parser_t;

/* feed received bytes; returns ESP_OK to continue, CBR_CLOSE_OK when the response is complete, ESP_FAIL on error */
esp_err_t http_parser_received_cbr(void *ctx, uint8_t *data, size_t length);
/* call when the connection was closed */
esp_err_t http_parser_closed_cbr(void *ctx);
