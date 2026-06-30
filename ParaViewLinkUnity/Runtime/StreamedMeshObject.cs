using UnityEngine;

namespace ParaViewLink
{
    /// <summary>
    /// Thin wrapper retained for manual use.  In the normal pipeline
    /// MeshReceiver manages its own GameObjects directly via the double-buffer
    /// system and does not use this component.
    /// </summary>
    [RequireComponent(typeof(MeshFilter), typeof(MeshRenderer))]
    [AddComponentMenu("ParaViewLink/Streamed Mesh (manual)")]
    public class StreamedMeshObject : MonoBehaviour
    {
        private MeshRenderer _renderer;

        private void Awake()
        {
            _renderer = GetComponent<MeshRenderer>();
        }

        public void SetMaterial(Material mat)
        {
            if (_renderer != null)
                _renderer.sharedMaterial = mat;
        }

        public void SetVisible(bool visible)
        {
            if (_renderer != null)
                _renderer.enabled = visible;
        }
    }
}
