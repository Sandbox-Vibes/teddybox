#!/bin/bash
# QEMU test: box settings changed in teddyCloud are taken over.
# Needs a local teddyCloud (cloud access off) with its web API on 127.0.0.1:80.
#   mkqtcset.sh       build
#   mkqtcset.sh run   build, run, change the slap settings in teddyCloud on cue,
#                     restore them afterwards
NAME=tcset
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_tcset_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
sed -i 's#define FRESHNESS_INTERVAL_MS .*#define FRESHNESS_INTERVAL_MS 15000#' "$WORK/main/freshness.h"
qemu_build
[ "$1" = run ] || exit 0
TC=${TC_API:-http://127.0.0.1}
set_tc() { curl -s -m5 -X POST --data "$2" "$TC/api/settings/set/toniebox.$1" > /dev/null; }
set_tc slap_enabled true
set_tc slap_back_left false
LOG=$WORK_BASE/teddybox-qemu-$NAME.log
QEMU=${QEMU:-qemu-system-xtensa}
timeout 240 "$QEMU" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw -nic user,model=open_eth > "$LOG" 2>&1 &
PID=$!
changed=
while kill -0 $PID 2>/dev/null; do
    if [ -z "$changed" ] && grep -aq "CHANGE NOW" "$LOG"; then
        set_tc slap_enabled false
        set_tc slap_back_left true
        changed=1
    fi
    grep -aqE "TCSTEST: (ALL OK|FAILED)" "$LOG" && break
    sleep 1
done
kill $PID 2>/dev/null
set_tc slap_enabled true
set_tc slap_back_left false
grep -aE "TCSTEST|TCSET|box settings" "$LOG"
grep -aq "TCSTEST: ALL OK" "$LOG"
