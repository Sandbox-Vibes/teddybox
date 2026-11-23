#!/bin/bash
# QEMU test of the nightly update report: a run that hung or crashed is
# reported at the next start with the step it was in (maint.c).
NAME=maint
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_maint_test.c"
qemu_build
if [ "$1" = run ]; then
    timeout 60 "${QEMU:-qemu-system-xtensa}" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw 2>&1 | tee "$WORK/qemu.log" | grep -aE "MTEST|Maint" &
    wait
    grep -aq "MTEST: ALL OK" "$WORK/qemu.log"
fi
