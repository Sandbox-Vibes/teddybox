#!/bin/bash
# QEMU test of the teddyCloud patch of the original firmware (ofw_patch.c)
# on the copy of the base image: patch, undo byte for byte, interrupted run
#   mkqofw.sh       build
#   mkqofw.sh run   build and run
NAME=ofw
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_ofwpatch_test.c"
# the assets FAT is mounted as /sdcard in the tests
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c" "$WORK/main/ofw_patch.c"
# a CA that is not teddyCloud's, as the original one in an unpatched box's flash
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 30 -subj "/CN=Test Boxine CA" \
    -keyout /dev/null -outform DER -out "$WORK/test_boxine_ca.der" 2>/dev/null
python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); print("static const unsigned char test_boxine_ca_der[] = {%s};\nstatic const unsigned int test_boxine_ca_der_len = %d;" % (",".join(str(b) for b in d), len(d)))' \
    "$WORK/test_boxine_ca.der" > "$WORK/main/test_boxine_ca.h"
qemu_build
if [ "$1" = run ]; then
    timeout 150 "${QEMU:-qemu-system-xtensa}" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw | tee "$WORK/run.log" | grep -a OFWTEST
fi
