#!/bin/bash
# QEMU download handling test against a local teddyCloud (cloud access off);
# the client certificate is read from the assets FAT mounted as /sdcard
NAME=dl
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_dl_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
qemu_build
