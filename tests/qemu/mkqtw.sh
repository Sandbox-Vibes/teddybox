#!/bin/bash
# QEMU test of the task watchdog (taskwatch.c): a stuck task makes the box
# restart, the reason names it. It restarts by itself inside one QEMU run.
NAME=taskwatch
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_taskwatch_test.c"
qemu_build
