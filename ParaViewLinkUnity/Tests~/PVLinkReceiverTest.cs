using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using System.Threading;
using ParaViewLink;
using UnityEditor;
using UnityEngine;
using Debug = UnityEngine.Debug;

public static class PVLinkReceiverTest
{
    static int failures;
    static void Check(bool ok, string m) { if (!ok) failures++; Debug.Log("[PVTEST] " + (ok ? "ok   " : "FAIL ") + m); }
    static MeshReceiver rx;
    static void Call(string method) => typeof(MeshReceiver).GetMethod(method, BindingFlags.NonPublic | BindingFlags.Instance).Invoke(rx, null);

    static bool Pump(Func<bool> until, int ms = 5000)
    {
        var sw = Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < ms) { Call("Update"); if (until()) return true; Thread.Sleep(2); }
        return false;
    }

    static Transform Pair(string name) => rx.transform.Find(name + "_Pair");
    static MeshFilter Front(string name)
    {
        var p = Pair(name);
        return p == null ? null : p.GetComponentsInChildren<MeshFilter>(true).FirstOrDefault(f => f.gameObject.activeSelf);
    }

    public static void Run()
    {
        var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
        go.AddComponent<SimContainerObject>();
        rx = go.AddComponent<MeshReceiver>();
        rx.LocalStorePath = Environment.GetEnvironmentVariable("DM_TEST_STORE");
        rx.Project = "Test/pv";
        rx.BaseScalarMaterial = AssetDatabase.LoadAssetAtPath<Material>("Packages/com.paraviewlink.unity/Materials/M_ScalarField.mat");
        Check(rx.BaseScalarMaterial != null, "ScalarField material found");
        Call("Awake"); Call("OnEnable");
        Check(Pump(() => rx.State.Project == "Test/pv"), "connected to the in-process store");

        rx.Seek(0);
        Check(Pump(() => rx.CurrentTime == 0 && Front("Contour1") != null && Front("isolines") != null), "t=0 shown");
        var c = Front("Contour1").sharedMesh;
        Check(c.vertexCount == 1654 && c.GetIndexCount(0) == 3 * 2516, $"contour: {c.vertexCount} vertices, {c.GetIndexCount(0) / 3} triangles");
        var p = Front("MaskPoints1").sharedMesh;
        Check(p.vertexCount == 186 * 6, $"points drawn as octahedra: {p.vertexCount} vertices");
        var l = Front("isolines").sharedMesh;
        Check(l.vertexCount == 286 * KeyDataGeometry.TubeSides, $"lines drawn as tubes (143 two-point segments): {l.vertexCount} vertices");
        var mat = Front("Contour1").GetComponent<MeshRenderer>().sharedMaterial;
        Check(mat != null && mat.name == "M_RTData" && mat.GetTexture("_Colormap") != null, "contour uses the RTData colormap material");
        var cols = c.colors32;
        Check(cols.Select(x => x.r).Distinct().Count() > 1 || cols.All(x => x.r == cols[0].r), "vertex colour values present");
        var b = c.bounds;
        Check(Mathf.Abs(b.min.z + 10) < 1e-3f && Mathf.Abs(b.max.z - 10) < 1e-3f, $"mesh in Unity frame, bounds {b.min}..{b.max}");
        foreach (var f in Pair("Contour1").GetComponentsInChildren<MeshFilter>(true))
            Debug.Log($"[PVTEST] {f.name} active={f.gameObject.activeSelf} localScale={f.transform.localScale} localPos={f.transform.localPosition}");
        Check(Front("Contour1").transform.localScale.x < 0.1f, "fitted into the container");

        rx.Seek(1);
        Check(Pump(() => rx.CurrentTime == 1), "t=1 shown");
        Check(Pair("Contour1") == null, "contour absent at t=1 is removed");
        Check(Pair("isolines") == null, "deleted sender's item removed at t=1");
        var pr = Front("MaskPoints1")?.GetComponent<MeshRenderer>();
        Check(pr != null && !pr.enabled, "points hidden at t=1 (Visibility 0)");

        rx.Seek(0.5);
        Check(Pump(() => rx.CurrentTime == 0.5 && Pair("Contour1") != null && Pair("isolines") != null), "seeking back restores removed items");
        Check(Front("MaskPoints1").GetComponent<MeshRenderer>().enabled, "points visible again at t=0.5");

        Call("OnDisable");
        Debug.Log("[PVTEST] failures " + failures);
        EditorApplication.Exit(failures == 0 ? 0 : 1);
    }
}
