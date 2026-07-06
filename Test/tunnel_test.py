"""
tunnel_test.py  —  run on TACC (with SSH tunnel active)

Connects to localhost:9001 (the SSH tunnel endpoint), sends a fake
BOUNDS message (type=11, 24-byte payload), then waits for the 4-byte
ack that Unity should send back.

Usage:
    python tunnel_test.py [port]

If this hangs at "waiting for ack..." the tunnel's reverse direction
(Mac→TACC) is broken.  If it prints "ack received" the tunnel is fine
and the problem is elsewhere.
"""

import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001

MSG_TYPE_BOUNDS = 11
payload = struct.pack('<6f', -10.0, 10.0, -10.0, 10.0, -10.0, 10.0)  # 24 bytes
header  = struct.pack('<ii', len(payload), MSG_TYPE_BOUNDS)

print(f"Connecting to localhost:{PORT} ...", flush=True)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.settimeout(5.0)
try:
    s.connect(('localhost', PORT))
except OSError as e:
    print(f"Connect failed: {e}")
    sys.exit(1)
print("Connected.", flush=True)

s.settimeout(10.0)   # 10-second ack timeout

print(f"Sending BOUNDS (type=11, {len(payload)}-byte payload) ...", flush=True)
t0 = time.monotonic()
s.sendall(header + payload)
print(f"sendall done in {(time.monotonic()-t0)*1000:.1f} ms", flush=True)

print("Waiting for 4-byte ack ...", flush=True)
try:
    buf = b''
    while len(buf) < 4:
        chunk = s.recv(4 - len(buf))
        if not chunk:
            print("Connection closed before ack — Unity closed the socket.")
            sys.exit(1)
        buf += chunk
    elapsed = time.monotonic() - t0
    status = struct.unpack('<i', buf)[0]
    print(f"Ack received in {elapsed*1000:.1f} ms — status={status}", flush=True)
except socket.timeout:
    print("TIMEOUT — no ack received in 10 seconds. Reverse tunnel direction is broken.", flush=True)
    sys.exit(1)

s.close()
print("Done.", flush=True)
