using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

// A play-mode check of the world in the editor (5 November 2026), never part of the world: copy this file to the project's
// Assets/ShaderEmuCheck/Editor and SeCheckProbe.cs to Assets/ShaderEmuCheck, run keys.ps1 beside the editor for real keys
// and buttons (bounce.ps1 makes an editor in the back compile changed scripts), and delete the folder afterwards.
// Do not change a script while in play mode: the compile stops it. Escape is ClientSim's menu key too (and VRChat's):
// SeCheckProbe's "key 1" gives the guest an Escape straight. Drives the editor from <project>/se_check_auto.txt, a command a line, each taken off the file before it runs:
//   sync DIR     shader_vm's sources of the repository at DIR, as "Sync shader sources from the repository" does
//   images DIR   the boot image of the folder DIR (linux_payload.bin, rootfs.bin), as "Import boot images" does
//   scene        the world's scene opened
//   hud          "Add the holodecks' HUD layer to the open scene"
//   check        every pass of the Generated materials compiled; errors logged
//   play         the Game view in front, play mode (SeCheckProbe runs se_check_probe.txt; its "stop" ends it)
//   quit
[InitializeOnLoad]
public static class SeCheckAuto
{
    static SeCheckAuto()
    {
        Later(3.0);
        EditorApplication.playModeStateChanged += s =>
        {
            if (s == PlayModeStateChange.EnteredEditMode) Later(3.0);
            if (s == PlayModeStateChange.EnteredPlayMode && File.Exists(ProbePath))
                new GameObject("SeCheckProbe").AddComponent<SeCheckProbe>();
        };
    }

    static string Root { get { return Path.GetDirectoryName(Application.dataPath); } }
    static string FilePath { get { return Path.Combine(Root, "se_check_auto.txt"); } }
    static string ProbePath { get { return Path.Combine(Root, "se_check_probe.txt"); } }

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

    static void Log(string s)
    {
        Debug.Log("[SeCheck] " + s);
        File.AppendAllText(Path.Combine(Root, "Logs", "se_check_auto.log"), System.DateTime.Now.ToString("HH:mm:ss ") + s + "\n");
    }

    static void Sync(string repo)
    {
        string from = repo + "/shader_vm", to = "Assets/ShaderEmu/Shaders";
        foreach (string file in new[] { "crt.cginc", "helpers.cginc" }) Write(to + "/" + file, File.ReadAllText(from + "/" + file));
        Directory.CreateDirectory(to + "/src");
        int n = 0;
        foreach (string path in Directory.GetFiles(from + "/src", "*.h"))
        {
            string text = Regex.Replace(File.ReadAllText(path), "(#include\\s+\"[^\"]+)\\.h\"", "$1.cginc\"");
            if (Write(to + "/src/" + Path.GetFileNameWithoutExtension(path) + ".cginc", text)) n++;
        }
        AssetDatabase.Refresh();
        Log("sync: " + n + " sources changed from " + from);
    }

    static bool Write(string path, string text)
    {
        if (File.Exists(path) && File.ReadAllText(path) == text) return false;
        File.WriteAllText(path, text);
        return true;
    }

    static bool Check()
    {
        int errors = 0, passes = 0;
        if (EditorUtility.scriptCompilationFailed) { Log("the scripts do not compile"); return false; }
        foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { "Assets/ShaderEmu/Generated" }))
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(guid));
            if (m == null || m.shader == null) continue;
            for (int i = 0; i < m.passCount; i++, passes++) ShaderUtil.CompilePass(m, i, true);
            foreach (ShaderMessage msg in ShaderUtil.GetShaderMessages(m.shader))
                if (msg.severity == UnityEditor.Rendering.ShaderCompilerMessageSeverity.Error)
                {
                    errors++;
                    Log("ERROR " + m.shader.name + ": " + msg.message + " (" + msg.file + ":" + msg.line + ")");
                }
        }
        Log("check: " + passes + " passes compiled, " + errors + " errors");
        return errors == 0;
    }

    static void Run()
    {
        if (EditorApplication.isPlayingOrWillChangePlaymode) return;
        if (EditorApplication.isCompiling || EditorApplication.isUpdating) { Later(1.0); return; }
        // (looked for again every few seconds, and the scripts refreshed: the editor in the back refreshes nothing by itself)
        if (!File.Exists(FilePath))
        {
            if (File.Exists(Path.Combine(Root, "se_check_refresh.txt"))) { File.Delete(Path.Combine(Root, "se_check_refresh.txt")); AssetDatabase.Refresh(); }
            Later(3.0);
            return;
        }
        while (File.Exists(FilePath))
        {
            List<string> lines = new List<string>(File.ReadAllLines(FilePath));
            while (lines.Count > 0 && lines[0].Trim().Length == 0) lines.RemoveAt(0);
            if (lines.Count == 0) { File.Delete(FilePath); Later(3.0); return; }
            string cmd = lines[0].Trim();
            lines.RemoveAt(0);
            File.WriteAllLines(FilePath, lines);
            Log(cmd);
            if (cmd.StartsWith("sync ")) { Sync(cmd.Substring(5).Trim()); Later(5.0); return; }
            else if (cmd.StartsWith("images "))
            {
                string dir = cmd.Substring(7).Trim();
                File.Copy(dir + "/linux_payload.bin", ShaderEmuImages.RamPath("linux"), true);
                File.Copy(dir + "/rootfs.bin", ShaderEmuImages.RomPath("linux"), true);
                AssetDatabase.Refresh();
                Log("images from " + dir);
                Later(5.0);
                return;
            }
            else if (cmd == "scene") EditorSceneManager.OpenScene("Assets/ShaderEmu/ShaderEmuWorld.unity", OpenSceneMode.Single);
            else if (cmd == "check") Check();
            else if (cmd == "play")
            {
                EditorApplication.ExecuteMenuItem("Window/General/Game");
                EditorApplication.isPlaying = true;
                return;
            }
            else if (cmd == "quit") { EditorApplication.Exit(0); return; }
            else Log("unknown: " + cmd);
        }
    }
}
