"""
synth_producer.py -- hand-build a few frames of valid PVLink wire traffic and
send them to a DataManager (or any PVLink receiver), without needing
ParaView running.  Useful for testing DataManager recording / a receiver in
isolation.

Usage:
    python synth_producer.py --host 127.0.0.1 --port 9000 --project synthtest --frames 3
"""

import argparse
import math
import socket
import struct

MSG_TYPE_UPDATE   = 2
MSG_TYPE_COLORMAP = 5
MSG_TYPE_MESH     = 10
MSG_TYPE_BOUNDS   = 11
MSG_TYPE_PROJECT  = 13
MSG_TYPE_TIME     = 14


def send_message(sock, msg_type, payload):
    sock.sendall(struct.pack('<ii', len(payload), msg_type) + payload)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def build_project_payload(name):
    name_bytes = name.encode('utf-8')
    return struct.pack('<i', len(name_bytes)) + name_bytes


def build_colormap_payload(var_name, rgb_samples):
    name_bytes = var_name.encode('utf-8')
    buf = struct.pack('<i', len(name_bytes)) + name_bytes
    buf += struct.pack('<ffi', 0.0, 1.0, len(rgb_samples))
    for r, g, b in rgb_samples:
        buf += struct.pack('<fff', r, g, b)
    return buf


def build_bounds_payload(bounds):
    return struct.pack('<6f', *bounds)


def build_triangle_mesh_payload(mesh_name, scalar_offset):
    """One triangle, per-point scalars, no normals (receiver computes them)."""
    positions = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    indices   = [(0, 1, 2)]
    scalars   = [scalar_offset, scalar_offset + 0.5, scalar_offset + 1.0]
    color_name_bytes = b'TestData'
    mesh_name_bytes  = mesh_name.encode('utf-8')

    header = struct.pack('<iiiiii', len(positions), len(indices), 0, 0,
                          len(color_name_bytes), len(mesh_name_bytes))
    buf = header
    for p in positions:
        buf += struct.pack('<fff', *p)
    for tri in indices:
        buf += struct.pack('<iii', *tri)
    for s in scalars:
        buf += struct.pack('<f', s)
    buf += struct.pack('<ff', min(scalars), max(scalars))
    buf += color_name_bytes
    buf += mesh_name_bytes
    return buf


def main():
    parser = argparse.ArgumentParser(description="Send synthetic PVLink wire traffic")
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--project', default='synthtest')
    parser.add_argument('--frames', type=int, default=3)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[synth] connected to {args.host}:{args.port}")

    send_message(sock, MSG_TYPE_PROJECT, build_project_payload(args.project))
    send_message(sock, MSG_TYPE_COLORMAP, build_colormap_payload(
        'TestData', [(0.0, 0.0, 1.0), (1.0, 0.0, 0.0)]))

    for i in range(args.frames):
        t = float(i)
        send_message(sock, MSG_TYPE_BOUNDS, build_bounds_payload((-1, 1, -1, 1, -1, 1)))
        send_message(sock, MSG_TYPE_MESH, build_triangle_mesh_payload(
            'synthtri', scalar_offset=math.sin(t) * 0.5))
        send_message(sock, MSG_TYPE_UPDATE, b'')
        ack = recv_exact(sock, 4)
        status = struct.unpack('<i', ack)[0] if ack else None
        print(f"[synth] frame {i}: UPDATE ack={status}")

    sock.close()
    print("[synth] done")


if __name__ == '__main__':
    main()
