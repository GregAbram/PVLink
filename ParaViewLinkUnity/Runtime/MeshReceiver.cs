using System;
using System.Collections;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using UnityEngine;
using UnityEngine.Rendering;

namespace ParaViewLink
{
    /// <summary>
    /// Receives ParaView mesh/colormap/bounds data over TCP and presents it in
    /// the Unity scene with minimal main-thread cost.
    ///
    /// Threading model
    /// ───────────────
    /// IO thread (SocketReceiver.OnMessage):
    ///   • Parses every message.
    ///   • For PVMesh: builds vertex/index/normal/colour buffers off the main
    ///     thread, enqueues the result, returns immediately.
    ///   • For Colormap/Bounds/Visibility: parses into plain structs and enqueues.
    ///   • For Update (flip): releases _flipSignal then BLOCKS on _flipDone until
    ///     the main thread confirms the swap.  No ack is sent (one-way protocol).
    ///
    /// Main thread (Update):
    ///   • Applies immediate visibility changes from the visibility queue.
    ///   • Checks _flipSignal (non-blocking).  When set:
    ///       1. Applies pending bounds/colormaps.
    ///       2. For each queued ParsedMesh: calls ApplyAndDisposeWritableMeshData
    ///          (fast GPU upload of the pre-built buffers) then swaps front/back.
    ///       3. Releases _flipDone → IO thread unblocks.
    ///
    /// Per-mesh double buffer
    /// ──────────────────────
    /// Each named mesh owns a MeshBufferPair: two GameObjects (front=visible,
    /// back=inactive).  ApplyAndDisposeWritableMeshData targets the back buffer's
    /// Mesh, then Swap() exchanges front↔back with two SetActive calls.
    /// The front buffer is never touched during data upload.
    /// </summary>
    [AddComponentMenu("ParaViewLink/Mesh Receiver")]
    public class MeshReceiver : MonoBehaviour
    {
        // ───────────────────────────────────────────────── Inspector ────

        [Header("Network")]
        [Tooltip("Host to dial the DataManager on. Leave empty to auto-connect: " +
                 "waits for the discovery list to settle and connects only if " +
                 "exactly one DataManager was found on the LAN.")]
        public string DataManagerHost = "";

        [Tooltip("Port to dial the DataManager on — must match its --client-port.")]
        public int DataManagerPort = 9010;

        [Tooltip("UDP port to listen for DataManager broadcast announcements on — " +
                 "must match its --discovery-port. See DiscoveredDataManagers/" +
                 "ConnectToDataManager() to use this without hardcoding an address.")]
        public int DiscoveryPort = 9011;

        [Header("Coordinates")]
        [Tooltip("Auto-compute transform from domain bounds + SimContainer transform.")]
        public bool AutoComputeTransform = true;

        [Header("Material")]
        [Tooltip("Base material using the ScalarField URP shader.  " +
                 "One runtime copy is made per variable.")]
        public Material BaseScalarMaterial;

        [Header("Debug")]
        [Tooltip("Log every mesh/colormap/bounds/update message received. " +
                 "Off by default -- this is one line per message, which under " +
                 "animation playback is many lines per second.")]
        public bool VerboseLogging = false;

        /// <summary>Gated by VerboseLogging.  Warnings/errors are never gated --
        /// use Debug.LogWarning/LogError directly for those.</summary>
        private void Log(string message)
        {
            if (VerboseLogging) Debug.Log(message);
        }

        // ──────────────────────────────────────────── Runtime state ─────

        [HideInInspector] public bool IsConnecting => _socket?.IsConnecting ?? false;
        [HideInInspector] public bool IsConnected  => _socket?.IsConnected  ?? false;

        // ─────────────────────── Vertex layout (interleaved, 28 bytes) ──

        [StructLayout(LayoutKind.Sequential)]
        private struct MeshVertex
        {
            public float X,  Y,  Z;   // Position  (12 bytes)
            public float NX, NY, NZ;  // Normal    (12 bytes)
            public byte  R,  G,  B, A;// Colour UNorm8 (4 bytes)
        }

        private static readonly VertexAttributeDescriptor[] k_Layout =
        {
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Normal,   VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Color,    VertexAttributeFormat.UNorm8,  4),
        };

        private const MeshUpdateFlags k_ApplyFlags =
            MeshUpdateFlags.DontValidateIndices  |
            MeshUpdateFlags.DontRecalculateBounds|
            MeshUpdateFlags.DontResetBoneBounds;

        // ──────────────────────────────────── Double-buffer structures ──

        private sealed class MeshInstance
        {
            public GameObject   Go;
            public MeshFilter   Filter;
            public MeshRenderer Renderer;
            public Mesh         Mesh;
        }

        private sealed class MeshBufferPair
        {
            public MeshInstance Front;  // active / visible
            public MeshInstance Back;   // inactive / being prepared
            public string VariableName; // colormap variable currently applied to Front

            /// <summary>Promote back to front.  Both SetActive calls happen here.</summary>
            public void Swap()
            {
                (Front, Back) = (Back, Front);
                Front.Go.SetActive(true);
                Back.Go.SetActive(false);
            }
        }

        // ───────────────────────── Data structs produced on IO thread ───

        /// <summary>
        /// Mesh data fully processed on the IO thread — normals computed,
        /// scalars normalised, winding flipped, AABB calculated.
        /// Managed arrays only; no Unity Mesh API called yet.
        /// ApplyMesh() on the main thread converts these to a Unity Mesh.
        /// </summary>
        private struct ParsedMesh
        {
            public string      MeshName;
            public MeshVertex[] Vertices;  // positions, normals, colours — all baked
            public int[]       Indices;    // winding-flipped, always int32
            public Bounds      MeshBounds;
            public string      VariableName;
        }

        private struct RawColormap
        {
            public string  VariableName;
            public float   Min, Max;
            public Color[] Samples;
        }

        private struct RawVisibility
        {
            public string MeshName;
            public bool   Visible;
        }

        // ─────────────────────────────────────────────── Queues ─────────

        private readonly ConcurrentQueue<ParsedMesh>    _meshQueue       = new ConcurrentQueue<ParsedMesh>();
        private readonly ConcurrentQueue<RawColormap>   _colormapQueue   = new ConcurrentQueue<RawColormap>();
        private readonly ConcurrentQueue<RawVisibility> _visibilityQueue = new ConcurrentQueue<RawVisibility>();

        // Latest bounds (IO thread writes, main thread reads under lock).
        private Bounds       _incomingBounds;
        private bool         _hasPendingBounds;
        private readonly object _boundsLock = new object();

        // ─────────────────────────────────── Flip synchronisation ───────

        /// <summary>IO thread releases to request a flip; main thread waits non-blocking.</summary>
        private readonly SemaphoreSlim _flipSignal = new SemaphoreSlim(0, 1);
        /// <summary>Main thread releases after swap; IO thread waits after signalling flip.</summary>
        private readonly SemaphoreSlim _flipDone   = new SemaphoreSlim(0, 1);

        // ───────────────────────────────────── Scene state (main thread) ─

        private SocketReceiver       _socket;
        private DataManagerDiscovery _discovery;

    private readonly Dictionary<string, MeshBufferPair> _meshPairs = new Dictionary<string, MeshBufferPair>();
    private readonly Dictionary<string, Texture2D>      _colormaps = new Dictionary<string, Texture2D>();
    private readonly Dictionary<string, Material>       _materials = new Dictionary<string, Material>();

    private Bounds     _pvBounds   = new Bounds();
    private bool       _haveBounds = false;
    private Vector3    _coordPos   = Vector3.zero;
    private Quaternion _coordRot   = Quaternion.identity;
    private Vector3    _coordScale = Vector3.one;
    // Parent object that will hold all A/B buffer GameObjects.
    private GameObject _bufferParent;

        // ───────────────────────────────────────── Unity lifecycle ───────

        /// <summary>How long to wait for the discovery list to settle before
        /// deciding whether to auto-connect (see OnEnable/AutoConnectAfterSettle).
        /// The DataManager announces every 2s (DataManager/datamanager.py's
        /// DISCOVERY_INTERVAL) -- this gives one full interval plus margin.</summary>
        private const float AutoConnectSettleSeconds = 3f;

        private void OnEnable()
        {
            _discovery                = new DataManagerDiscovery();
            _discovery.VerboseLogging = VerboseLogging;
            _discovery.Start(DiscoveryPort);

            if (string.IsNullOrEmpty(DataManagerHost))
            {
                Debug.Log($"[ParaViewLink] MeshReceiver: no DataManagerHost configured -- " +
                          $"waiting {AutoConnectSettleSeconds}s for discovery to settle");
                StartCoroutine(AutoConnectAfterSettle());
            }
            else
            {
                StartSocket();
            }
        }

        private IEnumerator AutoConnectAfterSettle()
        {
            yield return new WaitForSeconds(AutoConnectSettleSeconds);
            List<DiscoveredDataManager> found = _discovery.GetDiscovered();
            if (found.Count == 1)
            {
                Debug.Log($"[ParaViewLink] MeshReceiver: auto-connecting to the single discovered " +
                          $"DataManager {found[0].Host}:{found[0].ClientPort}");
                DataManagerHost = found[0].Host;
                DataManagerPort = found[0].ClientPort;
                StartSocket();
            }
            else
            {
                Debug.LogWarning($"[ParaViewLink] MeshReceiver: {found.Count} DataManager(s) found -- " +
                                  "not auto-connecting (need exactly 1). Call ConnectToDataManager() manually.");
            }
        }

        private void StartSocket()
        {
            _socket                = new SocketReceiver();
            _socket.OnMessage      = HandleNetworkMessage;
            _socket.VerboseLogging = VerboseLogging;
            _socket.Start(DataManagerHost, DataManagerPort);
        }

        /// <summary>Snapshot of DataManagers currently visible via UDP discovery
        /// broadcast. No UI reads this yet -- it's the underlying capability
        /// for whatever picker/settings UI comes later.</summary>
        public List<DiscoveredDataManager> DiscoveredDataManagers =>
            _discovery?.GetDiscovered() ?? new List<DiscoveredDataManager>();

        /// <summary>Re-point the live connection at a specific DataManager, e.g.
        /// one chosen from DiscoveredDataManagers. Stops the current connection
        /// and starts a fresh one -- same connect-with-retry logic, just
        /// re-targeted. Does not persist; only lasts for this running instance.</summary>
        public void ConnectToDataManager(string host, int port)
        {
            Debug.Log($"[ParaViewLink] MeshReceiver: switching DataManager to {host}:{port}");
            DataManagerHost = host;
            DataManagerPort = port;
            _socket?.Stop();
            StartSocket();
        }

        private void OnDisable()
        {
            // Unblock the IO thread if it is waiting for the flip.
            try { _flipDone.Release(); } catch { }
            _socket?.Stop();
            _socket = null;
            _discovery?.Stop();
            _discovery = null;

            // Drain any unprocessed parsed meshes (managed memory — GC handles it).
            while (_meshQueue.TryDequeue(out _)) { }
        }

        // -----------------------------------------------------------------
        // Safety: If a MeshRenderer accidentally lives on the same GameObject
        // as MeshReceiver, disable it so the container stays invisible.
        // -----------------------------------------------------------------
        private void Awake()
        {
            Application.runInBackground = true;

            var mr = GetComponent<MeshRenderer>();
            if (mr != null)
            {
                mr.enabled = false;
                Debug.Log("[ParaViewLink] Disabled stray MeshRenderer on MeshReceiver container.");
            }
        }

        private void Update()
        {
            // Visibility is applied immediately — no flip needed.
            while (_visibilityQueue.TryDequeue(out RawVisibility v))
            {
                if (_meshPairs.TryGetValue(v.MeshName, out MeshBufferPair p))
                    p.Front.Renderer.enabled = v.Visible;
            }

            // Colormap updates are applied every frame so live preset changes
            // in ParaView are reflected immediately without waiting for a mesh resend.
            while (_colormapQueue.TryDequeue(out RawColormap cm))
            {
                Log($"[ParaViewLink] Update(): applying colormap '{cm.VariableName}' from queue");
                ApplyColormap(cm);
            }

            // Non-blocking flip check.
            if (_flipSignal.Wait(0))
            {
                try   { SwapBuffers(); }
                catch (Exception ex) { Debug.LogError($"[ParaViewLink] SwapBuffers exception: {ex}"); }
                finally { _flipDone.Release(); }  // IO thread wakes → sends ack to ParaView
            }
        }

        // ──────────────────────────────────── IO thread entry point ─────

        /// <summary>
        /// Called on the IO thread for every message.  Ack is sent to ParaView
        /// immediately after this returns — so for mesh/colormap/bounds it
        /// returns at once; for Update it blocks until the swap is done.
        /// No Unity API calls are made here.
        /// </summary>
        private void HandleNetworkMessage(int cmd, byte[] payload)
        {
            switch (cmd)
            {
                case MeshCmd.Ping:
                    Log($"[ParaViewLink] Ping: " +
                              $"{(payload.Length > 0 ? Encoding.UTF8.GetString(payload) : "(empty)")}");
                    break;

                case MeshCmd.PVMesh:
                {
                    // Parse and process all mesh data on the IO thread
                    // (normals, scalar colours, winding flip, AABB) into plain
                    // managed arrays.  No Unity Mesh API is called here.
                    // ApplyMesh() on the main thread creates the actual Mesh object.
                    var parsed = ParseMeshData(payload);
                    Log($"[ParaViewLink] Parsed mesh '{parsed.MeshName}': " +
                              $"{parsed.Vertices.Length} verts  " +
                              $"bounds={parsed.MeshBounds}  var='{parsed.VariableName}'");
                    _meshQueue.Enqueue(parsed);
                    break;
                }

                case MeshCmd.Colormap:
                {
                    var cm = ParseColormap(payload);
                    Log($"[ParaViewLink] Colormap received: var='{cm.VariableName}' samples={cm.Samples.Length}");
                    _colormapQueue.Enqueue(cm);
                    break;
                }

                case MeshCmd.Bounds:
                {
                    var b = ParseBounds(payload);
                    Log($"[ParaViewLink] Bounds received: {b}");
                    lock (_boundsLock)
                    {
                        _incomingBounds   = b;
                        _hasPendingBounds = true;
                    }
                    break;
                }

                case MeshCmd.Visibility:
                {
                    var v = ParseVisibility(payload);
                    Log($"[ParaViewLink] Visibility received: '{v.MeshName}' → {v.Visible}");
                    _visibilityQueue.Enqueue(v);
                    break;
                }

                case MeshCmd.Update:
                    Log($"[ParaViewLink] Update received — signalling main thread flip");
                    _flipSignal.Release();
                    _flipDone.Wait();
                    Log($"[ParaViewLink] Update: flip complete, ack sent to ParaView");
                    break;
            }
        }

        // ─────────────────────────── Off-thread mesh parsing ────────────

        /// <summary>
        /// Parses the PVMesh payload into plain managed arrays on the IO thread.
        /// All CPU work (normal computation, scalar normalisation, winding flip,
        /// AABB) is done here.  No Unity Mesh API is called.
        /// ApplyMesh() on the main thread converts the result to a Unity Mesh.
        /// </summary>
        private static ParsedMesh ParseMeshData(byte[] data)
        {
            int offset = 0;

            int numVerts     = ReadInt32(data, ref offset);
            int numTris      = ReadInt32(data, ref offset);
            int scalarLoc    = ReadInt32(data, ref offset);
            int hasNormals   = ReadInt32(data, ref offset);
            int colorNameLen = ReadInt32(data, ref offset);
            int meshNameLen  = ReadInt32(data, ref offset);

            // ── Parse positions ──────────────────────────────────────────
            var pos = new Vector3[numVerts];
            for (int i = 0; i < numVerts; i++)
                pos[i] = new Vector3(ReadFloat(data, ref offset),
                                     ReadFloat(data, ref offset),
                                     ReadFloat(data, ref offset));

            // ── Parse normals from ParaView (optional) ───────────────────
            Vector3[] pvNormals = null;
            if (hasNormals == 1)
            {
                pvNormals = new Vector3[numVerts];
                for (int i = 0; i < numVerts; i++)
                    pvNormals[i] = new Vector3(ReadFloat(data, ref offset),
                                               ReadFloat(data, ref offset),
                                               ReadFloat(data, ref offset));
            }

            // ── Raw indices (before winding flip) ────────────────────────
            var rawIdx = new int[numTris * 3];
            for (int i = 0; i < rawIdx.Length; i++)
                rawIdx[i] = ReadInt32(data, ref offset);

            // ── Scalars ───────────────────────────────────────────────────
            float[] scalars = null;
            float scMin = 0f, scMax = 1f;
            if (scalarLoc >= 0)
            {
                int n = scalarLoc == 0 ? numVerts : numTris;
                scalars = new float[n];
                for (int i = 0; i < n; i++)
                    scalars[i] = ReadFloat(data, ref offset);
                scMin = ReadFloat(data, ref offset);
                scMax = ReadFloat(data, ref offset);
            }

            string varName  = colorNameLen > 0 ? Encoding.UTF8.GetString(data, offset, colorNameLen) : "";
            offset += colorNameLen;
            string meshName = meshNameLen  > 0 ? Encoding.UTF8.GetString(data, offset, meshNameLen)  : "ParaViewMesh";

            // ── Build MeshVertex array (all CPU work — no Mesh API) ───────
            var verts = new MeshVertex[numVerts];
            float span = scMax - scMin;

            // Per-point scalar → byte colour
            if (scalarLoc == 0 && scalars != null)
            {
                for (int i = 0; i < numVerts; i++)
                {
                    float t = span > 1e-10f ? (scalars[i] - scMin) / span : 0f;
                    verts[i] = new MeshVertex { X = pos[i].x, Y = pos[i].y, Z = pos[i].z,
                                                R = (byte)(Mathf.Clamp01(t) * 255f), G = 0, B = 0, A = 255 };
                }
            }
            else
            {
                for (int i = 0; i < numVerts; i++)
                    verts[i] = new MeshVertex { X = pos[i].x, Y = pos[i].y, Z = pos[i].z,
                                                R = 0, G = 0, B = 0, A = 255 };
            }

            // ── Index buffer with winding flip (ParaView RH → Unity LH) ─
            int   iCount  = numTris * 3;
            var   indices = new int[iCount];
            for (int i = 0; i < iCount; i += 3)
            {
                indices[i]     = rawIdx[i];
                indices[i + 1] = rawIdx[i + 2]; // swap 1↔2
                indices[i + 2] = rawIdx[i + 1];
            }

            // ── Normals ───────────────────────────────────────────────────
            if (pvNormals != null)
            {
                for (int i = 0; i < numVerts; i++)
                {
                    var v = verts[i];
                    v.NX = pvNormals[i].x; v.NY = pvNormals[i].y; v.NZ = pvNormals[i].z;
                    verts[i] = v;
                }
            }
            else
            {
                // Accumulate area-weighted face normals from the flipped winding.
                var accum = new Vector3[numVerts];
                for (int t = 0; t < numTris; t++)
                {
                    int a = indices[t * 3], b2 = indices[t * 3 + 1], c = indices[t * 3 + 2];
                    Vector3 n = Vector3.Cross(pos[b2] - pos[a], pos[c] - pos[a]);
                    accum[a] += n; accum[b2] += n; accum[c] += n;
                }
                for (int i = 0; i < numVerts; i++)
                {
                    Vector3 n = accum[i];
                    float len = Mathf.Sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    var v = verts[i];
                    if (len > 1e-10f) { v.NX = n.x / len; v.NY = n.y / len; v.NZ = n.z / len; }
                    else              { v.NX = 0f;         v.NY = 1f;        v.NZ = 0f;        }
                    verts[i] = v;
                }
            }

            // ── Per-cell scalar broadcast ─────────────────────────────────
            if (scalarLoc == 1 && scalars != null)
            {
                for (int t = 0; t < numTris && t < scalars.Length; t++)
                {
                    float s = span > 1e-10f ? (scalars[t] - scMin) / span : 0f;
                    byte  b = (byte)(Mathf.Clamp01(s) * 255f);
                    int i0 = rawIdx[t*3], i1 = rawIdx[t*3+1], i2 = rawIdx[t*3+2];
                    var v0 = verts[i0]; v0.R = b; verts[i0] = v0;
                    var v1 = verts[i1]; v1.R = b; verts[i1] = v1;
                    var v2 = verts[i2]; v2.R = b; verts[i2] = v2;
                }
            }

            // ── AABB ──────────────────────────────────────────────────────
            float xMin = float.MaxValue, xMax = float.MinValue;
            float yMin = float.MaxValue, yMax = float.MinValue;
            float zMin = float.MaxValue, zMax = float.MinValue;
            for (int i = 0; i < numVerts; i++)
            {
                var v = verts[i];
                if (v.X < xMin) xMin = v.X; if (v.X > xMax) xMax = v.X;
                if (v.Y < yMin) yMin = v.Y; if (v.Y > yMax) yMax = v.Y;
                if (v.Z < zMin) zMin = v.Z; if (v.Z > zMax) zMax = v.Z;
            }
            var meshBounds = new Bounds();
            meshBounds.SetMinMax(new Vector3(xMin, yMin, zMin), new Vector3(xMax, yMax, zMax));

            return new ParsedMesh
            {
                MeshName     = meshName,
                Vertices     = verts,
                Indices      = indices,
                MeshBounds   = meshBounds,
                VariableName = varName,
            };
        }

        // ───────────────────────── Main-thread buffer swap ───────────────

        private void SwapBuffers()
        {
            // 1. Apply updated bounds.
            bool boundsChanged = false;
            lock (_boundsLock)
            {
                if (_hasPendingBounds)
                {
                    _pvBounds         = _incomingBounds;
                    _haveBounds       = true;
                    _hasPendingBounds = false;
                    boundsChanged     = true;
                }
            }
            if (boundsChanged && AutoComputeTransform)
                ComputeCoordTransform();

            // Provisional bounds from first mesh if no BOUNDS message has arrived.
            if (!_haveBounds && AutoComputeTransform && _meshQueue.TryPeek(out ParsedMesh first))
            {
                _pvBounds   = first.MeshBounds;
                _haveBounds = true;
                Log($"[ParaViewLink] No BOUNDS message — fitting from mesh extents: {_pvBounds}");
                ComputeCoordTransform();
            }

            // 2. Flush any colormaps that arrived just before this flip
            //    (Update() drains the queue each frame; this catches the rare
            //    case where a colormap and mesh arrive in the same network burst).
            while (_colormapQueue.TryDequeue(out RawColormap cm))
                ApplyColormap(cm);

            // 3. Upload pre-built mesh data and swap front↔back.
            int meshCount = 0;
            while (_meshQueue.TryDequeue(out ParsedMesh rm))
            {
                ApplyMesh(rm);
                meshCount++;
            }
            Log($"[ParaViewLink] SwapBuffers: applied {meshCount} mesh(es). " +
                      $"Pairs: {_meshPairs.Count}  " +
                      $"coordScale: {_coordScale}  coordPos: {_coordPos}");
        }

        private void ApplyMesh(ParsedMesh pm)
        {
        if (!_meshPairs.TryGetValue(pm.MeshName, out MeshBufferPair pair))
        {
            pair = CreatePair(pm.MeshName);
            _meshPairs[pm.MeshName] = pair;
        }

            // Upload parsed data to the back-buffer Mesh on the main thread.
            Mesh mesh = pair.Back.Mesh;
            mesh.Clear();

            // Use the MeshDataArray API for an efficient single upload.
            var dataArray = Mesh.AllocateWritableMeshData(1);
            var md        = dataArray[0];

            int vCount = pm.Vertices.Length;
            int iCount = pm.Indices.Length;
            bool useU16 = vCount <= 65535;

            md.SetVertexBufferParams(vCount, k_Layout);
            md.SetIndexBufferParams(iCount, useU16 ? IndexFormat.UInt16 : IndexFormat.UInt32);

            // Copy pre-processed vertices (positions, normals, colours) into native buffer.
            var nativeVerts = md.GetVertexData<MeshVertex>();
            for (int i = 0; i < vCount; i++)
                nativeVerts[i] = pm.Vertices[i];

            // Copy pre-flipped indices.
            if (useU16)
            {
                var nativeIdx = md.GetIndexData<ushort>();
                for (int i = 0; i < iCount; i++)
                    nativeIdx[i] = (ushort)pm.Indices[i];
            }
            else
            {
                var nativeIdx = md.GetIndexData<uint>();
                for (int i = 0; i < iCount; i++)
                    nativeIdx[i] = (uint)pm.Indices[i];
            }

            md.subMeshCount = 1;
            md.SetSubMesh(0, new SubMeshDescriptor(0, iCount, MeshTopology.Triangles)
                { bounds = pm.MeshBounds }, k_ApplyFlags);

            Mesh.ApplyAndDisposeWritableMeshData(dataArray, mesh, k_ApplyFlags);
            mesh.bounds = pm.MeshBounds;

            if (!string.IsNullOrEmpty(pm.VariableName))
            {
                var mat = EnsureVarMaterial(pm.VariableName);
                pair.Back.Renderer.sharedMaterial = mat;
                pair.VariableName = pm.VariableName;
                Log($"[ParaViewLink] ApplyMesh '{pm.MeshName}': set back-buffer material for var='{pm.VariableName}'" +
                          $"  mat={mat?.name ?? "NULL"}" +
                          $"  colormap tex={(mat != null ? mat.GetTexture("_Colormap")?.name ?? "NULL" : "N/A")}");
            }
            else
            {
                Log($"[ParaViewLink] ApplyMesh '{pm.MeshName}': no VariableName — material unchanged");
            }

            pair.Back.Go.transform.SetPositionAndRotation(_coordPos, _coordRot);
            pair.Back.Go.transform.localScale = _coordScale;

            pair.Swap();
        }

        // ─────────────────────── Buffer pair allocation ──────────────────


        private MeshBufferPair CreatePair(string name)
        {
            var pair = new MeshBufferPair
            {
                Front = MakeInstance(name + "_A"),
                Back  = MakeInstance(name + "_B")
            };

            // Create a dedicated parent for this mesh pair.
            var pairParent = new GameObject(name + "_Pair");
            pairParent.transform.SetParent(transform, false);   // child of the receiver

            // Parent the front and back GameObjects under the pair parent.
            pair.Front.Go.transform.SetParent(pairParent.transform, false);
            pair.Back.Go.transform.SetParent(pairParent.transform, false);

            return pair;
        }

        private MeshInstance MakeInstance(string name)
        {
            var go       = new GameObject(name);
            go.transform.SetParent(_bufferParent?.transform, false);
            var filter   = go.AddComponent<MeshFilter>();
            var renderer = go.AddComponent<MeshRenderer>();
            var mesh     = new Mesh { name = name };
            filter.sharedMesh = mesh;
            go.SetActive(false);
            return new MeshInstance { Go = go, Filter = filter, Renderer = renderer, Mesh = mesh };
        }

        // ──────────────────────────────────── Colormap (main thread) ────

        private void ApplyColormap(RawColormap cm)
        {
            // Always create a new Texture2D so Unity sees a genuinely new object
            // on the material and flushes any cached GPU state.
            if (_colormaps.TryGetValue(cm.VariableName, out Texture2D old) && old != null)
                Destroy(old);

            int w = cm.Samples.Length;
            var tex = new Texture2D(w, 1, TextureFormat.RGBA32, false)
            {
                name       = $"PVColormap_{cm.VariableName}",
                wrapMode   = TextureWrapMode.Clamp,
                filterMode = FilterMode.Bilinear,
            };
            tex.SetPixels(cm.Samples);
            tex.Apply();
            _colormaps[cm.VariableName] = tex;
        var mat = EnsureVarMaterial(cm.VariableName);
        if (mat != null)
        {
            mat.SetTexture("_Colormap", tex);
            // Re-assign the material on any live renderers using this variable
            // so the updated texture is immediately visible without waiting for
            // the next mesh packet.
            bool anyUpdated = false;
            foreach (var pair in _meshPairs.Values)
            {
                Log($"[ParaViewLink] ApplyColormap: checking pair '{pair.Front.Go.name}'" +
                          $"  pair.VariableName='{pair.VariableName}'  looking for='{cm.VariableName}'");
                if (pair.VariableName == cm.VariableName)
                {
                    pair.Front.Renderer.sharedMaterial = mat;
                    anyUpdated = true;
                    Log($"[ParaViewLink] ApplyColormap: reassigned material on '{pair.Front.Go.name}'");
                }
            }
            if (!anyUpdated)
                Log($"[ParaViewLink] ApplyColormap '{cm.VariableName}': no live renderer matched (colormap cached, will apply at next mesh swap)");
        }
        else
        {
            Debug.LogWarning("[ParaViewLink] BaseScalarMaterial not set – cannot apply colormap for '" + cm.VariableName + "'.");
        }
        Log($"[ParaViewLink] Colormap '{cm.VariableName}' [{cm.Min:G4}, {cm.Max:G4}]");
        }

        private Material EnsureVarMaterial(string varName)
        {
        if (_materials.TryGetValue(varName, out Material mat) && mat != null)
            return mat;
        // If the base material is not set, create a simple fallback.
        if (BaseScalarMaterial == null)
        {
            Debug.LogWarning("[ParaViewLink] BaseScalarMaterial not assigned – creating fallback material.");
            // Use Unity's standard unlit shader as a simple placeholder.
            var fallbackShader = Shader.Find("Unlit/Texture");
            if (fallbackShader == null)
            {
                Debug.LogError("[ParaViewLink] Could not find Unlit/Texture shader for fallback.");
                return null;
            }
            mat = new Material(fallbackShader) { name = $"M_{varName}_fallback" };
        }
        else
        {
            mat = new Material(BaseScalarMaterial) { name = $"M_{varName}" };
        }
            if (_colormaps.TryGetValue(varName, out Texture2D tex) && tex != null)
                mat.SetTexture("_Colormap", tex);
            _materials[varName] = mat;
            return mat;
        }

        // ─────────────────────── Coordinate transform (main thread) ─────

        private void ComputeCoordTransform()
        {
            var container = GetComponent<SimContainerObject>();
            if (container == null)
            {
                Debug.LogWarning("[ParaViewLink] No SimContainerObject on this GameObject.");
                return;
            }
            Vector3 pvSize = _pvBounds.size;
            if (pvSize.x < 1e-10f || pvSize.y < 1e-10f || pvSize.z < 1e-10f)
            {
                Debug.LogWarning("[ParaViewLink] Degenerate PV bounds — skipping CoordTransform.");
                return;
            }
            Vector3 ws  = container.WorldSize;
            _coordScale = new Vector3(ws.x / pvSize.x, ws.y / pvSize.y, ws.z / pvSize.z);
            _coordPos   = container.WorldCenter - Vector3.Scale(_pvBounds.center, _coordScale);
            _coordRot   = container.transform.rotation;

            Log($"[ParaViewLink] CoordTransform — " +
                      $"pvSize:{pvSize} containerSize:{ws} scale:{_coordScale} pos:{_coordPos}");

            // Reposition any currently live objects immediately.
            foreach (var pair in _meshPairs.Values)
            {
                pair.Front.Go.transform.SetPositionAndRotation(_coordPos, _coordRot);
                pair.Front.Go.transform.localScale = _coordScale;
            }
        }

        // ─────────────────────── IO-thread parsers (no Unity API) ────────

        private static RawColormap ParseColormap(byte[] data)
        {
            int offset  = 0;
            int nameLen = ReadInt32(data, ref offset);
            string name = Encoding.UTF8.GetString(data, offset, nameLen);
            offset += nameLen;
            float min = ReadFloat(data, ref offset), max = ReadFloat(data, ref offset);
            int n = ReadInt32(data, ref offset);
            var samples = new Color[n];
            for (int i = 0; i < n; i++)
                samples[i] = new Color(ReadFloat(data, ref offset),
                                       ReadFloat(data, ref offset),
                                       ReadFloat(data, ref offset), 1f);
            return new RawColormap { VariableName = name, Min = min, Max = max, Samples = samples };
        }

        private static Bounds ParseBounds(byte[] data)
        {
            int offset = 0;
            float x0 = ReadFloat(data, ref offset), x1 = ReadFloat(data, ref offset);
            float y0 = ReadFloat(data, ref offset), y1 = ReadFloat(data, ref offset);
            float z0 = ReadFloat(data, ref offset), z1 = ReadFloat(data, ref offset);
            var b = new Bounds();
            b.SetMinMax(new Vector3(x0, y0, z0), new Vector3(x1, y1, z1));
            return b;
        }

        private static RawVisibility ParseVisibility(byte[] data)
        {
            int offset  = 0;
            int nameLen = ReadInt32(data, ref offset);
            string name = Encoding.UTF8.GetString(data, offset, nameLen);
            offset += nameLen;
            return new RawVisibility { MeshName = name, Visible = ReadInt32(data, ref offset) != 0 };
        }

        private static int   ReadInt32(byte[] d, ref int o) { int   v = BitConverter.ToInt32  (d, o); o += 4; return v; }
        private static float ReadFloat (byte[] d, ref int o) { float v = BitConverter.ToSingle (d, o); o += 4; return v; }
    }
}
