#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"

#include "cloud_http.h"

static const char *TAG = "CloudHTTP";

/* case insensitive search for a header name at the start of a line */
static const char *find_header(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    while (line && *line)
    {
        if (!strncasecmp(line, name, name_len))
        {
            const char *value = line + name_len;
            while (*value == ' ' || *value == '\t')
            {
                value++;
            }
            return value;
        }
        line = strstr(line, "\r\n");
        if (line)
        {
            line += 2;
        }
    }
    return NULL;
}

static esp_err_t http_finish(http_parser_t *p)
{
    if (!p->finished)
    {
        p->finished = true;
        if (p->http_end_cbr)
        {
            p->http_end_cbr(p->ctx);
        }
    }
    return CBR_CLOSE_OK;
}

static esp_err_t http_body(http_parser_t *p, uint8_t *data, size_t length)
{
    if (p->has_content_length && p->received_data_length + length > p->content_length)
    {
        /* never pass on more than announced */
        length = p->content_length - p->received_data_length;
    }

    if (length > 0)
    {
        p->received_data_length += length;
        if (p->http_data_cbr)
        {
            esp_err_t ret = p->http_data_cbr(p->ctx, data, length);
            if (ret != ESP_OK)
            {
                return ret;
            }
        }
    }

    if (p->has_content_length && p->received_data_length >= p->content_length)
    {
        return http_finish(p);
    }
    return ESP_OK;
}

static esp_err_t http_parse_header(http_parser_t *p, size_t header_len)
{
    const char *hdr = (const char *)p->header_buffer;
    int status_code = 0;

    if (sscanf(hdr, "HTTP/1.%*d %d", &status_code) != 1)
    {
        ESP_LOGE(TAG, "Invalid status line");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HTTP status %d", status_code);
    if (p->http_status_cbr)
    {
        esp_err_t ret = p->http_status_cbr(p->ctx, status_code);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    const char *te = find_header(hdr, "Transfer-Encoding:");
    if (te && !strncasecmp(te, "chunked", 7))
    {
        ESP_LOGE(TAG, "Chunked transfer encoding not supported");
        return ESP_FAIL;
    }

    const char *cl = find_header(hdr, "Content-Length:");
    if (cl)
    {
        p->content_length = strtoul(cl, NULL, 10);
        p->has_content_length = true;
    }

    size_t range_start = 0, range_end = 0, range_total = 0;
    const char *cr = find_header(hdr, "Content-Range:");
    if (cr && sscanf(cr, "bytes %zu-%zu/%zu", &range_start, &range_end, &range_total) == 3)
    {
        ESP_LOGI(TAG, "Content-Range %zu-%zu/%zu", range_start, range_end, range_total);
    }
    else
    {
        range_start = 0;
        range_total = p->has_content_length ? p->content_length : 0;
    }

    if (p->content_length_cbr)
    {
        esp_err_t ret = p->content_length_cbr(p->ctx, range_start, range_total);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    p->header_parsed = true;
    return ESP_OK;
}

esp_err_t http_parser_received_cbr(void *ctx, uint8_t *data, size_t length)
{
    http_parser_t *p = (http_parser_t *)ctx;

    if (p->finished)
    {
        return CBR_CLOSE_OK;
    }
    if (p->header_parsed)
    {
        return http_body(p, data, length);
    }

    /* collect the header, it may arrive in several pieces */
    size_t space = MAX_HTTP_HEADER_SIZE - p->header_size;
    size_t take = length < space ? length : space;
    memcpy(p->header_buffer + p->header_size, data, take);
    p->header_size += take;
    p->header_buffer[p->header_size] = 0;

    char *end = strstr((char *)p->header_buffer, "\r\n\r\n");
    if (!end)
    {
        if (p->header_size >= MAX_HTTP_HEADER_SIZE)
        {
            ESP_LOGE(TAG, "Header too large");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    size_t header_len = (end - (char *)p->header_buffer) + 4;
    /* cut off the body part for header parsing */
    uint8_t saved = p->header_buffer[header_len];
    p->header_buffer[header_len] = 0;
    esp_err_t ret = http_parse_header(p, header_len);
    p->header_buffer[header_len] = saved;
    if (ret != ESP_OK)
    {
        return ret;
    }

    /* body bytes that came with the header: the rest of the buffer plus what did not fit */
    size_t body_in_buffer = p->header_size - header_len;
    if (body_in_buffer > 0)
    {
        ret = http_body(p, p->header_buffer + header_len, body_in_buffer);
        if (ret != ESP_OK)
        {
            return ret;
        }
    }
    if (take < length)
    {
        return http_body(p, data + take, length - take);
    }
    if (p->has_content_length && p->content_length == 0)
    {
        return http_finish(p);
    }
    return ESP_OK;
}

esp_err_t http_parser_closed_cbr(void *ctx)
{
    http_parser_t *p = (http_parser_t *)ctx;

    if (!p->header_parsed)
    {
        return ESP_FAIL;
    }
    if (p->has_content_length && p->received_data_length < p->content_length)
    {
        ESP_LOGW(TAG, "Connection closed after %zu of %zu bytes", p->received_data_length, p->content_length);
    }
    http_finish(p);
    return ESP_OK;
}
