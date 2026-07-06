"""
mock_server.py  —  run on Mac (in place of Unity)

Listens on port 9001, accepts one connection, reads every PVLink message,
prints type and size, and sends the 4-byte ack only for UPDATE (type 2).
Exits when the client closes the connection.

Usage:
    python mock_server.py [port]
"""

import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001

MSG_NAMES = {2: 'UPDATE', 5: 'COLORMAP', 10: 'MESH', 11: 'BOUNDS', 12: 'VISIBILITY'}

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
print(f"mock_server: listening on port {PORT} ...", flush=True)

conn, addr = listener.accept()
conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
print(f"mock_server: client connected from {addr}", flush=True)

msg_count = 0
while True:
    hdr = recv_exact(conn, 8)
    if hdr is None:
        print("mock_server: connection closed.", flush=True)
        break
    payload_len, msg_type = struct.unpack('<ii', hdr)
    name = MSG_NAMES.get(msg_type, f'type={msg_type}')

    if payload_len < 0 or payload_len > 256 * 1024 * 1024:
        print(f"mock_server: bad payload_len {payload_len} — dropping", flush=True)
        break

    payload = recv_exact(conn, payload_len) if payload_len > 0 else b''
    if payload is None:
        print(f"mock_server: connection closed mid-payload for {name}", flush=True)
        break

    msg_count += 1
    print(f"mock_server: [{msg_count}] {name} payload={payload_len} bytes", flush=True)

    if msg_type == 2:   # UPDATE — send ack
        print(f"mock_server: sending UPDATE ack ...", flush=True)
        conn.sendall(struct.pack('<i', 0))
        print(f"mock_server: UPDATE ack sent", flush=True)

conn.close()
listener.close()
print(f"mock_server: done ({msg_count} messages received)", flush=True)
