#!/bin/bash
# QEMU test: names of the tonies from teddyCloud's web API.
#   mkqnames.sh       build
#   mkqnames.sh run   build, add a custom tonie (audio id 0x12345678, the
#                     test tag A1B2C3D4/500304E0) to the local teddyCloud
#                     and assign it to that tag (TC_DIR, default /tmp/tc),
#                     run, restore
NAME=names
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_names_test.c"
qemu_build
[ "$1" = run ] || exit 0
TC_DIR=${TC_DIR:-/tmp/tc}
TC=${TC_API:-http://127.0.0.1}
J=$TC_DIR/config/tonies.custom.json
cp "$J" "$J.bak"
cat > "$J" <<'J'
[{"no":"0","model":"TEST-1","audio_id":["305419896"],"hash":[],"title":"Testserie - Folge 1","series":"Testserie","episodes":"Folge 1","tracks":["Eins"],"release":"0","language":"de-de","category":"custom","pic":""}]
J
CJ=$TC_DIR/data/content/default/A1B2C3D4/500304E0.json
cp "$CJ" "$CJ.bak"
sed -i 's/"tonie_model":[^,]*,/"tonie_model": "TEST-1",/' "$CJ"
curl -s -m5 "$TC/api/toniesJsonReload" > /dev/null
LOG=$WORK_BASE/teddybox-qemu-$NAME.log
QEMU=${QEMU:-qemu-system-xtensa}
timeout 90 "$QEMU" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw -nic user,model=open_eth > "$LOG" 2>&1 &
PID=$!
while kill -0 $PID 2>/dev/null && ! grep -aqE "NMTEST: (ALL OK|FAILED)" "$LOG"; do sleep 1; done
kill $PID 2>/dev/null
mv "$J.bak" "$J"
mv "$CJ.bak" "$CJ"
curl -s -m5 "$TC/api/toniesJsonReload" > /dev/null
grep -aE "NMTEST|Names:" "$LOG"
grep -aq "NMTEST: ALL OK" "$LOG"
