using UdonSharpEditor;
using UnityEditor;
using UnityEngine;

// The computer's own sounds (EmuPcSound, world/sounds.py), at its tower by the desk.
public static partial class ShaderEmuBuilder
{
    static void PcSounds(Transform world)
    {
        Transform computer = world.Find("Computer");
        Transform tower = computer != null ? computer.Find("Tower") : null;
        EmuMachine machine = null;
        foreach (EmuMachine m in world.GetComponentsInChildren<EmuMachine>(true)) machine = m;
        if (tower == null || machine == null) return;
        Gone(world, "Computer sounds");
        if (UdonSharpEditorUtility.GetUdonSharpProgramAsset(typeof(EmuPcSound)) == null)
        {
            CreateProgramAssets();
            Debug.LogWarning("[ShaderEmu] EmuPcSound's program was just made: run this command once more for the computer's sounds");
            return;
        }
        Transform root = new GameObject("Computer sounds").transform;
        root.SetParent(world, false);
        Vector3 at = world.InverseTransformPoint(tower.position);
        string sounds = Root + "/Sounds/";
        EmuPcSound pc = Udon<EmuPcSound>(root.gameObject);
        pc.machine = machine;
        pc.running = Sound(root, "Running", at, AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "PcRun.wav"), 0f, 2f, 40f, true);   // heard all over the den, nearer louder
        pc.running.playOnAwake = false;
        pc.voice = Sound(root, "Voice", at, null, 0.8f, 2f, 40f, false);
        pc.boot = AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "PcBoot.wav");
        pc.off = AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "PcOff.wav");
        pc.seeks = new AudioClip[4];
        for (int i = 0; i < 4; i++) pc.seeks[i] = AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "PcSeek" + (i + 1) + ".wav");
        Apply(pc);
    }

    [MenuItem("ShaderEmu/Add the computer's sounds to the open scene")]
    public static void AddPcSounds()
    {
        GameObject world = GameObject.Find("ShaderEmu");
        if (world == null) throw new System.Exception("no world in the open scene: run ShaderEmu/Build world");
        PcSounds(world.transform);
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.scene);
    }
}
