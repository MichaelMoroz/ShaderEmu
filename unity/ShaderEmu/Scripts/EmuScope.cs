using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// What the memory screen shows: all of RAM with a legend of whose it is, the ROM, the sound
// card's ring of samples, or the GPU's VRAM (the textures programs uploaded). The panel's four
// buttons call this. (The CPU's state texels have a
// small screen of their own beside it.)
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuScope : UdonSharpBehaviour
{
    public Renderer screen;
    public Material[] views;           // RAM, ROM, sound, VRAM
    public GameObject legend;          // the RAM view's labels, over the screen
    public TextMeshProUGUI sign;       // the name above the screen
    public Image[] plates;             // the four buttons
    public Color plain, picked;

    private int shown;

    void Start()
    {
        Show(0);
    }

    // Whether the screen shows RAM now (the danger zone's laser burns only what it can see).
    public bool ShowsRam()
    {
        return shown == 0;
    }

    public void ShowRam()
    {
        Show(0);
    }

    public void ShowRom()
    {
        Show(1);
    }

    public void ShowSound()
    {
        Show(2);
    }

    public void ShowVram()
    {
        Show(3);
    }

    private void Show(int which)
    {
        shown = which;
        screen.sharedMaterial = views[which];
        legend.SetActive(which == 0);
        if (sign != null)
            sign.text = which == 0 ? "MEMORY (writes glow)" : which == 1 ? "ROM (the disk)" : which == 2 ? "SOUND (left, right)" : "VRAM (the GPU's textures)";
        for (int i = 0; i < plates.Length; i++) plates[i].color = i == which ? picked : plain;
    }
}
