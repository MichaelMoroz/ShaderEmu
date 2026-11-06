using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// The pens (docs/world/world.md, "The pens"): ureishi's QvPen, as the fluid world has it, a row of
// fifteen pens and three erasers under the board on the den's left wall. Its own package
// (world/install_packages.py), its own prefab and look; it stands outside "ShaderEmu", which
// the restyle and the build go through.
public static partial class ShaderEmuBuilder
{
    const string PenPrefab = "Packages/net.ureishi.qvpen/QvPen.prefab";

    static void Pens()
    {
        GameObject old = GameObject.Find("QvPen");
        if (old != null && old.transform.parent == null) Object.DestroyImmediate(old);
        GameObject prefab = AssetDatabase.LoadAssetAtPath<GameObject>(PenPrefab);
        if (prefab == null) { Debug.LogWarning("[ShaderEmu] no QvPen package: run world/install_packages.py"); return; }
        GameObject pens = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
        pens.name = "QvPen";
        // the row runs along the wall (its x), the pens before it (its -z into the room), under
        // the board's foot at 1.01 m; the settings' keys at its start, clear of the board's edge
        pens.transform.SetPositionAndRotation(new Vector3(-4.38f, 0.70f, -1.05f), Quaternion.Euler(0f, -90f, 0f));
    }

}
