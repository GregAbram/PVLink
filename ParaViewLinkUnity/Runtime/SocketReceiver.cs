using System;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using UnityEngine;

namespace ParaViewLink
{
    /// <summary>
    /// Background-thread TCP server.  Accepts one client at a time, reads
    /// length-prefixed messages, and invokes <see cref="OnMessage"/> on the IO
    /// thread for each one.
    ///
    /// Ack flow (backpressure):
    ///   Only UPDATE (type 2) receives a 4-byte ack (0 = OK).  All other
    ///   message types (BOUNDS, COLORMAP, MESH) are fire-and-forget — PVLink
    ///   sends them without waiting, so the tunnel isn't stalled by per-message
    ///   round trips.  The UPDATE ack is the single backpressure point: PVLink
    ///   waits for it before starting the next pipeline cycle, and Unity sends
    ///   it only after the main-thread buffer swap completes.
    ///
    /// Wire format (little-endian):
    ///   int32  payload_byte_count
    ///   int32  message_type
    ///   byte[payload_byte_count]  payload
    ///   ← int32 ack (0 = OK)  sent by Unity only for UPDATE (type 2)
    /// </summary>
    public class SocketReceiver
    {
        public const string Version = "1.2";
        // ---------------------------------------------------------------
        // Public state
        // ---------------------------------------------------------------
        public bool IsListening       => _listener != null;
        public bool IsClientConnected => _client   != null && _client.Connected;

        /// <summary>
        /// Invoked on the IO thread for every received message.
        /// The ack is sent to ParaView as soon as this delegate returns.
        /// </summary>
        public Action<int, byte[]> OnMessage { get; set; }

        // ---------------------------------------------------------------
        // Internal state
        // ---------------------------------------------------------------
        private TcpListener   _listener;
        private TcpClient     _client;
        private Thread        _acceptThread;
        private Thread        _readThread;
        private volatile bool _running;

        // ---------------------------------------------------------------
        // Start / Stop
        // ---------------------------------------------------------------

        public void Start(string address, int port)
        {
            if (_running) Stop();

            IPAddress ip = address == "0.0.0.0" || string.IsNullOrEmpty(address)
                ? IPAddress.Any
                : IPAddress.Parse(address);

            _listener = new TcpListener(ip, port);
            _listener.Start();
            _running = true;

            _acceptThread = new Thread(AcceptLoop)
                { IsBackground = true, Name = "PVLink-Accept" };
            _acceptThread.Start();

            Debug.Log($"[ParaViewLink] v{Version} Listening on {ip}:{port}");
        }

        public void Stop()
        {
            _running = false;
            try { _listener?.Stop(); } catch { }
            try { _client?.Close();  } catch { }
            _listener = null;
            _client   = null;
            _acceptThread?.Join(500);
            _readThread?.Join(500);
            _acceptThread = null;
            _readThread   = null;
            Debug.Log("[ParaViewLink] Stopped.");
        }

        // ---------------------------------------------------------------
        // Background threads
        // ---------------------------------------------------------------

        private void AcceptLoop()
        {
            while (_running)
            {
                try
                {
                    Debug.Log("[ParaViewLink] AcceptLoop: waiting for connection...");
                    TcpClient client = _listener.AcceptTcpClient();
                    client.NoDelay        = true;
                    client.ReceiveTimeout = 0;
                    Debug.Log($"[ParaViewLink] Client connected: {client.Client.RemoteEndPoint}");

                    // Start the new ReadLoop BEFORE closing the old client.
                    // If we close first, the new thread might inherit a stomped
                    // reference and GetStream() throws on a non-connected socket.
                    var prevClient = _client;
                    _client     = client;
                    _readThread = new Thread(() => ReadLoop(client))
                        { IsBackground = true, Name = "PVLink-Read" };
                    _readThread.Start();

                    if (prevClient != null)
                    {
                        Debug.Log("[ParaViewLink] AcceptLoop: closing previous client");
                        try { prevClient.Close(); } catch { }
                    }
                }
                catch (SocketException) when (!_running) { break; }
                catch (Exception ex)
                {
                    if (_running)
                        Debug.LogWarning($"[ParaViewLink] Accept error: {ex.Message}");
                    Thread.Sleep(500);
                }
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
                    Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: waiting for next message header...");
                    if (!ReadExact(stream, hdr, 8))
                    {
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: header read returned 0 bytes — client closed connection");
                        break;
                    }

                    int payloadLen = BitConverter.ToInt32(hdr, 0);
                    int msgType    = BitConverter.ToInt32(hdr, 4);
                    Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: got header cmd={msgType} payloadLen={payloadLen}");

                    if (payloadLen < 0 || payloadLen > 256 * 1024 * 1024)
                    {
                        Debug.LogWarning($"[ParaViewLink] Bad payload length {payloadLen}, dropping client.");
                        break;
                    }

                    byte[] payload = new byte[payloadLen];
                    if (payloadLen > 0 && !ReadExact(stream, payload, payloadLen))
                    {
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: payload read failed — client closed mid-message");
                        break;
                    }

                    // Invoke handler.  Only UPDATE (msgType==2) sends a 4-byte ack
                    // back to PVLink; all other types are fire-and-forget.
                    // The UPDATE handler blocks until the main-thread buffer swap
                    // completes, so the ack is the backpressure signal that tells
                    // PVLink the frame is live and the next cycle can begin.
                    try { OnMessage?.Invoke(msgType, payload); }
                    catch (Exception ex)
                    {
                        if (_running)
                            Debug.LogWarning($"[ParaViewLink] OnMessage error for cmd={msgType}: {ex.Message}");
                    }

                    if (msgType == 2)   // MSG_TYPE_UPDATE only
                    {
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: sending UPDATE ack");
                        stream.Write(BitConverter.GetBytes(0), 0, 4);
                        stream.Flush();
                        Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: UPDATE ack sent");
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
                try { client.Close(); } catch { }
                if (ReferenceEquals(_client, client)) _client = null;
                Debug.Log($"[ParaViewLink] ReadLoop [{ep}]: exited — client disconnected");
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
