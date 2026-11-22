#!/bin/bash
# QEMU test: teddyCloud streams (no Range, no resume, .tmp not kept).
# While QEMU runs:
#   python3 tests/qemu/fake_stream_server.py 4444 <teddyCloud>/certs/server/teddy-cert.pem <teddyCloud>/certs/server/teddy-key.pem
# (the server certificate of the local test teddyCloud, so the box trusts it)
NAME=stream
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_stream_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
qemu_build
