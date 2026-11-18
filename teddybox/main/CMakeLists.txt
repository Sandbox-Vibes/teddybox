
set(
    COMPONENT_SRCS "ledman.c" "main.c" "cloud.c" "cloud_http.c" "malloc.c" "nfc.c" "ota.c" "playback.c" "wifi.c" "webserver.c" "accel.c" "settings.c" "sfx.c" "battery.c" "bootsel.c" "freshness.c" "proto/proto/toniebox.pb.freshness-check.fc-request.pb-c.c" "proto/proto/toniebox.pb.freshness-check.fc-response.pb-c.c" "mqtt_ha.c" "mqtt_ha_payload.c" "setup_portal.c" "proto/protobuf-c.c" "proto/proto/toniebox.pb.taf-header.pb-c.c"
    EMBED_FILES "favicon.ico" "upload_script.html")
    
set(COMPONENT_EMBED_TXTFILES "setup.html")
set(COMPONENT_ADD_INCLUDEDIRS . "proto" "proto/proto")

register_component()
