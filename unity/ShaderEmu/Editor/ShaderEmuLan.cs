// The network's panel (docs/system/lan.md): on the right wall between the danger zone and the rack, every visitor's
// machine with its address and this visitor's own machine's counts. A canvas: no bake.
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static partial class ShaderEmuBuilder
{
    static void Lan(Transform world, EmuMachine machine)
    {
        Gone(world, "Network panel");
        Gone(world, "Lan");
        Color back = new Color(0.04f, 0.055f, 0.07f), dim = new Color(0.62f, 0.68f, 0.76f), amber = new Color(1f, 0.8f, 0.3f);
        const float width = 700, height = 600;
        // (the danger zone is 760 wide at z 3.5: this hangs between it and the rack, the same height)
        RectTransform panel = Panel(world, "Network panel", new Vector3(4.44f, 1.47f, 4.27f), new Vector3(0, 90, 0), width, height, back);
        EmuLan lan = Udon<EmuLan>(new GameObject("Lan"));
        lan.transform.SetParent(world, false);
        lan.machine = machine;
        lan.hub = Object.FindObjectOfType<EmuShareHub>();
        Label(panel, "Title", "NETWORK", 14, 12, 300, 44, 34, TextAnchor.MiddleLeft, Color.white);
        Label(panel, "Help", "Every machine that is on is on one network, 10.0.0.0/16: ping, telnet, Doom and Quake between them.", 14, 56, width - 28, 50, 17,
              TextAnchor.UpperLeft, dim);
        Label(panel, "Own title", "Your machine", 14, 110, width - 28, 24, 18, TextAnchor.MiddleLeft, dim);
        lan.ownLabel = Label(panel, "Own", "", 14, 136, width - 28, 96, 16, TextAnchor.UpperLeft, amber);
        Label(panel, "List title", "Everybody's (a sending is a batch of packets)", 14, 238, width - 28, 24, 18, TextAnchor.MiddleLeft, dim);
        lan.listLabel = Label(panel, "List", "", 14, 264, width - 28, 326, 18, TextAnchor.UpperLeft, amber);
        Apply(lan);
    }



}
