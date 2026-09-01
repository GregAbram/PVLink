"""
DataManager -- clients dial in; relays a live ParaView source, or replays a
cached project straight from disk with no ParaView involved at all.
=========================================================================
Unity/Unreal clients connect INTO the DataManager (on --client-port) rather
than the DataManager dialing out to them -- clients can join and leave
freely, with no restart and no advance knowledge of who's connecting. A
newly-joined client is caught up to current state (project name, colormaps,
bounds, time, visibility, and every mesh's latest geometry) before it's
added to the live broadcast set, so it doesn't just see a blank scene until
the next update (which, for a paused/static viz, might never come).

Two mutually-exclusive run modes, chosen by whether --project is given:

  Live mode (no --project): --listen-port accepts a ParaView source
  connection exactly as before; every message is broadcast to every
  connected client. If --cache-dir is given, everything is ALSO written to
  disk as it passes through (see Recorder below).

  Replay mode (--project NAME): no --listen-port is opened at all -- the
  DataManager reads a previously-recorded project from
  <cache-dir>/NAME and streams it through the exact same client-broadcast
  pipeline live ParaView traffic would use, looping indefinitely by
  default. Pacing is ack-driven: after each timestep's UPDATE is
  broadcast, the DataManager waits for every live client to ack before
  moving on (the same backpressure PVLink.py's mark_mesh_sent() already
  uses against a live source) -- --replay-delay is only a floor on top of
  that, not the primary pacer, so a slow client is what sets the pace, not
  a blind guess.

On-disk cache layout (written by Recorder in live mode, read by replay
mode) -- every file is the exact payload bytes already built for the
socket, so recording is "write what I already have" and replay is "read it
back and resend it," with no re-serialization on either end:

    <cache-dir>/<project>/
      meta.json
      colormaps/
        <variable-name>.bin      # raw COLORMAP payload, overwritten in place
                                  # (colormap is timestep-independent -- sent
                                  # once, up front, applies to the whole replay)
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

Usage:
    # Live: relay ParaView, clients dial into 9010, also record to disk
    python datamanager.py --listen-port 9000 --client-port 9010 \
                           --cache-dir ./recordings

    # Replay: no ParaView needed, serve a previously-recorded project
    python datamanager.py --client-port 9010 \
                           --cache-dir ./recordings --project Sphere
"""

import argparse
import glob
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

# Off by default -- gates only the per-timestep replay "flipped" line, which
# under a fast/no-delay replay loop would otherwise print once per frame.
# Enable with --verbose. All other prints here are one-time/state-change
# events (connect, disconnect, errors) and stay unconditional.
VERBOSE = False


def _log(msg):
    if VERBOSE:
        print(msg, flush=True)


def recv_exact(sock, n):
    """Read exactly n bytes, or return None if the peer closed first."""
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def _frame(msg_type, payload):
    return struct.pack('<ii', len(payload), msg_type) + payload


def _read_name(payload, offset=0):
    """Read a length-prefixed UTF-8 name: int32 name_len followed by that
    many bytes, starting at offset. Shared by PROJECT/COLORMAP/VISIBILITY,
    which all lead with this same shape."""
    name_len = struct.unpack('<i', payload[offset:offset + 4])[0]
    return payload[offset + 4:offset + 4 + name_len].decode('utf-8')


def _mesh_name(payload):
    """MESH payload name extraction -- the name's length lives at a fixed
    offset (after other header fields) and the name itself is the LAST
    bytes of the payload, not immediately after its length."""
    _, mesh_name_len = struct.unpack('<ii', payload[16:24])
    return payload[-mesh_name_len:].decode('utf-8') if mesh_name_len > 0 else 'ParaViewMesh'


class Recorder:
    """Tracks the current project/timestep and writes payloads to disk.
    A no-op (every method just returns) until a MSG_TYPE_PROJECT message
    tells it where to write.  Live mode only -- replay mode reads this same
    layout back but never records (there's nothing new to record).

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
        name = _read_name(payload)
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
        name = _read_name(payload)
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
        mesh_name = _mesh_name(payload)

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


class ClientRegistry:
    """Thread-safe set of live downstream client sockets that dial INTO the
    DataManager. Used by both the live ParaView relay and disk replay to
    broadcast to whichever clients happen to be connected right now."""

    def __init__(self):
        self._lock = threading.Lock()
        self._clients = {}   # addr -> socket

    def add(self, addr, sock):
        with self._lock:
            self._clients[addr] = sock
            count = len(self._clients)
        print(f"[DataManager] client {addr} live ({count} total)", flush=True)

    def snapshot(self):
        with self._lock:
            return list(self._clients.items())

    def _drop(self, addr, reason):
        with self._lock:
            sock = self._clients.pop(addr, None)
        if sock is not None:
            print(f"[DataManager] client {addr} dropped -- {reason}", flush=True)
            try:
                sock.close()
            except OSError:
                pass

    def broadcast(self, message):
        """Send raw framed bytes to every live client; drop any that fail."""
        for addr, sock in self.snapshot():
            try:
                sock.sendall(message)
            except OSError as exc:
                self._drop(addr, str(exc))

    def collect_update_acks(self):
        """After an UPDATE broadcast, wait for a 4-byte ack from every
        still-live client (dropping any that time out or disconnect) and
        return the aggregated status: 0 if all OK, else the worst non-zero
        status seen."""
        worst_status = 0
        for addr, sock in self.snapshot():
            sock.settimeout(30.0)
            ack = recv_exact(sock, 4)
            sock.settimeout(None)
            if ack is None:
                self._drop(addr, "closed/timed out waiting for UPDATE ack")
                continue
            status = struct.unpack('<i', ack)[0]
            if status != 0:
                print(f"[DataManager] client {addr} returned non-zero UPDATE ack status {status}", flush=True)
                worst_status = status
        return worst_status


class LatestState:
    """In-memory cache of the most recently seen payload per category, used
    to catch a newly-joined client up to current state without waiting for
    the next live update (which, for a paused/static viz, might never come).
    Always active, independent of --cache-dir/Recorder."""

    def __init__(self):
        self.project_payload     = None
        self.colormap_payloads   = {}   # name -> bytes
        self.bounds_payload      = None
        self.time_payload        = None
        self.visibility_payloads = {}   # name -> bytes
        self.mesh_payloads       = {}   # name -> bytes

    def update(self, msg_type, payload):
        if msg_type == MSG_TYPE_PROJECT:
            # New project -- old cached state no longer applies.
            self.project_payload = payload
            self.colormap_payloads   = {}
            self.bounds_payload      = None
            self.time_payload        = None
            self.visibility_payloads = {}
            self.mesh_payloads       = {}
        elif msg_type == MSG_TYPE_COLORMAP:
            self.colormap_payloads[_read_name(payload)] = payload
        elif msg_type == MSG_TYPE_BOUNDS:
            self.bounds_payload = payload
        elif msg_type == MSG_TYPE_TIME:
            self.time_payload = payload
        elif msg_type == MSG_TYPE_VISIBILITY:
            self.visibility_payloads[_read_name(payload)] = payload
        elif msg_type == MSG_TYPE_MESH:
            self.mesh_payloads[_mesh_name(payload)] = payload

    def has_any(self):
        return self.project_payload is not None or bool(self.mesh_payloads)

    def catch_up_frames(self):
        """Ordered (msg_type, payload) list to replay to a newly-joined
        client. Order doesn't affect correctness -- each type is applied
        independently on the receiver side -- this is just a sensible
        default ordering."""
        frames = []
        if self.project_payload is not None:
            frames.append((MSG_TYPE_PROJECT, self.project_payload))
        for payload in self.colormap_payloads.values():
            frames.append((MSG_TYPE_COLORMAP, payload))
        if self.bounds_payload is not None:
            frames.append((MSG_TYPE_BOUNDS, self.bounds_payload))
        if self.time_payload is not None:
            frames.append((MSG_TYPE_TIME, self.time_payload))
        for payload in self.visibility_payloads.values():
            frames.append((MSG_TYPE_VISIBILITY, payload))
        for payload in self.mesh_payloads.values():
            frames.append((MSG_TYPE_MESH, payload))
        return frames


def emit(msg_type, payload, registry, latest_state):
    """One message from whichever producer is currently active (live
    ParaView source or disk replay): remember it for catch-up, then
    broadcast it to every live client."""
    latest_state.update(msg_type, payload)
    registry.broadcast(_frame(msg_type, payload))


def handle_new_client(sock, addr, registry, latest_state):
    """Catch a newly-connected client up to current state, then add it to
    the live broadcast set. Runs on its own thread so multiple clients
    joining concurrently don't block each other. Never adds a client that
    fails mid-handshake, and never races a concurrent broadcast -- the
    client is only added to the registry after catch-up fully completes."""
    print(f"[DataManager] client connecting from {addr}", flush=True)
    try:
        frames = latest_state.catch_up_frames()
        for msg_type, payload in frames:
            sock.sendall(_frame(msg_type, payload))

        if latest_state.has_any():
            sock.sendall(_frame(MSG_TYPE_UPDATE, b''))
            sock.settimeout(10.0)
            ack = recv_exact(sock, 4)
            sock.settimeout(None)
            if ack is None:
                print(f"[DataManager] client {addr} disconnected during catch-up", flush=True)
                sock.close()
                return
            print(f"[DataManager] client {addr} caught up ({len(frames)} frame(s))", flush=True)

        registry.add(addr, sock)
    except OSError as exc:
        print(f"[DataManager] client {addr} catch-up failed: {exc}", flush=True)
        try:
            sock.close()
        except OSError:
            pass


def client_accept_loop(client_listener, registry, latest_state):
    while True:
        sock, addr = client_listener.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(
            target=handle_new_client,
            args=(sock, addr, registry, latest_state),
            daemon=True,
        ).start()


DISCOVERY_INTERVAL = 2.0   # seconds between broadcasts


def announce_loop(client_port, discovery_port, latest_state):
    """Broadcast this DataManager's presence (client_port + whatever project
    it's currently serving) on the LAN every DISCOVERY_INTERVAL seconds, so
    Unity/Unreal clients can discover it without a hardcoded address.

    Plain text, not the existing binary TCP framing -- this is tiny,
    infrequent, and benefits from being readable while debugging (e.g. with
    `nc -ul <discovery-port>`), unlike the main wire protocol which is
    high-frequency enough that a hand-rolled binary format earns its keep.

    A listener identifies this DataManager by (this packet's source IP,
    client_port) -- the source IP is directly visible to the receiver via
    recvfrom(), so this never needs to know or report its own IP itself."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    print(f"[DataManager] announcing on UDP broadcast port {discovery_port} "
          f"every {DISCOVERY_INTERVAL}s", flush=True)
    while True:
        project_name = ''
        if latest_state.project_payload is not None:
            project_name = _read_name(latest_state.project_payload)
        payload = (f"PVLINK-DISCOVERY 1\n"
                   f"client_port={client_port}\n"
                   f"project={project_name}\n").encode('utf-8')
        try:
            sock.sendto(payload, ('255.255.255.255', discovery_port))
        except OSError as exc:
            print(f"[DataManager] announce_loop: broadcast failed -- {exc}", flush=True)
        time.sleep(DISCOVERY_INTERVAL)


def relay_source_connection(source_sock, source_addr, registry, latest_state, cache_root):
    """Relay one live ParaView source connection to every currently-live
    client, optionally recording every message to disk. Runs on its own
    thread."""
    print(f"[DataManager] source connected from {source_addr}", flush=True)

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

            latest_state.update(msg_type, payload)
            registry.broadcast(header + payload)
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
                # backpressure; a repeated mesh name is the real timestep
                # boundary (see Recorder.on_mesh).

            if msg_type == MSG_TYPE_UPDATE:
                # Wait for an ack from every still-live client before acking
                # the source, so backpressure applies against the slowest
                # client, not just the first one to respond.
                status = registry.collect_update_acks()
                source_sock.sendall(struct.pack('<i', status))
    except (OSError, ConnectionError) as exc:
        print(f"[DataManager] relay error for {source_addr}: {exc}", flush=True)
    finally:
        if recorder is not None:
            recorder._finalize_current_timestep()
        source_sock.close()
        print(f"[DataManager] session with {source_addr} ended after {msg_count} message(s)", flush=True)


def replay_project(project_dir, project_name, registry, latest_state, delay, loop):
    """Stream a cached project from disk through the same registry/
    latest_state pipeline a live ParaView source would use -- no socket of
    its own. Pacing is ack-driven, not sleep-driven: after broadcasting an
    UPDATE, collect_update_acks() is what triggers moving to the next
    timestep, mirroring PVLink.py's own mark_mesh_sent() backpressure
    against a live source. --delay only applies as a floor on top of that
    (so replay isn't pointlessly fast when clients ack near-instantly, and
    so it isn't fully idle-spinning when the registry is empty and acks
    return immediately with nothing to wait for)."""
    name_bytes = project_name.encode('utf-8')
    emit(MSG_TYPE_PROJECT, struct.pack('<i', len(name_bytes)) + name_bytes, registry, latest_state)

    colormap_files = sorted(glob.glob(os.path.join(project_dir, 'colormaps', '*.bin')))
    for path in colormap_files:
        with open(path, 'rb') as f:
            payload = f.read()
        emit(MSG_TYPE_COLORMAP, payload, registry, latest_state)
        print(f"[DataManager] replay: sent colormap '{os.path.basename(path)}'", flush=True)

    timestep_dirs = sorted(
        d for d in glob.glob(os.path.join(project_dir, 'timesteps', '*'))
        if os.path.isdir(d)
    )
    if not timestep_dirs:
        print(f"[DataManager] replay: no timesteps found under {project_dir}", flush=True)
        return

    while True:
        for tdir in timestep_dirs:
            frame_start = time.monotonic()

            bounds_path = os.path.join(tdir, 'bounds.bin')
            if os.path.isfile(bounds_path):
                with open(bounds_path, 'rb') as f:
                    emit(MSG_TYPE_BOUNDS, f.read(), registry, latest_state)

            time_path = os.path.join(tdir, 'time.bin')
            if os.path.isfile(time_path):
                with open(time_path, 'rb') as f:
                    emit(MSG_TYPE_TIME, f.read(), registry, latest_state)

            mesh_files = sorted(
                p for p in glob.glob(os.path.join(tdir, '*.bin'))
                if os.path.basename(p) not in ('bounds.bin', 'time.bin')
            )
            for mpath in mesh_files:
                with open(mpath, 'rb') as f:
                    emit(MSG_TYPE_MESH, f.read(), registry, latest_state)

            registry.broadcast(_frame(MSG_TYPE_UPDATE, b''))
            registry.collect_update_acks()

            elapsed = time.monotonic() - frame_start
            if elapsed < delay:
                time.sleep(delay - elapsed)

            _log(f"[DataManager] replay: flipped {os.path.basename(tdir)} "
                 f"({len(mesh_files)} mesh(es))")

        if not loop:
            print("[DataManager] replay: finished (not looping)", flush=True)
            break


def main():
    parser = argparse.ArgumentParser(
        description="PVLink DataManager -- clients dial in; relays a live ParaView "
                    "source or replays a cached project")
    parser.add_argument('--listen-host', default='0.0.0.0',
                         help='address to listen on for the ParaView source connection (live mode only)')
    parser.add_argument('--listen-port', type=int, default=9000,
                         help='port to listen on for the ParaView source connection (live mode only)')
    parser.add_argument('--client-host', default='0.0.0.0',
                         help='address to listen on for Unity/Unreal clients to dial into')
    parser.add_argument('--client-port', type=int, default=9010,
                         help='port for Unity/Unreal clients to dial into')
    parser.add_argument('--cache-dir', default=None,
                         help='live mode: if given, record every project/timestep to this directory. '
                              'replay mode: required -- --project is resolved under this directory.')
    parser.add_argument('--project', default=None, metavar='NAME',
                         help='replay a cached project (<cache-dir>/NAME) instead of waiting for a '
                              'live ParaView source -- --listen-port is not opened in this mode')
    parser.add_argument('--replay-delay', type=float, default=0.5,
                         help='replay mode: minimum seconds between timesteps -- a floor on top of '
                              'ack-driven pacing, not the primary pacer (default: 0.5)')
    loop_group = parser.add_mutually_exclusive_group()
    loop_group.add_argument('--loop', dest='loop', action='store_true', default=True,
                             help='replay mode: loop the recording indefinitely (default)')
    loop_group.add_argument('--no-loop', dest='loop', action='store_false',
                             help='replay mode: play the recording once and stop')
    parser.add_argument('--verbose', '-v', action='store_true',
                         help='print the per-timestep replay "flipped" line (off by default)')
    parser.add_argument('--discovery-port', type=int, default=9011,
                         help='UDP port to broadcast this DataManager\'s presence on (default: 9011)')
    parser.add_argument('--no-discovery', dest='discovery', action='store_false', default=True,
                         help='disable the UDP broadcast announcement')
    args = parser.parse_args()

    global VERBOSE
    VERBOSE = args.verbose

    project_dir = None
    if args.project:
        if not args.cache_dir:
            parser.error('--project requires --cache-dir (the project is read from <cache-dir>/<project>)')
        project_dir = os.path.join(args.cache_dir, args.project)
        if not os.path.isdir(project_dir):
            parser.error(f"--project '{args.project}' not found under --cache-dir '{args.cache_dir}' "
                         f"(expected {project_dir})")

    registry = ClientRegistry()
    latest_state = LatestState()

    client_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    client_listener.bind((args.client_host, args.client_port))
    client_listener.listen(5)
    threading.Thread(
        target=client_accept_loop,
        args=(client_listener, registry, latest_state),
        daemon=True,
    ).start()

    if args.discovery:
        threading.Thread(
            target=announce_loop,
            args=(args.client_port, args.discovery_port, latest_state),
            daemon=True,
        ).start()

    if project_dir is not None:
        print(f"[DataManager] replay mode: '{args.project}' from {project_dir}, "
              f"clients dial into {args.client_host}:{args.client_port}"
              + (", looping" if args.loop else ", single pass"), flush=True)
        try:
            replay_project(project_dir, args.project, registry, latest_state, args.replay_delay, args.loop)
        except KeyboardInterrupt:
            print("[DataManager] shutting down", flush=True)
        return

    if args.cache_dir:
        os.makedirs(args.cache_dir, exist_ok=True)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_host, args.listen_port))
    listener.listen(5)
    print(f"[DataManager] live mode: listening for ParaView on {args.listen_host}:{args.listen_port}, "
          f"clients dial into {args.client_host}:{args.client_port}"
          + (f", recording to {args.cache_dir}" if args.cache_dir else ""), flush=True)

    try:
        while True:
            source_sock, source_addr = listener.accept()
            source_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(
                target=relay_source_connection,
                args=(source_sock, source_addr, registry, latest_state, args.cache_dir),
                daemon=True,
            ).start()
    except KeyboardInterrupt:
        print("[DataManager] shutting down", flush=True)
    finally:
        listener.close()


if __name__ == '__main__':
    main()
