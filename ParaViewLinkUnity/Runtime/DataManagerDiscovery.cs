using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using UnityEngine;

namespace ParaViewLink
{
    /// <summary>One DataManager currently visible on the LAN.</summary>
    public struct DiscoveredDataManager
    {
        public string Host;
        public int    ClientPort;
        /// <summary>Empty if the DataManager hasn't received a PROJECT
        /// message yet (live mode with no ParaView source connected yet).</summary>
        public string ProjectName;
    }

    /// <summary>
    /// Background-thread UDP listener for PVLink DataManager broadcast
    /// announcements. Maintains a thread-safe, continuously-pruned list of
    /// what's currently on the LAN.
    ///
    /// Wire format (plain text, UDP, separate from the TCP mesh-streaming
    /// protocol -- see DataManager/datamanager.py's announce_loop):
    ///   PVLINK-DISCOVERY 1
    ///   client_port=9010
    ///   project=Sphere
    ///
    /// A DataManager is identified by (source IP of the packet, client_port)
    /// -- it never needs to report its own IP. An entry is pruned if not
    /// re-announced within StaleAfterSeconds (~3 missed broadcasts), so a
    /// DataManager that exits eventually drops off the list on its own.
    ///
    /// This class only builds the discovered list and exposes it -- no UI
    /// reads it yet; that's deferred to whatever picker/settings UI comes
    /// later.
    /// </summary>
    public class DataManagerDiscovery
    {
        private const double StaleAfterSeconds = 6.0;

        /// <summary>Gated diagnostic logging -- set from MeshReceiver's own
        /// VerboseLogging field, same pattern as SocketReceiver.</summary>
        public bool VerboseLogging { get; set; }

        private UdpClient     _client;
        private Thread        _thread;
        private volatile bool _running;

        private sealed class Entry
        {
            public string   Host;
            public int      ClientPort;
            public string   ProjectName;
            public DateTime LastSeenUtc;
        }

        private readonly object _lock = new object();
        private readonly Dictionary<string, Entry> _entries = new Dictionary<string, Entry>();

        private void Log(string message)
        {
            if (VerboseLogging) Debug.Log(message);
        }

        public void Start(int discoveryPort)
        {
            if (_running) Stop();

            _client = new UdpClient();
            _client.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _client.Client.Bind(new IPEndPoint(IPAddress.Any, discoveryPort));
            _client.Client.ReceiveTimeout = 200;   // lets the loop observe _running promptly
            _running = true;

            _thread = new Thread(ListenLoop) { IsBackground = true, Name = "PVLink-Discovery" };
            _thread.Start();
            Log($"[ParaViewLink] DataManagerDiscovery: listening for broadcasts on UDP port {discoveryPort}");
        }

        public void Stop()
        {
            _running = false;
            try { _client?.Close(); } catch { }
            _client = null;
            _thread?.Join(500);
            _thread = null;
        }

        /// <summary>Thread-safe snapshot of currently-known DataManagers.</summary>
        public List<DiscoveredDataManager> GetDiscovered()
        {
            lock (_lock)
            {
                var result = new List<DiscoveredDataManager>(_entries.Count);
                foreach (var e in _entries.Values)
                    result.Add(new DiscoveredDataManager
                        { Host = e.Host, ClientPort = e.ClientPort, ProjectName = e.ProjectName });
                return result;
            }
        }

        private void ListenLoop()
        {
            var remote = new IPEndPoint(IPAddress.Any, 0);
            while (_running)
            {
                byte[] data;
                try
                {
                    data = _client.Receive(ref remote);
                }
                catch (SocketException)
                {
                    PruneStale();   // timeout -- just a chance to prune and re-check _running
                    continue;
                }
                catch (ObjectDisposedException)
                {
                    break;   // Stop() closed the socket
                }

                ParseAndRecord(Encoding.UTF8.GetString(data), remote.Address.ToString());
                PruneStale();
            }
        }

        private void ParseAndRecord(string text, string host)
        {
            string[] lines = text.Split('\n');
            if (lines.Length == 0 || !lines[0].StartsWith("PVLINK-DISCOVERY"))
                return;   // not our protocol -- ignore silently, some other UDP traffic on this port

            int clientPort = 0;
            string projectName = "";
            for (int i = 1; i < lines.Length; i++)
            {
                int eq = lines[i].IndexOf('=');
                if (eq < 0) continue;
                string key   = lines[i].Substring(0, eq);
                string value = lines[i].Substring(eq + 1).TrimEnd('\r');
                if (key == "client_port")    int.TryParse(value, out clientPort);
                else if (key == "project")   projectName = value;
            }
            if (clientPort <= 0) return;

            string mapKey = $"{host}:{clientPort}";
            DateTime now = DateTime.UtcNow;

            lock (_lock)
            {
                bool isNew = !_entries.TryGetValue(mapKey, out Entry existing);
                bool projectChanged = existing != null && existing.ProjectName != projectName;

                if (existing == null)
                {
                    existing = new Entry { Host = host, ClientPort = clientPort };
                    _entries[mapKey] = existing;
                }
                existing.ProjectName = projectName;
                existing.LastSeenUtc = now;

                if (isNew || projectChanged)
                    Log($"[ParaViewLink] DataManagerDiscovery: {mapKey} -- project='{projectName}'" +
                        (isNew ? " (new)" : " (updated)"));
            }
        }

        private void PruneStale()
        {
            DateTime now = DateTime.UtcNow;
            lock (_lock)
            {
                List<string> stale = null;
                foreach (var kv in _entries)
                {
                    if ((now - kv.Value.LastSeenUtc).TotalSeconds > StaleAfterSeconds)
                        (stale ??= new List<string>()).Add(kv.Key);
                }
                if (stale != null)
                {
                    foreach (var key in stale)
                    {
                        Log($"[ParaViewLink] DataManagerDiscovery: {key} -- pruned (stale)");
                        _entries.Remove(key);
                    }
                }
            }
        }
    }
}
