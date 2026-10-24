using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// Drives the editor from the project's gvm_auto.txt (unity/gpuvm/README.md, "Measuring"), a command a line, each taken
// off the file before it runs:
//   image DIR   the boot image's folder (EditorPrefs ShaderEmu.GvmImage)
//   programs    the Udon program assets made and compiled (before "scene": U# serializes no behaviour of a script
//               whose program has not compiled yet)
//   scene       ShaderEmu/GPU-VM: make the machine's scene
//   check       every pass of the machine's materials compiled now; on any error the rest of the file is dropped
//               (a shader that has not compiled cleanly is never run on the card)
//   play        the scene opened, the Game view in front, play mode (GvmProbe runs gvm_probe.txt; its "stop" ends it)
//   quit        the editor exits
// The rest of the file is taken up again when play mode has ended.
[InitializeOnLoad]
public static class GvmAuto
{
    static GvmAuto()
    {
        // (a delayCall set while the editor starts is dropped: an update a few seconds on)
        Later(3.0);
        EditorApplication.playModeStateChanged += s => { if (s == PlayModeStateChange.EnteredEditMode) Later(3.0); };
    }

    static void Later(double seconds)
    {
        double at = EditorApplication.timeSinceStartup + seconds;
        EditorApplication.CallbackFunction wait = null;
        wait = () =>
        {
            if (EditorApplication.timeSinceStartup < at) return;
            EditorApplication.update -= wait;
            Run();
        };
        EditorApplication.update += wait;
    }

    static bool Check()
    {
        int errors = 0, passes = 0;
        if (EditorUtility.scriptCompilationFailed)
        {
            Debug.LogError("[GvmAuto] the scripts do not compile: the editor would play the last ones that did");
            return false;
        }
        foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { "Assets/ShaderEmu/Generated" }))
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(guid));
            for (int i = 0; i < m.passCount; i++, passes++) ShaderUtil.CompilePass(m, i, true);
            foreach (ShaderMessage msg in ShaderUtil.GetShaderMessages(m.shader))
                if (msg.severity == UnityEditor.Rendering.ShaderCompilerMessageSeverity.Error)
                {
                    errors++;
                    Debug.LogError("[GvmAuto] " + m.shader.name + ": " + msg.message + " (" + msg.file + ":" + msg.line + ")");
                }
            if (ShaderUtil.ShaderHasError(m.shader)) { errors++; Debug.LogError("[GvmAuto] " + m.shader.name + " has an error"); }
        }
        Debug.Log("[GvmAuto] check: " + passes + " passes compiled, " + errors + " errors");
        return errors == 0;
    }

    static string FilePath { get { return Path.Combine(Path.GetDirectoryName(Application.dataPath), "gvm_auto.txt"); } }

    static void Run()
    {
        if (EditorApplication.isPlayingOrWillChangePlaymode) return;
        if (EditorApplication.isCompiling || EditorApplication.isUpdating)
        {
            Later(1.0);
            return;
        }
        while (File.Exists(FilePath))
        {
            List<string> lines = new List<string>(File.ReadAllLines(FilePath));
            while (lines.Count > 0 && lines[0].Trim().Length == 0) lines.RemoveAt(0);
            if (lines.Count == 0) { File.Delete(FilePath); return; }
            string cmd = lines[0].Trim();
            lines.RemoveAt(0);
            File.WriteAllLines(FilePath, lines);
            Debug.Log("[GvmAuto] " + cmd);
            if (cmd.StartsWith("image ")) EditorPrefs.SetString("ShaderEmu.GvmImage", cmd.Substring(6).Trim());
            else if (cmd == "programs")
            {
                ShaderEmuBuilder.CreateProgramAssets();
                Later(10.0);   // the compile and the domain's reload that follow it
                return;
            }
            else if (cmd == "scene") ShaderEmuBuilder.GvmScene();
            else if (cmd == "check")
            {
                if (!Check())
                {
                    File.Delete(FilePath);
                    return;
                }
            }
            else if (cmd == "play")
            {
                EditorSceneManager.OpenScene("Assets/ShaderEmu/GvmMachine.unity", OpenSceneMode.Single);
                EditorApplication.ExecuteMenuItem("Window/General/Game");   // the Game view in front: else the Scene view draws too
                EditorApplication.isPlaying = true;
                return;
            }
            else if (cmd == "quit")
            {
                EditorApplication.Exit(0);
                return;
            }
            else Debug.LogWarning("[GvmAuto] unknown: " + cmd);
        }
    }
}
