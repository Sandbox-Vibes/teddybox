# Shared setup of the QEMU test scripts (sourced). Builds a variant of the
# firmware in $WORK and writes a flash image $OUT for qemu-system-xtensa.
#
#   QEMU_BASE_IMAGE  8 MB flash image with bootloader, partition table, NVS and
#                    the assets FAT (your own; it is copied, never changed)
#   QEMU_MEDIA       directory with jingle.ogg, music.ogg, speech.ogg (Ogg Opus)
#                    and tc_ca.der (CA of the test teddyCloud), see README.md
#   IDF_EXPORT       ESP-IDF environment script (default $IDF_PATH/export.sh)
#   WORK_BASE        where the build trees go (default /tmp)
set -e
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$T/../.." && pwd)
: "${QEMU_BASE_IMAGE:?set QEMU_BASE_IMAGE, see tests/qemu/README.md}"
: "${QEMU_MEDIA:?set QEMU_MEDIA, see tests/qemu/README.md}"
IDF_EXPORT=${IDF_EXPORT:-$IDF_PATH/export.sh}
WORK_BASE=${WORK_BASE:-/tmp}
APP_OFFSET=$((0x3a0000))
APP_SIZE=$((0x220000))

# copy the firmware tree to $WORK and turn it into the QEMU test variant
# with test file $1 as main/qemu_mem_test.c
qemu_prepare() {
    WORK=$WORK_BASE/teddybox-qemu-$NAME
    rm -rf "$WORK" && mkdir -p "$WORK"
    (cd "$REPO/teddybox" && tar cf - --exclude=build --exclude=sdkconfig --exclude=sdkconfig.old .) | tar xf - -C "$WORK"
    cp "$QEMU_MEDIA"/{jingle.ogg,music.ogg,speech.ogg,tc_ca.der} "$WORK/main/"
    cp "$1" "$WORK/main/qemu_mem_test.c"
    (cd "$WORK" && python3 "$T/qemu_variant.py")
}

# build $WORK and write the flash image $OUT
qemu_build() {
    . "$IDF_EXPORT" >/dev/null
    cd "$WORK"
    idf.py set-target esp32s3 >/dev/null 2>&1
    idf.py build > "$WORK/build.log" 2>&1 || { grep -E "error" "$WORK/build.log" | head; exit 1; }
    python3 "$T/qemu_patch.py" build/teddybox.elf "$WORK/app.elf" "$WORK/app.bin" > /dev/null
    OUT=$WORK_BASE/teddybox-qemu-$NAME.bin
    cp "$QEMU_BASE_IMAGE" "$OUT"
    dd if=/dev/zero of="$OUT" bs=1 seek=$APP_OFFSET count=$APP_SIZE conv=notrunc status=none
    dd if="$WORK/app.bin" of="$OUT" bs=1 seek=$APP_OFFSET conv=notrunc status=none
    echo "built $OUT, run:"
    echo "  qemu-system-xtensa -nographic -machine esp32s3 -drive file=$OUT,if=mtd,format=raw${QEMU_NET:+ -nic user,model=open_eth}"
}
