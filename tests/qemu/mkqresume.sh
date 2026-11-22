#!/bin/bash
# QEMU test: automatic resume of a download that loses its connection.
# Needs a local teddyCloud (cloud access off) on 443 and, while QEMU runs,
#   python3 tests/qemu/cut_proxy.py 4443 127.0.0.1 443 50000
NAME=resume
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_resume_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
qemu_build
