#!/usr/bin/env python3
"""TCP proxy for qemu_resume_test.c: forwards LISTEN_PORT to TARGET and cuts
every connection after CUT bytes from the server (default 50 KB)."""
import socket, sys, threading
LISTEN_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4443
TARGET = (sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1', int(sys.argv[3]) if len(sys.argv) > 3 else 443)
CUT = int(sys.argv[4]) if len(sys.argv) > 4 else 50000

def pipe(src, dst, limit, label):
    n = 0
    try:
        while True:
            d = src.recv(4096)
            if not d:
                break
            if limit and n + len(d) > limit:
                d = d[:limit - n]
                dst.sendall(d)
                print(f'cut {label} after {limit} bytes', flush=True)
                break
            dst.sendall(d)
            n += len(d)
    except OSError:
        pass
    for s in (src, dst):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        s.close()

def handle(c):
    s = socket.create_connection(TARGET)
    threading.Thread(target=pipe, args=(c, s, 0, 'up'), daemon=True).start()
    threading.Thread(target=pipe, args=(s, c, CUT, 'down'), daemon=True).start()

l = socket.socket()
l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
l.bind(('0.0.0.0', LISTEN_PORT))
l.listen(8)
while True:
    c, _ = l.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
