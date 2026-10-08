using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Events;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The memory screen's other pictures and the legend over the RAM view (EmuScope), and the
// panel's button for how the speed slider is read (EmuMachine's fixed and steady modes).
public static partial class ShaderEmuBuilder
{
    static Material ScopeMaterial(string name, int mode)
    {
        Material m = Mat(name, "ShaderEmu/Scope");
        m.SetInt("_Mode", mode);
        return m;
    }

    static void Scope(Transform world, Transform computer, RectTransform panel, EmuMachine machine)
    {
        foreach (string old in new[] { "Scope", "Memory legend", "CPU screen", "CPU sign" })
        {
            Transform found = old == "Scope" ? world.Find(old) : computer.Find(old);
            if (found != null) Object.DestroyImmediate(found.gameObject);
        }
        for (int i = panel.childCount - 1; i >= 0; i--)
            if (panel.GetChild(i).name.StartsWith("Scope ") || panel.GetChild(i).name == "Speed mode")
                Object.DestroyImmediate(panel.GetChild(i).gameObject);

        Transform screen = computer.Find("Memory screen");
        MeshRenderer renderer = screen.GetComponent<MeshRenderer>();
        Material ram = Mat("MemView", "ShaderEmu/MemView");
        ram.SetInt("_Strips", 2);
        Material cpu = ScopeMaterial("ScopeCpu", 1), rom = ScopeMaterial("ScopeRom", 2), wave = ScopeMaterial("ScopeSound", 3);
        RenderTexture mix = AssetDatabase.LoadAssetAtPath<RenderTexture>(Generated + "/SoundMix.renderTexture");
        if (mix != null) wave.SetTexture("_Mix", mix);

        // The legend: a canvas over the screen, a millimetre a unit as the panels are. The
        // screen's two strips are 63 MiB each; the names sit beside their colours.
        float wide = screen.localScale.x * 1000f, high = screen.localScale.y * 1000f;
        RectTransform legend = Panel(computer, "Memory legend", screen.localPosition + new Vector3(0, 0, -0.004f), Vector3.zero,
                                     wide, high, new Color(0, 0, 0, 0));
        Object.DestroyImmediate(legend.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(legend.GetComponent<BoxCollider>());
        Object.DestroyImmediate(legend.GetComponent<GraphicRaycaster>());
        string[] names = { "Linux: the kernel, then its memory", "Linux", "window buffers", "display, GPU lists, fonts, textures", "a 3D program's", "sound" };
        // megabytes at which each name's region is centred; the first is in the left strip
        float[] middle = { 6.5f, 79.5f, 104f, 115.5f, 121f, 124.5f };
        for (int i = 0; i < names.Length; i++)
        {
            float strip = Mathf.Floor(middle[i] / 63f), y = (middle[i] / 63f - strip) * high;
            float x = strip * wide / 2 + wide / 2 * 0.022f + 8f, w = names[i].Length * 11f + 16f;
            RectTransform plate = Child(legend, "Name " + i, x, Mathf.Clamp(y - 13f, 2f, high - 28f), w, 26f);
            Plate(plate, new Color(0, 0, 0, 0.62f)).raycastTarget = false;
            Label(plate, "Text", names[i], 6, 0, w - 8f, 26f, 18, TextAnchor.MiddleLeft, Color.white);
        }

        // The CPU's state texels are few: a small square screen of their own, always on, in
        // the gap above the players' panel, between the display and the memory screen. No
        // frame: a box on the wall would want the light baked again. Its name is on it.
        const float cpuSide = 0.38f;   // from the players' panel up to the light strip
        Transform players = computer.Find("Players panel");
        float cpuX = players != null ? players.localPosition.x : screen.localPosition.x - screen.localScale.x / 2 - 0.32f;
        Vector3 cpuAt = new Vector3(cpuX, 2.54f, screen.localPosition.z - 0.002f);   // a little in front of the signs' canvas
        Screen(computer, "CPU screen", cpuAt, cpuSide, cpuSide, cpu);
        RectTransform cpuSign = Panel(computer, "CPU sign", cpuAt + new Vector3(0, 0, -0.004f), Vector3.zero, cpuSide * 1000, cpuSide * 1000,
                                      new Color(0, 0, 0, 0));
        Object.DestroyImmediate(cpuSign.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(cpuSign.GetComponent<BoxCollider>());
        Object.DestroyImmediate(cpuSign.GetComponent<GraphicRaycaster>());
        RectTransform cpuName = Child(cpuSign, "Name", 4, 4, 124, 26);
        Plate(cpuName, new Color(0, 0, 0, 0.62f)).raycastTarget = false;
        Label(cpuName, "Text", "CPU state", 6, 0, 116, 26, 18, TextAnchor.MiddleLeft, Color.white);

        EmuScope scope = Udon<EmuScope>(new GameObject("Scope"));
        scope.transform.SetParent(world, false);
        scope.screen = renderer;
        scope.views = new[] { ram, rom, wave };
        scope.legend = legend.gameObject;
        GameObject signs = GameObject.Find("Signs");
        if (signs != null && signs.transform.Find("Memory") != null) scope.sign = signs.transform.Find("Memory").GetComponent<TextMeshProUGUI>();
        scope.plain = KeyColour;
        scope.picked = new Color(0.15f, 0.45f, 0.95f);

        // on the panel: which picture, under the sound controls
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        const float bx = 1060;
        Label(panel, "Scope title", "The memory screen shows", bx, 584, 600, 30, 22, TextAnchor.MiddleLeft, dim);
        string[] texts = { "RAM", "ROM", "Sound" }, methods = { "ShowRam", "ShowRom", "ShowSound" };
        Button[] buttons = new Button[3];
        scope.plates = new Image[3];
        TextMeshProUGUI unused;
        for (int i = 0; i < 3; i++)
        {
            buttons[i] = MakeButton(panel, "Scope " + texts[i], texts[i], bx + i * 205, 618, 190, 60, 24, out unused);
            scope.plates[i] = buttons[i].GetComponent<Image>();
        }
        Apply(scope);
        for (int i = 0; i < 3; i++) OnClick(buttons[i], scope, methods[i]);

        // and how the speed slider is read, beside its title
        Button mode = MakeButton(panel, "Speed mode", "Fixed: per frame", bx + 300, 190, 300, 44, 22, out machine.modeLabel);
        machine.stateViewMaterial = cpu;
        machine.romViewMaterial = rom;
        Apply(machine);
        OnClick(mode, machine, "SpeedMode");
    }

    // Into the scene as it is, without building the world again (which would need a bake).
    [MenuItem("ShaderEmu/Add the memory screen's views and speed modes to the open scene")]
    public static void AddScope()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panel = GameObject.Find("Control panel");
        if (machine == null || panel == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        Scope(machine.transform.parent, GameObject.Find("Computer").transform, panel.GetComponent<RectTransform>(), machine);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] memory screen views and speed modes added");
    }
}
