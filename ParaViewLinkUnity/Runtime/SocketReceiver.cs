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
    /// Ack flow:
    ///   The 4-byte ack (0 = OK) is sent to ParaView immediately after
    ///   <see cref="OnMessage"/> returns.  The handler may block as long as
    ///   needed — the ack is deliberately withheld until it does return.
    ///   For mesh and colormap messages the handler returns as soon as the
    ///   raw data is enqueued (fast).  For the flip/update message it blocks
    ///   until the main thread signals that the buffer swap is complete.
    ///
    /// Wire format (little-endian):
    ///   int32  payload_byte_count
    ///   int32  message_type
    ///   byte[payload_byte_count]  payload
    ///   ← int32 ack (0 = OK)  sent after OnMessage returns
    /// </summary>
    public class SocketReceiver
    {
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

            Debug.Log($"[ParaViewLink] Listening on {ip}:{port}");
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
                    TcpClient client = _listener.AcceptTcpClient();
                    client.NoDelay        = true;
                    client.ReceiveTimeout = 0;
                    try { _client?.Close(); } catch { }
                    _client = client;
                    Debug.Log($"[ParaViewLink] Client connected: {client.Client.RemoteEndPoint}");
                    _readThread = new Thread(() => ReadLoop(client))
                        { IsBackground = true, Name = "PVLink-Read" };
                    _readThread.Start();
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

            try
            {
                while (_running && client.Connected)
                {
                    if (!ReadExact(stream, hdr, 8)) break;

                    int payloadLen = BitConverter.ToInt32(hdr, 0);
                    int msgType    = BitConverter.ToInt32(hdr, 4);

                    if (payloadLen < 0 || payloadLen > 256 * 1024 * 1024)
                    {
                        Debug.LogWarning($"[ParaViewLink] Bad payload length {payloadLen}, dropping client.");
                        break;
                    }

                    byte[] payload = new byte[payloadLen];
                    if (payloadLen > 0 && !ReadExact(stream, payload, payloadLen)) break;

                    // Invoke handler (may block for flip synchronisation).
                    // Ack is sent only after the handler returns.
                    try { OnMessage?.Invoke(msgType, payload); }
                    catch (Exception ex)
                    {
                        if (_running)
                            Debug.LogWarning($"[ParaViewLink] OnMessage error for cmd={msgType}: {ex.Message}");
                    }

                    stream.Write(BitConverter.GetBytes(0), 0, 4);
                }
            }
            catch (Exception ex)
            {
                if (_running)
                    Debug.Log($"[ParaViewLink] Read loop ended: {ex.Message}");
            }
            finally
            {
                try { client.Close(); } catch { }
                if (ReferenceEquals(_client, client)) _client = null;
                Debug.Log("[ParaViewLink] Client disconnected.");
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
