#!/bin/bash
# QEMU TAF hash check test; TAF = any small tonie file (< 300 KB, fits the assets FAT)
NAME=verify
. "$(dirname "$0")/common.sh"
: "${TAF:?set TAF to a small .taf file}"
qemu_prepare "$T/qemu_verify_test.c"
python3 "$T/gen_taf_test_data.py" "$TAF" > "$WORK/main/taf_test_data.h"
qemu_build
