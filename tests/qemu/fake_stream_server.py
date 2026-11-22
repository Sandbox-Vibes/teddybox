#!/usr/bin/env python3
"""HTTPS server for qemu_stream_test.c: answers /v2/content/<uid> like a
teddyCloud stream (Content-Length 251658239, no multiple of 4096, TAF header
announcing that length), sends 50000 bytes and closes the connection.
/v1/time answers the time. A request with a Range header gets 416, the test fails then.
Usage: fake_stream_server.py <port> <server cert.pem> <server key.pem>"""
import socket, ssl, sys, threading

STREAM_LEN = 1024 * 1024 * 40 * 6 - 1  # teddyCloud encode.stream_max_size default
SEND = 50000

def varint(v):
    out = b''
    while True:
        b = v & 0x7f
        v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v:
            return out

def taf_header(num_bytes, audio_id=0x12345678):
    pb = b'\x0a\x14' + b'\0' * 20 + b'\x10' + varint(num_bytes) + b'\x18' + varint(audio_id)
    # _fill (field 5) pads the protobuf to 4092 bytes
    for fill_len in range(4092, 0, -1):
        f = b'\x2a' + varint(fill_len) + b'\0' * fill_len
        if len(pb) + len(f) == 4092:
            break
    pb += f
    assert len(pb) == 4092
    return len(pb).to_bytes(4, 'big') + pb

def handle(conn):
    try:
        req = b''
        while b'\r\n\r\n' not in req:
            d = conn.recv(4096)
            if not d:
                return
            req += d
        head = req.split(b'\r\n\r\n')[0].decode(errors='replace')
        line = head.split('\r\n')[0]
        has_range = any(h.lower().startswith('range:') for h in head.split('\r\n')[1:])
        print(f'{line} range={has_range}', flush=True)
        if ' /v1/time ' in line:
            import time
            now = b'%d' % int(time.time())
            conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' % (len(now), now))
            return
        if has_range:
            conn.sendall(b'HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            return
        conn.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n'
                     b'Content-Length: %d\r\nConnection: close\r\n\r\n' % STREAM_LEN)
        body = taf_header(STREAM_LEN - 4096)
        body += bytes((i * 7) & 0xff for i in range(SEND - len(body)))
        for i in range(0, len(body), 2048):
            conn.sendall(body[i:i + 2048])
    except OSError:
        pass
    finally:
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        conn.close()

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[2], sys.argv[3])
ctx.verify_mode = ssl.CERT_NONE
l = socket.socket()
l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
l.bind(('0.0.0.0', int(sys.argv[1])))
l.listen(4)
while True:
    c, _ = l.accept()
    try:
        s = ctx.wrap_socket(c, server_side=True)
    except (ssl.SSLError, OSError) as e:
        print('TLS failed:', e, flush=True)
        c.close()
        continue
    threading.Thread(target=handle, args=(s,), daemon=True).start()
