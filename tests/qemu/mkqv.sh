#!/bin/bash
# QEMU TAF hash check test of the current tb5 tree in /tmp/qv5
# run: /opt/qemu-build/qemu/build/qemu-system-xtensa -nographic -machine esp32s3 \
#        -drive file=/tmp/qv.bin,if=mtd,format=raw
set -e
T=$(cd "$(dirname "$0")" && pwd)
rm -rf /tmp/qv5 && mkdir /tmp/qv5
cd /home/claude/tb5/teddybox && tar cf - --exclude=build --exclude=sdkconfig --exclude=sdkconfig.old . | tar xf - -C /tmp/qv5
cd /tmp/qv5 && cp /tmp/qmem/main/{jingle.ogg,music.ogg,speech.ogg,tc_ca.der} main/
cp "$T/qemu_verify_test.c" main/qemu_mem_test.c && cp /tmp/qv/taf_test_data.h main/
python3 "$T/qemu_variant.py"
. /opt/esp5/env.sh >/dev/null
idf.py set-target esp32s3 >/dev/null 2>&1
idf.py build > /tmp/qv.log 2>&1 || { grep -E "error" /tmp/qv.log | head; exit 1; }
python "$T/qemu_patch.py" build/teddybox.elf /tmp/qvp.elf /tmp/qvp.bin > /dev/null
cd /tmp && cp qemu_final.bin qv.bin && dd if=/dev/zero of=qv.bin bs=1 seek=$((0x3a0000)) count=$((0x220000)) conv=notrunc status=none && dd if=qvp.bin of=qv.bin bs=1 seek=$((0x3a0000)) conv=notrunc status=none
echo built
