#!/bin/bash
# QEMU test: RTNL events reach teddyCloud.
# Needs a local teddyCloud (cloud access off), web API on 127.0.0.1:80.
#   mkqrtnl.sh       build
#   mkqrtnl.sh run   build, run, check teddyCloud's server-sent events
NAME=rtnl
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_rtnl_test.c"
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' "$WORK/main/cloud.c"
qemu_build
[ "$1" = run ] || exit 0
TC=${TC_API:-http://127.0.0.1}
LOG=$WORK_BASE/teddybox-qemu-$NAME.log
SSE=$WORK_BASE/teddybox-qemu-$NAME.sse
QEMU=${QEMU:-qemu-system-xtensa}
curl -s -N "$TC/api/sse" > "$SSE" &
CURL=$!
timeout 90 "$QEMU" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw -nic user,model=open_eth > "$LOG" 2>&1 &
PID=$!
while kill -0 $PID 2>/dev/null && ! grep -aq "EVENTS SENT" "$LOG"; do sleep 1; done
sleep 1
kill $PID $CURL 2>/dev/null
grep -a "RTNL" "$LOG" | grep -av "^W.*esp-tls"
fail=0
expect() {
    if grep -aqE "$2" "$SSE"; then echo "OK   $1"; else echo "FAIL $1"; fail=1; fi
}
expect "tag placed (UID as teddyCloud reads it)" 'TagValid.*E00403501234ABCD'
expect "audio id" 'ContentAudioId.*305419896'
expect "playback started" 'playback.*started'
expect "volume level" 'VolumeLevel'
expect "charger on" 'charger.*on'
expect "playback stopped" 'playback.*stopped'
expect "firmware version (log2 group 8)" '"function_group": 8'
[ $fail = 0 ] && echo "ALL OK" || { echo FAILED; grep -a "^data" "$SSE" | head -40; }
exit $fail
