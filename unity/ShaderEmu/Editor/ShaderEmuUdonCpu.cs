using System.IO;
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;
using VRC.Udon;

// The second machine model (experiments/udon_aot): the kernel translated to Udon, its program from
// build/udon_linux (made with L2U_DIRTY=1), its driver EmuUdonCpu and a toggle on the control panel.
public static partial class ShaderEmuBuilder
{
    [MenuItem("ShaderEmu/Add the Udon machine to the open scene")]
    public static void AddUdonCpu()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panelObject = GameObject.Find("Control panel");
        if (machine == null || panelObject == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        string dir = ShaderEmuImages.Repo + "/build/udon_linux";
        if (!File.Exists(dir + "/prog.syms")) throw new System.Exception("no translated kernel: run experiments/udon_aot/l2u/l2u.exe into build/udon_linux");
        CreateProgramAssets();
        UdonSharpEditorUtility.CopyUdonToProxy(machine);
        Transform world = machine.transform.parent;
        RectTransform panel = panelObject.GetComponent<RectTransform>();
        Gone(world, "Udon machine");
        Gone(panel, "Udon CPU");
        Gone(panel, "Udon CPU status");

        UdonAotProgramAsset program = LoadOrCreate(Generated + "/UdonLinux.asset", () => ScriptableObject.CreateInstance<UdonAotProgramAsset>());
        program.RefreshProgram();
        EditorUtility.SetDirty(program);

        GameObject root = new GameObject("Udon machine");
        root.transform.SetParent(world, false);
        GameObject cpuObject = new GameObject("Udon CPU program");
        cpuObject.transform.SetParent(root.transform, false);
        UdonBehaviour cpu = cpuObject.AddComponent<UdonBehaviour>();
        cpu.programSource = program;
        SerializedObject so = new SerializedObject(cpu);
        so.FindProperty("serializedProgramAsset").objectReferenceValue = program.SerializedProgramAsset;
        so.ApplyModifiedPropertiesWithoutUndo();

        EmuUdonCpu drv = Udon<EmuUdonCpu>(root);
        drv.cpu = cpu;
        drv.machine = machine;
        drv.ramLanes = new Texture2D[] { machine.imageTextures[0], machine.imageTextures[1], machine.imageTextures[2], machine.imageTextures[3] };
        // (the machine's ROM is one RGBAFloat texture; the Udon model reads lanes: ShaderEmu/Import the ROM's lanes for the Udon model)
        drv.romLanes = new Texture2D[4];
        for (int lane = 0; lane < 4; lane++)
            drv.romLanes[lane] = AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath(ShaderEmuImages.Images[0].name, "rom", lane));
        if (drv.romLanes[0] == null) Debug.LogWarning("[ShaderEmu] the Udon model has no ROM: run ShaderEmu/Import the ROM's lanes for the Udon model");
        drv.unpackMaterial = Mat("UdonAotUnpack", "ShaderEmu/UdonAotUnpack");
        drv.unpackTarget = Target("UdonAotUnpack", 2048, 2048, 0);
        drv.pageStages = new Texture2D[] { PageStage("UdonAotPages64", 64), PageStage("UdonAotPages256", 256), PageStage("UdonAotPages1024", 1024) };
        drv.scatterMaterial = Mat("UdonAotScatter", "ShaderEmu/UdonAotScatter");
        drv.wordsMaterial = Mat("UdonAotWords", "ShaderEmu/UdonAotWords");
        drv.devWords = Target("UdonAotDevWords", 1024, 3, 0);
        drv.fetchWords = Target("UdonAotFetchWords", 1024, 64, 0);
        drv.copyWords = Target("UdonAotCopyWords", 1024, 2048, 0);
        foreach (string line in File.ReadAllLines(dir + "/prog.syms"))
        {
            string[] p = line.Split(' ');
            if (p[0] == "@textwords") drv.textWords = int.Parse(p[1]);
            if (p[0] == "@textsum") drv.textSum = int.Parse(p[1]);
        }
        Button toggle = MakeButton(panel, "Udon CPU", "CPU: shader", ToggleX, ToggleY, ToggleW, ToggleH, 24, out drv.powerLabel);
        drv.label = Label(panel, "Udon CPU status", "The shader CPU runs the machine", ToggleX + ToggleW + 15, ToggleY, StatusW, ToggleH, 19,
                          TextAnchor.MiddleLeft, new Color(0.85f, 0.87f, 0.9f));
        Apply(drv);
        OnClick(toggle, drv, "ToggleCpu");
        RetroNewPanels();
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] Udon machine added: " + program.SerializedProgramAsset.GetSerializedProgramSize() + " bytes of program");
    }

    // where the toggle between the processors sits on the control panel
    const float ToggleX = 1060, ToggleY = 14, ToggleW = 190, ToggleH = 60, StatusW = 395;   // the title row's right, free

    // A page a row (1,024 words, a word a pixel) under a row of page numbers
    static Texture2D PageStage(string name, int rows)
    {
        Texture2D t = LoadOrCreate(Generated + "/" + name + ".asset", () => new Texture2D(1024, rows, TextureFormat.RGBA32, false, true));
        t.filterMode = FilterMode.Point;
        t.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(t);
        return t;
    }
}
