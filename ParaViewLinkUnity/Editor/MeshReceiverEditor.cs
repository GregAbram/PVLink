using UnityEditor;
using UnityEngine;
using ParaViewLink;

namespace ParaViewLink.Editor
{
    [CustomEditor(typeof(MeshReceiver))]
    public class MeshReceiverEditor : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            DrawDefaultInspector();

            MeshReceiver receiver = (MeshReceiver)target;

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Status", EditorStyles.boldLabel);

            GUI.enabled = false;
            EditorGUILayout.Toggle("Connecting",  receiver.IsConnecting);
            EditorGUILayout.Toggle("Connected",   receiver.IsConnected);
            GUI.enabled = true;

            if (Application.isPlaying)
            {
                EditorGUILayout.Space();
                if (GUILayout.Button("Reconnect"))
                {
                    receiver.enabled = false;
                    receiver.enabled = true;
                }
            }
        }
    }
}
