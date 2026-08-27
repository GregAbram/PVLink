"""
replay.py -- stream a cached project back to a PVLink receiver.
=================================================================
Reads a project recorded by datamanager.py (--cache-dir) and sends it to a
receiver (Unity/Unreal directly, or through the DataManager acting as if it
were ParaView) using the exact same wire framing -- no receiver-side changes
needed, replay looks exactly like a live ParaView producer.

Colormaps are sent once, up front, before the first timestep -- they are
timestep-independent by design, so the whole replay uses whatever the
recording's last-known colormap was, not a per-timestep historical version.

Usage:
    python replay.py --project ./recordings/default \
                      --host 127.0.0.1 --port 9001 --delay 0.5
"""

import argparse
import glob
import os
import socket
import struct
import time

MSG_TYPE_UPDATE   = 2
MSG_TYPE_COLORMAP = 5
MSG_TYPE_MESH     = 10
MSG_TYPE_BOUNDS   = 11
MSG_TYPE_TIME     = 14

ACK_TIMEOUT = 10.0


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


def send_update_and_wait_ack(sock):
    send_message(sock, MSG_TYPE_UPDATE, b'')
    sock.settimeout(ACK_TIMEOUT)
    try:
        ack = recv_exact(sock, 4)
    finally:
        sock.settimeout(None)
    if ack is None:
        raise OSError("connection closed before UPDATE ack")
    status = struct.unpack('<i', ack)[0]
    if status != 0:
        raise OSError(f"non-zero UPDATE ack status {status}")


def replay(project_dir, host, port, delay, loop):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[replay] connected to {host}:{port}", flush=True)

    colormap_files = sorted(glob.glob(os.path.join(project_dir, 'colormaps', '*.bin')))
    for path in colormap_files:
        with open(path, 'rb') as f:
            payload = f.read()
        send_message(sock, MSG_TYPE_COLORMAP, payload)
        print(f"[replay] sent colormap '{os.path.basename(path)}'", flush=True)

    timestep_dirs = sorted(
        d for d in glob.glob(os.path.join(project_dir, 'timesteps', '*'))
        if os.path.isdir(d)
    )
    if not timestep_dirs:
        print(f"[replay] no timesteps found under {project_dir}", flush=True)
        sock.close()
        return

    first = True
    while True:
        for tdir in timestep_dirs:
            if not first:
                time.sleep(delay)
            first = False

            bounds_path = os.path.join(tdir, 'bounds.bin')
            if os.path.isfile(bounds_path):
                with open(bounds_path, 'rb') as f:
                    send_message(sock, MSG_TYPE_BOUNDS, f.read())

            time_path = os.path.join(tdir, 'time.bin')
            if os.path.isfile(time_path):
                with open(time_path, 'rb') as f:
                    send_message(sock, MSG_TYPE_TIME, f.read())

            mesh_files = sorted(
                p for p in glob.glob(os.path.join(tdir, '*.bin'))
                if os.path.basename(p) not in ('bounds.bin', 'time.bin')
            )
            for mpath in mesh_files:
                with open(mpath, 'rb') as f:
                    send_message(sock, MSG_TYPE_MESH, f.read())

            send_update_and_wait_ack(sock)
            print(f"[replay] flipped {os.path.basename(tdir)} "
                  f"({len(mesh_files)} mesh(es))", flush=True)

        if not loop:
            break

    sock.close()
    print("[replay] done", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Replay a cached PVLink project to a receiver")
    parser.add_argument('--project', required=True, help='path to the recorded project directory')
    parser.add_argument('--host', default='127.0.0.1', help='receiver host')
    parser.add_argument('--port', type=int, required=True, help='receiver port')
    parser.add_argument('--delay', type=float, default=0.5, help='seconds between timesteps')
    parser.add_argument('--loop', action='store_true', help='loop the recording indefinitely')
    args = parser.parse_args()
    replay(args.project, args.host, args.port, args.delay, args.loop)


if __name__ == '__main__':
    main()
