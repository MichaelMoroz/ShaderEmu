using System.Collections.Generic;
using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// Physical buttons (docs/world/world.md, "Physical buttons"): a switch on the control panel, and
// the panels whose buttons and sliders a fingertip presses (EmuPointer.Touch).
public static partial class ShaderEmuBuilder
{
    // The control panel's words under the read-out, beside the switch.
    public const string PanelHelp =
        "Your machine is a computer emulated in a pixel shader on your own graphics card; every player runs their own, and can show you theirs.\n" +
        "Each hand has a beam. On the big screen it is the mouse: trigger = left, grip = right, right stick = wheel. " +
        "On a keyboard a key stays down while the trigger is held; Shift, Ctrl and Alt latch.\n" +
        "Physical buttons ON: a fingertip presses keys, buttons and sliders. Steady pointer ON: the beam's mouse is smoothed and a click holds still.\n" +
        "Desktop: 'Use my keyboard' types with the real one.";

    // The switches beside the panel's words, which they narrow.
    static void PhysicalSwitch(RectTransform panel, EmuPointer pointer)
    {
        Gone(panel, "Physical buttons");
        Gone(panel, "Steady pointer");
        Transform help = panel.Find("Help");
        if (help != null)
        {
            RectTransform rect = (RectTransform)help;
            rect.sizeDelta = new Vector2(800, 170);
            TextMeshProUGUI words = help.GetComponent<TextMeshProUGUI>();
            words.text = PanelHelp;
            words.fontSize = 21;
            EditorUtility.SetDirty(words);
        }
        TextMeshProUGUI label;
        Button toggle = MakeButton(panel, "Physical buttons", "Physical buttons: OFF", 850, 596, 180, 72, 22, out label);
        pointer.physicalLabel = label;
        OnClick(toggle, pointer, "PhysicalButtons");
        // and the pointer's smoothing and click tolerance, under it (on unless turned off)
        Button steady = MakeButton(panel, "Steady pointer", "Steady pointer: ON", 850, 676, 180, 72, 22, out label);
        pointer.steadyLabel = label;
        OnClick(steady, pointer, "SteadyPointer");
    }

    // Every panel with buttons or sliders a beam's laser presses, but the keyboards (a finger
    // presses those as keys): its collider, and each button's and slider's rectangle.
    static void TouchPanels(Transform world, EmuPointer pointer)
    {
        List<Collider> panels = new List<Collider>();
        List<Button> buttons = new List<Button>();
        List<RectTransform> buttonRects = new List<RectTransform>();
        List<int> buttonPanel = new List<int>();
        List<Slider> sliders = new List<Slider>();
        List<RectTransform> sliderRects = new List<RectTransform>();
        List<int> sliderPanel = new List<int>();
        foreach (Selectable control in world.GetComponentsInChildren<Selectable>(true))
        {
            if (!(control is Button) && !(control is Slider)) continue;
            if (control.GetComponentInParent<EmuKeyboard>(true) != null) continue;
            Canvas canvas = control.GetComponentInParent<Canvas>(true);
            while (canvas != null && canvas.GetComponent<BoxCollider>() == null && canvas.transform.parent != null)
                canvas = canvas.transform.parent.GetComponentInParent<Canvas>(true);
            BoxCollider box = canvas != null ? canvas.GetComponent<BoxCollider>() : null;
            if (box == null) continue;
            int at = panels.IndexOf(box);
            if (at < 0) { at = panels.Count; panels.Add(box); }
            if (control is Button) { buttons.Add((Button)control); buttonRects.Add((RectTransform)control.transform); buttonPanel.Add(at); }
            else { sliders.Add((Slider)control); sliderRects.Add((RectTransform)control.transform); sliderPanel.Add(at); }
        }
        pointer.touchPanels = panels.ToArray();
        pointer.touchButtons = buttons.ToArray();
        pointer.touchButtonRects = buttonRects.ToArray();
        pointer.touchButtonPanel = buttonPanel.ToArray();
        pointer.touchSliders = sliders.ToArray();
        pointer.touchSliderRects = sliderRects.ToArray();
        pointer.touchSliderPanel = sliderPanel.ToArray();
        Debug.Log("[ShaderEmu] physical buttons: " + buttons.Count + " buttons and " + sliders.Count + " sliders on " + panels.Count + " panels");
    }




}
