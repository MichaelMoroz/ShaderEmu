using TMPro;
using UdonSharp;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The console's extras (docs/console.md): buttons above its screen for the four terminals and
// for scrolling back, and above its keyboard a field whose text is typed at the console.
// And the red power button on the computer's tower.
public static partial class ShaderEmuBuilder
{
    static void Console(Transform computer, EmuTerminal terminal, EmuKeyboard keys)
    {
        foreach (string old in new[] { "Console tabs", "Console paste" })
            if (computer.Find(old) != null) Object.DestroyImmediate(computer.Find(old).gameObject);
        Color back = new Color(0.05f, 0.055f, 0.07f), dim = new Color(0.62f, 0.68f, 0.76f);
        TextMeshProUGUI unused;

        // above the screen, clear of its bezel
        Transform screen = terminal.transform;
        float screenHeight = screen.GetComponent<Renderer>().bounds.size.y;
        RectTransform tabs = Panel(computer, "Console tabs", screen.localPosition + new Vector3(0, screenHeight / 2 + 0.10f, 0), Vector3.zero,
                                   1500, 90, back);
        terminal.tabPlates = new Image[EmuTerminal.Tabs];
        for (int t = 0; t < EmuTerminal.Tabs; t++)
        {
            Button tab = MakeButton(tabs, "Terminal " + (t + 1), "Terminal " + (t + 1), 10 + t * 200, 10, 190, 70, 26, out unused);
            OnClick(tab, terminal, "Tab" + t);
            terminal.tabPlates[t] = tab.GetComponent<Image>();
        }
        terminal.tabColour = KeyColour;
        terminal.tabShownColour = new Color(0.15f, 0.45f, 0.95f);
        terminal.tabNewsColour = new Color(0.62f, 0.40f, 0.10f);
        terminal.scrollLabel = Label(tabs, "Scrolled", "", 820, 10, 220, 70, 24, TextAnchor.MiddleCenter, dim);
        OnClick(MakeButton(tabs, "Scroll up", "Up", 1050, 10, 140, 70, 26, out unused), terminal, "ScrollUp");
        OnClick(MakeButton(tabs, "Scroll down", "Down", 1200, 10, 140, 70, 26, out unused), terminal, "ScrollDown");
        OnClick(MakeButton(tabs, "Scroll end", "Latest", 1350, 10, 140, 70, 26, out unused), terminal, "ScrollEnd");
        terminal.keyboard = keys;
        Apply(terminal);

        // along the keyboard's far edge, in its plane
        RectTransform board = keys.GetComponent<RectTransform>();
        const float height = 150f;
        float width = board.sizeDelta.x;
        Vector3 at = board.localPosition + board.localRotation * new Vector3(0, (board.sizeDelta.y / 2 + height / 2 + 14f) * 0.001f, 0);
        RectTransform panel = Panel(computer, "Console paste", at, board.localEulerAngles, width, height, back);
        Label(panel, "Title", "Text for the console: click the field, type or paste (Ctrl+V), then:", 12, 4, width - 24, 30, 20, TextAnchor.MiddleLeft, dim);
        float buttons = 3 * 170 + 10, fieldWidth = width - buttons - 20;
        RectTransform box = Child(panel, "Field", 10, 40, fieldWidth, height - 50);
        Plate(box, new Color(0.14f, 0.16f, 0.22f));
        Text text = Child(box, "Text", 10, 6, fieldWidth - 20, height - 62).gameObject.AddComponent<Text>();
        text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        text.fontSize = 22;
        text.color = Color.white;
        text.alignment = TextAnchor.UpperLeft;
        text.supportRichText = false;
        InputField field = box.gameObject.AddComponent<InputField>();
        field.textComponent = text;
        field.targetGraphic = box.GetComponent<Image>();
        field.lineType = InputField.LineType.MultiLineNewline;
        field.characterLimit = 0;

        EmuPaste paste = Udon<EmuPaste>(panel.gameObject);
        paste.field = field;
        paste.keyboard = keys;
        float x = 10 + fieldWidth + 10;
        OnClick(MakeButton(panel, "Type", "Type it", x, 40, 160, height - 50, 24, out unused), paste, "Send");
        OnClick(MakeButton(panel, "Type line", "Type it\n+ Enter", x + 170, 40, 160, height - 50, 24, out unused), paste, "SendLine");
        OnClick(MakeButton(panel, "Drop", "Clear", x + 340, 40, 160, height - 50, 24, out unused), paste, "Drop");
        Text status = Child(panel, "Status", width - 260, 6, 250, 28).gameObject.AddComponent<Text>();
        status.font = text.font;
        status.fontSize = 20;
        status.color = new Color(1f, 0.8f, 0.3f);
        status.alignment = TextAnchor.MiddleRight;
        status.raycastTarget = false;
        paste.status = status;
        Apply(paste);
    }

    // The switch where a visitor looks for it: a small sign standing on the computer's tower,
    // leaning back, with a button that is red while the computer is off and green while it runs.
    // (The control panel's own button stays.)
    static void PowerSign(Transform computer, EmuMachine machine)
    {
        if (computer.Find("Power sign") != null) Object.DestroyImmediate(computer.Find("Power sign").gameObject);
        Transform tower = computer.Find("Tower");
        Vector3 top = tower.localPosition + new Vector3(0, tower.localScale.y / 2, 0);
        RectTransform sign = Panel(computer, "Power sign", top + new Vector3(0, 0.11f, -0.14f), new Vector3(32f, 0, 0), 240, 220,
                                   new Color(0.05f, 0.055f, 0.07f));
        Label(sign, "Title", "THE COMPUTER", 6, 4, 228, 28, 20, TextAnchor.MiddleCenter, Color.white);
        Label(sign, "Model", "ShaderEmu RV32, a RISC-V machine in shaders", 6, 30, 228, 34, 13, TextAnchor.MiddleCenter,
              new Color(0.62f, 0.68f, 0.76f));
        Button button = MakeButton(sign, "Power button", "POWER", 50, 70, 140, 100, 30, out machine.powerSign);
        machine.powerPlate = button.GetComponent<Image>();
        machine.powerPlate.color = new Color(0.80f, 0.07f, 0.05f);   // red while it is off, green while it runs (EmuMachine)
        machine.powerSign.color = Color.white;
        machine.powerSign.fontStyle = FontStyles.Bold;
        OnClick(button, machine, "Power");
        machine.powerSays = Label(sign, "Says", "Off. Press to start.", 6, 176, 228, 38, 16, TextAnchor.MiddleCenter,
                                  new Color(0.85f, 0.87f, 0.9f));
        Apply(machine);
    }

    [MenuItem("ShaderEmu/Add the power button to the open scene")]
    public static void AddPowerSign()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject computer = GameObject.Find("Computer");
        if (machine == null || computer == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        PowerSign(computer.transform, machine);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] power button added");
    }

    [MenuItem("ShaderEmu/Add the console's tabs and paste field to the open scene")]
    public static void AddConsole()
    {
        EmuTerminal terminal = Object.FindObjectOfType<EmuTerminal>();
        EmuKeyboard keys = null;
        foreach (EmuKeyboard k in Object.FindObjectsOfType<EmuKeyboard>())
            if (k.name == "Console keyboard") keys = k;
        if (terminal == null || keys == null) throw new System.Exception("no console in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        Console(terminal.transform.parent, terminal, keys);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(terminal.gameObject.scene);
        EditorSceneManager.SaveScene(terminal.gameObject.scene);
        Debug.Log("[ShaderEmu] console tabs and paste field added");
    }
}
