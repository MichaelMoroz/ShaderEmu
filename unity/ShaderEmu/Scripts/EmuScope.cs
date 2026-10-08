using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// What the memory screen shows: all of RAM with a legend of whose it is, the ROM, or the sound
// card's ring of samples. The panel's three buttons call this. (The CPU's state texels have a
// small screen of their own beside it.)
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuScope : UdonSharpBehaviour
{
    public Renderer screen;
    public Material[] views;           // RAM, ROM, sound
    public GameObject legend;          // the RAM view's labels, over the screen
    public TextMeshProUGUI sign;       // the name above the screen
    public Image[] plates;             // the three buttons
    public Color plain, picked;

    private int shown;

    void Start()
    {
        Show(0);
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

    private void Show(int which)
    {
        shown = which;
        screen.sharedMaterial = views[which];
        legend.SetActive(which == 0);
        if (sign != null)
            sign.text = which == 0 ? "MEMORY (writes glow)" : which == 1 ? "ROM (the disk)" : "SOUND (left, right)";
        for (int i = 0; i < plates.Length; i++) plates[i].color = i == which ? picked : plain;
    }
}
