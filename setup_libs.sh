#!/bin/bash
# Fetches the exact framework versions TeddyBox is built and tested with:
# ESP-ADF v2.6 with its bundled ESP-IDF v4.4.4
set -e

echo "[SETUP] Install ESP-ADF v2.6"
if [ ! -d esp-adf ]; then
    git clone --depth 1 -b v2.6 https://github.com/espressif/esp-adf.git
    (cd esp-adf && git submodule update --init --recursive --depth 1)
fi

cd esp-adf
export ADF_PATH=$PWD

cd ${ADF_PATH}/esp-idf
if git apply --check ../../patches/malloc.diff 2>/dev/null; then
    git apply ../../patches/malloc.diff
else
    echo "[SETUP] malloc.diff already applied"
fi
./install.sh esp32s3

. ./export.sh
