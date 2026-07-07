using System.Collections.Generic;
using System.Text;
using UnityEditor;
using UnityEngine;

public class ConsoleCopierWindow : EditorWindow
{
    private readonly List<string>  _pending   = new List<string>();
    private readonly object        _lock      = new object();
    private volatile bool          _hasPending;
    private          StringBuilder _logBuffer = new StringBuilder();
    private          Vector2       _scrollPos;

    [MenuItem("Tools/Console Copy Panel")]
    public static void ShowWindow()
    {
        GetWindow<ConsoleCopierWindow>("Console Copier").Show();
    }

    private void OnEnable()
    {
        Application.logMessageReceived         += HandleLog;
        Application.logMessageReceivedThreaded += HandleLogThreaded;
        EditorApplication.update               += CheckPending;
        _logBuffer.Clear();
    }

    private void OnDisable()
    {
        Application.logMessageReceived         -= HandleLog;
        Application.logMessageReceivedThreaded -= HandleLogThreaded;
        EditorApplication.update               -= CheckPending;
    }

    // Called on the main thread — safe to write and repaint immediately.
    private void HandleLog(string condition, string stackTrace, LogType type)
    {
        Append(condition, stackTrace, type);
        Repaint();
    }

    // Called from ANY thread — buffer; main thread flushes via CheckPending.
    private void HandleLogThreaded(string condition, string stackTrace, LogType type)
    {
        string line = FormatLine(condition, stackTrace, type);
        lock (_lock)
        {
            _pending.Add(line);
            _hasPending = true;
        }
    }

    // Runs every editor frame on the main thread.
    private void CheckPending()
    {
        if (!_hasPending) return;
        lock (_lock) { _hasPending = false; }
        Repaint();  // safe — this is the main thread
    }

    private void Append(string condition, string stackTrace, LogType type)
    {
        _logBuffer.AppendLine(FormatLine(condition, stackTrace, type));
    }

    private static string FormatLine(string condition, string stackTrace, LogType type)
    {
        if ((type == LogType.Error || type == LogType.Exception) && !string.IsNullOrEmpty(stackTrace))
            return $"[{type}] {condition}\n{stackTrace}";
        return $"[{type}] {condition}";
    }

    private void OnGUI()
    {
        // Flush any background-thread messages accumulated since last frame.
        lock (_lock)
        {
            foreach (var line in _pending)
                _logBuffer.AppendLine(line);
            _pending.Clear();
        }

        GUILayout.Space(6);
        if (GUILayout.Button("📋 Copy Captured Logs to Clipboard", GUILayout.Height(36)))
        {
            if (_logBuffer.Length > 0)
            {
                GUIUtility.systemCopyBuffer = _logBuffer.ToString();
                ShowNotification(new GUIContent("Logs Copied!"));
            }
            else
            {
                ShowNotification(new GUIContent("No logs yet — run the game first."));
            }
        }
        if (GUILayout.Button("Clear", GUILayout.Height(20)))
            _logBuffer.Clear();

        GUILayout.Space(6);
        GUILayout.Label("Captured Logs:", EditorStyles.boldLabel);
        _scrollPos = GUILayout.BeginScrollView(_scrollPos, EditorStyles.helpBox);
        GUILayout.Label(_logBuffer.ToString(), EditorStyles.wordWrappedLabel);
        GUILayout.EndScrollView();
    }
}
