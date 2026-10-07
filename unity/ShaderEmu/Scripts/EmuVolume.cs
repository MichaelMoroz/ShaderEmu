using UdonSharp;
using UnityEngine;
using TMPro;
using UnityEngine.UI;

// The volume display's controls (docs/volume.md): its three renderers on or off, and where
// the screen lies in the guest camera's view, from the near plane (0) to the far plane (1).
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuVolume : UdonSharpBehaviour
{
    public GameObject content;         // the mask, the scene's mesh and the seal
    public GameObject dark;            // the screen as it looks switched off
    public TextMeshProUGUI powerLabel;
    public Material sceneMaterial;     // GpuVolume.shader
    public Slider planeSlider;
    public bool on = true;

    void Start()
    {
        Show();
    }

    public void Toggle()
    {
        on = !on;
        Show();
    }

    public void PlaneChanged()
    {
        Show();
    }

    private void Show()
    {
        content.SetActive(on);
        dark.SetActive(!on);
        sceneMaterial.SetFloat("_Plane", planeSlider != null ? planeSlider.value : 0f);
        if (powerLabel != null) powerLabel.text = on ? "3D screen: on" : "3D screen: off";
    }
}
