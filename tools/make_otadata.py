#!/usr/bin/env python3
"""
Create an otadata image (0x16f000, 8 KiB) that boots a given OTA slot,
based on the otadata of an existing flash dump.

The ESP-IDF bootloader picks the entry with the highest valid sequence
number; the slot is (seq - 1) % number_of_ota_slots. This script keeps the
newer of the two existing entries and overwrites the older one with
seq = next value that selects the requested slot, so switching back is
always possible by writing the original otadata from the dump.

usage: make_otadata.py <flash_dump.bin> <slot 0..2> <out.bin>
"""
import binascii
import struct
import sys

OTADATA_OFFSET = 0x16F000
OTADATA_SIZE = 0x2000
SECTOR = 0x1000
OTA_SLOTS = 3
OTA_IMG_VALID = 2


def entry_crc(seq):
    return binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF


def read_entry(buf):
    seq, = struct.unpack_from("<I", buf, 0)
    state, crc = struct.unpack_from("<II", buf, 24)
    valid = seq != 0xFFFFFFFF and crc == entry_crc(seq)
    return seq, state, valid


def make_entry(seq):
    return struct.pack("<I20sII", seq, b"\xff" * 20, OTA_IMG_VALID, entry_crc(seq)) + b"\xff" * (SECTOR - 32)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    dump_path, slot, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    if not 0 <= slot < OTA_SLOTS:
        sys.exit("slot must be 0..2")

    with open(dump_path, "rb") as f:
        f.seek(OTADATA_OFFSET)
        otadata = bytearray(f.read(OTADATA_SIZE))

    entries = [read_entry(otadata[i * SECTOR:(i + 1) * SECTOR]) for i in range(2)]
    for i, (seq, state, valid) in enumerate(entries):
        info = f"seq={seq} -> ota_{(seq - 1) % OTA_SLOTS}" if valid else "empty/invalid"
        print(f"  current entry {i}: {info}")

    max_seq = max((seq for seq, _, valid in entries if valid), default=0)
    new_seq = max_seq + 1
    while (new_seq - 1) % OTA_SLOTS != slot:
        new_seq += 1

    # overwrite the older (or invalid) entry
    older = 0 if not entries[0][2] else 1 if not entries[1][2] else (0 if entries[0][0] < entries[1][0] else 1)
    otadata[older * SECTOR:(older + 1) * SECTOR] = make_entry(new_seq)

    with open(out_path, "wb") as f:
        f.write(otadata)
    print(f"  new entry {older}: seq={new_seq} -> ota_{slot}")
    print(f"wrote {out_path}, flash it to 0x{OTADATA_OFFSET:x}")


if __name__ == "__main__":
    main()
