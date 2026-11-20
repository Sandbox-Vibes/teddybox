# Slim copy of ESP-ADF's audio_stream (v2.8): only the I2S and raw streams.
# The original component pulls in esp-sr, esp_actions and the HTTP/HLS
# streams on the ESP32-S3, which TeddyBox does not use.
idf_component_register(SRCS "i2s_stream_idf5.c" "raw_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES audio_pipeline driver esp_driver_i2s audio_sal audio_board esp-adf-libs esp_dispatcher)
