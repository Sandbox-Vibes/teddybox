
set(
    COMPONENT_SRCS "ledman.c" "main.c" "cloud.c" "cloud_http.c" "memdiag.c" "mempeak.c" "tls_mfl.c" "ogg_pos.c" "nfc.c" "ota.c" "playback.c" "wifi.c" "accel.c" "settings.c" "sfx.c" "battery.c" "bootsel.c" "freshness.c" "tonie_names.c" "tonie_names_tc.c" "tonies_portal.c" "tonie_seen.c" "crashlog.c" "ear_gestures.c" "sleep_timer.c" "tc_box_settings.c" "rtnl.c" "rtnl_encode.c" "proto/proto/toniebox.pb.freshness-check.fc-request.pb-c.c" "proto/proto/toniebox.pb.freshness-check.fc-response.pb-c.c" "mqtt_ha.c" "mqtt_ha_payload.c" "setup_portal.c" "backup.c" "backup_zip.c" "maint.c" "ogg_demux.c" "tb_opus_dec.c" "readahead.c" "taf_verify.c" "proto/protobuf-c.c" "proto/proto/toniebox.pb.taf-header.pb-c.c")
    
set(COMPONENT_ADD_INCLUDEDIRS . "proto" "proto/proto")
set(COMPONENT_REQUIRES toniebox lis3dh trf7962a audio_pipeline audio_stream audio_hal audio_board audio_sal opus_celt
    esp-adf-libs esp_peripherals esp_dispatcher display_service
    espcoredump esp_wifi esp_netif esp_event esp-tls mbedtls esp_http_server esp_http_client mqtt json nvs_flash app_update spi_flash
    esp_partition fatfs vfs wear_levelling spiffs driver esp_adc esp_timer bootloader_support efuse esp_eth protobuf-c)

# own task watchdog (taskwatch.h)
list(APPEND COMPONENT_SRCS "taskwatch.c")
list(APPEND COMPONENT_SRCS "mdns_min.c")
list(APPEND COMPONENT_SRCS "fw_fetch.c")

register_component()
# see tls_mfl.c; -u because nothing else references that file
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mbedtls_ssl_conf_ca_chain" "-u __wrap_mbedtls_ssl_conf_ca_chain")

# web files served from flash, gzip compressed (setup page, TAF conversion:
# www/taf_worker.js, www/opusenc.js from opus-recorder 8.0.5, MIT, with
# libopus, BSD)
idf_build_get_property(python PYTHON)
foreach(web setup.html www/taf_worker.js www/opusenc.js)
    get_filename_component(name ${web} NAME)
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz)
    string(MAKE_C_IDENTIFIER ${name} target)
    add_custom_command(OUTPUT ${gz}
        COMMAND ${python} ${COMPONENT_DIR}/../tools/gzip_embed.py ${COMPONENT_DIR}/${web} ${gz}
        DEPENDS ${COMPONENT_DIR}/${web} ${COMPONENT_DIR}/../tools/gzip_embed.py VERBATIM)
    add_custom_target(${target}_gz DEPENDS ${gz})
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY DEPENDS ${target}_gz)
endforeach()
