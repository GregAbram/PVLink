using System;
using System.Collections;
using System.Collections.Generic;
using System.Threading.Tasks;
using DataManager;
using UnityEngine;
using UnityEngine.Rendering;

namespace ParaViewLink
{
    /// <summary>
    /// Shows a DataManager project (SPEC.md in the DataManager repository) in
    /// the scene: every KeyData item becomes a mesh under this SimContainer,
    /// coloured by its colour-by array through the ScalarField material.
    ///
    /// Data comes from a DataManager over the network (DataManagerHost, or
    /// discovery when empty) or, with LocalStorePath set, straight from a
    /// store on this device with no network.
    ///
    /// Each batch is applied whole: meshes are built on a worker thread,
    /// uploaded into hidden back buffers, revealed together in one frame, and
    /// only then acknowledged (SPEC.md 9).
    /// </summary>
    [AddComponentMenu("ParaViewLink/Mesh Receiver")]
    public class MeshReceiver : MonoBehaviour
    {
        // ───────────────────────────────────────────────── Inspector ────

        [Header("DataManager")]
        [Tooltip("DataManager address. Empty: use discovery and connect when exactly one DataManager is announcing.")]
        public string DataManagerHost = "";
        public int DataManagerPort = 9010;
        public int DiscoveryPort = 9011;
        [Tooltip("Project to watch. Empty: the DataManager's default (the one most recently written).")]
        public string Project = "";
        [Tooltip("Ask for compressed data. Worth it on Wi-Fi or cellular; not for a DataManager on this machine.")]
        public bool Compress = true;
        [Tooltip("Play a store on this device instead of connecting (in-process DataManager).")]
        public string LocalStorePath = "";
        [Tooltip("Start playing when connected (otherwise it shows the latest time, or follows a live source).")]
        public bool PlayOnStart = false;
        [Tooltip("Playback rate in timesteps per second for PlayOnStart; 0 = as fast as clients acknowledge.")]
        public float PlaybackRate = 5f;

        [Header("Coordinates")]
        [Tooltip("Fit the domain bounds into this SimContainer.")]
        public bool AutoComputeTransform = true;

        [Header("Appearance")]
        [Tooltip("Base material using the ScalarField shader. One copy is made per variable.")]
        public Material BaseScalarMaterial;
        [Tooltip("Colour of items with no colour-by array.")]
        public Color SolidColor = new Color(0.8f, 0.8f, 0.8f);
        [Tooltip("Half-size of drawn points, as a fraction of the domain diagonal.")]
        public float PointSize = 0.004f;
        [Tooltip("Radius of drawn lines, as a fraction of the domain diagonal.")]
        public float LineWidth = 0.0015f;

        [Header("Debug")]
        public bool VerboseLogging = false;

        // ──────────────────────────────────────────── Public state ──────

        public bool IsConnected => _client != null && _client.IsConnected;
        public bool IsConnecting => _client != null && !_client.IsConnected || _discovering;
        public DMPlaybackState State => _client != null ? _client.State : default;
        public double CurrentTime { get; private set; }
        public DMClient Client => _client;

        public void Play() => _client?.Play();
        public void Pause() => _client?.Pause();
        public void Seek(double t) => _client?.Seek(t);
        public void Step(int delta) => _client?.Step(delta);
        public void SetRate(double r) => _client?.SetRate(r);

        // ───────────────────────────────────────── Scene state ──────────

        private sealed class MeshInstance
        {
            public GameObject Go;
            public MeshRenderer Renderer;
            public Mesh Mesh;
        }

        private sealed class MeshPair
        {
            public GameObject Parent;
            public MeshInstance Front, Back;
            public BuiltMesh Shown;

            public void Swap()
            {
                (Front, Back) = (Back, Front);
                Front.Go.SetActive(true);
                Back.Go.SetActive(false);
            }
        }

        private DMClient _client;
        private bool _discovering;
        private bool _startedPlaying;
        private readonly Dictionary<string, MeshPair> _pairs = new Dictionary<string, MeshPair>();
        private readonly Dictionary<string, DMColormap> _colormaps = new Dictionary<string, DMColormap>();
        private readonly Dictionary<string, Texture2D> _textures = new Dictionary<string, Texture2D>();
        private readonly Dictionary<string, Material> _materials = new Dictionary<string, Material>();
        private readonly Dictionary<string, bool> _visible = new Dictionary<string, bool>();
        private Material _solidMaterial;
        private Bounds _pvBounds;          // Unity frame
        private bool _haveBounds;
        private Vector3 _coordPos = Vector3.zero, _coordScale = Vector3.one;

        private DMBatch _pendingBatch;
        private Task<List<BuiltMesh>> _pendingBuild;

        private void Log(string m) { if (VerboseLogging) Debug.Log("[ParaViewLink] " + m); }

        private static void SafeDestroy(UnityEngine.Object o)
        {
            if (Application.isPlaying) Destroy(o); else DestroyImmediate(o);
        }

        // ───────────────────────────────────────── Lifecycle ────────────

        private void Awake()
        {
            Application.runInBackground = true;
            var mr = GetComponent<MeshRenderer>();
            if (mr != null) mr.enabled = false;   // the container cube is an editor handle only
        }

        private void OnEnable()
        {
            if (!string.IsNullOrEmpty(LocalStorePath))
                Attach(DMClient.OpenStore(LocalStorePath, Project, PlaybackRate));
            else if (!string.IsNullOrEmpty(DataManagerHost))
                ConnectToDataManager(DataManagerHost, DataManagerPort);
            else
                StartCoroutine(DiscoverAndConnect());
        }

        private void OnDisable()
        {
            StopAllCoroutines();
            _client?.Dispose();
            _client = null;
            _pendingBatch = null;
            _pendingBuild = null;
            _discovering = false;
        }

        private IEnumerator DiscoverAndConnect()
        {
            _discovering = true;
            int lastCount = -1;
            while (_client == null)
            {
                var t = Task.Run(() => DMClient.Discover(DiscoveryPort, 3000));
                while (!t.IsCompleted) yield return null;
                var found = t.Result;
                if (found.Count == 1)
                {
                    Debug.Log($"[ParaViewLink] connecting to the discovered DataManager {found[0]}");
                    _discovering = false;
                    ConnectToDataManager(found[0].Address, found[0].Port);
                    yield break;
                }
                if (found.Count != lastCount)
                {
                    lastCount = found.Count;
                    Debug.LogWarning($"[ParaViewLink] {found.Count} DataManager(s) announcing; need exactly one " +
                                     "to auto-connect (set DataManagerHost, or call ConnectToDataManager). Still looking.");
                }
            }
            _discovering = false;
        }

        /// <summary>Lists DataManagers announcing on the LAN (blocks for timeoutMs).</summary>
        public List<DMAnnouncement> DiscoverDataManagers(int timeoutMs = 2500) =>
            DMClient.Discover(DiscoveryPort, timeoutMs);

        /// <summary>Connects (or re-connects) to a specific DataManager.</summary>
        public void ConnectToDataManager(string host, int port)
        {
            DataManagerHost = host;
            DataManagerPort = port;
            _client?.Dispose();
            Attach(DMClient.Connect(host, port, Project, SystemInfo.deviceName, compress: Compress));
        }

        private void Attach(DMClient c)
        {
            _client = c;
            _startedPlaying = false;
            c.Connected += p => Debug.Log($"[ParaViewLink] connected to DataManager, project '{p}'");
            c.Disconnected += r => Debug.Log($"[ParaViewLink] DataManager connection lost ({r}); reconnecting");
            c.Rejected += (code, m) => Debug.LogWarning($"[ParaViewLink] DataManager refused the connection: {m}");
            c.Error += (code, m) => Debug.LogWarning($"[ParaViewLink] DataManager: {m}");
            c.PlaybackStateChanged += s =>
            {
                if (PlayOnStart && !_startedPlaying)
                {
                    _startedPlaying = true;
                    c.SetRate(PlaybackRate);
                    c.Play();
                }
            };
            c.BatchReady += OnBatch;
        }

        private void Update()
        {
            if (_client == null) return;
            if (_pendingBuild != null)
            {
                if (!_pendingBuild.IsCompleted) return;
                ApplyPendingBatch();
            }
            _client.Dispatch(1);
        }

        // ──────────────────────────────────────── Batches ───────────────

        private void OnBatch(DMBatch b)
        {
            // Colormaps in effect after this batch, for normalising colour values.
            var colormaps = new Dictionary<string, DMColormap>(_colormaps);
            foreach (var it in b.Items.Values)
                if (it.Colormap != null) colormaps[it.Name] = it.Colormap;

            Bounds domain = _pvBounds;
            bool haveDomain = _haveBounds;
            foreach (var it in b.Items.Values)
                if (it.Bounds.HasValue) { domain = DMConvert.Bounds(it.Bounds.Value); haveDomain = true; }

            // What to (re)build: new or changed KeyData, plus shown items whose
            // colormap range changed in this batch.
            var work = new List<KeyValuePair<string, DMKeyData>>();
            foreach (var it in b.Items.Values)
                if (it.KeyData != null) work.Add(new KeyValuePair<string, DMKeyData>(it.Name, it.KeyData));
                else if (it.Type == "KeyData" && it.DecodeError != null)
                    Debug.LogWarning($"[ParaViewLink] {it.Key}: {it.DecodeError}");
            foreach (var kv in _pairs)
            {
                var shown = kv.Value.Shown;
                if (shown?.ColorBy == null || b.Items.ContainsKey("KeyData/" + kv.Key)) continue;
                if (colormaps.TryGetValue(shown.ColorBy, out var cm) && _colormaps.TryGetValue(shown.ColorBy, out var old)
                    && (cm.Min != old.Min || cm.Max != old.Max))
                    work.Add(new KeyValuePair<string, DMKeyData>(kv.Key, shown.Source));
            }

            if (!haveDomain && work.Count > 0)
            {
                domain = DMConvert.Bounds(work[0].Value.Bounds);
                foreach (var w in work) domain.Encapsulate(DMConvert.Bounds(w.Value.Bounds));
            }
            float diag = haveDomain || work.Count > 0 ? domain.size.magnitude : 1f;
            float pointR = PointSize * diag, lineR = LineWidth * diag;

            _pendingBatch = b;
            _pendingBuild = Task.Run(() =>
            {
                var built = new List<BuiltMesh>();
                foreach (var w in work)
                {
                    var kd = w.Value;
                    Vector2? range = null;
                    if (kd.ColorBy != null && colormaps.TryGetValue(kd.ColorBy, out var cm))
                        range = new Vector2(cm.Min, cm.Max);
                    float r = kd.Topology == DMTopology.Points ? pointR : lineR;
                    var m = KeyDataGeometry.Build(w.Key, kd, r, range);
                    if (m != null) built.Add(m);
                }
                return built;
            });
        }

        private void ApplyPendingBatch()
        {
            var b = _pendingBatch;
            var task = _pendingBuild;
            _pendingBatch = null;
            _pendingBuild = null;
            var dropped = new List<string>();
            if (task.IsFaulted)
            {
                Debug.LogError($"[ParaViewLink] building batch {b.Id}: {task.Exception}");
                foreach (var it in b.Items.Values) if (it.Type == "KeyData") dropped.Add(it.Key);
            }

            // 1. Removals.
            foreach (var key in b.Removed)
            {
                int slash = key.IndexOf('/');
                string type = key.Substring(0, slash), name = key.Substring(slash + 1);
                if (type == "KeyData" && _pairs.TryGetValue(name, out var pair))
                {
                    SafeDestroy(pair.Parent);
                    _pairs.Remove(name);
                }
                else if (type == "Visibility") _visible.Remove(name);
                else if (type == "Colormap") _colormaps.Remove(name);
            }

            // 2. Domain, colormaps, visibility.
            foreach (var it in b.Items.Values)
            {
                if (it.Bounds.HasValue)
                {
                    _pvBounds = DMConvert.Bounds(it.Bounds.Value);
                    _haveBounds = true;
                    ComputeCoordTransform();
                }
                else if (it.Colormap != null) ApplyColormap(it.Name, it.Colormap);
                else if (it.Visible.HasValue) _visible[it.Name] = it.Visible.Value;
            }

            // 3. Meshes into back buffers, then reveal everything together.
            var swapped = new List<MeshPair>();
            if (!task.IsFaulted)
            {
                var built = task.Result;
                if (!_haveBounds && built.Count > 0 && AutoComputeTransform)
                {
                    _pvBounds = built[0].Bounds;
                    foreach (var m in built) _pvBounds.Encapsulate(m.Bounds);
                    _haveBounds = true;
                    ComputeCoordTransform();
                }
                foreach (var m in built)
                {
                    try
                    {
                        if (!_pairs.TryGetValue(m.Name, out var pair)) _pairs[m.Name] = pair = CreatePair(m.Name);
                        Upload(pair.Back.Mesh, m);
                        pair.Back.Renderer.sharedMaterial = m.ColorBy != null ? VarMaterial(m.ColorBy) : SolidMaterial();
                        ApplyCoordTransform(pair.Back.Go.transform);
                        pair.Shown = m;
                        swapped.Add(pair);
                    }
                    catch (Exception e)
                    {
                        Debug.LogError($"[ParaViewLink] {m.Name}: {e.Message}");
                        dropped.Add("KeyData/" + m.Name);
                    }
                }
            }
            foreach (var p in swapped) p.Swap();
            foreach (var kv in _pairs)
                kv.Value.Front.Renderer.enabled = !_visible.TryGetValue(kv.Key, out var v) || v;

            CurrentTime = b.Time;
            Log($"batch {b.Id} t={b.Time}: {swapped.Count} mesh(es) updated, {b.Removed.Count} removed, {_pairs.Count} shown");
            _client.Ack(b, dropped);
        }

        // ──────────────────────────────────── Meshes ────────────────────

        private const MeshUpdateFlags ApplyFlags =
            MeshUpdateFlags.DontValidateIndices | MeshUpdateFlags.DontRecalculateBounds | MeshUpdateFlags.DontResetBoneBounds;

        private static void Upload(Mesh mesh, BuiltMesh m)
        {
            mesh.Clear();
            var array = Mesh.AllocateWritableMeshData(1);
            var md = array[0];
            bool u16 = m.Vertices.Length <= 65535;
            md.SetVertexBufferParams(m.Vertices.Length, ScalarVertex.Layout);
            md.SetIndexBufferParams(m.Indices.Length, u16 ? IndexFormat.UInt16 : IndexFormat.UInt32);
            md.GetVertexData<ScalarVertex>().CopyFrom(m.Vertices);
            if (u16)
            {
                var ix = md.GetIndexData<ushort>();
                for (int i = 0; i < m.Indices.Length; i++) ix[i] = (ushort)m.Indices[i];
            }
            else
            {
                md.GetIndexData<int>().CopyFrom(m.Indices);
            }
            md.subMeshCount = 1;
            md.SetSubMesh(0, new SubMeshDescriptor(0, m.Indices.Length) { bounds = m.Bounds, vertexCount = m.Vertices.Length }, ApplyFlags);
            Mesh.ApplyAndDisposeWritableMeshData(array, mesh, ApplyFlags);
            mesh.bounds = m.Bounds;
        }

        private MeshPair CreatePair(string name)
        {
            var parent = new GameObject(name + "_Pair");
            parent.transform.SetParent(transform, false);
            var pair = new MeshPair { Parent = parent, Front = MakeInstance(name + "_A", parent), Back = MakeInstance(name + "_B", parent) };
            return pair;
        }

        private static MeshInstance MakeInstance(string name, GameObject parent)
        {
            var go = new GameObject(name);
            go.transform.SetParent(parent.transform, false);
            go.AddComponent<MeshFilter>().sharedMesh = new Mesh { name = name };
            var r = go.AddComponent<MeshRenderer>();
            go.SetActive(false);
            return new MeshInstance { Go = go, Renderer = r, Mesh = go.GetComponent<MeshFilter>().sharedMesh };
        }

        // ──────────────────────────────────── Colour ────────────────────

        private void ApplyColormap(string variable, DMColormap cm)
        {
            _colormaps[variable] = cm;
            if (_textures.TryGetValue(variable, out var old) && old != null) SafeDestroy(old);
            var tex = new Texture2D(cm.Samples.Length, 1, TextureFormat.RGBA32, false)
            {
                name = "PVColormap_" + variable, wrapMode = TextureWrapMode.Clamp, filterMode = FilterMode.Bilinear,
            };
            tex.SetPixels(cm.Samples);
            tex.Apply();
            _textures[variable] = tex;
            VarMaterial(variable)?.SetTexture("_Colormap", tex);
        }

        private Material VarMaterial(string variable)
        {
            if (_materials.TryGetValue(variable, out var m) && m != null) return m;
            m = NewMaterial("M_" + variable);
            if (m == null) return null;
            if (_textures.TryGetValue(variable, out var tex) && tex != null) m.SetTexture("_Colormap", tex);
            _materials[variable] = m;
            return m;
        }

        private Material SolidMaterial()
        {
            if (_solidMaterial != null) return _solidMaterial;
            _solidMaterial = NewMaterial("M_Solid");
            if (_solidMaterial == null) return null;
            var tex = new Texture2D(1, 1, TextureFormat.RGBA32, false) { name = "PVSolid" };
            tex.SetPixel(0, 0, SolidColor);
            tex.Apply();
            _solidMaterial.SetTexture("_Colormap", tex);
            return _solidMaterial;
        }

        private Material NewMaterial(string name)
        {
            if (BaseScalarMaterial != null) return new Material(BaseScalarMaterial) { name = name };
            var shader = Shader.Find("ParaViewLink/ScalarField") ?? Shader.Find("Unlit/Texture");
            if (shader == null)
            {
                Debug.LogError("[ParaViewLink] BaseScalarMaterial not set and no fallback shader found");
                return null;
            }
            return new Material(shader) { name = name };
        }

        // ──────────────────────────────── Coordinates ───────────────────

        // Maps the domain onto the container's local unit cube; the container's
        // Transform supplies world position, rotation and size.
        private void ComputeCoordTransform()
        {
            if (!AutoComputeTransform) return;
            Vector3 size = _pvBounds.size;
            for (int i = 0; i < 3; i++) if (size[i] < 1e-10f) size[i] = Mathf.Max(size.x, size.y, size.z, 1e-10f);
            _coordScale = new Vector3(1f / size.x, 1f / size.y, 1f / size.z);
            _coordPos = -Vector3.Scale(_pvBounds.center, _coordScale);
            foreach (var p in _pairs.Values)
            {
                ApplyCoordTransform(p.Front.Go.transform);
                ApplyCoordTransform(p.Back.Go.transform);
            }
        }

        private void ApplyCoordTransform(Transform t)
        {
            t.localPosition = _coordPos;
            t.localRotation = Quaternion.identity;
            t.localScale = _coordScale;
        }
    }
}
