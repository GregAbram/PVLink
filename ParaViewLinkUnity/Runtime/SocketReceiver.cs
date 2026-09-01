using System;
using System.Net.Sockets;
using System.Threading;
using UnityEngine;

namespace ParaViewLink
{
    /// <summary>
    /// Background-thread TCP client.  Dials out to the DataManager and reads
    /// length-prefixed messages, invoking <see cref="OnMessage"/> on the IO
    /// thread for each one.  If the connection can't be established, or
    /// drops, this keeps retrying (about once a second) rather than giving
    /// up -- the DataManager may not be up yet, or may restart independently
    /// of this client.
    ///
    /// Ack flow (backpressure):
    ///   Only UPDATE (type 2) receives a 4-byte ack (0 = OK).  All other
    ///   message types (BOUNDS, COLORMAP, MESH) are fire-and-forget — the
    ///   DataManager sends them without waiting, so the tunnel isn't stalled
    ///   by per-message round trips. The UPDATE ack is the single
    ///   backpressure point: the sender waits for it before starting the
    ///   next pipeline cycle, and Unity sends it only after the main-thread
    ///   buffer swap completes.
    ///
    /// Wire format (little-endian):
    ///   int32  payload_byte_count
    ///   int32  message_type
    ///   byte[payload_byte_count]  payload
    ///   ← int32 ack (0 = OK)  sent by Unity only for UPDATE (type 2)
    /// </summary>
    public class SocketReceiver
    {
        public const string Version = "2.0";
        // ---------------------------------------------------------------
        // Public state
        // ---------------------------------------------------------------
        public bool IsConnecting => _running && !IsConnected;
        public bool IsConnected  => _client != null && _client.Connected;

        /// <summary>Log every connect attempt and message header/ack.  Off by
        /// default -- set by MeshReceiver from its own VerboseLogging field.</summary>
        public bool VerboseLogging { get; set; }

        /// <summary>
        /// Invoked on the IO thread for every received message.
        /// The ack is sent to the DataManager as soon as this delegate returns.
        /// </summary>
        public Action<int, byte[]> OnMessage { get; set; }

        private void Log(string message)
        {
            if (VerboseLogging) Debug.Log(message);
        }

        // ---------------------------------------------------------------
        // Internal state
        // ---------------------------------------------------------------
        private TcpClient     _client;
        private Thread        _connectThread;
        private volatile bool _running;
        private string        _host;
        private int           _port;

        // ---------------------------------------------------------------
        // Start / Stop
        // ---------------------------------------------------------------

        public void Start(string host, int port)
        {
            if (_running) Stop();

            _host    = host;
            _port    = port;
            _running = true;

            _connectThread = new Thread(ConnectLoop)
                { IsBackground = true, Name = "PVLink-Connect" };
            _connectThread.Start();

            Debug.Log($"[ParaViewLink] v{Version} dialing {host}:{port}");
        }

        public void Stop()
        {
            _running = false;
            try { _client?.Close(); } catch { }
            _client = null;
            _connectThread?.Join(500);
            _connectThread = null;
            Debug.Log("[ParaViewLink] Stopped.");
        }

        // ---------------------------------------------------------------
        // Background thread
        // ---------------------------------------------------------------

        private void ConnectLoop()
        {
            while (_running)
            {
                var client = new TcpClient();
                try
                {
                    Log($"[ParaViewLink] ConnectLoop: connecting to {_host}:{_port} ...");
                    client.Connect(_host, _port);
                    client.NoDelay        = true;
                    client.ReceiveTimeout = 0;
                    Debug.Log($"[ParaViewLink] Connected to DataManager: {client.Client.RemoteEndPoint}");

                    _client = client;
                    ReadLoop(client);
                }
                catch (SocketException ex)
                {
                    if (_running)
                        Debug.LogWarning($"[ParaViewLink] Connect failed: {ex.Message}");
                }
                catch (Exception ex)
                {
                    if (_running)
                        Debug.LogWarning($"[ParaViewLink] ConnectLoop error: {ex.Message}");
                }
                finally
                {
                    try { client.Close(); } catch { }
                    if (ReferenceEquals(_client, client)) _client = null;
                }

                if (!_running) break;

                // Wait before retrying, in short steps so Stop() is observed
                // promptly rather than after one long sleep.
                for (int i = 0; i < 10 && _running; i++)
                    Thread.Sleep(100);
            }
        }

        private void ReadLoop(TcpClient client)
        {
            NetworkStream stream = client.GetStream();
            byte[] hdr = new byte[8];
            string ep = client.Client.RemoteEndPoint?.ToString() ?? "?";

            try
            {
                while (_running && client.Connected)
                {
                    Log($"[ParaViewLink] ReadLoop [{ep}]: waiting for next message header...");
                    if (!ReadExact(stream, hdr, 8))
                    {
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: header read returned 0 bytes — DataManager closed connection");
                        break;
                    }

                    int payloadLen = BitConverter.ToInt32(hdr, 0);
                    int msgType    = BitConverter.ToInt32(hdr, 4);
                    Log($"[ParaViewLink] ReadLoop [{ep}]: got header cmd={msgType} payloadLen={payloadLen}");

                    if (payloadLen < 0 || payloadLen > 256 * 1024 * 1024)
                    {
                        Debug.LogWarning($"[ParaViewLink] Bad payload length {payloadLen}, dropping connection.");
                        break;
                    }

                    byte[] payload = new byte[payloadLen];
                    if (payloadLen > 0 && !ReadExact(stream, payload, payloadLen))
                    {
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: payload read failed — connection closed mid-message");
                        break;
                    }

                    // Invoke handler.  Only UPDATE (msgType==2) sends a 4-byte ack
                    // back to the DataManager; all other types are fire-and-forget.
                    // The UPDATE handler blocks until the main-thread buffer swap
                    // completes, so the ack is the backpressure signal that tells
                    // the sender the frame is live and the next cycle can begin.
                    try { OnMessage?.Invoke(msgType, payload); }
                    catch (Exception ex)
                    {
                        if (_running)
                            Debug.LogWarning($"[ParaViewLink] OnMessage error for cmd={msgType}: {ex.Message}");
                    }

                    if (msgType == 2)   // MSG_TYPE_UPDATE only
                    {
                        Log($"[ParaViewLink] ReadLoop [{ep}]: sending UPDATE ack");
                        stream.Write(BitConverter.GetBytes(0), 0, 4);
                        stream.Flush();
                        Log($"[ParaViewLink] ReadLoop [{ep}]: UPDATE ack sent");
                    }
                }
            }
            catch (Exception ex)
            {
                if (_running)
                    Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: exception — {ex.Message}");
            }
            finally
            {
                Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: exited — connection to DataManager lost");
            }
        }

        private static bool ReadExact(NetworkStream stream, byte[] buf, int count)
        {
            int offset = 0;
            while (offset < count)
            {
                int n = stream.Read(buf, offset, count - offset);
                if (n == 0) return false;
                offset += n;
            }
            return true;
        }
    }
}
