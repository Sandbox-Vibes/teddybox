#!/bin/bash
# QEMU read-ahead test of the current tb5 tree in /tmp/qra5
# run: /opt/qemu-build/qemu/build/qemu-system-xtensa -nographic -machine esp32s3 \
#        -drive file=/tmp/qra.bin,if=mtd,format=raw
set -e
T=$(cd "$(dirname "$0")" && pwd)
rm -rf /tmp/qra5 && mkdir /tmp/qra5
cd /home/claude/tb5/teddybox && tar cf - --exclude=build --exclude=sdkconfig --exclude=sdkconfig.old . | tar xf - -C /tmp/qra5
cd /tmp/qra5 && cp /tmp/qmem/main/{jingle.ogg,music.ogg,speech.ogg,tc_ca.der} main/
cp "$T/qemu_ra_test.c" main/qemu_mem_test.c
python3 "$T/qemu_variant.py"
. /opt/esp5/env.sh >/dev/null
idf.py set-target esp32s3 >/dev/null 2>&1
idf.py build > /tmp/qra.log 2>&1 || { grep -E "error" /tmp/qra.log | head; exit 1; }
python "$T/qemu_patch.py" build/teddybox.elf /tmp/qrap.elf /tmp/qrap.bin > /dev/null
cd /tmp && cp qemu_final.bin qra.bin && dd if=/dev/zero of=qra.bin bs=1 seek=$((0x3a0000)) count=$((0x220000)) conv=notrunc status=none && dd if=qrap.bin of=qra.bin bs=1 seek=$((0x3a0000)) conv=notrunc status=none
echo built
