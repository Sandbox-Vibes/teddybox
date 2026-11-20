#!/bin/bash
# QEMU download-handling test of the current tb5 tree in /tmp/qdl5
set -e
rm -rf /tmp/qdl5 && mkdir /tmp/qdl5
cd /home/claude/tb5/teddybox && tar cf - --exclude=build --exclude=sdkconfig --exclude=sdkconfig.old . | tar xf - -C /tmp/qdl5
cd /tmp/qdl5 && cp /tmp/qmem/main/{jingle.ogg,music.ogg,speech.ogg,tc_ca.der} main/
cp /tmp/q5tools/qemu_dl_test.c main/qemu_mem_test.c
python3 /tmp/q5tools/qemu_variant.py
sed -i 's#/spiflash/cert/#/sdcard/CERT/#g' main/cloud.c
. /opt/esp5/env.sh >/dev/null
idf.py set-target esp32s3 >/dev/null 2>&1
idf.py build > /tmp/qdl.log 2>&1 || { grep -E "error" /tmp/qdl.log | head; exit 1; }
python /tmp/q5tools/qemu_patch.py build/teddybox.elf /tmp/qdlp.elf /tmp/qdlp.bin > /dev/null
cd /tmp && cp qemu_final.bin qdl.bin && dd if=/dev/zero of=qdl.bin bs=1 seek=$((0x3a0000)) count=$((0x220000)) conv=notrunc status=none && dd if=qdlp.bin of=qdl.bin bs=1 seek=$((0x3a0000)) conv=notrunc status=none
echo built
