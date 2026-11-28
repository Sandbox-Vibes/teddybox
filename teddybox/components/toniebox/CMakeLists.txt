# Board support for the Toniebox (ESP32-S3 v1.6.C): pins, LED, DAC3100 and
# speaker EQ. ESP-ADF's audio_board component links against it.
if(CONFIG_AUDIO_BOARD_CUSTOM)
    message(STATUS "Current board name is " CONFIG_AUDIO_BOARD_CUSTOM)
    set(srcs
        toniebox_esp32_v1.6.C/led.c
        toniebox_esp32_v1.6.C/board.c
        toniebox_esp32_v1.6.C/board_pins_config.c
        dac3100/dac3100.c
        dac3100/speaker_eq.c)
    set(incs toniebox_esp32_v1.6.C dac3100)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs}
                       REQUIRES lis3dh trf7962a
                       PRIV_REQUIRES audio_sal audio_hal esp_dispatcher esp_peripherals display_service esp_adc)

idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})
