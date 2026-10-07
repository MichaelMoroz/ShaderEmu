using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Events;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The sound card's host side (docs/sound.md): the texture its mix is drawn into, the audio
// source the mix leaves by, and the panel's switch and volume.
public static partial class ShaderEmuBuilder
{
    static void SyncShaders()
    {
        ShaderEmuImages.SyncShaders();   // sound.h, and gpu.h which includes it
    }

    static void Sound(Transform world, RectTransform panel, EmuMachine machine)
    {
        if (world.Find("Sound") != null) Object.DestroyImmediate(world.Find("Sound").gameObject);
        for (int i = panel.childCount - 1; i >= 0; i--)
            if (panel.GetChild(i).name.StartsWith("Sound ")) Object.DestroyImmediate(panel.GetChild(i).gameObject);

        RenderTexture mix = LoadOrCreate(Generated + "/SoundMix.renderTexture",
            () => new RenderTexture(128, 128, 0, RenderTextureFormat.RGFloat, RenderTextureReadWrite.Linear));
        mix.antiAliasing = 1;
        mix.filterMode = FilterMode.Point;
        mix.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(mix);

        // The audio source and the behaviour the audio thread calls share an object: VRChat
        // puts its filter there. Heard the same everywhere in the room.
        GameObject go = new GameObject("Sound");
        go.transform.SetParent(world, false);
        AudioSource source = go.AddComponent<AudioSource>();
        source.playOnAwake = false;
        source.loop = true;
        source.spatialBlend = 0f;
        source.volume = 1f;
        // the SDK asks for this on every source; it is told to leave the sound as it is
        VRC.SDK3.Components.VRCSpatialAudioSource spatial = go.AddComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
        spatial.EnableSpatialization = false;
        spatial.Gain = 0f;
        EmuSoundOut output = Udon<EmuSoundOut>(go);
        Apply(output);

        GameObject host = new GameObject("Sound mixer");
        host.transform.SetParent(go.transform, false);
        EmuSound sound = Udon<EmuSound>(host);
        sound.output = output;
        sound.source = source;
        sound.mixMaterial = Mat("Sound", "ShaderEmu/Sound");
        sound.mixTexture = mix;

        // on the panel, under the beam buttons
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        const float bx = 1060;
        Button toggle = MakeButton(panel, "Sound switch", "Sound: on", bx, 500, 190, 70, 26, out sound.switchLabel);
        sound.volumeLabel = Label(panel, "Sound volume", "Volume 10%", bx + 205, 494, 395, 34, 24, TextAnchor.MiddleLeft, dim);
        GameObject sliderObject = DefaultControls.CreateSlider(new DefaultControls.Resources());
        sliderObject.name = "Sound slider";
        RectTransform rect = sliderObject.GetComponent<RectTransform>();
        rect.SetParent(panel, false);
        rect.anchorMin = rect.anchorMax = new Vector2(0, 1);
        rect.pivot = new Vector2(0, 1);
        rect.anchoredPosition = new Vector2(bx + 205, -530);
        rect.sizeDelta = new Vector2(395, 44);
        Slider slider = sliderObject.GetComponent<Slider>();
        slider.minValue = 0;
        slider.maxValue = 100;
        slider.wholeNumbers = true;
        slider.value = 10;
        foreach (Image part in sliderObject.GetComponentsInChildren<Image>())
        {
            part.material = UiMaterial();
            part.color = part.name == "Handle" ? Color.white : part.name == "Fill" ? new Color(0.3f, 0.65f, 1f) : new Color(0.2f, 0.21f, 0.25f);
        }
        slider.handleRect.sizeDelta = new Vector2(36, 0);
        DeafToWalking(slider);
        sound.volumeSlider = slider;
        Apply(sound);
        OnClick(toggle, sound, "Toggle");
        UnityEventTools.AddStringPersistentListener(slider.onValueChanged,
            UdonSharpEditorUtility.GetBackingUdonBehaviour(sound).SendCustomEvent, "VolumeChanged");

        machine.sound = sound;
        Apply(machine);
    }

    // Into the scene as it is, without building the world again (which would need a bake).
    [MenuItem("ShaderEmu/Add sound to the open scene")]
    public static void AddSound()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panel = GameObject.Find("Control panel");
        if (machine == null || panel == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        SyncShaders();
        CreateProgramAssets();
        Sound(machine.transform.parent, panel.GetComponent<RectTransform>(), machine);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] sound added");
    }
}
