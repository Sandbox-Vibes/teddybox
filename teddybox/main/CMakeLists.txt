
set(
    COMPONENT_SRCS "ledman.c" "main.c" "cloud.c" "cloud_http.c" "memdiag.c" "mempeak.c" "tls_mfl.c" "ogg_pos.c" "nfc.c" "ota.c" "playback.c" "wifi.c" "accel.c" "settings.c" "sfx.c" "battery.c" "bootsel.c" "freshness.c" "ear_gestures.c" "sleep_timer.c" "tc_box_settings.c" "rtnl.c" "rtnl_encode.c" "proto/proto/toniebox.pb.freshness-check.fc-request.pb-c.c" "proto/proto/toniebox.pb.freshness-check.fc-response.pb-c.c" "mqtt_ha.c" "mqtt_ha_payload.c" "setup_portal.c" "backup.c" "backup_zip.c" "maint.c" "ogg_demux.c" "tb_opus_dec.c" "readahead.c" "taf_verify.c" "proto/protobuf-c.c" "proto/proto/toniebox.pb.taf-header.pb-c.c")
    
set(COMPONENT_EMBED_TXTFILES "setup.html")
set(COMPONENT_ADD_INCLUDEDIRS . "proto" "proto/proto")
set(COMPONENT_REQUIRES toniebox lis3dh trf7962a audio_pipeline audio_stream audio_hal audio_board audio_sal opus_celt
    esp-adf-libs esp_peripherals esp_dispatcher display_service
    esp_wifi esp_netif esp_event esp-tls mbedtls esp_http_server mqtt json nvs_flash app_update spi_flash
    esp_partition fatfs vfs wear_levelling spiffs driver esp_adc esp_timer bootloader_support efuse esp_eth protobuf-c)

# own task watchdog (taskwatch.h)
list(APPEND COMPONENT_SRCS "taskwatch.c")

register_component()
# see tls_mfl.c; -u because nothing else references that file
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mbedtls_ssl_conf_ca_chain" "-u __wrap_mbedtls_ssl_conf_ca_chain")
