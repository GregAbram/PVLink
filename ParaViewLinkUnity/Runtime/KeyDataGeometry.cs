using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using DataManager;
using UnityEngine;
using UnityEngine.Rendering;

namespace ParaViewLink
{
    /// <summary>Interleaved vertex used by the ScalarField shaders: the colour's
    /// R channel carries the normalised colour-by value.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScalarVertex
    {
        public Vector3 Position;
        public Vector3 Normal;
        public byte R, G, B, A;

        public static readonly VertexAttributeDescriptor[] Layout =
        {
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Normal,   VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Color,    VertexAttributeFormat.UNorm8,  4),
        };
    }

    /// <summary>Triangle geometry ready for upload, built off the main thread.</summary>
    public class BuiltMesh
    {
        public string Name;
        public ScalarVertex[] Vertices;
        public int[] Indices;
        public Bounds Bounds;
        public string ColorBy;     // variable whose colormap/material to use, or null
        public DMKeyData Source;   // kept so a colormap range change can rebuild it
    }

    /// <summary>
    /// Turns KeyData into triangle meshes in Unity's frame (SPEC.md 4: z negated,
    /// winding reversed). Points become small octahedra and line strips thin
    /// tubes: Unity's point and line topologies draw 1-pixel primitives, which
    /// are close to invisible in a headset. Uses no Unity object APIs, so it
    /// runs on worker threads.
    /// </summary>
    public static class KeyDataGeometry
    {
        public const int TubeSides = 6;

        /// <param name="radius">point half-size / tube radius, in ParaView units</param>
        /// <param name="colorRange">range for normalising the colour-by array,
        /// or null to use the array's own range</param>
        public static BuiltMesh Build(string name, DMKeyData kd, float radius, Vector2? colorRange)
        {
            float[] values = kd.ColorValues();
            float lo = 0, hi = 1;
            if (values != null)
            {
                if (colorRange.HasValue) { lo = colorRange.Value.x; hi = colorRange.Value.y; }
                else
                {
                    int i = Array.IndexOf(kd.Meta.scalarArrayNames, kd.ColorBy);
                    if (i >= 0) { lo = kd.Meta.scalarMins[i]; hi = kd.Meta.scalarMaxes[i]; }
                    else { lo = float.MaxValue; hi = float.MinValue; foreach (var v in values) { lo = Mathf.Min(lo, v); hi = Mathf.Max(hi, v); } }
                }
            }
            float span = hi - lo;
            Func<int, byte> color = values == null ? (Func<int, byte>)(_ => 0)
                : i => (byte)(Mathf.Clamp01(span > 1e-20f ? (values[i] - lo) / span : 0f) * 255f);

            BuiltMesh m;
            switch (kd.Topology)
            {
                case DMTopology.Triangles:
                case DMTopology.Quads:
                    m = Surface(kd, color);
                    break;
                case DMTopology.LineStrip:
                case DMTopology.Lines:
                    m = Tubes(kd, color, radius);
                    break;
                case DMTopology.Points:
                    m = Octahedra(kd, color, radius);
                    break;
                default:
                    return null;   // Voxels: volume rendering is deferred
            }
            m.Name = name;
            m.ColorBy = values != null ? kd.ColorBy : null;
            m.Source = kd;
            m.Bounds = ComputeBounds(m.Vertices);
            return m;
        }

        static BuiltMesh Surface(DMKeyData kd, Func<int, byte> color)
        {
            int n = kd.PointCount;
            var verts = new ScalarVertex[n];
            for (int i = 0; i < n; i++)
                verts[i] = new ScalarVertex { Position = DMConvert.Point(kd.Positions, i), R = color(i), A = 255 };
            int[] tris = DMConvert.Triangles(kd.Cells, kd.Meta.num_cells);
            if (kd.Vectors.TryGetValue("Normals", out var nrm))
            {
                for (int i = 0; i < n; i++) verts[i].Normal = DMConvert.Vector(nrm, i);
            }
            else
            {
                var acc = new Vector3[n];
                for (int t = 0; t < tris.Length; t += 3)
                {
                    int a = tris[t], b = tris[t + 1], c = tris[t + 2];
                    var f = Vector3.Cross(verts[b].Position - verts[a].Position, verts[c].Position - verts[a].Position);
                    acc[a] += f; acc[b] += f; acc[c] += f;
                }
                for (int i = 0; i < n; i++)
                    verts[i].Normal = acc[i].sqrMagnitude > 1e-30f ? acc[i].normalized : Vector3.up;
            }
            return new BuiltMesh { Vertices = verts, Indices = tris };
        }

        static readonly Vector3[] OctDirs =
            { Vector3.right, Vector3.left, Vector3.up, Vector3.down, Vector3.forward, Vector3.back };
        static readonly int[] OctTris =
            { 0, 2, 4,  2, 1, 4,  1, 3, 4,  3, 0, 4,  2, 0, 5,  1, 2, 5,  3, 1, 5,  0, 3, 5 };

        static BuiltMesh Octahedra(DMKeyData kd, Func<int, byte> color, float r)
        {
            // Points are listed as cells [1, i]; draw each referenced point once.
            var ids = new List<int>(kd.Meta.num_cells);
            for (int i = 0; i < kd.Cells.Length; i += kd.Cells[i] + 1)
                for (int j = 1; j <= kd.Cells[i]; j++) ids.Add(kd.Cells[i + j]);
            var verts = new ScalarVertex[ids.Count * 6];
            var idx = new int[ids.Count * 24];
            for (int p = 0; p < ids.Count; p++)
            {
                int id = ids[p];
                var c = DMConvert.Point(kd.Positions, id);
                byte col = color(id);
                for (int k = 0; k < 6; k++)
                    verts[p * 6 + k] = new ScalarVertex { Position = c + r * OctDirs[k], Normal = OctDirs[k], R = col, A = 255 };
                for (int k = 0; k < 24; k++) idx[p * 24 + k] = p * 6 + OctTris[k];
            }
            return new BuiltMesh { Vertices = verts, Indices = idx };
        }

        static BuiltMesh Tubes(DMKeyData kd, Func<int, byte> color, float r)
        {
            var verts = new List<ScalarVertex>();
            var idx = new List<int>();
            for (int i = 0; i < kd.Cells.Length; i += kd.Cells[i] + 1)
            {
                int k = kd.Cells[i];
                if (k < 2) continue;
                // Rotation-minimising frame along the polyline (parallel transport).
                var pts = new Vector3[k];
                for (int j = 0; j < k; j++) pts[j] = DMConvert.Point(kd.Positions, kd.Cells[i + 1 + j]);
                Vector3 tangent = Tangent(pts, 0);
                Vector3 normal = Vector3.Cross(tangent, Mathf.Abs(tangent.y) < 0.9f ? Vector3.up : Vector3.right).normalized;
                int baseVertex = verts.Count;
                for (int j = 0; j < k; j++)
                {
                    Vector3 t = Tangent(pts, j);
                    if (j > 0)
                    {
                        normal -= Vector3.Dot(normal, t) * t;
                        normal = normal.sqrMagnitude > 1e-12f ? normal.normalized
                               : Vector3.Cross(t, Mathf.Abs(t.y) < 0.9f ? Vector3.up : Vector3.right).normalized;
                    }
                    Vector3 binormal = Vector3.Cross(t, normal);
                    byte col = color(kd.Cells[i + 1 + j]);
                    for (int s = 0; s < TubeSides; s++)
                    {
                        float a = 2 * Mathf.PI * s / TubeSides;
                        Vector3 dir = Mathf.Cos(a) * normal + Mathf.Sin(a) * binormal;
                        verts.Add(new ScalarVertex { Position = pts[j] + r * dir, Normal = dir, R = col, A = 255 });
                    }
                }
                for (int j = 0; j < k - 1; j++)
                    for (int s = 0; s < TubeSides; s++)
                    {
                        int a0 = baseVertex + j * TubeSides + s, a1 = baseVertex + j * TubeSides + (s + 1) % TubeSides;
                        int b0 = a0 + TubeSides, b1 = a1 + TubeSides;
                        idx.Add(a0); idx.Add(a1); idx.Add(b0);
                        idx.Add(a1); idx.Add(b1); idx.Add(b0);
                    }
            }
            return new BuiltMesh { Vertices = verts.ToArray(), Indices = idx.ToArray() };
        }

        static Vector3 Tangent(Vector3[] p, int j)
        {
            Vector3 d = j == 0 ? p[1] - p[0] : j == p.Length - 1 ? p[j] - p[j - 1] : p[j + 1] - p[j - 1];
            return d.sqrMagnitude > 1e-24f ? d.normalized : Vector3.forward;
        }

        static Bounds ComputeBounds(ScalarVertex[] v)
        {
            if (v.Length == 0) return new Bounds();
            Vector3 min = v[0].Position, max = v[0].Position;
            for (int i = 1; i < v.Length; i++) { min = Vector3.Min(min, v[i].Position); max = Vector3.Max(max, v[i].Position); }
            var b = new Bounds();
            b.SetMinMax(min, max);
            return b;
        }
    }
}
