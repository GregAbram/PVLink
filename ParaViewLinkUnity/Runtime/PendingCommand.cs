using UnityEngine;

namespace ParaViewLink
{
    public enum PendingCmdType
    {
        Flip,
        Mesh,
        Scalars,
        Variable,
        Colormap,
        PVMesh,
        Bounds,
        Visibility,
    }

    /// <summary>
    /// Parsed message passed from the network thread to the main thread via
    /// MeshReceiver's install queue.
    /// </summary>
    public class PendingCommand
    {
        public PendingCmdType Type = PendingCmdType.Flip;

        // ---- Geometry (Mesh / PVMesh) ----
        public string     MeshName     = "";
        public Vector3[]  Vertices;
        public int[]      Triangles;
        public Vector3[]  Normals;      // null → Unity auto-computes

        // ---- Scalars (Scalars / PVMesh) ----
        public string     VariableName = "";
        public float[]    Scalars;
        public float      ColormapMin  = 0f;
        public float      ColormapMax  = 1f;

        // PVMesh: -1 = none, 0 = per-point, 1 = per-cell
        public int        ScalarLocation = -1;

        // ---- Colormap ----
        public Color[]    ColormapRGB;

        // ---- Bounds ----
        public Bounds     BoundsBox;

        // ---- Visibility (MeshName reused as key) ----
        public bool       Visible = true;
    }
}
