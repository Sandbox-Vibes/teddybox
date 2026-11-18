/*
 * Minimal OTA: send a raw teddybox.bin to TCP port 63660, e.g.
 *     nc -q 2 <box-ip> 63660 < teddybox.bin
 *
 * The target slot is chosen by bootsel so that the original Boxine firmware
 * (EU app, and the PD app unless ota_allow_overwrite_pd=1) is never touched.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "errno.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "ota.h"
#include "bootsel.h"
#include "settings.h"

#define OTA_PORT 63660
#define BUFFSIZE 1024
#define OTA_RECV_TIMEOUT_S 10

static const char *TAG = "OTA";
static char rx_buffer[BUFFSIZE];
static esp_app_desc_t running_app_info;

/* receives one image from sock into part, returns ESP_OK if it was written and validated */
static esp_err_t ota_receive(int sock, const esp_partition_t *part)
{
    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return err;
    }

    bool header_checked = false;
    size_t total = 0;

    while (true)
    {
        int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
        if (len < 0)
        {
            ESP_LOGE(TAG, "recv failed: errno %d", errno);
            esp_ota_abort(update_handle);
            return ESP_FAIL;
        }
        if (len == 0)
        {
            break;
        }

        if (!header_checked)
        {
            const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if (len < desc_off + sizeof(esp_app_desc_t) || (uint8_t)rx_buffer[0] != ESP_IMAGE_HEADER_MAGIC)
            {
                ESP_LOGE(TAG, "Not an ESP32 app image");
                esp_ota_abort(update_handle);
                return ESP_ERR_INVALID_ARG;
            }
            esp_app_desc_t new_app_info;
            memcpy(&new_app_info, &rx_buffer[desc_off], sizeof(new_app_info));
            if (!strncmp(new_app_info.project_name, "toniebox-esp32-", 15))
            {
                ESP_LOGE(TAG, "Refusing to install original firmware '%s' via TeddyBox OTA", new_app_info.project_name);
                esp_ota_abort(update_handle);
                return ESP_ERR_INVALID_ARG;
            }
            ESP_LOGI(TAG, "Receiving '%s' %s (running %s)", new_app_info.project_name, new_app_info.version, running_app_info.version);
            header_checked = true;
        }

        err = esp_ota_write(update_handle, rx_buffer, len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return err;
        }
        total += len;
    }

    ESP_LOGI(TAG, "Received %u bytes, validating", total);
    err = esp_ota_end(update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", err == ESP_ERR_OTA_VALIDATE_FAILED ? "image corrupted" : esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void ota_mainthread(void *arg)
{
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(OTA_PORT),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0 || listen(listen_sock, 1) != 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind/listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        ESP_LOGI(TAG, "Listening on port %d", OTA_PORT);
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0)
        {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        struct timeval tv = {.tv_sec = OTA_RECV_TIMEOUT_S};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const esp_partition_t *target = bootsel_get_update_target(settings_get()->ota_allow_overwrite_pd);
        esp_err_t err = ESP_FAIL;
        if (!target)
        {
            ESP_LOGE(TAG, "No safe slot for an update: all other slots hold the original firmware. "
                          "Set ota_allow_overwrite_pd=1 in teddybox.ini to use the PD slot.");
        }
        else
        {
            ESP_LOGI(TAG, "Writing update to %s @0x%06x", target->label, target->address);
            err = ota_receive(sock, target);
        }
        shutdown(sock, 0);
        close(sock);

        if (err != ESP_OK)
        {
            continue;
        }

        err = esp_ota_set_boot_partition(target);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
            continue;
        }
        ESP_LOGW(TAG, "Update installed, restarting");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_restart();
    }
}

void ota_init()
{
    esp_log_level_set(TAG, ESP_LOG_INFO);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_partition_description(running, &running_app_info) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to fetch firmware version");
        return;
    }
    ESP_LOGI(TAG, "Running %s from %s", running_app_info.version, running->label);

    /* esp_ota_end() validates the image (SHA256), needs more than 2k stack */
    xTaskCreatePinnedToCore(ota_mainthread, "[TB] OTA", 4096, NULL, 5, NULL, 0);
}
