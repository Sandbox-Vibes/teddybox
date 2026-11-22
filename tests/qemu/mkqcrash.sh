#!/bin/bash
# QEMU test of crash dumps (crashlog.c): crash, restart, summary + file once.
# Run the image twice in one QEMU session (it restarts by itself after the panic).
NAME=crash
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_crash_test.c"
qemu_build
