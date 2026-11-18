#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "mqtt_client.h"

#include "mqtt_ha.h"
#include "mqtt_ha_payload.h"
#include "settings.h"
#include "playback.h"
#include "battery.h"
#include "control.h"
#include "wifi.h"

static const char *TAG = "MQTT";

#define STATE_INTERVAL_MS 30000
#define POLL_INTERVAL_MS 1000

static esp_mqtt_client_handle_t client = NULL;
static volatile bool connected = false;
static volatile bool announce = false;

static char device_id[32];
static char base_topic[64];
static char avail_topic[80];
static char state_topic[80];
static char cmd_topic[80];
static char sw_version[32];
static ha_ctx_t ctx;

static void mqtt_publish_discovery(void)
{
    char topic[128];
    char *payload = malloc(1024);
    if (!payload)
    {
        return;
    }
    for (int i = 0; i < ha_discovery_count(); i++)
    {
        int len = ha_discovery(&ctx, i, topic, sizeof(topic), payload, 1024);
        if (len < 0)
        {
            ESP_LOGE(TAG, "Discovery message %d too large", i);
            continue;
        }
        /* QoS 0: QoS 1 would keep all ~11 KB in the outbox until acknowledged */
        esp_mqtt_client_publish(client, topic, payload, len, 0, 1);
    }
    free(payload);
    ESP_LOGI(TAG, "Home Assistant discovery sent (%d entities)", ha_discovery_count());
}

static void mqtt_collect_state(ha_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->playing = pb_is_playing();
    st->uid = st->playing ? pb_get_current_uid() : 0;
    st->chapters = st->playing ? pb_get_chapter_count() : 0;
    st->chapter = st->chapters ? pb_get_chapter() + 1 : 0;
    st->volume = ctrl_get_volume();
    st->battery_mv = battery_get_mv();
    st->battery_state = battery_state_name(battery_get_state());
    st->battery_percent = battery_get_state() == BATTERY_UNKNOWN ? -1 : ha_battery_percent(st->battery_mv, settings_get()->bat_critical_mv);
    st->charging = battery_is_charging();
    st->headphones = pb_headphones_connected();

    wifi_ap_record_t ap;
    st->rssi = (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

/* true if something changed the user would notice; rssi and voltage alone only go out periodically */
static bool state_differs(const ha_state_t *a, const ha_state_t *b)
{
    return a->playing != b->playing || a->uid != b->uid || a->chapter != b->chapter || a->chapters != b->chapters ||
           a->volume != b->volume || a->charging != b->charging || a->headphones != b->headphones ||
           strcmp(a->battery_state, b->battery_state);
}

static void mqtt_handle_command(const char *topic, int topic_len, const char *data, int data_len)
{
    int value = 0;
    switch (ha_parse_command(&ctx, topic, topic_len, data, data_len, &value))
    {
    case HA_CMD_VOLUME:
        ESP_LOGI(TAG, "Command: volume %d", value);
        ctrl_request_volume(value);
        break;
    case HA_CMD_STOP:
        ESP_LOGI(TAG, "Command: stop");
        pb_stop();
        break;
    case HA_CMD_NEXT:
        ESP_LOGI(TAG, "Command: next chapter");
        pb_seek_chapter(1);
        break;
    case HA_CMD_PREV:
        ESP_LOGI(TAG, "Command: previous chapter");
        pb_seek_chapter(-1);
        break;
    case HA_CMD_POWEROFF:
        ESP_LOGI(TAG, "Command: power off");
        ctrl_request_poweroff();
        break;
    default:
        ESP_LOGW(TAG, "Unknown command topic '%.*s'", topic_len, topic);
        break;
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to %s:%d", settings_get()->mqtt_host, settings_get()->mqtt_port);
        connected = true;
        esp_mqtt_client_publish(client, avail_topic, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(client, cmd_topic, 1);
        /* discovery and first state are sent from the poll task, keeps this stack small */
        announce = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        if (connected)
        {
            ESP_LOGW(TAG, "Disconnected");
        }
        connected = false;
        break;

    case MQTT_EVENT_DATA:
        /* commands are tiny, ignore fragmented messages */
        if (event->current_data_offset == 0 && event->total_data_len == event->data_len)
        {
            mqtt_handle_command(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(TAG, "Connection refused (code %d), check mqtt_user/mqtt_pass", event->error_handle->connect_return_code);
        }
        break;

    default:
        break;
    }
}

static void mqtt_ha_task(void *arg)
{
    ha_state_t last = {.battery_state = ""};
    ha_state_t now;
    char json[320];
    TickType_t last_sent = 0;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        if (!connected)
        {
            continue;
        }

        bool force = false;
        if (announce)
        {
            announce = false;
            mqtt_publish_discovery();
            force = true;
        }

        mqtt_collect_state(&now);
        bool periodic = (xTaskGetTickCount() - last_sent) >= pdMS_TO_TICKS(STATE_INTERVAL_MS);
        if (force || periodic || state_differs(&now, &last))
        {
            int len = ha_state_json(&now, json, sizeof(json));
            if (len > 0 && esp_mqtt_client_publish(client, state_topic, json, len, 0, 1) >= 0)
            {
                last = now;
                last_sent = xTaskGetTickCount();
            }
        }
    }
}

void mqtt_ha_init(void)
{
    const settings_t *s = settings_get();
    esp_log_level_set(TAG, ESP_LOG_INFO);

    if (!s->mqtt_host[0])
    {
        ESP_LOGI(TAG, "No mqtt_host configured, Home Assistant integration off");
        return;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(device_id, sizeof(device_id), "teddybox_%s", mac_str);
    if (s->mqtt_topic[0])
    {
        snprintf(base_topic, sizeof(base_topic), "%s", s->mqtt_topic);
    }
    else
    {
        snprintf(base_topic, sizeof(base_topic), "teddybox/%s", mac_str);
    }
    snprintf(avail_topic, sizeof(avail_topic), "%s/availability", base_topic);
    snprintf(state_topic, sizeof(state_topic), "%s/state", base_topic);
    snprintf(cmd_topic, sizeof(cmd_topic), "%s/cmd/+", base_topic);

    esp_app_desc_t app;
    esp_ota_get_partition_description(esp_ota_get_running_partition(), &app);
    snprintf(sw_version, sizeof(sw_version), "%s", app.version);

    ctx = (ha_ctx_t){
        .device_id = device_id,
        .base_topic = base_topic,
        .prefix = s->ha_discovery_prefix,
        .sw_version = sw_version,
        .volume_max = s->volume_max,
    };

    esp_mqtt_client_config_t cfg = {
        .host = s->mqtt_host,
        .port = s->mqtt_port,
        .transport = MQTT_TRANSPORT_OVER_TCP,
        .client_id = device_id,
        .username = s->mqtt_user[0] ? s->mqtt_user : NULL,
        .password = s->mqtt_pass[0] ? s->mqtt_pass : NULL,
        .lwt_topic = avail_topic,
        .lwt_msg = "offline",
        .lwt_qos = 1,
        .lwt_retain = 1,
        .keepalive = 30,
        .task_stack = 4096,
        .buffer_size = 1024,
        .out_buffer_size = 1280,
    };

    client = esp_mqtt_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "Client init failed");
        return;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xTaskCreate(&mqtt_ha_task, "[TB] mqtt_ha", 3584, NULL, 4, NULL);
    ESP_LOGI(TAG, "Home Assistant integration started, topic %s", base_topic);
}

void mqtt_ha_shutdown(void)
{
    if (!client)
    {
        return;
    }
    bool was_connected = connected;
    /* stops the poll task from publishing */
    connected = false;
    if (was_connected)
    {
        /* retained so Home Assistant shows the box as off instead of waiting for the LWT */
        esp_mqtt_client_publish(client, avail_topic, "offline", 0, 1, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    esp_mqtt_client_stop(client);
}
