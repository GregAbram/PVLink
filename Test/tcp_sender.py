"""
tcp_sender.py  —  run on TACC
Usage: python tcp_sender.py <mac-ip> [port]

Connects to the receiver on the Mac and sends payloads of increasing size,
printing how long each sendall takes.  If sendall blocks on large payloads
the line will hang, confirming the TCP window / firewall ACK hypothesis.
"""

import socket
import struct
import time
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 19001

SIZES = [
    100,
    1_000,
    10_000,
    50_000,
    100_000,
    145_000,
    200_000,
]

def send_msg(sock, size):
    payload = b'X' * size
    header  = struct.pack('<ii', size, 42)
    msg     = header + payload
    print(f"  sendall {size:>8} bytes ... ", end='', flush=True)
    t0 = time.monotonic()
    sock.sendall(msg)
    elapsed = time.monotonic() - t0
    print(f"done in {elapsed*1000:.1f} ms", flush=True)

print(f"Connecting to {HOST}:{PORT} ...", flush=True)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.settimeout(5.0)
s.connect((HOST, PORT))
s.settimeout(None)
print("Connected.", flush=True)

for size in SIZES:
    send_msg(s, size)
    time.sleep(0.1)   # brief gap so receiver can drain between sends

# Send a termination marker (size=-1)
s.sendall(struct.pack('<ii', -1, 0))
print("All done.", flush=True)
s.close()
