using UnityEngine;

namespace ParaViewLink
{
    /// <summary>
    /// Marks the world-space volume that the ParaView dataset maps into.
    ///
    /// The host GameObject should be a Cube so it is visible and selectable
    /// in the Scene view.  The MeshRenderer is disabled at runtime — the cube
    /// is a positioning aid only.  Size and position are defined entirely by
    /// the Transform:
    ///
    ///   Position  — world-space centre of the target volume
    ///   Rotation  — axis remapping (e.g. rotate -90° around X for Z-up data)
    ///   Scale     — world-space extents in each axis
    ///
    /// A green wire-cube gizmo reinforces the volume boundary in the Scene view.
    /// </summary>
    [AddComponentMenu("ParaViewLink/Sim Container")]
    public class SimContainerObject : MonoBehaviour
    {
        // Hide the cube mesh at runtime — it is only needed as an editor handle.
        private void Awake()
        {
            MeshRenderer mr = GetComponent<MeshRenderer>();
            if (mr != null) mr.enabled = false;
        }

        /// <summary>World-space centre of the container.</summary>
        public Vector3 WorldCenter => transform.position;

        /// <summary>World-space size of the container (= lossy scale of the Transform).</summary>
        public Vector3 WorldSize   => transform.lossyScale;

        private void OnDrawGizmos()
        {
            Gizmos.color = new Color(0.2f, 0.8f, 0.2f, 0.4f);
            Matrix4x4 old = Gizmos.matrix;
            Gizmos.matrix = transform.localToWorldMatrix;
            Gizmos.DrawWireCube(Vector3.zero, Vector3.one);
            Gizmos.matrix = old;
        }
    }
}
