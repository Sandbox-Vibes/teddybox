#!/bin/bash
# Fetches the framework versions TeddyBox is built and tested with:
# ESP-IDF v5.5.6 (supported by Espressif until January 2028) and ESP-ADF v2.8
set -e
IDF_TAG=v5.5.6
ADF_TAG=v2.8

if [ ! -d esp-idf ]; then
    echo "[SETUP] ESP-IDF $IDF_TAG"
    git clone --depth 1 -b $IDF_TAG --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git
fi
if [ ! -d esp-adf ]; then
    echo "[SETUP] ESP-ADF $ADF_TAG (only esp-adf-libs; its own esp-idf copy is not used)"
    git clone --depth 1 -b $ADF_TAG https://github.com/espressif/esp-adf.git
    (cd esp-adf && git submodule update --init --depth 1 components/esp-adf-libs)
fi
(cd esp-idf && ./install.sh esp32s3)

# Without access to components.espressif.com (the component manager), fetch
# the three registry components ESP-ADF needs from GitHub instead:
#   ./setup_libs.sh offline   and then   export IDF_EXTRA_COMPONENTS=$PWD/idf-extra IDF_COMPONENT_MANAGER=0
if [ "$1" = "offline" ] && [ ! -d idf-extra ]; then
    git clone --depth 1 https://github.com/espressif/idf-extra-components.git idf-extra-src
    (cd idf-extra-src && git submodule update --init --depth 1 nghttp/nghttp2 zlib/zlib)
    mkdir idf-extra && cp -r idf-extra-src/jsmn idf-extra-src/nghttp idf-extra-src/zlib idf-extra/
    rm -rf idf-extra/*/test_apps
fi
