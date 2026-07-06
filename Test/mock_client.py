"""
mock_client.py  —  run on TACC (in place of ParaView/PVLink)

Connects to localhost:9001 (SSH tunnel → Mac:9001) and sends the same
message sequence PVLink sends for a two-mesh pipeline cycle:

  BOUNDS    (type=11,  24 bytes)   — replay state
  BOUNDS    (type=11,  24 bytes)   — queue send
  COLORMAP  (type= 5,  3094 bytes)
  MESH      (type=10, 145325 bytes) — Contour
  COLORMAP  (type= 5,  3094 bytes)
  MESH      (type=10,  48499 bytes) — Slice
  UPDATE    (type= 2,   0 bytes)   — waits for 4-byte ack

Usage:
    python mock_client.py [port]
"""

import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001

MESSAGES = [
    (11,  24),       # BOUNDS replay
    (11,  24),       # BOUNDS queue
    ( 5,  3094),     # COLORMAP
    (10, 145325),    # MESH Contour
    ( 5,  3094),     # COLORMAP (second sender)
    (10,  48499),    # MESH Slice
    ( 2,  0),        # UPDATE  ← expects 4-byte ack
]

MSG_NAMES = {2: 'UPDATE', 5: 'COLORMAP', 10: 'MESH', 11: 'BOUNDS'}

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)

print(f"mock_client: connecting to localhost:{PORT} ...", flush=True)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.settimeout(5.0)
s.connect(('localhost', PORT))
s.settimeout(None)
print(f"mock_client: connected", flush=True)

for msg_type, payload_len in MESSAGES:
    name = MSG_NAMES.get(msg_type, f'type={msg_type}')
    payload = b'\x00' * payload_len
    header  = struct.pack('<ii', payload_len, msg_type)

    t0 = time.monotonic()
    print(f"mock_client: sending {name} ({payload_len} bytes) ...", flush=True)
    s.sendall(header + payload)
    elapsed = (time.monotonic() - t0) * 1000
    print(f"mock_client: {name} sendall done in {elapsed:.1f} ms", flush=True)

    if msg_type == 2:   # UPDATE — wait for ack
        print(f"mock_client: waiting for UPDATE ack ...", flush=True)
        t0 = time.monotonic()
        ack = recv_exact(s, 4)
        elapsed = (time.monotonic() - t0) * 1000
        if ack is None:
            print(f"mock_client: connection closed before ack", flush=True)
        else:
            status = struct.unpack('<i', ack)[0]
            print(f"mock_client: UPDATE ack received in {elapsed:.1f} ms — status={status}", flush=True)

s.close()
print("mock_client: done", flush=True)
