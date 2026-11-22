#!/usr/bin/env python3
"""Writes a small TAF file (valid header: SHA-1, length, audio id, chapters)
with pseudo random audio data: make_test_taf.py out.taf [kbytes] [audio_id]"""
import hashlib, random, sys

def varint(v):
    out = b''
    while True:
        b = v & 0x7f
        v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v:
            return out

out = sys.argv[1]
kb = int(sys.argv[2]) if len(sys.argv) > 2 else 64
audio_id = int(sys.argv[3]) if len(sys.argv) > 3 else 0x65000001
random.seed(audio_id)
data = bytes(random.getrandbits(8) for _ in range(kb * 1024))
pb = b'\x0a\x14' + hashlib.sha1(data).digest() + b'\x10' + varint(len(data)) + b'\x18' + varint(audio_id)
pb += b'\x22' + bytes([3]) + bytes([0, 4, 9])  # track_page_nums (packed): 3 chapters
for fill in range(4092, 0, -1):
    f = b'\x2a' + varint(fill) + b'\0' * fill
    if len(pb) + len(f) == 4092:
        break
pb += f
assert len(pb) == 4092
open(out, 'wb').write(len(pb).to_bytes(4, 'big') + pb + data)
