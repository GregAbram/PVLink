"""
tcp_server.py  —  run on TACC
Usage: python tcp_server.py [port]

Listens for a connection from the Mac client and reads messages of
increasing size, printing each one as it arrives.
"""

import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 19001

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)

listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('0.0.0.0', PORT))
listener.listen(1)
print(f"Listening on port {PORT} ...", flush=True)

conn, addr = listener.accept()
conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print(f"Connected from {addr}", flush=True)

while True:
    hdr = recv_exact(conn, 8)
    if hdr is None:
        print("Connection closed.", flush=True)
        break
    size, tag = struct.unpack('<ii', hdr)
    if size == -1:
        print("Done.", flush=True)
        break
    payload = recv_exact(conn, size)
    if payload is None:
        print(f"Connection closed mid-payload (got {size} bytes header).", flush=True)
        break
    print(f"  received {size:>8} bytes", flush=True)

conn.close()
listener.close()
