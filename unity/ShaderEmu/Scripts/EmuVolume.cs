using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using TMPro;
using UnityEngine.UI;

// The holodeck's controls at its door (docs/holodeck.md): its renderers on or off, the world's
// scale (the plane of the guest camera's view that is the old screen's size, near to far), what
// stands at the control a visitor carries, which way the program's up is, and the control's recall.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuVolume : UdonSharpBehaviour
{
    public GameObject content;         // the mask and the seal
    public GameObject scene;           // the program's world: it goes with the control
    public GameObject dark;            // what shows switched off, if anything does
    public TextMeshProUGUI powerLabel, anchorLabel, upLabel;
    public Material sceneMaterial;     // GpuHolodeck.shader
    public Slider planeSlider;
    public Transform control;          // the thing a visitor carries
    public RenderTexture originTexture;   // one texel: where the program's camera stood when last asked
    public Material originMaterial;       // HolodeckOrigin.shader
    public bool on = true;
    public int anchor = 1;             // 1: the world's origin is at the control; 0: the player is, and the world slides by; 2: the camera, turn and all
    public int up;                     // the program's up: 0 found from its camera, 1 y, 2 z
    private Vector3 homeAt;
    private Quaternion homeTurn;

    void Start()
    {
        if (control != null)
        {
            homeAt = control.position;
            homeTurn = control.rotation;
        }
        Show();
    }

    public void NextAnchor()
    {
        anchor = (anchor + 1) % 3;
        if (anchor == 1) Centre();
        Show();
    }

    // The world's middle is where the program's camera stands now: the level comes to the control.
    public void Centre()
    {
        if (originTexture == null || originMaterial == null) return;
        Texture state = sceneMaterial.GetTexture("_State");
        originMaterial.SetTexture("_State", state);
        VRCGraphics.Blit(state, originTexture, originMaterial);
    }

    public void ToggleUp()
    {
        up = (up + 1) % 3;
        Show();
    }

    public void Recall()
    {
        if (control == null) return;
        control.SetPositionAndRotation(homeAt, homeTurn);
    }

    public void Toggle()
    {
        on = !on;
        if (on) Centre();
        Show();
    }

    public void PlaneChanged()
    {
        Show();
    }

    private void Show()
    {
        content.SetActive(on);
        if (scene != null) scene.SetActive(on);
        if (dark != null) dark.SetActive(!on);
        sceneMaterial.SetFloat("_Plane", planeSlider != null ? planeSlider.value : 0f);
        sceneMaterial.SetFloat("_Anchor", anchor);
        sceneMaterial.SetFloat("_UpAxis", up);
        if (powerLabel != null) powerLabel.text = on ? "Program: on" : "Program: off";
        if (anchorLabel != null) anchorLabel.text = anchor == 0 ? "Player" : anchor == 1 ? "World" : "Camera";
        if (upLabel != null) upLabel.text = up == 0 ? "Up: auto" : up == 1 ? "Up: Y" : "Up: Z";
    }
}
