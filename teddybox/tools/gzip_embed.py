#!/usr/bin/env python3
"""gzip for files served from flash: gzip_embed.py in out (mtime 0: same
bytes for the same input)"""
import gzip, sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'wb') as f:
    f.write(gzip.compress(data, 9, mtime=0))
