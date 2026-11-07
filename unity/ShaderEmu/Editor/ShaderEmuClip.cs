using System.Collections.Generic;
using TMPro;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

// The text field in front of the screen (docs/system/clipboard.md): EmuClip, its readback's target and material,
// its panel before the wall's display (a classroom place's tube has it before itself: EmuClip.Place), and the
// Clipboard keys of the console's and the display's strips and of a classroom place's (ShaderEmuStations.cs).
public static partial class ShaderEmuBuilder
{
    // Keys made before the field: wired to it when it is made (the console's strip comes before the machine).
    static readonly List<Button> ClipKeys = new List<Button>();

    static Button ClipKey(RectTransform panel, float x, float y, float w, float h, int size)
    {
        TextMeshProUGUI unused;
        Button key = MakeButton(panel, "Clipboard", "Clipboard", x, y, w, h, size, out unused);
        ClipKeys.Add(key);
        return key;
    }

    static void Clip(Transform computer, EmuMachine machine, Vector3 dispAt)
    {
        Gone(computer, "Clip panel");
        Gone(machine.transform.parent, "Clip");
        Color dim = new Color(0.62f, 0.68f, 0.76f), amber = new Color(1f, 0.8f, 0.3f);
        TextMeshProUGUI unused;
        EmuClip clip = Udon<EmuClip>(new GameObject("Clip"));
        clip.transform.SetParent(machine.transform.parent, false);
        clip.machine = machine;
        clip.readMaterial = Mat("ClipRead", "ShaderEmu/PersistRead");
        clip.readTexture = Target("ClipRead", 256, 256, 0);

        // (a hair before the links panel, which may be open too)
        RectTransform panel = Panel(computer, "Clip panel", dispAt + new Vector3(0, 0, -0.32f), Vector3.zero, 1700, 900,
                                    new Color(0.05f, 0.055f, 0.07f, 0.97f));
        clip.titleLabel = Label(panel, "Title", "", 30, 14, 1420, 70, 24, TextAnchor.MiddleLeft, Color.white);
        OnClick(MakeButton(panel, "Close", "Close", 1490, 16, 180, 70, 26, out unused), clip, "Close");
        RectTransform box = Child(panel, "Field", 30, 96, 1640, 700);
        Plate(box, new Color(0.14f, 0.16f, 0.22f));
        Text text = Child(box, "Text", 12, 8, 1616, 684).gameObject.AddComponent<Text>();
        text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        text.material = UiMaterial();
        text.fontSize = 16;
        text.color = Color.white;
        text.alignment = TextAnchor.UpperLeft;
        text.horizontalOverflow = HorizontalWrapMode.Wrap;
        text.verticalOverflow = VerticalWrapMode.Truncate;
        text.supportRichText = false;
        InputField field = box.gameObject.AddComponent<InputField>();
        field.textComponent = text;
        field.targetGraphic = box.GetComponent<Image>();
        field.lineType = InputField.LineType.MultiLineNewline;
        field.characterLimit = 0;
        clip.field = field;
        Button back = MakeButton(panel, "Last page", "< Page", 30, 810, 180, 70, 26, out unused);
        Button on = MakeButton(panel, "Next page", "Page >", 220, 810, 180, 70, 26, out unused);
        OnClick(back, clip, "LastPage");
        OnClick(on, clip, "NextPage");
        clip.pageLabel = Label(panel, "Page", "", 420, 810, 560, 70, 22, TextAnchor.MiddleLeft, dim);
        clip.giveLabel = Label(panel, "Given", "", 990, 810, 420, 70, 20, TextAnchor.MiddleRight, amber);
        Button give = MakeButton(panel, "Give", "To the machine", 1430, 810, 240, 70, 26, out unused);
        OnClick(give, clip, "Give");
        clip.panel = panel.gameObject;
        clip.backButton = back.gameObject;
        clip.onButton = on.gameObject;
        clip.giveButton = give.gameObject;
        Apply(clip);
        machine.clip = clip;
        Apply(machine);
        foreach (Button key in ClipKeys)
            if (key != null) OnClick(key, clip, "ShowBoard");
        ClipKeys.Clear();
    }
}
