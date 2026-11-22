#!/bin/bash
# QEMU test of the "Tonies" tab backend: list, upload (good, damaged, too
# short), protection, delete. mkqtonies.sh run: build, start QEMU with port
# 18080 forwarded to the box's port 80, drive it with curl.
NAME=tonies
QEMU_NET=1
. "$(dirname "$0")/common.sh"
qemu_prepare "$T/qemu_tonies_test.c"
qemu_build
[ "$1" = run ] || exit 0
LOG=$WORK_BASE/teddybox-qemu-$NAME.log
QEMU=${QEMU:-qemu-system-xtensa}
D=$WORK_BASE/tonies-test
rm -rf "$D" && mkdir -p "$D"
python3 "$T/make_test_taf.py" "$D/good.taf" 64 1694498817
python3 "$T/make_test_taf.py" "$D/other.taf" 32 1694498818
python3 - "$D" <<'P'
import sys
d = sys.argv[1]
g = bytearray(open(d + '/good.taf', 'rb').read())
g[5000] ^= 0xff
open(d + '/bad.taf', 'wb').write(g)
open(d + '/short.taf', 'wb').write(open(d + '/good.taf', 'rb').read()[:40000])
P
timeout 300 "$QEMU" -nographic -machine esp32s3 -drive file="$OUT",if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:18080-:80 > "$LOG" 2>&1 &
PID=$!
for i in $(seq 120); do grep -aq "TNTEST: READY" "$LOG" && break; sleep 1; done
B=http://127.0.0.1:18080
U=E00403501234ABCD
fail=0
t() { if eval "$2"; then echo "OK   $1"; else echo "FAIL $1"; fail=1; fi; }
up() { curl -s -m 120 -X POST --data-binary @"$D/$1" "$B/api/tonie/upload?uid=$2"; }
list() { curl -s -m 30 "$B/api/tonies"; }
L=$(list); echo "$L" | head -c 400; echo
t "empty list, seen tonies listed" '[[ "$L" == *"\"tonies\":[]"* && "$L" == *"$U"* && "$L" == *E004035011223344* ]]'
R=$(up good.taf $U); echo "$R"
t "good upload accepted, 3 chapters" '[[ "$R" == *"\"ok\":true"* && "$R" == *"\"chapters\":3"* ]]'
L=$(list)
t "listed complete and own" '[[ "$L" == *"\"uid\":\"$U\",\"alias\":\"\",\"tc_name\":\"\",\"state\":\"ok\""* && "$L" == *"\"own\":true"* && "$L" == *"\"audio_id\":1694498817"* ]]'
R=$(up bad.taf $U); echo "$R"
t "damaged file refused (SHA-1)" '[[ "$R" == *"\"ok\":false"* && "$R" == *SHA-1* ]]'
R=$(up short.taf $U); echo "$R"
t "short file refused (length)" '[[ "$R" == *"\"ok\":false"* && "$R" == *nge* ]]'
L=$(list)
t "good file still there" '[[ "$L" == *"\"audio_id\":1694498817"* ]]'
R=$(up other.taf $U); L=$(list)
t "replaced by another file" '[[ "$R" == *"\"ok\":true"* && "$L" == *"\"audio_id\":1694498818"* ]]'
curl -s -m 30 -X POST --data-binary 'Gute Nacht "Bär"' "$B/api/tonie/alias?uid=$U" > /dev/null
curl -s -m 30 -X POST --data-binary 'Neuer Tonie' "$B/api/tonie/alias?uid=E004035011223344" > /dev/null; L=$(list)
t "alias shown (UTF-8, quotes escaped), also for a seen tag" '[[ "$L" == *"\"alias\":\"Gute Nacht \\\"Bär\\\"\""* && "$L" == *"\"alias\":\"Neuer Tonie\""* ]]'
t "list is valid JSON" 'echo "$L" | python3 -c "import json,sys; json.load(sys.stdin)"'
curl -s -m 30 -X POST --data-binary '' "$B/api/tonie/alias?uid=E004035011223344" > /dev/null; L=$(list)
t "alias removed" '[[ "$L" != *"Neuer Tonie"* && "$L" == *Nacht* ]]'
curl -s -m 30 -X POST "$B/api/tonie/own?uid=$U&on=0" > /dev/null; L=$(list)
t "protection off" '[[ "$L" == *"\"own\":false"* ]]'
R=$(curl -s -m 30 -X POST "$B/api/tonie/upload?uid=XYZ"); 
t "invalid UID refused" '[[ "$R" == *"\"ok\":false"* ]]'
curl -s -m 30 -X POST "$B/api/tonie/delete?uid=$U" > /dev/null; L=$(list)
t "deleted" '[[ "$L" == *"\"tonies\":[]"* ]]'
kill $PID 2>/dev/null
grep -a "Tonies:" "$LOG" | tail -6
[ $fail = 0 ] && echo "ALL OK" || echo FAILED
exit $fail
