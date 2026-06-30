using UnityEngine;
using UnityEditor;
using System.IO;

namespace ParaViewLink.Editor
{
    /// <summary>
    /// Runs once when the package is first imported (and on every subsequent
    /// editor startup, but exits immediately if the prefab already exists).
    ///
    /// Creates:
    ///   Assets/ParaViewLink/SimContainer.prefab  — ready-to-use container cube
    ///
    /// The material (M_ScalarField) is shipped inside the package itself at
    ///   Packages/com.paraviewlink.unity/Materials/M_ScalarField.mat
    /// and is pre-wired to the bundled CoolToWarm colormap texture, so no
    /// per-project material copy is needed.
    /// </summary>
    [InitializeOnLoad]
    public static class ParaViewLinkSetup
    {
        private const string OutputDir  = "Assets/ParaViewLink";
        private const string PrefabPath = "Assets/ParaViewLink/SimContainer.prefab";
        private const string PackageMat = "Packages/com.paraviewlink.unity/Materials/M_ScalarField.mat";

        static ParaViewLinkSetup()
        {
            EditorApplication.delayCall += CreateAssets;
        }

        private static void CreateAssets()
        {
            if (TryCreatePrefab())
            {
                AssetDatabase.SaveAssets();
                AssetDatabase.Refresh();
            }
        }

        // ---------------------------------------------------------------
        // SimContainer prefab
        // ---------------------------------------------------------------

        private static bool TryCreatePrefab()
        {
            if (AssetDatabase.LoadAssetAtPath<GameObject>(PrefabPath) != null)
                return false;

            // Load the material shipped inside the package.
            Material mat = AssetDatabase.LoadAssetAtPath<Material>(PackageMat);
            if (mat == null)
                Debug.LogWarning(
                    "[ParaViewLink] Could not load package material — " +
                    $"expected at {PackageMat}. Prefab will be created without it.");

            // Build the GameObject in memory.
            // Keep the MeshRenderer and Collider so the cube is visible and
            // selectable in the Scene view.  SimContainerObject.Awake() disables
            // the renderer at runtime so it doesn't appear in the game.
            GameObject go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = "SimContainer";

            // Add ParaViewLink components.
            go.AddComponent<SimContainerObject>();
            MeshReceiver receiver = go.AddComponent<MeshReceiver>();
            if (mat != null)
                receiver.BaseScalarMaterial = mat;

            // Save as a prefab asset.
            EnsureDir();
            bool success;
            PrefabUtility.SaveAsPrefabAsset(go, PrefabPath, out success);
            Object.DestroyImmediate(go);

            if (success)
                Debug.Log(
                    $"[ParaViewLink] Created {PrefabPath}. " +
                    "Drag it into your scene to get started.");
            else
                Debug.LogWarning("[ParaViewLink] Failed to create SimContainer prefab.");

            return success;
        }

        // ---------------------------------------------------------------

        private static void EnsureDir()
        {
            if (!Directory.Exists(OutputDir))
                Directory.CreateDirectory(OutputDir);
        }
    }
}
