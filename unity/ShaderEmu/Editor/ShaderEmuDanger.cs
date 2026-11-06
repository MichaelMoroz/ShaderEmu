// The "Danger zone" panel (docs/world/danger.md): on the right wall by the rack, the keys that let a
// visitor harm their own machine's memory and watch what it does. A canvas: no bake.
using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

public static partial class ShaderEmuBuilder
{
    static void Danger(Transform world, Transform computer, EmuMachine machine)
    {
        Gone(world, "Danger zone");
        Gone(world, "Danger");
        Color back = new Color(0.07f, 0.045f, 0.04f), dim = new Color(0.62f, 0.68f, 0.76f), amber = new Color(1f, 0.8f, 0.3f);
        Color warn = new Color(1f, 0.82f, 0.1f);
        const float width = 760, height = 600, key = 170, tall = 54;
        RectTransform panel = Panel(world, "Danger zone", new Vector3(4.44f, 1.47f, 3.5f), new Vector3(0, 90, 0), width, height, back);
        EmuDanger danger = Udon<EmuDanger>(new GameObject("Danger"));
        danger.transform.SetParent(world, false);
        danger.machine = machine;
        danger.scope = Object.FindObjectOfType<EmuScope>();
        danger.memoryView = Mat("MemView", "ShaderEmu/MemView");
        danger.plain = KeyColour;
        danger.picked = new Color(0.75f, 0.2f, 0.12f);
        TextMeshProUGUI unused;

        // a band of warning stripes, and what this is
        for (int i = 0; i < 19; i++)
            Plate(Child(panel, "Stripe " + i, i * 40f, 0, 40, 16), i % 2 == 0 ? warn : new Color(0.06f, 0.06f, 0.06f)).raycastTarget = false;
        Label(panel, "Title", "DANGER ZONE", 14, 24, 440, 44, 34, TextAnchor.MiddleLeft, warn);
        Label(panel, "Help", "Your own machine's memory, harmed on purpose: only yours.", 14, 68, width - 28, 28, 18, TextAnchor.MiddleLeft, dim);

        // radiation: single bits flipped at random
        Label(panel, "Radiation title", "Stray particles flip one bit at a time, anywhere in what is exposed", 14, 108, width - 28, 26, 17,
              TextAnchor.MiddleLeft, dim);
        Button radiation = MakeButton(panel, "Radiation", "Radiation: off", 14, 138, 250, tall, 24, out danger.radiationLabel);
        Button fewer = MakeButton(panel, "Rate down", "-", 276, 138, 60, tall, 30, out unused);
        danger.rateLabel = Label(panel, "Rate", "", 342, 138, 240, tall, 22, TextAnchor.MiddleCenter, amber);
        Button more = MakeButton(panel, "Rate up", "+", 588, 138, 60, tall, 30, out unused);

        // the laser: a square of memory scrambled where a beam points on the memory screen
        Label(panel, "Laser title", "Point at the memory screen (showing RAM) and hold the trigger: it burns what is there", 14, 206,
              width - 28, 26, 17, TextAnchor.MiddleLeft, dim);
        Button laser = MakeButton(panel, "Laser", "Laser: off", 14, 236, 250, tall, 24, out danger.laserLabel);
        Button finer = MakeButton(panel, "Brush down", "-", 276, 236, 60, tall, 30, out unused);
        danger.brushLabel = Label(panel, "Brush", "", 342, 236, 240, tall, 22, TextAnchor.MiddleCenter, amber);
        Button wider = MakeButton(panel, "Brush up", "+", 588, 236, 60, tall, 30, out unused);

        // which memory may be harmed: the memory screen's own six kinds
        Label(panel, "Zones title", "What is exposed (the memory screen's legend says where each is)", 14, 304, width - 28, 26, 17,
              TextAnchor.MiddleLeft, dim);
        Button[] zones = new Button[6];
        danger.zoneLabels = new TextMeshProUGUI[6];
        danger.zonePlates = new Image[6];
        for (int i = 0; i < 6; i++)
        {
            zones[i] = MakeButton(panel, "Zone " + i, "", 14 + (i % 3) * (240 + 6), 334 + (i / 3) * (tall + 8), 240, tall, 20, out danger.zoneLabels[i]);
            danger.zonePlates[i] = zones[i].GetComponent<Image>();
        }
        Label(panel, "Count title", "So far", 14, 470, key, 40, 20, TextAnchor.MiddleLeft, dim);
        danger.countLabel = Label(panel, "Count", "", 100, 470, width - 114, 40, 22, TextAnchor.MiddleLeft, amber);
        Label(panel, "Mend", "Reset on the control panel starts the machine over, whole again.", 14, 520, width - 28, 28, 18,
              TextAnchor.MiddleLeft, dim);
        for (int i = 0; i < 19; i++)
            Plate(Child(panel, "Stripe low " + i, i * 40f, height - 16, 40, 16), i % 2 == 1 ? warn : new Color(0.06f, 0.06f, 0.06f)).raycastTarget = false;

        Apply(danger);
        OnClick(radiation, danger, "ToggleRadiation");
        OnClick(fewer, danger, "RateDown");
        OnClick(more, danger, "RateUp");
        OnClick(laser, danger, "ToggleLaser");
        OnClick(finer, danger, "BrushDown");
        OnClick(wider, danger, "BrushUp");
        for (int i = 0; i < 6; i++) OnClick(zones[i], danger, "Zone" + i);

        // the memory screen answers a beam now: a trigger over its glass, which stops nothing
        Transform screen = computer.Find("Memory screen");
        BoxCollider glass = screen.GetComponent<BoxCollider>();
        if (glass == null) glass = screen.gameObject.AddComponent<BoxCollider>();
        glass.isTrigger = true;
        glass.size = new Vector3(1f, 1f, 0.01f);
        EmuPointer pointer = Object.FindObjectOfType<EmuPointer>();
        if (pointer != null)
        {
            UdonSharpEditorUtility.CopyUdonToProxy(pointer);
            pointer.memoryScreen = glass;
            pointer.danger = danger;
            Apply(pointer);
        }
    }

}
