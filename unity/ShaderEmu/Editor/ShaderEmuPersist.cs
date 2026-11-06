using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The guest's kept folder (docs/system/persist.md): EmuPersist, its readback's target and material, and
// its sign on the computer's tower, over the power button's and as wide.
public static partial class ShaderEmuBuilder
{
    static void Persist(Transform computer, EmuMachine machine)
    {
        Gone(computer, "Storage sign");
        Gone(machine.transform.parent, "Persist");
        EmuPersist persist = Udon<EmuPersist>(new GameObject("Persist"));
        persist.transform.SetParent(machine.transform.parent, false);
        persist.machine = machine;
        persist.readMaterial = Mat("PersistRead", "ShaderEmu/PersistRead");
        persist.readTexture = Target("PersistRead", 256, 100, 0);
        // the power sign's plane carried on upwards: it leans back 32 degrees
        Transform power = computer.Find("Power sign");
        Vector3 up = Quaternion.Euler(32f, 0, 0) * Vector3.up;
        Vector3 at = (power != null ? power.localPosition : new Vector3(1.55f, 0.482f, 4.92f)) + up * (0.110f + 0.006f + 0.075f);
        RectTransform sign = Panel(computer, "Storage sign", at, new Vector3(32f, 0, 0), 240, 150, new Color(0.05f, 0.055f, 0.07f));
        Label(sign, "Title", "STORAGE", 6, 4, 228, 24, 18, TextAnchor.MiddleCenter, Color.white);
        persist.saysLabel = Label(sign, "Says", "Waiting for VRChat...", 6, 28, 228, 22, 13, TextAnchor.MiddleCenter, new Color(0.85f, 0.87f, 0.9f));
        TextMeshProUGUI saveLabel;
        Button save = MakeButton(sign, "Save", "SAVE /persistence", 6, 54, 228, 42, 18, out saveLabel);
        Button auto = MakeButton(sign, "Auto", "Load at start: on", 6, 102, 150, 40, 13, out persist.autoLabel);
        Button clear = MakeButton(sign, "Clear", "Clear", 162, 102, 72, 40, 13, out persist.clearLabel);
        Apply(persist);
        OnClick(save, persist, "Save");
        OnClick(auto, persist, "ToggleAuto");
        OnClick(clear, persist, "Clear");
        machine.persist = persist;
        Apply(machine);
    }

}
