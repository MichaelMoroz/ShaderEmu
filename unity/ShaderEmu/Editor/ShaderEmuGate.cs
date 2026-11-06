using System.Collections.Generic;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using VRC.SDK3.Components;
using VRC.Udon;

// The corridor's gate (EmuGate): the world's parts are off in an uploaded build until the visitor
// leaves the corridor. The scene keeps them on, so a bake and the editor's play mode see them.
public static partial class ShaderEmuBuilder
{
    static readonly string[] Ungated = { "Light volume" };   // LTCGI is gated: it looks for its screens once, at start


    // The gate over every part with a behaviour or a station: made last, as it takes the parts there are then
    // (Build world calls it at its end).
    static void Gate(GameObject root)
    {
        Transform old = root.transform.Find("Gate");
        if (old != null) Object.DestroyImmediate(old.gameObject);

        var parts = new List<GameObject>();
        foreach (Transform child in root.transform)
        {
            GameObject go = child.gameObject;
            if (!go.activeSelf || System.Array.Exists(Ungated, u => go.name.StartsWith(u))) continue;
            if (go.GetComponentsInChildren<UdonBehaviour>(true).Length == 0
                && go.GetComponentsInChildren<VRCStation>(true).Length == 0) continue;
            parts.Add(go);
        }

        GameObject gate = new GameObject("Gate");
        gate.transform.SetParent(root.transform, false);
        EmuGate script = Udon<EmuGate>(gate);
        script.parts = parts.ToArray();
        Apply(script);
        foreach (EmuPointer pointer in root.GetComponentsInChildren<EmuPointer>(true)) { pointer.gate = script; Apply(pointer); }
        foreach (EmuPersist persist in root.GetComponentsInChildren<EmuPersist>(true)) { persist.gate = script; Apply(persist); }
        Debug.Log("[ShaderEmu] the gate holds " + parts.Count + " parts: " + string.Join(", ", parts.ConvertAll(p => p.name)));
    }
}

// Turns the gate's parts off in the scene a build takes, never in the saved scene; in play mode
// only with the editor pref ShaderEmu.GateInPlay set (a test of the gate under ClientSim).
class ShaderEmuGateBuild : IProcessSceneWithReport
{
    public int callbackOrder => 0;

    public void OnProcessScene(UnityEngine.SceneManagement.Scene scene, BuildReport report)
    {
        if (EditorApplication.isPlayingOrWillChangePlaymode && !EditorPrefs.GetBool("ShaderEmu.GateInPlay")) return;
        var program = UdonSharpEditorUtility.GetUdonSharpProgramAsset(typeof(EmuGate));
        foreach (GameObject root in scene.GetRootGameObjects())
            foreach (UdonBehaviour gate in root.GetComponentsInChildren<UdonBehaviour>(true))
            {
                if (gate.programSource != program || !gate.publicVariables.TryGetVariableValue("parts", out GameObject[] parts)) continue;
                foreach (GameObject part in parts) if (part != null) part.SetActive(false);
                Debug.Log("[ShaderEmu] gate: " + parts.Length + " parts off in the build");
            }
    }
}
