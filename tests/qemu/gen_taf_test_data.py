#!/usr/bin/env python3
"""Writes taf_test_data.h for qemu_verify_test.c from a TAF file:
the file itself, the SHA-1 and length of its audio data from the header."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack('>I', d[:4])[0]
pb = d[4:4 + n]
assert pb[0] == 0x0a and pb[1] == 20, "header without SHA-1"
sha1 = pb[2:22]
print('#include <stdint.h>')
print('static const uint8_t taf_sha1[20] = {%s};' % ','.join(str(b) for b in sha1))
print('static const uint64_t taf_num_bytes = %d;' % (len(d) - 4096))
print('static const unsigned taf_len = %d;' % len(d))
print('static const uint8_t taf_data[] = {%s};' % ','.join(str(b) for b in d))
