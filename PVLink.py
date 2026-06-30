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
  Outer envelope:
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

import socket
import struct
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

@smproxy.filter(name="HelloPassThrough", label="Hello Pass Through")
@smhint.xml('<ShowInMenu category="PVLink"/>')
@smproperty.input(name="Input", port_index=0)
@smdomain.datatype(dataTypes=["vtkDataSet"])
class HelloPassThrough(VTKPythonAlgorithmBase):
    def __init__(self):
        super().__init__(nInputPorts=1, nOutputPorts=1)

    def FillOutputPortInformation(self, port, info):
        # Mirrors vtkPassInputTypeAlgorithm: declare output as the most general
        # type so ParaView doesn't lock the representation to vtkPolyData before
        # the pipeline runs.  RequestDataObject creates the concrete type.
        info.Set(vtk.vtkDataObject.DATA_TYPE_NAME(), "vtkDataObject")
        return 1

    def RequestDataObject(self, request, inInfo, outInfo):
        inp = inInfo[0].GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if inp is None:
            return 1
        out = outInfo.GetInformationObject(0).Get(vtk.vtkDataObject.DATA_OBJECT())
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        return 1

    def RequestInformation(self, request, inInfo, outInfo):
        from vtkmodules.vtkCommonExecutionModel import vtkStreamingDemandDrivenPipeline as SDDP
        in_info  = inInfo[0].GetInformationObject(0)
        out_info = outInfo.GetInformationObject(0)
        if in_info.Has(SDDP.WHOLE_EXTENT()):
            out_info.Set(SDDP.WHOLE_EXTENT(), in_info.Get(SDDP.WHOLE_EXTENT()), 6)
        return 1

    def RequestUpdateExtent(self, request, inInfo, outInfo):
        from vtkmodules.vtkCommonExecutionModel import vtkStreamingDemandDrivenPipeline as SDDP
        in_info  = inInfo[0].GetInformationObject(0)
        out_info = outInfo.GetInformationObject(0)
        if out_info.Has(SDDP.UPDATE_EXTENT()):
            in_info.Set(SDDP.UPDATE_EXTENT(), out_info.Get(SDDP.UPDATE_EXTENT()), 6)
        return 1

    def RequestData(self, request, inInfo, outInfo):
        inp = self.GetInputData(inInfo, 0, 0)
        if inp is None:
            print("HelloPassThrough: input is None")
            return 1
        out = self.GetOutputData(outInfo, 0)
        if out is None:
            print("HelloPassThrough: output is None")
            return 1
        out.ShallowCopy(inp)
        print(f"HelloPassThrough: hello — {inp.GetClassName()}, "
              f"{inp.GetNumberOfPoints()} points, "
              f"{inp.GetNumberOfCells()} cells")
        return 1


# ---------------------------------------------------------------------------
# TCP connection
# ---------------------------------------------------------------------------

class _Connection:
    """
    Persistent TCP socket to the PVLink receiver.  Reconnects only when the address changes
    or when a send/receive error is detected.
    """

    _RETRY_INTERVAL = 5.0   # seconds between reconnect attempts when disconnected

    def __init__(self):
        self._sock          = None
        self._host          = None
        self._port          = None
        self._next_retry    = 0.0   # monotonic time after which reconnect is allowed
        self._just_connected = False  # True for one call after a successful connect

    def ensure_connected(self, host, port):
        # Already connected to the right address — nothing to do.
        if self._sock and host == self._host and port == self._port:
            return True
        # Address changed — force an immediate reconnect attempt.
        if host != self._host or port != self._port:
            self._next_retry = 0.0
        self.disconnect()
        # Respect the cooldown so a missing Unity instance doesn't block every
        # pipeline update for 2 s (the connect timeout).
        now = time.monotonic()
        if now < self._next_retry:
            return False
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(2.0)
            s.connect((host, port))
            s.settimeout(5.0)
            self._sock           = s
            self._host           = host
            self._port           = port
            self._next_retry     = 0.0
            self._just_connected = True
            print(f"PVLink:connected to {host}:{port}")
            return True
        except OSError as exc:
            print(f"PVLink:connect to {host}:{port} failed — {exc}")
            self._sock       = None
            self._next_retry = time.monotonic() + self._RETRY_INTERVAL
            return False

    def send_message(self, msg_type, payload):
        if not self._sock:
            return False
        try:
            header = struct.pack('<ii', len(payload), msg_type)
            self._sock.sendall(header + payload)
            ack = self._recv_exact(4)
            if ack is None:
                raise OSError("connection closed before ack")
            status = struct.unpack('<i', ack)[0]
            if status != 0:
                print(f"PVLink:ack status {status}")
            return status == 0
        except OSError as exc:
            print(f"PVLink:send error — {exc}; will reconnect on next update")
            self.disconnect()
            return False

    def take_just_connected(self):
        """Returns True (once) immediately after a successful connect."""
        v = self._just_connected
        self._just_connected = False
        return v

    def disconnect(self):
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        self._host           = None
        self._port           = None
        self._next_retry     = 0.0
        self._just_connected = False

    @property
    def is_connected(self):
        return self._sock is not None

    def _recv_exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self._sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data


# ---------------------------------------------------------------------------
# Colormap watcher
# ---------------------------------------------------------------------------

class _ColormapWatcher:
    """
    Observes vtkSMTransferFunctionProxy for each watched variable and sends
    MSG_TYPE_COLORMAP whenever the LUT is created or subsequently modified.
    """

    def __init__(self, conn):
        self._conn          = conn
        self._observed      = {}   # var_name -> (lut_proxy, observer_tag)
        self._last_payloads = {}   # var_name -> last built payload (for reconnect replay)
        self._last_rgb_pts  = {}   # var_name -> tuple of RGBPoints at last send
        self._host          = "127.0.0.1"
        self._port          = 9001

    def set_address(self, host, port):
        self._host = host
        self._port = port

    def watch(self, var_name):
        """Watch *var_name*.
        Always sends the current colormap immediately so Unity has it before
        the accompanying mesh arrives, even if already watching.
        Re-registers the ModifiedEvent observer each call in case ParaView
        replaced the LUT proxy (happens when switching colormap presets).
        """
        if not var_name:
            return
        try:
            import paraview.simple as pvs
            lut = pvs.GetColorTransferFunction(var_name)
            if lut is None:
                return

            # Always send current state so Unity gets the right colormap
            # even after a reconnect or pipeline re-execute.
            self._send(var_name, lut)

            # If we already have an observer, check whether the LUT proxy is
            # still the same object.  ParaView replaces the proxy when the user
            # applies a new colormap preset, so we must re-register in that case.
            existing = self._observed.get(var_name)
            if existing is not None:
                old_lut, old_tag = existing
                if old_lut.SMProxy.GetGlobalIDAsString() == lut.SMProxy.GetGlobalIDAsString():
                    return              # same proxy, observer still valid
                # Proxy was replaced — remove the stale observer.
                try:
                    old_lut.SMProxy.RemoveObserver(old_tag)
                except Exception:
                    pass

            def _on_modified(obj, event, vn=var_name):
                try:
                    import paraview.simple as pvs2
                    lut2 = pvs2.GetColorTransferFunction(vn)
                    if lut2:
                        self._send(vn, lut2)
                except Exception as exc:
                    print(f"PVLink:colormap observer error for '{vn}': {exc}")

            tag = lut.SMProxy.AddObserver('ModifiedEvent', _on_modified)
            self._observed[var_name] = (lut, tag)

        except Exception as exc:
            print(f"PVLink:could not watch colormap for '{var_name}': {exc}")

    def get_lut_range(self, var_name):
        """Return (min, max) of the current LUT for *var_name*, or None.

        lut.RGBPoints is a flat list [val, R, G, B, val, R, G, B, ...].
        The first value element is at index 0 and the last at index -4.
        """
        entry = self._observed.get(var_name)
        if entry is None:
            return None
        lut, _tag = entry
        try:
            pts = lut.RGBPoints
            if pts and len(pts) >= 8:          # at least 2 control points
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
        try:
            payload = _build_colormap_payload(var_name, lut)
        except Exception as exc:
            print(f"PVLink:colormap payload error for '{var_name}': {exc}")
            return
        # Snapshot RGBPoints so the render observer can detect future changes.
        try:
            self._last_rgb_pts[var_name] = tuple(lut.RGBPoints) if lut.RGBPoints else ()
        except Exception:
            pass
        # Store so the manager can replay this on reconnect.
        self._last_payloads[var_name] = payload
        if self._conn.ensure_connected(self._host, self._port):
            if self._conn.send_message(MSG_TYPE_COLORMAP, payload):
                print(f"PVLink:sent colormap for '{var_name}'")


# ---------------------------------------------------------------------------
# Update (flip) trigger
# ---------------------------------------------------------------------------

class _UpdateTrigger:
    """
    Observes the render view's EndEvent and sends MSG_TYPE_UPDATE once after
    each pipeline execution that produced at least one mesh send.
    """

    def __init__(self, conn):
        self._conn       = conn
        self._tag        = None
        self._view_proxy = None
        self._pending    = False
        self._host       = "127.0.0.1"
        self._port       = 9001

    def mark_pending(self, host, port):
        self._host    = host
        self._port    = port
        self._pending = True
        self._ensure_observer(force=False)

    def reset(self):
        if self._tag is not None and self._view_proxy is not None:
            try:
                self._view_proxy.RemoveObserver(self._tag)
            except Exception:
                pass
        self._tag        = None
        self._view_proxy = None
        self._pending    = False

    def _ensure_observer(self, force=False):
        try:
            import paraview.simple as pvs
            view = pvs.GetActiveViewOrCreate('RenderView')
            # Re-register if forced, or if the view proxy changed, or if never registered.
            current_proxy = view.SMProxy
            if not force and self._tag is not None and self._view_proxy is current_proxy:
                return
            # Remove stale observer if any.
            if self._tag is not None and self._view_proxy is not None:
                try:
                    self._view_proxy.RemoveObserver(self._tag)
                except Exception:
                    pass
                self._tag = None

            def _on_end_render(obj, event):
                print("On End Render")
                if not self._pending:
                    return
                self._pending = False
                if self._conn.ensure_connected(self._host, self._port):
                    if self._conn.send_message(MSG_TYPE_UPDATE, b''):
                        print("PVLink: sent Update — buffered meshes now active")

            self._tag        = view.SMProxy.AddObserver('EndEvent', _on_end_render)
            self._view_proxy = view.SMProxy
        except Exception as exc:
            print(f"PVLink:could not install update trigger — {exc}")


# ---------------------------------------------------------------------------
# Session-global connection manager
# ---------------------------------------------------------------------------

class PVLinkConnectionManager:
    """
    Owns the TCP connection plus the colormap watcher and update trigger.
    Stored on paraview.servermanager so it survives filter re-creation and
    is accessible from any ParaView Python code via get_pvlink_manager().

    set_address() only disconnects when the address actually changes, so
    repeated Apply calls that leave Host/Port unchanged keep the socket open.
    """

    def __init__(self):
        self._host    = "127.0.0.1"
        self._port    = 9001
        self._conn    = _Connection()
        self._watcher = _ColormapWatcher(self._conn)
        self._trigger = _UpdateTrigger(self._conn)
        # Messages to replay automatically every time a new connection is made.
        # Keys are caller-defined strings (e.g. 'bounds'); values are (msg_type, payload).
        self._reconnect_msgs = {}

    # --- address management --------------------------------------------------

    def set_address(self, host, port):
        """Change target address.  Disconnects and resets watchers only if changed."""
        host = str(host)
        port = int(port)
        if host == self._host and port == self._port:
            return
        self._host = host
        self._port = port
        self._conn.disconnect()
        self._watcher.set_address(host, port)
        self._watcher.unwatch_all()
        self._trigger.reset()

    @property
    def host(self):
        return self._host

    @property
    def port(self):
        return self._port

    # --- public API ----------------------------------------------------------

    def connect(self, host=None, port=None):
        """Explicitly connect (or reconnect).  Returns True on success."""
        if host is not None or port is not None:
            self.set_address(host or self._host, port or self._port)
        return self._conn.ensure_connected(self._host, self._port)

    def disconnect(self):
        self._conn.disconnect()
        self._watcher.unwatch_all()
        self._trigger.reset()

    @property
    def is_connected(self):
        return self._conn.is_connected

    def register_reconnect_message(self, key, msg_type, payload):
        """Store (or update) a message to be replayed on every new connection.

        Call this whenever a piece of state should always be re-sent to a
        freshly connected receiver (e.g. domain bounds, colormaps).
        The message is also sent immediately if already connected.
        """
        self._reconnect_msgs[key] = (msg_type, payload)

    def send_message(self, msg_type, payload):
        if self._conn.ensure_connected(self._host, self._port):
            if self._conn.take_just_connected():
                self._replay_reconnect_messages()
            return self._conn.send_message(msg_type, payload)
        return False

    def _replay_reconnect_messages(self):
        for key, (mt, pl) in self._reconnect_msgs.items():
            self._conn.send_message(mt, pl)
            print(f"PVLink:replayed '{key}' on reconnect")
        for var_name, pl in self._watcher._last_payloads.items():
            self._conn.send_message(MSG_TYPE_COLORMAP, pl)
            print(f"PVLink:replayed colormap '{var_name}' on reconnect")

    def watch_colormap(self, var_name):
        self._watcher.set_address(self._host, self._port)
        self._watcher.watch(var_name)

    def get_lut_range(self, var_name):
        """Return (min, max) from the watched LUT for *var_name*, or None."""
        return self._watcher.get_lut_range(var_name)

    def mark_update_pending(self):
        self._trigger.mark_pending(self._host, self._port)


# Module-level fallback for use outside a full ParaView session (pvpython, tests).
_fallback_manager = None


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
        if not hasattr(sm, '_pvlink_manager') or not isinstance(sm._pvlink_manager, PVLinkConnectionManager):
            sm._pvlink_manager = PVLinkConnectionManager()
        return sm._pvlink_manager
    except ImportError:
        if _fallback_manager is None:
            _fallback_manager = PVLinkConnectionManager()
        return _fallback_manager


# ---------------------------------------------------------------------------
# Payload builders
# ---------------------------------------------------------------------------

def _build_colormap_payload(var_name, lut):
    rgb_pts = lut.RGBPoints
    if not rgb_pts or len(rgb_pts) < 8:
        raise ValueError(f"LUT for '{var_name}' has no RGB points")

    pts       = np.array(rgb_pts, dtype=np.float64).reshape(-1, 4)
    x_vals    = pts[:, 0]
    range_min = float(x_vals[0])
    range_max = float(x_vals[-1])
    if range_max == range_min:
        range_max = range_min + 1.0

    x_norm  = (x_vals - range_min) / (range_max - range_min)
    N       = 256
    t       = np.linspace(0.0, 1.0, N)
    r_samp  = np.interp(t, x_norm, pts[:, 1]).astype(np.float32)
    g_samp  = np.interp(t, x_norm, pts[:, 2]).astype(np.float32)
    b_samp  = np.interp(t, x_norm, pts[:, 3]).astype(np.float32)

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

        # ShallowCopy into the pre-allocated output object.  Direct reference
        # assignment broke when FillOutputPortInformation opened the type to
        # vtkDataObject — VTK no longer guarantees a live output object exists.
        out = self.GetOutputData(outInfo, 0)
        if out is None or out.GetClassName() != inp.GetClassName():
            out = inp.NewInstance()
            outInfo.GetInformationObject(0).Set(vtk.vtkDataObject.DATA_OBJECT(), out)
        out.ShallowCopy(inp)

        mgr = get_pvlink_manager()

        color_name, is_point_data = self._resolve_color_array(inp)

        if color_name:
            mgr.watch_colormap(color_name)

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
            mgr.mark_update_pending()

        self._ensure_visibility_observer(mgr)
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
            src  = pvs.GetActiveSource()
            view = pvs.GetActiveView()
            if src is None or view is None:
                return
            display = pvs.GetDisplayProperties(src, view)
            if display is None:
                return

            mesh_name = self._mesh_name
            last_vis  = [bool(display.Visibility)]   # mutable cell for closure

            def _on_display_modified(obj, event):
                try:
                    visible = bool(display.Visibility)
                    if visible == last_vis[0]:
                        return          # some other property changed — ignore
                    last_vis[0] = visible
                    name_bytes = mesh_name.encode('utf-8')
                    payload = (struct.pack('<i', len(name_bytes))
                               + name_bytes
                               + struct.pack('<i', int(visible)))
                    # Read host/port from the session manager at call time so
                    # that changes made via PVLinkDomainBoundsFilter are picked up.
                    m = get_pvlink_manager()
                    if m._conn.ensure_connected(m._host, m._port):
                        if m._conn.send_message(MSG_TYPE_VISIBILITY, payload):
                            state = "shown" if visible else "hidden"
                            print(f"PVLink:'{mesh_name}' {state}")
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

        # Always keep the reconnect payload current so Unity gets bounds on connect.
        mgr.register_reconnect_message('bounds', MSG_TYPE_BOUNDS, payload)

        # Only send over the wire when the bounds have actually changed.
        if bounds != self._last_bounds:
            self._last_bounds = bounds
            if mgr.send_message(MSG_TYPE_BOUNDS, payload):
                print(f"PVLink:sent domain bounds [{source}]  "
                      f"X[{bounds[0]:.4g}, {bounds[1]:.4g}]  "
                      f"Y[{bounds[2]:.4g}, {bounds[3]:.4g}]  "
                      f"Z[{bounds[4]:.4g}, {bounds[5]:.4g}]")

        return 1
