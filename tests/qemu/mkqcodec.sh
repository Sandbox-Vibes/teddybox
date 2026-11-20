#!/bin/bash
# QEMU decoder test of the current tb5 tree in /tmp/qcodec5
set -e
rm -rf /tmp/qcodec5 && mkdir /tmp/qcodec5
cd /home/claude/tb5/teddybox && tar cf - --exclude=build --exclude=sdkconfig --exclude=sdkconfig.old . | tar xf - -C /tmp/qcodec5
cd /tmp/qcodec5 && cp /tmp/qmem/main/{jingle.ogg,music.ogg,speech.ogg,tc_ca.der} main/
sed -e "s/JUMP_FROM/137/; s/JUMP_TO/82009/" /tmp/q5tools/qemu_codec_test.c > main/qemu_mem_test.c
python3 /tmp/q5tools/qemu_variant.py
. /opt/esp5/env.sh >/dev/null
idf.py set-target esp32s3 >/dev/null 2>&1
idf.py build > /tmp/qcodec.log 2>&1 || { grep -E "error" /tmp/qcodec.log | head; exit 1; }
python /tmp/q5tools/qemu_patch.py build/teddybox.elf /tmp/qcp.elf /tmp/qcp.bin > /dev/null
cd /tmp && cp qemu_final.bin qcodec.bin && dd if=/dev/zero of=qcodec.bin bs=1 seek=$((0x3a0000)) count=$((0x220000)) conv=notrunc status=none && dd if=qcp.bin of=qcodec.bin bs=1 seek=$((0x3a0000)) conv=notrunc status=none
echo built
