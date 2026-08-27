"""
DataManager -- Stage 1 (+ recording): transparent single-client relay,
with an optional on-disk cache of everything that passes through.
=========================================================================
Sits between a PVLink data source (the ParaView PVLink plugin) and a single
PVLink-enabled receiver (Unity/Unreal), relaying every message byte-for-byte.
Requires no changes to Unity/Unreal receivers -- from the receiver's side,
being connected to by the DataManager looks identical to being connected to
directly by ParaView.

If --cache-dir is given, every message is ALSO written to disk as it passes
through, in a format replay.py can read back and stream to a receiver with
no live ParaView needed:

    <cache-dir>/<project>/
      meta.json
      colormaps/
        <variable-name>.bin      # raw COLORMAP payload, overwritten in place
                                  # (colormap is timestep-independent -- see
                                  # replay.py, which sends these once, up
                                  # front, so they apply to the whole replay)
      timesteps/
        000000/
          time.bin                # raw TIME payload (float64)
          bounds.bin               # raw BOUNDS payload for this timestep
                                    # (copied forward from the last-known
                                    # value if unchanged this frame, so every
                                    # timestep dir is self-contained)
          <mesh-name>.bin           # raw MESH payload, one file per mesh
        000001/
          ...

Every on-disk file is the exact payload bytes already built for the socket
-- recording is "write what I already have," replay is "read it back and
resend it," with no re-serialization on either end.

Usage:
    python datamanager.py --listen-port 9000 \
                           --downstream-host 127.0.0.1 --downstream-port 9001 \
                           --cache-dir ./recordings
"""

import argparse
import json
import os
import socket
import struct
import threading
import time

MSG_TYPE_UPDATE     = 2
MSG_TYPE_COLORMAP   = 5
MSG_TYPE_MESH       = 10
MSG_TYPE_BOUNDS     = 11
MSG_TYPE_VISIBILITY = 12
MSG_TYPE_PROJECT    = 13
MSG_TYPE_TIME       = 14

HEADER_SIZE = 8   # int32 payload_len + int32 msg_type


def recv_exact(sock, n):
    """Read exactly n bytes, or return None if the peer closed first."""
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class Recorder:
    """Tracks the current project/timestep and writes payloads to disk.
    A no-op (every method just returns) until a MSG_TYPE_PROJECT message
    tells it where to write.

    The timestep boundary is NOT MSG_TYPE_UPDATE (each mesh sender fires its
    own UPDATE for live-sync backpressure -- keying off it splits one real
    frame with N mesh senders into N folders) and NOT MSG_TYPE_TIME either:
    TIME comes from PVLinkDomainBoundsFilter, which only re-executes when
    something upstream of it changes.  An animation driven by keyframes on
    downstream filters (e.g. animating Contour1's ContourValues or Slice1's
    Slice Origin directly, rather than real dataset timesteps) never marks
    PVLinkDomainBoundsFilter dirty, so TIME/BOUNDS may only ever arrive once
    for the whole recording even though real per-frame data keeps flowing.

    The boundary that actually works regardless of pipeline topology: a
    MESH message for a mesh name that's already present in the current
    bucket means that mesh has moved on to new data, so the current
    timestep must be finalized and a new one started before writing it."""

    def __init__(self, cache_root):
        self.cache_root           = cache_root
        self.project_dir          = None
        self.timestep_index       = 0
        self.have_data            = False   # True once anything has been written
        self.meshes_in_current    = set()   # mesh names already written this timestep
        self.last_bounds_bytes    = None
        self.bounds_written       = False

    def _timestep_dir(self):
        d = os.path.join(self.project_dir, 'timesteps', f'{self.timestep_index:06d}')
        os.makedirs(d, exist_ok=True)
        return d

    def on_project(self, payload):
        name_len = struct.unpack('<i', payload[:4])[0]
        name = payload[4:4 + name_len].decode('utf-8')
        self.project_dir = os.path.join(self.cache_root, name)
        os.makedirs(os.path.join(self.project_dir, 'colormaps'), exist_ok=True)
        os.makedirs(os.path.join(self.project_dir, 'timesteps'), exist_ok=True)
        with open(os.path.join(self.project_dir, 'meta.json'), 'w') as f:
            json.dump({'project': name, 'created': time.strftime('%Y-%m-%dT%H:%M:%S')}, f, indent=2)
        self.timestep_index    = 0
        self.have_data         = False
        self.meshes_in_current = set()
        self.last_bounds_bytes = None
        self.bounds_written    = False
        print(f"[DataManager] recording project '{name}' -> {self.project_dir}", flush=True)

    def on_colormap(self, payload):
        if self.project_dir is None:
            return
        name_len = struct.unpack('<i', payload[:4])[0]
        name = payload[4:4 + name_len].decode('utf-8')
        with open(os.path.join(self.project_dir, 'colormaps', f'{name}.bin'), 'wb') as f:
            f.write(payload)

    def on_bounds(self, payload):
        if self.project_dir is None:
            return
        self.last_bounds_bytes = payload
        with open(os.path.join(self._timestep_dir(), 'bounds.bin'), 'wb') as f:
            f.write(payload)
        self.bounds_written = True

    def on_time(self, payload):
        """TIME does NOT drive advancement (see class docstring -- it may
        not fire every frame at all, depending on pipeline topology).  Just
        write it into whichever timestep is currently open."""
        if self.project_dir is None:
            return
        with open(os.path.join(self._timestep_dir(), 'time.bin'), 'wb') as f:
            f.write(payload)

    def on_mesh(self, payload):
        """A mesh name reappearing in the current bucket means that mesh has
        moved on to new data -- finalize the current timestep and start a
        new one before writing it."""
        if self.project_dir is None:
            return
        color_name_len, mesh_name_len = struct.unpack('<ii', payload[16:24])
        mesh_name = payload[-mesh_name_len:].decode('utf-8') if mesh_name_len > 0 else 'ParaViewMesh'

        if mesh_name in self.meshes_in_current:
            self._finalize_current_timestep()
            self.timestep_index += 1
            self.meshes_in_current = set()
            self.bounds_written    = False

        with open(os.path.join(self._timestep_dir(), f'{mesh_name}.bin'), 'wb') as f:
            f.write(payload)
        self.meshes_in_current.add(mesh_name)
        self.have_data = True

    def _finalize_current_timestep(self):
        """Copy forward the last-known bounds if none arrived fresh this
        timestep, so every timestep dir is self-contained for replay."""
        if self.project_dir is None or not self.have_data:
            return
        if not self.bounds_written and self.last_bounds_bytes is not None:
            with open(os.path.join(self._timestep_dir(), 'bounds.bin'), 'wb') as f:
                f.write(self.last_bounds_bytes)
            self.bounds_written = True


def relay_source_connection(source_sock, source_addr, downstream_host, downstream_port, cache_root):
    """Relay one source (ParaView) connection to a single downstream receiver
    until either side closes, optionally recording every message.  Runs on
    its own thread."""
    print(f"[DataManager] source connected from {source_addr}", flush=True)

    downstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    downstream.settimeout(5.0)
    try:
        downstream.connect((downstream_host, downstream_port))
    except OSError as exc:
        print(f"[DataManager] could not reach downstream {downstream_host}:{downstream_port} -- {exc}", flush=True)
        source_sock.close()
        return
    downstream.settimeout(None)
    downstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[DataManager] relaying {source_addr} -> {downstream_host}:{downstream_port}", flush=True)

    recorder = Recorder(cache_root) if cache_root else None
    msg_count = 0
    try:
        while True:
            header = recv_exact(source_sock, HEADER_SIZE)
            if header is None:
                print(f"[DataManager] source {source_addr} closed connection", flush=True)
                break

            payload_len, msg_type = struct.unpack('<ii', header)
            payload = b''
            if payload_len > 0:
                payload = recv_exact(source_sock, payload_len)
                if payload is None:
                    print(f"[DataManager] source {source_addr} closed mid-message", flush=True)
                    break

            downstream.sendall(header + payload)
            msg_count += 1

            if recorder is not None:
                if msg_type == MSG_TYPE_PROJECT:
                    recorder.on_project(payload)
                elif msg_type == MSG_TYPE_COLORMAP:
                    recorder.on_colormap(payload)
                elif msg_type == MSG_TYPE_BOUNDS:
                    recorder.on_bounds(payload)
                elif msg_type == MSG_TYPE_TIME:
                    recorder.on_time(payload)
                elif msg_type == MSG_TYPE_MESH:
                    recorder.on_mesh(payload)
                # MSG_TYPE_UPDATE is intentionally NOT a recorder trigger --
                # each mesh sender fires its own UPDATE for live-sync
                # backpressure, but TIME (once per real frame, upstream of
                # every mesh sender) is what actually marks a new timestep.

            if msg_type == MSG_TYPE_UPDATE:
                ack = recv_exact(downstream, 4)
                if ack is None:
                    print("[DataManager] downstream closed while waiting for UPDATE ack", flush=True)
                    break
                source_sock.sendall(ack)
    except (OSError, ConnectionError) as exc:
        print(f"[DataManager] relay error for {source_addr}: {exc}", flush=True)
    finally:
        if recorder is not None:
            recorder._finalize_current_timestep()
        downstream.close()
        source_sock.close()
        print(f"[DataManager] session with {source_addr} ended after {msg_count} message(s)", flush=True)


def main():
    parser = argparse.ArgumentParser(description="PVLink DataManager -- Stage 1 transparent relay + recorder")
    parser.add_argument('--listen-host', default='0.0.0.0',
                         help='address to listen on for the source (ParaView) connection')
    parser.add_argument('--listen-port', type=int, default=9000,
                         help='port to listen on for the source (ParaView) connection')
    parser.add_argument('--downstream-host', default='127.0.0.1',
                         help='host of the downstream receiver (Unity/Unreal)')
    parser.add_argument('--downstream-port', type=int, required=True,
                         help='port of the downstream receiver (Unity/Unreal)')
    parser.add_argument('--cache-dir', default=None,
                         help='if given, record every project/timestep to this directory')
    args = parser.parse_args()

    if args.cache_dir:
        os.makedirs(args.cache_dir, exist_ok=True)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_host, args.listen_port))
    listener.listen(5)
    print(f"[DataManager] listening on {args.listen_host}:{args.listen_port}, "
          f"relaying to {args.downstream_host}:{args.downstream_port}"
          + (f", recording to {args.cache_dir}" if args.cache_dir else ""), flush=True)

    try:
        while True:
            source_sock, source_addr = listener.accept()
            source_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(
                target=relay_source_connection,
                args=(source_sock, source_addr, args.downstream_host, args.downstream_port, args.cache_dir),
                daemon=True,
            ).start()
    except KeyboardInterrupt:
        print("[DataManager] shutting down", flush=True)
    finally:
        listener.close()


if __name__ == '__main__':
    main()
