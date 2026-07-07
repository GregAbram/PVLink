"""
PVLink.py  —  ParaView plugin
=====================================
Sits in the ParaView pipeline as a transparent pass-through filter.
On every pipeline update it triangulates the input mesh, detects the
active color-mapping variable, and sends both to the PVLink receiver
over a TCP socket.

Installation
------------
  ParaView  →  Tools  →  Manage Plugins  →  Load New…  →  select this file
  Tick "Auto Load" to persist across sessions.

The filter then appears under  Filters → PVLink → PVLink Mesh Sender.

Global connection manager
-------------------------
The connection is managed by a session-global PVLinkConnectionManager
stored on paraview.servermanager.  Any ParaView Python code can reach it:

    from PVLink import get_pvlink_manager
    mgr = get_pvlink_manager()
    mgr.connect('192.168.1.10', 9001)   # optional explicit connect

Wire format (little-endian)
----------------------------------------------------------------------
  Outer envelope (no ack — one-way fire-and-forget):
    int32   payload_byte_count
    int32   message_type   (MSG_TYPE_MESH=10 | MSG_TYPE_COLORMAP=5 | MSG_TYPE_UPDATE=2)

  Mesh payload (MSG_TYPE_MESH):
    int32   num_points
    int32   num_triangles
    int32   scalar_location       (-1=none, 0=per-point, 1=per-cell)
    int32   has_normals           (0=none, 1=per-point normals follow positions)
    int32   color_name_len
    int32   mesh_name_len
    float32[num_points * 3]       vertex positions (x, y, z)
    float32[num_points * 3]       vertex normals   (nx, ny, nz) — only if has_normals==1
    int32[num_triangles * 3]      triangle vertex indices
    float32[num_scalars]          scalar values     (if scalar_location >= 0)
    float32[2]                    [scalar_min, scalar_max]
    utf8[color_name_len]          color-array name
    utf8[mesh_name_len]           mesh/actor name

  Colormap payload (MSG_TYPE_COLORMAP):
    int32               name_len
    utf8[name_len]      variable name
    float32             range_min
    float32             range_max
    int32               num_samples
    float32[N * 3]      R, G, B per sample

  Update payload (MSG_TYPE_UPDATE):
    (empty — 0 bytes)
"""

PVLINK_VERSION = "1.19"

import socket
import struct
import threading
import time
import numpy as np

import vtk
from vtkmodules.util.numpy_support import vtk_to_numpy
from vtkmodules.util.vtkAlgorithm import VTKPythonAlgorithmBase
from paraview.util.vtkAlgorithm import smdomain, smhint, smproperty, smproxy

# ---------------------------------------------------------------------------
# Message type constants — must match MeshTypes.h
# ---------------------------------------------------------------------------

MSG_TYPE_MESH       = 10   # MeshCmd::PVMesh      — geometry + scalars (all-in-one)
MSG_TYPE_COLORMAP   =  5   # MeshCmd::Colormap    — variable name + RGB table
MSG_TYPE_UPDATE     =  2   # MeshCmd::Update      — flip: make all buffered meshes active
MSG_TYPE_BOUNDS     = 11   # MeshCmd::Bounds      — computational domain AABB (6 floats)
MSG_TYPE_VISIBILITY = 12   # MeshCmd::Visibility  — show/hide a named mesh actor


# ---------------------------------------------------------------------------
# HelloPassThrough — minimal diagnostic filter
# Attach to any source, hit Apply, confirm data flows through unchanged.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# TCP connection  (low-level — only touched by _SenderThread)
# ---------------------------------------------------------------------------

class _Connection:
    """Raw socket wrapper.  The sender thread is the ONLY caller after start."""

    def __init__(self):
        self._sock = None

    def connect(self, host, port, timeout=1.0):
        self.disconnect()
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((host, port))          # raises OSError on failure
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(None)               # blocking — UPDATE ack uses timer/shutdown instead
        self._sock = s
        print(f"PVLink: socket local={s.getsockname()} remote={s.getpeername()}", flush=True)

    _ACK_TIMEOUT = 30.0   # seconds to wait for UPDATE ack

    _CHUNK = 65536   # 64 KB chunks — lets us log progress through the SSH tunnel

    def send_raw(self, msg_type, payload):
        """Send one message.  Only UPDATE waits for a 4-byte ack (backpressure);
        all other message types are fire-and-forget over the tunnel."""
        data = struct.pack('<ii', len(payload), msg_type) + payload
        total = len(data)
        sent  = 0
        print(f"PVLink: send_raw type={msg_type} len={len(payload)} total={total} ...", flush=True)
        while sent < total:
            chunk = data[sent:sent + self._CHUNK]
            n = self._sock.send(chunk)
            sent += n
            if total > self._CHUNK:
                print(f"PVLink: send_raw type={msg_type} {sent}/{total} bytes sent", flush=True)
        print(f"PVLink: send_raw type={msg_type} done", flush=True)

        if msg_type != MSG_TYPE_UPDATE:
            return   # no ack expected for non-UPDATE messages

        # Wait for UPDATE ack with a timer-based timeout (select/settimeout
        # are unreliable inside ParaView's Python environment).
        print(f"PVLink: send_raw UPDATE — waiting for ack ...", flush=True)
        _sock = self._sock
        def _timeout():
            print(f"PVLink: UPDATE ack timeout ({self._ACK_TIMEOUT}s) — shutting down socket", flush=True)
            try:
                _sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
        timer = threading.Timer(self._ACK_TIMEOUT, _timeout)
        timer.daemon = True
        timer.start()
        try:
            ack = self._recv_exact(4)
        finally:
            timer.cancel()

        print(f"PVLink: UPDATE ack={ack!r}", flush=True)
        if ack is None:
            raise OSError("connection closed before UPDATE ack")
        status = struct.unpack('<i', ack)[0]
        if status != 0:
            raise OSError(f"non-zero UPDATE ack status {status}")

    def _recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def disconnect(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None



# ---------------------------------------------------------------------------
# Direct sender  —  synchronous sends on the calling thread (no queue/thread)
# ---------------------------------------------------------------------------

class _DirectSender:
    """
    Sends TCP messages synchronously on the calling thread.

    No background thread or queue.  Every enqueue() / send_update() call
    blocks until the data (and for UPDATE, the ack) is fully sent.
    This is identical to how mock_client.py works — a simple linear
    send loop — which proved reliable through the SSH tunnel.

    State replay on reconnect: bounds and colormaps registered via
    register_state() are re-sent before the next message whenever the
    connection drops and is re-established.
    """

    _CONNECT_TIMEOUT = 1.0   # seconds for the TCP handshake

    def __init__(self):
        self._conn       = _Connection()
        self._state      = {}          # key -> (msg_type, payload)
        self._host       = "127.0.0.1"
        self._port       = 9001

    def start(self):  pass   # no-op; kept for API compatibility

    def stop(self):
        self._conn.disconnect()

    def set_address(self, host, port):
        host = str(host); port = int(port)
        if host != self._host or port != self._port:
            self._host = host
            self._port = port
            self._conn.disconnect()

    # ------------------------------------------------------------------
    # Send API
    # ------------------------------------------------------------------

    def enqueue(self, msg_type, payload):
        """Send one message synchronously.  Connects/reconnects if needed."""
        self._ensure_connected()
        if not self._conn._sock:
            print(f"PVLink: no connection — dropping type={msg_type}", flush=True)
            return
        try:
            self._conn.send_raw(msg_type, payload)
            if msg_type == MSG_TYPE_MESH:
                print(f"PVLink: sent mesh len={len(payload)}", flush=True)
            elif msg_type == MSG_TYPE_COLORMAP:
                print(f"PVLink: sent colormap len={len(payload)}", flush=True)
            elif msg_type == MSG_TYPE_BOUNDS:
                print(f"PVLink: sent bounds len={len(payload)}", flush=True)
            elif msg_type == MSG_TYPE_UPDATE:
                print(f"PVLink: sent UPDATE — Unity ack received", flush=True)
            else:
                print(f"PVLink: sent type={msg_type} len={len(payload)}", flush=True)
        except OSError as exc:
            print(f"PVLink: send error type={msg_type}: {exc}", flush=True)
            self._conn.disconnect()

    def register_state(self, key, msg_type, payload):
        """Store for replay and send immediately."""
        self._state[key] = (msg_type, payload)
        self.enqueue(msg_type, payload)

    def register_state_only(self, key, msg_type, payload):
        """Store for replay without sending now."""
        self._state[key] = (msg_type, payload)

    def send_update(self):
        self.enqueue(MSG_TYPE_UPDATE, b'')

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _ensure_connected(self):
        if self._conn._sock:
            return
        try:
            self._conn.connect(self._host, self._port, self._CONNECT_TIMEOUT)
            print(f"PVLink: connected to {self._host}:{self._port}", flush=True)
            self._replay_state()
        except OSError as exc:
            print(f"PVLink: connect failed — {exc}", flush=True)

    def _replay_state(self):
        if not self._state:
            return
        print(f"PVLink: replay_state: {len(self._state)} item(s)", flush=True)
        for key, (mt, pl) in list(self._state.items()):
            try:
                self._conn.send_raw(mt, pl)
                print(f"PVLink: replay_state: '{key}' sent OK", flush=True)
            except OSError as exc:
                print(f"PVLink: replay_state: '{key}' failed — {exc}", flush=True)
                self._conn.disconnect()
                return


# ---------------------------------------------------------------------------
# Colormap watcher
# ---------------------------------------------------------------------------

class _ColormapWatcher:
    """
    Observes vtkSMTransferFunctionProxy for each watched variable and enqueues
    MSG_TYPE_COLORMAP whenever the LUT is created or subsequently modified.
    All sends go through _SenderThread.enqueue() — never blocking.
    """

    def __init__(self, sender):
        self._sender        = sender
        self._observed      = {}   # var_name -> (lut_proxy, observer_tag)
        self._last_payloads = {}   # var_name -> last payload (for reconnect state)
        self._data_ranges   = {}   # var_name -> (d_min, d_max) from last mesh build

    def set_data_range(self, var_name, d_min, d_max):
        """Record the actual scalar data range so colormaps are sampled correctly."""
        self._data_ranges[var_name] = (d_min, d_max)

    def watch(self, var_name, lut=None):
        if not var_name:
            return
        try:
            if lut is None:
                import paraview.simple as pvs
                lut = pvs.GetColorTransferFunction(var_name)
            if lut is None:
                return

            self._send(var_name, lut)

            existing = self._observed.get(var_name)
            if existing is not None:
                old_lut, old_tag = existing
                if old_lut.SMProxy.GetGlobalIDAsString() == lut.SMProxy.GetGlobalIDAsString():
                    return
                try:
                    old_lut.SMProxy.RemoveObserver(old_tag)
                except Exception:
                    pass

            def _on_modified(obj, event, vn=var_name, watched_lut=lut):
                try:
                    self._send(vn, watched_lut)
                except Exception as exc:
                    print(f"PVLink: colormap observer error for '{vn}': {exc}")

            tag = lut.SMProxy.AddObserver('ModifiedEvent', _on_modified)
            self._observed[var_name] = (lut, tag)

        except Exception as exc:
            print(f"PVLink: could not watch colormap for '{var_name}': {exc}")

    def get_lut_range(self, var_name):
        entry = self._observed.get(var_name)
        if entry is None:
            return None
        lut, _ = entry
        try:
            pts = lut.RGBPoints
            if pts and len(pts) >= 8:
                return (float(pts[0]), float(pts[-4]))
        except Exception:
            pass
        return None

    def unwatch_all(self):
        for _vn, (lut, tag) in self._observed.items():
            try:
                lut.SMProxy.RemoveObserver(tag)
            except Exception:
                pass
        self._observed.clear()
        self._last_payloads.clear()

    def _send(self, var_name, lut):
        data_range = self._data_ranges.get(var_name)
        try:
            payload = _build_colormap_payload(var_name, lut, data_range=data_range)
        except Exception as exc:
            print(f"PVLink: colormap payload error for '{var_name}': {exc}")
            return
        self._last_payloads[var_name] = payload
        # Register as persistent state so reconnects replay it, then enqueue.
        self._sender.register_state(f'colormap:{var_name}', MSG_TYPE_COLORMAP, payload)
        print(f"PVLink: enqueued colormap for '{var_name}'")


# ---------------------------------------------------------------------------
# Session-global connection manager
# ---------------------------------------------------------------------------

class PVLinkConnectionManager:
    """
    Owns the sender thread and colormap watcher.
    Stored on paraview.servermanager so it survives filter re-creation.
    """

    def __init__(self):
        self._host    = "127.0.0.1"
        self._port    = 9001
        self._sender  = _DirectSender()
        self._watcher = _ColormapWatcher(self._sender)
        self._sender.start()
        self._meshes_pending      = False   # True when ≥1 mesh was sent this cycle
        self._update_timer        = None    # threading.Timer for debounced UPDATE
        self._render_window       = None    # vtkRenderWindow being observed (kept for cleanup)
        self._render_observer_tag = None    # observer handle (kept for cleanup)

    # --- address management --------------------------------------------------

    def set_address(self, host, port):
        host = str(host)
        port = int(port)
        if host == self._host and port == self._port:
            return
        self._host = host
        self._port = port
        self._sender.set_address(host, port)
        self._watcher.unwatch_all()

    @property
    def host(self):
        return self._host

    @property
    def port(self):
        return self._port

    # --- public API ----------------------------------------------------------

    def connect(self, host=None, port=None):
        if host is not None or port is not None:
            self.set_address(host or self._host, port or self._port)
        # Connection is managed by the sender thread; nothing to do here.

    def disconnect(self):
        self._sender.stop()
        self._watcher.unwatch_all()

    @property
    def is_connected(self):
        return self._sender._conn._sock is not None

    def send_message(self, msg_type, payload):
        """Enqueue a message.  Returns True immediately (fire-and-forget)."""
        self._sender.enqueue(msg_type, payload)
        return True

    def register_reconnect_message(self, key, msg_type, payload):
        self._sender.register_state_only(key, msg_type, payload)

    def send_update(self):
        self._sender.send_update()

    def mark_mesh_sent(self):
        """Signal that a mesh was sent this pipeline cycle.
        Resets a 50 ms debounce timer; when it fires (after all MeshSenders
        in the pipeline have run) a single UPDATE is sent to the receiver so
        all meshes flip atomically."""
        self._meshes_pending = True
        if self._update_timer is not None:
            self._update_timer.cancel()
        self._update_timer = threading.Timer(0.05, self._deferred_update)
        self._update_timer.daemon = True
        self._update_timer.start()

    def _deferred_update(self):
        """Called from the debounce timer thread — send UPDATE if still pending."""
        if not self._meshes_pending:
            return
        self._meshes_pending = False
        try:
            self._sender.send_update()
        except Exception as exc:
            print(f"PVLink: deferred UPDATE failed — {exc}", flush=True)

    def register_render_observer(self, view):
        """Attach a one-time StartEvent observer to the render window so that
        a single UPDATE is sent after the full pipeline has executed."""
        if self._render_observer_tag is not None:
            return
        try:
            rw = view.GetClientSideObject().GetRenderWindow()
            if rw is None:
                print("PVLink: no render window — UPDATE will be sent per-mesh (unsync'd)", flush=True)
                return
            mgr = self
            def _on_start_render(obj, event):
                if mgr._meshes_pending:
                    mgr._meshes_pending = False
                    try:
                        mgr._sender.send_update()
                    except Exception as exc:
                        print(f"PVLink: render-triggered UPDATE failed — {exc}", flush=True)
            self._render_observer_tag = rw.AddObserver('StartEvent', _on_start_render)
            self._render_window       = rw
            print("PVLink: render observer registered — UPDATE deferred to end of pipeline", flush=True)
        except Exception as exc:
            print(f"PVLink: could not register render observer — {exc}", flush=True)

    def watch_colormap(self, var_name, lut=None):
        self._watcher.watch(var_name, lut=lut)

    def set_data_range(self, var_name, d_min, d_max):
        self._watcher.set_data_range(var_name, d_min, d_max)

    def get_lut_range(self, var_name):
        return self._watcher.get_lut_range(var_name)


# Module-level fallback for use outside a full ParaView session (pvpython, tests).
_fallback_manager = None


def _find_upstream_domain_bounds(vtk_algo):
    """Walk the VTK pipeline upstream from *vtk_algo* to find the nearest
    PVLinkDomainBoundsFilter instance.  Returns the Python filter object or None."""
    visited = set()
    queue   = [vtk_algo]
    while queue:
        node = queue.pop()
        node_id = id(node)
        if node_id in visited:
            continue
        visited.add(node_id)
        if type(node).__name__ == 'PVLinkDomainBoundsFilter':
            return node
        for i in range(node.GetNumberOfInputConnections(0)):
            up = node.GetInputDataObject(0, i)
            if up is not None:
                # GetInputDataObject returns data, not algo — get the producer.
                prod = node.GetInputAlgorithm(0, i)
                if prod is not None:
                    queue.append(prod)
    return None


def _apply_upstream_address(vtk_algo, mgr):
    """Set *mgr*'s host/port from the nearest upstream DomainBounds filter."""
    try:
        db = _find_upstream_domain_bounds(vtk_algo)
        if db is not None:
            mgr.set_address(db._host, db._port)
    except Exception as exc:
        print(f"PVLink: could not inherit address from DomainBounds — {exc}")


def _display_color_array(display):
    """Return the active color-by array name from a display proxy, or ''."""
    try:
        arr = display.ColorArrayName
        return arr[1] if arr and len(arr) > 1 else ''
    except Exception:
        return ''


def _display_lut(display):
    """Return the LUT proxy actually used by this display representation, or None.

    Uses display.LookupTable so per-representation colormaps ('Use Separate
    Color Map') are returned correctly, unlike GetColorTransferFunction which
    always returns the global shared LUT for the variable name.
    """
    try:
        return display.LookupTable
    except Exception:
        return None


def get_pvlink_manager():
    """
    Return the session-global PVLinkConnectionManager, creating it on first call.

    The instance is stored on paraview.servermanager so it persists across
    filter re-creations and is reachable from any ParaView Python code:

        from PVLink import get_pvlink_manager
        mgr = get_pvlink_manager()
        mgr.connect('192.168.1.10', 9001)
        mgr.send_message(MSG_TYPE_UPDATE, b'')
    """
    global _fallback_manager
    try:
        import paraview.servermanager as sm
        if not hasattr(sm, '_pvlink_manager') or type(sm._pvlink_manager).__name__ != 'PVLinkConnectionManager':
            # Stop any stale sender thread from a previous manager before replacing it.
            old = getattr(sm, '_pvlink_manager', None)
            if old is not None and hasattr(old, '_sender'):
                try:
                    old._sender.stop()
                except Exception:
                    pass
            print(f"PVLink: creating new connection manager (version {PVLINK_VERSION})", flush=True)
            sm._pvlink_manager = PVLinkConnectionManager()
        return sm._pvlink_manager
    except ImportError:
        if _fallback_manager is None:
            _fallback_manager = PVLinkConnectionManager()
        return _fallback_manager


# ---------------------------------------------------------------------------
# Payload builders
# ---------------------------------------------------------------------------

def _build_colormap_payload(var_name, lut, data_range=None):
    """Build the COLORMAP payload.

    *data_range* — (d_min, d_max) of the actual scalar data in the mesh.
    When supplied, the 256 texture samples are computed at positions that
    uniformly cover [d_min, d_max] using the LUT for colour lookup.  This
    aligns the texture with the vertex R bytes (which are also normalised to
    the data range), so a LUT range change only requires a new texture — the
    mesh vertex data stays valid and does not need to be resent.

    Without *data_range* (first-call fallback) the samples cover the LUT's
    own range as before.
    """
    rgb_pts = lut.RGBPoints
    if not rgb_pts or len(rgb_pts) < 8:
        raise ValueError(f"LUT for '{var_name}' has no RGB points")

    pts      = np.array(rgb_pts, dtype=np.float64).reshape(-1, 4)
    x_vals   = pts[:, 0]
    lut_min  = float(x_vals[0])
    lut_max  = float(x_vals[-1])
    if lut_max == lut_min:
        lut_max = lut_min + 1.0

    # Normalise the LUT control-point x-coords to [0, 1] within the LUT range.
    x_norm = (x_vals - lut_min) / (lut_max - lut_min)

    N = 256
    if data_range is not None and (data_range[1] - data_range[0]) > 1e-10:
        # Sample the LUT at 256 positions that uniformly cover the DATA range.
        # The texture index (vertex R / 255) then directly addresses the correct
        # colour without any re-normalisation in the shader.
        d_min, d_max = float(data_range[0]), float(data_range[1])
        t_physical = np.linspace(d_min, d_max, N)
        # Map data values into LUT [0,1] coordinates; clamp outside LUT range.
        t_lut = np.clip((t_physical - lut_min) / (lut_max - lut_min), 0.0, 1.0)
        r_samp = np.interp(t_lut, x_norm, pts[:, 1]).astype(np.float32)
        g_samp = np.interp(t_lut, x_norm, pts[:, 2]).astype(np.float32)
        b_samp = np.interp(t_lut, x_norm, pts[:, 3]).astype(np.float32)
        range_min, range_max = d_min, d_max
    else:
        # Fallback: sample uniformly across the LUT's own range.
        t = np.linspace(0.0, 1.0, N)
        r_samp = np.interp(t, x_norm, pts[:, 1]).astype(np.float32)
        g_samp = np.interp(t, x_norm, pts[:, 2]).astype(np.float32)
        b_samp = np.interp(t, x_norm, pts[:, 3]).astype(np.float32)
        range_min, range_max = lut_min, lut_max

    name_bytes      = var_name.encode('utf-8')
    rgb_interleaved = np.column_stack([r_samp, g_samp, b_samp])

    buf  = struct.pack('<i', len(name_bytes)) + name_bytes
    buf += struct.pack('<ffi', range_min, range_max, N)
    buf += rgb_interleaved.astype(np.float32).tobytes()
    return buf


def _build_mesh_payload(data, color_name, is_point_data, mesh_name,
                        lut_range=None, compute_normals=True):
    """Build the binary mesh payload for the PVLink receiver.

    *lut_range* — optional (min, max) from the ParaView LUT for this variable.
    Used as the scalar range when the mesh-local range is degenerate (e.g. a
    contour surface where every vertex has exactly the isovalue, so the local
    range is (V, V)).  Without this, the receiver would always normalise to 0
    and the surface would be stuck at the first colour in the map.

    *compute_normals* — if True and the mesh has no point normals, run
    vtkPolyDataNormals (SplittingOff) before packing.  If False and no normals
    are present, has_normals=0 is sent and the receiver computes its own.
    """
    from vtkmodules.vtkFiltersCore import vtkTriangleFilter
    from vtkmodules.vtkFiltersGeometry import vtkDataSetSurfaceFilter
    import io

    surf = vtkDataSetSurfaceFilter()
    surf.SetInputData(data)
    surf.Update()

    tri = vtkTriangleFilter()
    tri.SetInputConnection(surf.GetOutputPort())
    tri.Update()
    poly = tri.GetOutput()

    if poly.GetNumberOfPoints() == 0:
        raise ValueError("mesh has no points after triangulation")

    # ── Normals ────────────────────────────────────────────────────────
    # Use existing point normals if present; otherwise optionally compute them.
    # SplittingOff preserves point count so per-point scalars stay aligned.
    normals_arr = poly.GetPointData().GetNormals()
    if normals_arr is None and compute_normals:
        from vtkmodules.vtkFiltersCore import vtkPolyDataNormals
        pn = vtkPolyDataNormals()
        pn.SetInputData(poly)
        pn.ComputePointNormalsOn()
        pn.ComputeCellNormalsOff()
        pn.SplittingOff()
        pn.Update()
        poly = pn.GetOutput()
        normals_arr = poly.GetPointData().GetNormals()

    has_normals = 1 if normals_arr is not None else 0

    # ── Geometry ───────────────────────────────────────────────────────
    pts = vtk_to_numpy(poly.GetPoints().GetData()).astype(np.float32)
    num_points = len(pts)

    raw = vtk_to_numpy(poly.GetPolys().GetData())
    tris = np.empty((0, 3), dtype=np.int32) if len(raw) == 0 \
           else raw.reshape(-1, 4)[:, 1:].astype(np.int32)
    num_tris = len(tris)

    # ── Scalars ────────────────────────────────────────────────────────
    scalar_location = -1
    scalars         = None
    scalar_range    = (0.0, 1.0)

    if color_name:
        arr = (poly.GetPointData() if is_point_data else poly.GetCellData()).GetArray(color_name)
        if arr is not None:
            s = vtk_to_numpy(arr).astype(np.float32)
            if s.ndim > 1:
                s = np.linalg.norm(s, axis=1).astype(np.float32)
            scalars         = s
            r               = arr.GetRange()
            if r[0] == r[1] and lut_range is not None:
                scalar_range = (float(lut_range[0]), float(lut_range[1]))
            else:
                scalar_range = (float(r[0]), float(r[1]))
            scalar_location = 0 if is_point_data else 1

    color_name_bytes = color_name.encode('utf-8') if color_name else b''
    mesh_name_bytes  = (mesh_name or 'ParaViewMesh').encode('utf-8')

    buf = io.BytesIO()
    buf.write(struct.pack('<iiiiii',
                          num_points, num_tris, scalar_location,
                          has_normals,
                          len(color_name_bytes), len(mesh_name_bytes)))
    buf.write(pts.tobytes())
    if has_normals:
        buf.write(vtk_to_numpy(normals_arr).astype(np.float32).tobytes())
    buf.write(tris.tobytes())
    if scalars is not None:
        buf.write(scalars.tobytes())
        buf.write(struct.pack('<ff', *scalar_range))
    buf.write(color_name_bytes)
    buf.write(mesh_name_bytes)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# ParaView filter
# ---------------------------------------------------------------------------

@smproxy.filter(name="PVLinkMeshSender", label="PVLink Mesh Sender")
@smhint.xml('<ShowInMenu category="PVLink"/>')
@smproperty.input(name="Input", port_index=0)
@smdomain.datatype(dataTypes=["vtkPolyData", "vtkUnstructuredGrid"])
class PVLinkMeshSenderFilter(VTKPythonAlgorithmBase):
    """
    Pass-through filter that sends the input mesh to the PVLink receiver on
    every update.  Output is identical to input so downstream filters are
    unaffected.

    Connection settings (Host / TCPPort) are owned by PVLinkDomainBoundsFilter.
    Place a PVLinkDomainBoundsFilter upstream of every PVLink Mesh Sender in
    the pipeline; the session-level connection manager propagates the address
    automatically to all senders.
    """

    def __init__(self):
        super().__init__(nInputPorts=1, nOutputPorts=1)
        self._color_arr       = ""
        self._mesh_name       = "ParaViewMesh"
        self._compute_normals = True

    # ------------------------------------------------------------------
    # Properties
    # ------------------------------------------------------------------

    @smproperty.stringvector(name="MeshName", default_values="ParaViewMesh")
    @smdomain.xml("""
        <Documentation>
            Actor/GameObject name. Use different names to stream multiple meshes.
        </Documentation>
    """)
    def SetMeshName(self, val):
        self._mesh_name = val
        self.Modified()

    def GetMeshName(self):
        return self._mesh_name

    @smproperty.stringvector(name="ColorArrayName", default_values="")
    @smdomain.xml("""
        <ArrayListDomain name="array_list" none_string="Auto-detect"
                         attribute_type="Scalars">
            <RequiredProperties>
                <Property name="Input" function="Input"/>
            </RequiredProperties>
        </ArrayListDomain>
    """)
    def SetColorArrayName(self, val):
        self._color_arr = val
        self.Modified()

    def GetColorArrayName(self):
        return self._color_arr

    @smproperty.intvector(name="ComputeNormals", default_values=1, number_of_elements=1)
    @smdomain.xml('<BooleanDomain name="bool"/>')
    def SetComputeNormals(self, val):
        """Compute smooth point normals via vtkPolyDataNormals if the mesh
        has none.  Disable to save CPU time if the receiver computes its own,
        or if your pipeline already applies a normals filter upstream."""
        self._compute_normals = bool(val)
        self.Modified()

    def GetComputeNormals(self):
        return int(self._compute_normals)

    # ------------------------------------------------------------------
    # Pipeline
    # ------------------------------------------------------------------

    def RequestDataObject(self, request, inInfo, outInfo):
        # Read input type from pipeline information — self.GetInputData() returns
        # None here because upstream hasn't executed yet.
        inp = inInfo[0].GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if inp is None:
            return 1
        out = outInfo.GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        return 1

    def RequestData(self, request, inInfo, outInfo):
        print("Mesh Sender RequestData called");
        inp = self.GetInputData(inInfo, 0, 0)
        if inp is None:
            return 1

        # Cache input so _on_display_modified and LUT-change callbacks can
        # rebuild the mesh without re-triggering the pipeline (UpdatePipeline is
        # re-entrant-blocked inside VTK observer callbacks).
        self._last_input_data = inp

        # ShallowCopy into the pre-allocated output object.  Direct reference
        # assignment broke when FillOutputPortInformation opened the type to
        # vtkDataObject — VTK no longer guarantees a live output object exists.
        out = self.GetOutputData(outInfo, 0)
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        out.ShallowCopy(inp)

        mgr = get_pvlink_manager()

        # Inherit host/port from the nearest upstream DomainBounds filter so
        # the user only has to configure the address in one place.
        _apply_upstream_address(self, mgr)

        # Prefer the display-representation's color array / LUT (set by
        # _ensure_visibility_observer) because it correctly reflects what the
        # user has chosen in the Properties panel, including solid-color (empty
        # string) and per-representation colormaps ('Use Separate Color Map').
        # Fall back to VTK-active-scalars detection only on first execution
        # before the observer has run.
        # Register the display observer first so _display_color_arr/_display_lut
        # are populated before we resolve the color array below.
        self._ensure_visibility_observer(mgr)

        display_arr = getattr(self, '_display_color_arr', None)
        display_lut = getattr(self, '_display_lut', None)
        if display_arr is not None:
            color_name = display_arr
            is_point_data = True
            if color_name:
                arr = inp.GetPointData().GetArray(color_name)
                if arr is None:
                    arr = inp.GetCellData().GetArray(color_name)
                    is_point_data = False
        else:
            color_name, is_point_data = self._resolve_color_array(inp)
            display_lut = None

        if color_name:
            # Register the data range BEFORE watch_colormap so _send samples the
            # LUT at data-range positions, aligning the texture with vertex R bytes.
            # A subsequent LUT range change in ParaView then only requires a new
            # colormap texture — the mesh vertex data stays valid without resending.
            arr_vtk = (inp.GetPointData() if is_point_data
                       else inp.GetCellData()).GetArray(color_name)
            if arr_vtk is not None:
                dr = arr_vtk.GetRange()
                if dr[1] > dr[0]:
                    mgr.set_data_range(color_name, dr[0], dr[1])
            mgr.watch_colormap(color_name, lut=display_lut)

        # Fetch the LUT range AFTER watch_colormap so the watcher entry exists.
        # Passed to _build_mesh_payload as a fallback for degenerate local ranges
        # (e.g. isosurface where every vertex == isovalue → local range is (V,V)).
        lut_range = mgr.get_lut_range(color_name) if color_name else None

        try:
            payload = _build_mesh_payload(inp, color_name, is_point_data,
                                          self._mesh_name, lut_range=lut_range,
                                          compute_normals=self._compute_normals)
        except Exception as exc:
            print(f"PVLink:mesh serialization error — {exc}")
            return 1

        if mgr.send_message(MSG_TYPE_MESH, payload):
            loc = "point" if is_point_data else "cell"
            scalar_desc = f"scalar='{color_name}' ({loc})" if color_name else "no scalar"
            print(f"PVLink:sent '{self._mesh_name}' — "
                  f"{inp.GetNumberOfPoints()} pts, "
                  f"{inp.GetNumberOfCells()} cells, {scalar_desc}")
            mgr.mark_mesh_sent()   # deferred UPDATE fires via render observer

        return 1

    def _ensure_visibility_observer(self, mgr):
        """Register a one-time observer on this filter's display properties so that
        toggling the eye icon in the Pipeline Browser sends MSG_TYPE_VISIBILITY to UE."""
        if getattr(self, '_vis_observer_registered', False):
            return
        # Mark registered BEFORE the pvs calls so that any re-entrant RequestData
        # triggered by those calls hits the guard above and returns immediately.
        self._vis_observer_registered = True
        try:
            import paraview.simple as pvs
            import paraview.servermanager as sm
            view = pvs.GetActiveView()
            if view is None:
                # GetActiveView() returns None on remote pvserver (TACC).
                # Fall back to the first available render view.
                views = pvs.GetViews()
                view = views[0] if views else None
            if view is None:
                print(f"PVLink: no view found for '{self._mesh_name}' — color-by observer skipped", flush=True)
                return
            mgr.register_render_observer(view)
            # Find the proxy for *this* filter by comparing the underlying VTK
            # client-side object against self.  GetGlobalIDAsString() is a proxy
            # method and is not available on the raw VTK algorithm (self).
            src = None
            for proxy in pvs.GetSources().values():
                try:
                    if proxy.SMProxy.GetClientSideObject() is self:
                        src = proxy
                        break
                except Exception:
                    pass
            if src is None:
                print(f"PVLink: could not find own proxy for '{self._mesh_name}' — skipping display observer")
                return
            display = pvs.GetDisplayProperties(src, view)
            if display is None:
                return

            mesh_name      = self._mesh_name
            last_vis       = [bool(display.Visibility)]
            last_color_arr = [_display_color_array(display)]

            # Seed the display-derived color info so RequestData can use it
            # immediately on the first execution without waiting for a change.
            self._display_color_arr = last_color_arr[0]
            self._display_lut       = _display_lut(display)

            def _on_display_modified(obj, event):
                try:
                    # --- visibility change ---
                    visible = bool(display.Visibility)
                    if visible != last_vis[0]:
                        last_vis[0] = visible
                        name_bytes = mesh_name.encode('utf-8')
                        payload = (struct.pack('<i', len(name_bytes))
                                   + name_bytes
                                   + struct.pack('<i', int(visible)))
                        m = get_pvlink_manager()
                        m.send_message(MSG_TYPE_VISIBILITY, payload)
                        state = "shown" if visible else "hidden"
                        print(f"PVLink:'{mesh_name}' {state}")

                    # --- color-by array / LUT change ---
                    new_arr = _display_color_array(display)
                    new_lut = _display_lut(display)
                    if new_arr != last_color_arr[0]:
                        last_color_arr[0] = new_arr
                        # Keep RequestData in sync so it sends the right scalar.
                        self._display_color_arr = new_arr
                        self._display_lut       = new_lut
                        print(f"PVLink:color-by changed to '{new_arr}' for '{mesh_name}' — re-sending directly", flush=True)
                        # pvs.UpdatePipeline() is silently blocked here because
                        # VTK prevents re-entrant pipeline execution inside an
                        # observer callback.  Rebuild and resend using cached data.
                        last_data = getattr(self, '_last_input_data', None)
                        if last_data is not None:
                            try:
                                m = get_pvlink_manager()
                                if new_arr:
                                    m.watch_colormap(new_arr, lut=new_lut)
                                lut_range = m.get_lut_range(new_arr) if new_arr else None
                                is_pt = last_data.GetPointData().GetArray(new_arr) is not None if new_arr else True
                                payload = _build_mesh_payload(
                                    last_data, new_arr, is_pt, mesh_name,
                                    lut_range=lut_range,
                                    compute_normals=self._compute_normals)
                                m.send_message(MSG_TYPE_MESH, payload)
                                loc = "point" if is_pt else "cell"
                                scalar_desc = f"scalar='{new_arr}' ({loc})" if new_arr else "no scalar"
                                print(f"PVLink:re-sent '{mesh_name}' — "
                                      f"{last_data.GetNumberOfPoints()} pts, "
                                      f"{last_data.GetNumberOfCells()} cells, {scalar_desc}", flush=True)
                                m.mark_mesh_sent()
                            except Exception as exc2:
                                print(f"PVLink:direct re-send failed for '{mesh_name}' — {exc2}", flush=True)
                        else:
                            print(f"PVLink:no cached input for '{mesh_name}' — re-send skipped", flush=True)
                    elif new_lut is not None and new_lut != self._display_lut:
                        # Same array, different LUT (e.g. colormap preset changed
                        # on a separate-colormap representation).
                        self._display_lut = new_lut
                except Exception as exc:
                    print(f"PVLink:visibility observer error: {exc}")

            display.SMProxy.AddObserver('ModifiedEvent', _on_display_modified)
        except Exception as exc:
            print(f"PVLink:could not register visibility observer: {exc}")

    # ------------------------------------------------------------------
    # Color array resolution
    # ------------------------------------------------------------------

    def _resolve_color_array(self, data):
        name = self._color_arr.strip()

        # 1. Explicit override from the ColorArrayName property.
        if name:
            if data.GetPointData().GetArray(name) is not None:
                return name, True
            if data.GetCellData().GetArray(name) is not None:
                return name, False
            print(f"PVLink:ColorArrayName '{name}' not found, falling back to auto-detect")

        # 2. VTK active scalars — ParaView sets this whenever the user
        #    picks a color-by array in the Properties panel.  Calling
        #    pvs.GetDisplayProperties() here is deliberately avoided:
        #    ParaView API calls inside RequestData can retrigger pipeline
        #    execution and cause infinite recursion.
        for pd, is_pt in ((data.GetPointData(), True), (data.GetCellData(), False)):
            arr = pd.GetScalars()
            if arr and arr.GetName():
                return arr.GetName(), is_pt

        # 3. First available array as last resort.
        for i in range(data.GetPointData().GetNumberOfArrays()):
            n = data.GetPointData().GetArrayName(i)
            if n:
                return n, True
        for i in range(data.GetCellData().GetNumberOfArrays()):
            n = data.GetCellData().GetArrayName(i)
            if n:
                return n, False

        return "", True


# ---------------------------------------------------------------------------
# PVLink Domain Bounds filter
# ---------------------------------------------------------------------------

@smproxy.filter(name="PVLinkDomainBounds", label="PVLink Domain Bounds")
@smhint.xml('<ShowInMenu category="PVLink"/>')
@smproperty.input(name="Input", port_index=0)
@smdomain.datatype(dataTypes=["vtkDataSet"])
class PVLinkDomainBoundsFilter(VTKPythonAlgorithmBase):
    """
    Pass-through filter that reports the computational domain bounding box to
    the PVLink receiver.

    Place it immediately after the reader in the pipeline.  On every update it
    sends a MSG_TYPE_BOUNDS packet so the receiver can map ParaView coordinates
    into the designer's world-space container.

    Behaviour:
      OverrideBounds = OFF (default)
          Bounds are taken from GetBounds() on the input data and sent
          automatically every time the pipeline executes.

      OverrideBounds = ON
          The XRange / YRange / ZRange properties are sent instead.
          Use this when the physical domain is larger than the current
          dataset (e.g. a slice of a full CFD grid) or when you want to
          lock the mapping to a known extent.
    """

    def __init__(self):
        super().__init__(nInputPorts=1, nOutputPorts=1)
        self._host        = "127.0.0.1"
        self._port        = 9001
        self._override    = False
        self._xmin = 0.0; self._xmax = 1.0
        self._ymin = 0.0; self._ymax = 1.0
        self._zmin = 0.0; self._zmax = 1.0
        self._last_bounds = None   # last sent bounds tuple; avoids re-sending every frame

    def FillOutputPortInformation(self, port, info):
        # Mirrors vtkPassInputTypeAlgorithm: declare output as the most general
        # type so ParaView doesn't lock the representation to vtkPolyData before
        # the pipeline runs.  RequestDataObject creates the concrete type.
        info.Set(vtk.vtkDataObject.DATA_TYPE_NAME(), "vtkDataObject")
        return 1

    # ------------------------------------------------------------------
    # Connection properties  (shared session manager — keep in sync with
    # PVLinkMeshSenderFilter if both are used in the same pipeline)
    # ------------------------------------------------------------------

    @smproperty.stringvector(name="Host", default_values="127.0.0.1")
    def SetHost(self, val):
        self._host = val
        get_pvlink_manager().set_address(val, self._port)
        self.Modified()

    def GetHost(self):
        return self._host

    @smproperty.intvector(name="TCPPort", default_values=9001)
    def SetTCPPort(self, val):
        self._port = int(val)
        get_pvlink_manager().set_address(self._host, self._port)
        self.Modified()

    def GetTCPPort(self):
        return self._port

    # ------------------------------------------------------------------
    # Override toggle
    # ------------------------------------------------------------------

    @smproperty.intvector(name="OverrideBounds", default_values=0, number_of_elements=1)
    @smdomain.xml('<BooleanDomain name="bool"/>')
    def SetOverrideBounds(self, val):
        self._override = bool(val)
        self.Modified()

    def GetOverrideBounds(self):
        return int(self._override)

    # ------------------------------------------------------------------
    # Manual bound properties (active when OverrideBounds = ON)
    # ------------------------------------------------------------------

    @smproperty.doublevector(name="XRange", default_values=[0.0, 1.0],
                             number_of_elements=2)
    def SetXRange(self, xmin, xmax):
        self._xmin = float(xmin)
        self._xmax = float(xmax)
        self.Modified()

    def GetXRange(self):
        return self._xmin, self._xmax

    @smproperty.doublevector(name="YRange", default_values=[0.0, 1.0],
                             number_of_elements=2)
    def SetYRange(self, ymin, ymax):
        self._ymin = float(ymin)
        self._ymax = float(ymax)
        self.Modified()

    def GetYRange(self):
        return self._ymin, self._ymax

    @smproperty.doublevector(name="ZRange", default_values=[0.0, 1.0],
                             number_of_elements=2)
    def SetZRange(self, zmin, zmax):
        self._zmin = float(zmin)
        self._zmax = float(zmax)
        self.Modified()

    def GetZRange(self):
        return self._zmin, self._zmax

    # ------------------------------------------------------------------
    # Pipeline
    # ------------------------------------------------------------------

    def RequestDataObject(self, request, inInfo, outInfo):
        # Read input type from pipeline information — self.GetInputData() returns
        # None here because upstream hasn't executed yet.
        inp = inInfo[0].GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if inp is None:
            return 1
        out = outInfo.GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        return 1

    def RequestInformation(self, request, inInfo, outInfo):
        # Propagate whole extent and other metadata so downstream filters
        # (surface, contour, etc.) know the dataset dimensions.
        from vtkmodules.vtkCommonExecutionModel import vtkStreamingDemandDrivenPipeline as SDDP
        in_info  = inInfo[0].GetInformationObject(0)
        out_info = outInfo.GetInformationObject(0)
        if in_info.Has(SDDP.WHOLE_EXTENT()):
            out_info.Set(SDDP.WHOLE_EXTENT(),
                         in_info.Get(SDDP.WHOLE_EXTENT()), 6)
        return 1

    def RequestUpdateExtent(self, request, inInfo, outInfo):
        # Pass the downstream update-extent request back upstream unchanged.
        from vtkmodules.vtkCommonExecutionModel import vtkStreamingDemandDrivenPipeline as SDDP
        in_info  = inInfo[0].GetInformationObject(0)
        out_info = outInfo.GetInformationObject(0)
        if out_info.Has(SDDP.UPDATE_EXTENT()):
            in_info.Set(SDDP.UPDATE_EXTENT(),
                        out_info.Get(SDDP.UPDATE_EXTENT()), 6)
        return 1

    def RequestData(self, request, inInfo, outInfo):
        print("Domain Bounds  RequestData called")
        inp = self.GetInputData(inInfo, 0, 0)
        if inp is None:
            return 1

        out = self.GetOutputData(outInfo, 0)
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        out.ShallowCopy(inp)

        # Resolve bounds.
        if self._override:
            bounds = (self._xmin, self._xmax,
                      self._ymin, self._ymax,
                      self._zmin, self._zmax)
            source = "manual"
        else:
            bounds = inp.GetBounds()
            source = "data"

        mgr = get_pvlink_manager()
        mgr.set_address(self._host, self._port)
        payload = struct.pack('<6f', *bounds)

        # Always keep replay state current.  Only wire-send when bounds change.
        mgr._sender.register_state_only('bounds', MSG_TYPE_BOUNDS, payload)
        if bounds != self._last_bounds:
            self._last_bounds = bounds
            mgr.send_message(MSG_TYPE_BOUNDS, payload)
            print(f"PVLink:sent domain bounds [{source}]  "
                  f"X[{bounds[0]:.4g}, {bounds[1]:.4g}]  "
                  f"Y[{bounds[2]:.4g}, {bounds[3]:.4g}]  "
                  f"Z[{bounds[4]:.4g}, {bounds[5]:.4g}]")

        return 1


# ---------------------------------------------------------------------------
# PVLink menu
# ---------------------------------------------------------------------------

def _qt_widgets():
    # Only use ParaView's own bundled Qt binding.  Importing a different Qt
    # binding (PyQt5/PySide2/PySide6) into a process that already has
    # ParaView's Qt runtime loaded can hang or crash the app, so we do not
    # attempt any fallback here.
    from paraview.qt import QtWidgets
    return QtWidgets


def _build_pvlink_menu():
    """
    Adds a top-level 'PVLink' menu to the ParaView main window.
    Called once when the plugin loads.  Safe to call multiple times — skips
    if the menu already exists.
    """
    try:
        QtWidgets = _qt_widgets()

        app = QtWidgets.QApplication.instance()
        if app is None:
            return

        # Find the main window.
        main_win = None
        for widget in app.topLevelWidgets():
            if widget.inherits('QMainWindow'):
                main_win = widget
                break
        if main_win is None:
            return

        menu_bar = main_win.menuBar()

        # Skip if already added.
        for action in menu_bar.actions():
            if action.text() == 'PVLink':
                return

        menu = menu_bar.addMenu('PVLink')

        # ── Connection ──────────────────────────────────────────────────
        conn_action = menu.addAction('Connection Settings…')
        conn_action.triggered.connect(_show_connection_dialog)

        menu.addSeparator()

        # ── Re-send all ─────────────────────────────────────────────────
        resend_action = menu.addAction('Re-send All Data')
        resend_action.triggered.connect(_resend_all)

        menu.addSeparator()

        # ── Per-filter submenu (populated dynamically on open) ───────────
        filters_menu = menu.addMenu('Filters')
        filters_menu.aboutToShow.connect(lambda: _populate_filters_menu(filters_menu))

        print('PVLink: menu installed')

    except Exception as exc:
        print(f'PVLink: could not install menu — {exc}')


def _show_connection_dialog():
    """Modal dialog to edit Host / Port on the session manager."""
    try:
        QtWidgets = _qt_widgets()

        mgr = get_pvlink_manager()

        dialog = QtWidgets.QDialog()
        dialog.setWindowTitle('PVLink — Connection Settings')
        dialog.setMinimumWidth(340)

        layout = QtWidgets.QFormLayout(dialog)

        host_edit = QtWidgets.QLineEdit(mgr.host)
        port_spin = QtWidgets.QSpinBox()
        port_spin.setRange(1, 65535)
        port_spin.setValue(mgr.port)

        layout.addRow('Host:', host_edit)
        layout.addRow('Port:', port_spin)

        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)
        layout.addRow(buttons)

        if dialog.exec_() == QtWidgets.QDialog.Accepted:
            new_host = host_edit.text().strip() or '127.0.0.1'
            new_port = port_spin.value()
            mgr.set_address(new_host, new_port)
            print(f'PVLink: connection set to {new_host}:{new_port}')

    except Exception as exc:
        print(f'PVLink: connection dialog error — {exc}')


def _resend_all():
    """Force-execute every PVLink filter in the pipeline to re-send all data."""
    try:
        import paraview.simple as pvs

        sources = pvs.GetSources()
        sent = 0
        for _key, proxy in sources.items():
            if type(proxy).__name__ == 'PVLinkDomainBoundsFilter':
                proxy.Modified()
                pvs.UpdatePipeline(proxy=proxy)
                sent += 1

        if sent == 0:
            print('PVLink: no PVLink filters found in pipeline')
        else:
            print(f'PVLink: re-sent {sent} filter(s)')

    except Exception as exc:
        print(f'PVLink: re-send error — {exc}')


def _populate_filters_menu(menu):
    """Rebuild the Filters submenu with one entry per PVLink filter."""
    try:
        import paraview.simple as pvs

        menu.clear()

        sources = pvs.GetSources()
        pvlink_filters = {}
        for key, proxy in sources.items():
            if type(proxy).__name__ == 'PVLinkDomainBoundsFilter':
                label = key[0] if isinstance(key, tuple) else str(key)
                pvlink_filters[label] = proxy

        if not pvlink_filters:
            menu.addAction('(no PVLink filters in pipeline)').setEnabled(False)
            return

        for label, proxy in pvlink_filters.items():
            action = menu.addAction(f'Re-send: {label}')
            # Capture proxy by default-arg binding.
            action.triggered.connect(lambda checked=False, p=proxy, n=label: _resend_filter(p, n))

    except Exception as exc:
        print(f'PVLink: filters menu error — {exc}')


def _resend_filter(proxy, name):
    """Force-execute a single PVLink filter."""
    try:
        import paraview.simple as pvs
        proxy.Modified()
        pvs.UpdatePipeline(proxy=proxy)
        print(f'PVLink: re-sent filter "{name}"')
    except Exception as exc:
        print(f'PVLink: re-send filter error — {exc}')


# Install the menu when the module is loaded by ParaView.
#
# DISABLED PERMANENTLY: confirmed that none of paraview.qt, PyQt5, PySide6,
# or PySide2 are importable in this ParaView 6.1.1 install's Python
# environment — its Qt GUI is statically linked with no Python binding
# exposed, so there is no way for a plugin script to reach the Qt event
# loop here. One attempt also wedged ParaView outright. Not viable on this
# build; left commented for reference in case a future ParaView version
# exposes paraview.qt.
#
# def _install_menu():
#     try:
#         try:
#             from paraview.qt import QtWidgets as _QtWidgets
#             from paraview.qt import QtCore   as _QtCore
#         except Exception:
#             try:
#                 import PyQt5.QtWidgets as _QtWidgets
#                 import PyQt5.QtCore    as _QtCore
#             except Exception:
#                 try:
#                     import PySide6.QtWidgets as _QtWidgets
#                     import PySide6.QtCore    as _QtCore
#                 except Exception:
#                     import PySide2.QtWidgets as _QtWidgets
#                     import PySide2.QtCore    as _QtCore
#
#         app = _QtWidgets.QApplication.instance()
#         if app is None:
#             print('PVLink: no QApplication — running in batch mode, skipping menu')
#             return
#
#         # Defer so the main window is fully built before we look for it.
#         _QtCore.QTimer.singleShot(500, _build_pvlink_menu)
#         print('PVLink: menu install scheduled')
#
#     except Exception as exc:
#         print(f'PVLink: could not schedule menu install — {exc}')
#
# _install_menu()
