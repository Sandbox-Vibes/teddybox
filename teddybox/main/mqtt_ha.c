#include <time.h>
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "maint.h"
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
/* progress values change every second, they alone go out at most this often */
#define PROGRESS_INTERVAL_MS 10000
#define POLL_INTERVAL_MS 1000

static esp_mqtt_client_handle_t client = NULL;
static volatile bool connected = false;
static volatile bool announce = false;

static char device_id[32];
static char base_topic[64];
static char avail_topic[80];
static char state_topic[80];
static char diag_topic[80];
static volatile bool publish_now = false;

void mqtt_ha_publish_now(void)
{
    publish_now = true;
}

static volatile uint32_t diag_sent;

bool mqtt_ha_flush(int timeout_ms)
{
    if (!client)
    {
        return false;
    }
    uint32_t before = diag_sent;
    publish_now = true;
    for (int t = 0; t < timeout_ms; t += 100)
    {
        if (diag_sent != before)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* diagnostics (heap, start time, reset reason, slot) */
#define DIAG_INTERVAL_MS 60000
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
    /* unknown while charging: the charger lifts the voltage */
    st->battery_percent = battery_get_state() == BATTERY_UNKNOWN || battery_is_charging()
                              ? -1
                              : ha_battery_percent(battery_get_rest_mv(), settings_get()->bat_critical_mv);
    st->charging = battery_is_charging();
    st->headphones = pb_headphones_connected();
    st->speaker_eq = ctrl_get_speaker_eq();
    st->led = ctrl_get_led_brightness();
    st->slap = settings_get()->slap_level;
    st->tilt = settings_get()->tilt_level;
    st->slap_reverse = settings_get()->slap_reverse;
    st->sleep_timer = ctrl_get_sleep_timer();

    pb_progress_t p;
    pb_get_progress(&p);
    st->progress = st->playing ? p.progress : -1;
    st->chapter_progress = st->playing ? p.chapter_progress : -1;
    st->position_ds = st->playing ? p.position_ds : -1;
    st->duration_ds = st->playing ? p.duration_ds : -1;
    st->download = p.download;

    wifi_ap_record_t ap;
    st->rssi = (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

/* true if something changed the user would notice; rssi and voltage alone only go out periodically */
static bool state_differs(const ha_state_t *a, const ha_state_t *b)
{
    return a->playing != b->playing || a->uid != b->uid || a->chapter != b->chapter || a->chapters != b->chapters ||
           a->volume != b->volume || a->charging != b->charging || a->headphones != b->headphones || a->speaker_eq != b->speaker_eq || a->led != b->led ||
           a->slap != b->slap || a->tilt != b->tilt || a->slap_reverse != b->slap_reverse || a->sleep_timer != b->sleep_timer ||
           /* a value appearing or disappearing (download starts/ends, duration known) goes out at once */
           (a->download < 0) != (b->download < 0) || (a->duration_ds < 0) != (b->duration_ds < 0) ||
           (a->download == 100) != (b->download == 100) ||
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
    case HA_CMD_SPEAKER_EQ:
        ESP_LOGI(TAG, "Command: speaker EQ %d", value);
        ctrl_request_speaker_eq(value);
        break;
    case HA_CMD_LED:
        ESP_LOGI(TAG, "Command: LED brightness %d", value);
        ctrl_request_led_brightness(value);
        break;
    case HA_CMD_SLAP:
        ESP_LOGI(TAG, "Command: slap level %d", value);
        ctrl_request_motion(value, -1, -1);
        break;
    case HA_CMD_TILT:
        ESP_LOGI(TAG, "Command: tilt level %d", value);
        ctrl_request_motion(-1, value, -1);
        break;
    case HA_CMD_SLAP_REVERSE:
        ESP_LOGI(TAG, "Command: slap reverse %d", value);
        ctrl_request_motion(-1, -1, value);
        break;
    case HA_CMD_SLEEP_TIMER:
        ESP_LOGI(TAG, "Command: sleep timer %d min", value);
        ctrl_request_sleep_timer(value);
        break;
    case HA_CMD_BOOT_ORIGINAL:
        ESP_LOGW(TAG, "Command: start the original firmware");
        ctrl_request_boot_original();
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
            if (event->retain)
            {
                /* a command stored on the broker (published with retain) would
                   be repeated at every start: ignore it and delete it there */
                if (event->data_len > 0 && event->topic_len < 120)
                {
                    char topic[128];
                    memcpy(topic, event->topic, event->topic_len);
                    topic[event->topic_len] = 0;
                    ESP_LOGW(TAG, "Ignoring retained command on '%s', removing it from the broker", topic);
                    esp_mqtt_client_publish(client, topic, "", 0, 1, 1);
                }
            }
            else
            {
                mqtt_handle_command(event->topic, event->topic_len, event->data, event->data_len);
            }
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

static const char *reset_reason_text(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_SW:
        return "restart (update, setup)";
    case ESP_RST_PANIC:
        return "crash";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "wake from deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout (low voltage)";
    default:
        return "other";
    }
}

static void mqtt_publish_diag(void)
{
    static char boot_iso[32];
    /* start time once the clock is set (from teddyCloud) */
    if (!boot_iso[0])
    {
        time_t now = time(NULL);
        if (now > 1700000000)
        {
            time_t boot = now - (time_t)(esp_timer_get_time() / 1000000);
            struct tm tm;
            gmtime_r(&boot, &tm);
            strftime(boot_iso, sizeof(boot_iso), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
        }
    }
    /* last nightly update: "21.11. 03:12: 2 Tonie(s) aktualisiert" */
    char update[96] = "";
    const char *result;
    time_t last = maint_last(&result);
    if (last > 1700000000)
    {
        struct tm lt;
        localtime_r(&last, &lt);
        snprintf(update, sizeof(update), "%02d.%02d. %02d:%02d: %s", lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min, result);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    ha_diag_t d = {
        .free_kb = (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        .min_free_kb = (int)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        .boot_iso = boot_iso,
        .reset = reset_reason_text(esp_reset_reason()),
        .slot = running ? running->label : "",
        .update = update,
    };
    char json[320];
    int len = ha_diag_json(&d, json, sizeof(json));
    if (len > 0)
    {
        if (esp_mqtt_client_publish(client, diag_topic, json, len, 0, 1) >= 0)
        {
            diag_sent++;
        }
    }
}

static void mqtt_ha_task(void *arg)
{
    ha_state_t last = {.battery_state = ""};
    ha_state_t now;
    char json[512];
    TickType_t last_sent = 0;
    TickType_t last_diag = 0;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        if (!connected)
        {
            continue;
        }

        bool force = publish_now;
        publish_now = false;
        if (announce)
        {
            announce = false;
            mqtt_publish_discovery();
            force = true;
        }

        if (force || xTaskGetTickCount() - last_diag >= pdMS_TO_TICKS(DIAG_INTERVAL_MS))
        {
            mqtt_publish_diag();
            last_diag = xTaskGetTickCount();
        }

        mqtt_collect_state(&now);
        TickType_t since = xTaskGetTickCount() - last_sent;
        bool periodic = since >= pdMS_TO_TICKS(STATE_INTERVAL_MS);
        bool progress = since >= pdMS_TO_TICKS(PROGRESS_INTERVAL_MS) &&
                        (now.progress != last.progress || now.chapter_progress != last.chapter_progress ||
                         now.position_ds / 10 != last.position_ds / 10 || now.download != last.download);
        if (force || periodic || progress || state_differs(&now, &last))
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
    snprintf(diag_topic, sizeof(diag_topic), "%s/diag", base_topic);
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
        .broker.address.hostname = s->mqtt_host,
        .broker.address.port = s->mqtt_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = device_id,
        .credentials.username = s->mqtt_user[0] ? s->mqtt_user : NULL,
        .credentials.authentication.password = s->mqtt_pass[0] ? s->mqtt_pass : NULL,
        .session.last_will.topic = avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .task.stack_size = 4096,
        .buffer.size = 1024,
        .buffer.out_size = 1280,
    };

    client = esp_mqtt_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "Client init failed");
        return;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xTaskCreate(&mqtt_ha_task, "[TB] mqtt_ha", 4096, NULL, 4, NULL);
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
