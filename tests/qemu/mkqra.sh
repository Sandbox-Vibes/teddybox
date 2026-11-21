#!/bin/bash
# QEMU read-ahead test: simulated SD hangs, seek, growing file, end during a hang
NAME=ra
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_ra_test.c"
qemu_build
