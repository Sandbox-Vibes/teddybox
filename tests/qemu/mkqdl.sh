#!/bin/bash
# QEMU download handling test against a local teddyCloud (cloud access off);
# the client certificate is read from the assets FAT mounted as /sdcard
NAME=dl
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_dl_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
# a CA that is not teddyCloud's, as the original one in an unpatched box's flash
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 30 -subj "/CN=Test Boxine CA" \
    -keyout /dev/null -outform DER -out "$WORK/test_boxine_ca.der" 2>/dev/null
python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); print("static const unsigned char test_boxine_ca_der[] = {%s};\nstatic const unsigned int test_boxine_ca_der_len = %d;" % (",".join(str(b) for b in d), len(d)))' \
    "$WORK/test_boxine_ca.der" > "$WORK/main/test_boxine_ca.h"
qemu_build
