#!/usr/bin/env python3
"""Checks the layout of a TAF file as the box expects it:
taf_check.py file.taf [expected chapters]. Exit 1 on any problem."""
import hashlib, struct, sys

def varint(b, i):
    v = s = 0
    while True:
        x = b[i]; i += 1; v |= (x & 0x7f) << s; s += 7
        if not x & 0x80: return v, i

def crc(b):
    c = 0
    for x in b:
        c ^= x << 24
        for _ in range(8): c = ((c << 1) ^ 0x04c11db7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c

d = open(sys.argv[1], 'rb').read()
errs = []
hl = struct.unpack('>I', d[:4])[0]
if hl != 4092: errs.append(f'header length {hl}')
pb, i, f = d[4:4096], 0, {}
while i < len(pb):
    key, i = varint(pb, i); fn, wt = key >> 3, key & 7
    if wt == 0: v, i = varint(pb, i); f[fn] = v
    elif wt == 2:
        l, i = varint(pb, i); f[fn] = pb[i:i + l]; i += l
    else: errs.append(f'wire type {wt}'); break
audio = d[4096:]
if f.get(1) != hashlib.sha1(audio).digest(): errs.append('sha1')
if f.get(2) != len(audio): errs.append(f'num_bytes {f.get(2)} != {len(audio)}')
if len(audio) % 4096: errs.append('audio not a multiple of 4096')
tp, j = [], 0
while j < len(f.get(4, b'')): v, j = varint(f[4], j); tp.append(v)
aid = f.get(3)
# pages
o, seq, last_g, pages, starts, celt = 0, 0, 0, [], {}, True
while o < len(audio):
    if audio[o:o + 4] != b'OggS': errs.append(f'no page at {o}'); break
    flags, g, serial, sq, cs, ns = struct.unpack('<BqIIIB', audio[o + 5:o + 27])
    segt = audio[o + 27:o + 27 + ns]; ln = 27 + ns + sum(segt)
    pg = bytearray(audio[o:o + ln]); pg[22:26] = b'\0\0\0\0'
    if crc(pg) != cs: errs.append(f'crc page {sq}')
    if sq != seq: errs.append(f'seq {sq} != {seq}')
    if serial != aid and seq == 0: print(f'note: Ogg serial {serial:08x} != audio_id')
    if seq >= 2 and ln != 4096: errs.append(f'page {sq} has {ln} bytes')
    if seq >= 2 and (o % 4096): errs.append(f'page {sq} not aligned')
    if segt and segt[-1] == 255: errs.append(f'page {sq} ends in a continued packet')
    if g < last_g: errs.append(f'granule goes back at page {sq}')
    last_g = g
    if seq >= 2:
        # packets: CELT only (TOC config >= 16), padding sane
        p = o + 27 + ns; acc = 0
        for s in segt:
            acc += s
            if s < 255:
                if audio[p] >> 3 < 16: celt = False
                p += acc; acc = 0
    starts[o // 4096] = sq
    pages.append(sq); seq += 1; o += ln
if seq >= 2 and pages and starts.get(1) != 2: errs.append('first audio page not at block 1')
if (audio[5] if len(audio) > 5 else 0) != 2: errs.append('first page not BOS')
if not celt: errs.append('non-CELT packet')
if not tp or tp[0] != 0: errs.append(f'first chapter {tp[:1]}')
for c in tp[1:]:
    if starts.get(c) != c + 1: errs.append(f'chapter page {c} not a page start')
if len(sys.argv) > 2 and len(tp) != int(sys.argv[2]): errs.append(f'{len(tp)} chapters')
dur = (last_g - struct.unpack('<H', audio[38:40])[0]) / 48000
print(f"{sys.argv[1]}: {len(audio) // 4096 + 1} blocks, chapters {tp}, audio_id {aid:08x}, {dur:.3f} s, last page flags {audio[-4096+5]}",
      'OK' if not errs else 'ERRORS: ' + '; '.join(errs[:8]))
sys.exit(1 if errs else 0)
