#!/bin/bash
# QEMU decoder test (CELT and ESP-ADF, bit exact against celt_host_reference.c)
NAME=codec
. "$(dirname "$0")/common.sh"
qemu_prepare /dev/null
sed -e "s/JUMP_FROM/137/; s/JUMP_TO/82009/" "$T/qemu_codec_test.c" > "$WORK/main/qemu_mem_test.c"
qemu_build
