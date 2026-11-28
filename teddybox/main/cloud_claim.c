/*
 * Claim: "this tonie was placed", sent to teddyCloud like the original box
 * does (only to teddyCloud, never to the Boxine fallback).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cloud_internal.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "Cloud";

/********************************************************/
/* claim: "this tonie was placed", like the original box */
/********************************************************/

/* teddyCloud (handleCloudClaim) remembers it as the last tonie of the box,
   sends an MQTT event and marks the tonie as claimed. Only to teddyCloud,
   never to the Boxine fallback. A few seconds after placing, so the TLS
   handshake does not compete with the playback start for RAM. */
#define CLAIM_DELAY_MS 5000
#define CLAIM_GIVE_UP_MS 60000
static portMUX_TYPE claim_lock = portMUX_INITIALIZER_UNLOCKED;
static struct
{
    bool pending;
    uint64_t uid;
    uint8_t token[32];
    TickType_t since;
} claim;

void cloud_claim(uint64_t uid, const uint8_t token[32])
{
    if (!settings_get()->claim || !uid || !token)
    {
        return;
    }
    taskENTER_CRITICAL(&claim_lock);
    claim.pending = true;
    claim.uid = uid;
    memcpy(claim.token, token, 32);
    claim.since = xTaskGetTickCount();
    taskEXIT_CRITICAL(&claim_lock);
}

/* "/v1/claim/" + rUID, the same 16 hex digits as in /v2/content/ */
void cloud_claim_path(uint64_t uid, char *path, size_t size)
{
    int n = snprintf(path, size, "/v1/claim/");
    for (int i = 0; i < 8 && n < (int)size; i++)
    {
        n += snprintf(path + n, size - n, "%02X", (unsigned)(uid >> (i * 8)) & 0xFF);
    }
}

static esp_err_t cloud_claim_status_cbr(void *ctx, int status_code)
{
    *(int *)ctx = status_code;
    return ESP_OK;
}

static esp_err_t cloud_ignore_data_cbr(void *ctx, uint8_t *data, size_t length)
{
    return ESP_OK;
}

static void cloud_claim_send(void)
{
    uint64_t uid;
    uint8_t token[32];
    taskENTER_CRITICAL(&claim_lock);
    uid = claim.uid;
    memcpy(token, claim.token, 32);
    claim.pending = false;
    taskEXIT_CRITICAL(&claim_lock);

    char path[32];
    cloud_claim_path(uid, path, sizeof(path));
    char auth[65];
    for (int i = 0; i < 32; i++)
    {
        sprintf(auth + 2 * i, "%02X", token[i]);
    }
    int status = 0;
    http_parser_t parser = {
        .http_status_cbr = &cloud_claim_status_cbr,
        .http_data_cbr = &cloud_ignore_data_cbr,
        .ctx = &status,
        .header_buffer = cloud_header_buffer()};
    if (!parser.header_buffer)
    {
        return;
    }
    cloud_req_t req = {
        .host = CLOUD_HOST,
        .port = CLOUD_PORT,
        .path = path,
        .auth = auth,
        .data_received_cbr = &http_parser_received_cbr,
        .data_received_ctx = &parser,
        .connection_closed_cbr = &http_parser_closed_cbr,
        .connection_closed_ctx = &parser};
    cloud_request(&req);
    memset(auth, 0, sizeof(auth));
    memset(token, 0, sizeof(token));
    if (status == 200 || status == 204)
    {
        ESP_LOGI(TAG, "Claim %016llX sent", (unsigned long long)uid);
    }
    else
    {
        ESP_LOGW(TAG, "Claim %016llX: HTTP %d", (unsigned long long)uid, status);
    }
}


void cloud_claim_step(bool cloud_available)
{
    if (!claim.pending)
    {
        return;
    }
    TickType_t age = xTaskGetTickCount() - claim.since;
    if (age >= pdMS_TO_TICKS(CLAIM_GIVE_UP_MS) || active_backend != BACKEND_TEDDYCLOUD)
    {
        claim.pending = false;
    }
    else if (cloud_available && wifi_is_connected() && age >= pdMS_TO_TICKS(CLAIM_DELAY_MS))
    {
        cloud_claim_send();
    }
}
