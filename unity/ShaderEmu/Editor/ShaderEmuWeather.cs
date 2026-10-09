using System.Collections.Generic;
using pi.LTCGI;
using UdonSharpEditor;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;
using UnityEngine.Rendering;
using VRCLightVolumes;

// What the room has from its three shader packages (world/install_packages.py) and the storm
// outside: rain on the window's pane (Mochie's Glass), sheets of rain and a drifting sky that
// lightning lights (EmuWeather), the display's own light on the room (LTCGI), and a light
// volume, so that what moves is lit as the room is.
public static partial class ShaderEmuBuilder
{
    const string MochieTextures = "Assets/Mochie/Unity/Textures";

    static void WeatherMaterials(Dictionary<string, Material> made)
    {
        // the pane: clear, with drops running down it. Keywords as Mochie's GlassEditor sets them.
        Material glass = Mat("Glass", "Mochie/Glass");
        glass.shaderKeywords = new string[0];
        glass.SetColor("_GrabpassTint", Color.white);
        glass.SetColor("_SpecularityTint", Color.white);
        glass.SetColor("_BaseColorTint", new Color(0, 0, 0, 1));
        glass.SetFloat("_Roughness", 0f);
        glass.SetFloat("_Metallic", 0f);
        glass.SetFloat("_Blur", 0f);
        glass.SetInt("_BlurQuality", 0);
        glass.SetFloat("_Refraction", 5f);
        glass.SetInt("_RainMode", 1);   // droplets
        glass.SetTexture("_RainSheet", AssetDatabase.LoadAssetAtPath<Texture2D>(MochieTextures + "/Glass_Rain_Texturesheet.png"));
        glass.SetTexture("_DropletMask", AssetDatabase.LoadAssetAtPath<Texture2D>(MochieTextures + "/Droplet Mask.tif"));
        glass.SetFloat("_Rows", 8f);
        glass.SetFloat("_Columns", 8f);
        glass.SetFloat("_Speed", 60f);
        glass.SetFloat("_XScale", 1.5f);
        glass.SetFloat("_YScale", 1.5f);
        glass.SetFloat("_Strength", 0.3f);
        glass.SetFloat("_DynamicDroplets", 0.5f);
        glass.SetInt("_ReflectionsToggle", 1);
        glass.SetFloat("_ReflectionStrength", 0.6f);
        glass.SetInt("_SpecularToggle", 1);
        glass.SetInt("_LitBaseColor", 1);
        glass.SetInt("_BlendMode", 0);   // grabs what is behind it, to bend it through the drops
        glass.SetInt("_Culling", (int)CullMode.Off);
        glass.SetInt("_LTCGI", 0);
        glass.SetInt("_MaterialResetCheck", 1);
        foreach (string keyword in new[] { "_RAIN_ON", "_BLURQUALITY_LOW", "_REFLECTIONS_ON", "_SPECULAR_HIGHLIGHTS_ON", "_GRABPASS_ON", "_LIT_BASECOLOR_ON" })
            glass.EnableKeyword(keyword);
        glass.SetOverrideTag("RenderType", "Transparent");
        glass.SetInt("_SrcBlend", (int)BlendMode.One);
        glass.SetInt("_DstBlend", (int)BlendMode.Zero);
        glass.SetInt("_ZWrite", 0);
        glass.renderQueue = (int)RenderQueue.Transparent;
        glass.SetShaderPassEnabled("GrabPass", true);
        glass.globalIlluminationFlags = MaterialGlobalIlluminationFlags.EmissiveIsBlack;
        made["Glass"] = glass;

        Material rain = Mat("Rain", "ShaderEmu/Rain");
        rain.mainTexture = WorldTexture("RainStreaks");
        made["Rain"] = rain;
    }

    static AudioSource Sound(Transform parent, string name, Vector3 at, AudioClip clip, float volume, float near, float far, bool loop)
    {
        AudioSource source = new GameObject(name).AddComponent<AudioSource>();
        source.transform.SetParent(parent, false);
        source.transform.localPosition = at;
        source.clip = clip;
        source.loop = loop;
        source.playOnAwake = loop;
        source.volume = volume;
        source.spatialBlend = 1f;
        source.rolloffMode = AudioRolloffMode.Linear;
        source.minDistance = near;
        source.maxDistance = far;
        source.dopplerLevel = 0f;
        VRC.SDK3.Components.VRCSpatialAudioSource spatial = source.gameObject.AddComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
        spatial.Gain = 0f;
        spatial.Near = near;
        spatial.Far = far;
        spatial.VolumetricRadius = near;
        spatial.EnableSpatialization = true;
        spatial.UseAudioSourceVolumeCurve = false;
        return source;
    }

    // A plate of two switches on the right wall, towards the front: the storm, and the light volumes.
    static void RoomSwitches(Transform world, Transform weather, EmuWeather storm)
    {
        Gone(world, "Room switches");
        Transform rain = world.Find("Models/City/Rain");
        Transform sound = weather.Find("Rain");
        storm.rainThings = new[] { rain != null ? rain.gameObject : null, sound != null ? sound.gameObject : null };
        foreach (VRCLightVolumes.LightVolumeManager manager in Object.FindObjectsOfType<VRCLightVolumes.LightVolumeManager>(true))
            storm.lightVolumes = manager.gameObject;
        RectTransform plate = Panel(world, "Room switches", new Vector3(4.44f, 1.35f, 2.4f), new Vector3(0, 90, 0), 300, 150, new Color(0.05f, 0.055f, 0.07f));
        Label(plate, "Title", "The room", 10, 6, 280, 30, 20, TextAnchor.MiddleLeft, Color.white);
        Button rainKey = MakeButton(plate, "Rain", "Rain: on", 10, 42, 280, 46, 20, out storm.rainLabel);
        Button volumesKey = MakeButton(plate, "Volumes", "Light volumes: on", 10, 94, 280, 46, 20, out storm.volumesLabel);
        Apply(storm);
        OnClick(rainKey, storm, "ToggleRain");
        OnClick(volumesKey, storm, "ToggleVolumes");
    }

    [MenuItem("ShaderEmu/Add the room's switches to the open scene")]
    public static void AddRoomSwitches()
    {
        GameObject world = GameObject.Find("ShaderEmu");
        EmuWeather storm = Object.FindObjectOfType<EmuWeather>();
        if (world == null || storm == null) throw new System.Exception("no weather in the open scene: run ShaderEmu/Put the modelled room into the open scene");
        UdonSharpEditorUtility.CopyUdonToProxy(storm);
        storm.thunder.volume = 0.3f;
        RoomSwitches(world.transform, storm.transform, storm);
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.scene);
    }

    static void Weather(Transform world, Transform computer)
    {
        // ---- the sky: a panorama under drifting cloud
        Material sky = Mat("NightSky", "ShaderEmu/Sky");
        sky.SetTexture("_MainTex", WorldTexture("NightSky"));
        sky.SetTexture("_Clouds", WorldTexture("CloudNoise", false, false));
        sky.SetFloat("_Exposure", 0.5f);
        sky.SetFloat("_Cover", 0.8f);
        RenderSettings.skybox = sky;

        // ---- rain's sound at the window, thunder from over the city
        Transform old = world.Find("Weather");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        Transform weather = new GameObject("Weather").transform;
        weather.SetParent(world, false);
        string sounds = Root + "/Sounds/";
        Sound(weather, "Rain", new Vector3(4.3f, WindowY, WindowZ), AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "Rain.wav"), 0.22f, 4f, 45f, true);
        AudioSource thunder = Sound(weather, "Thunder", new Vector3(12f, 6f, WindowZ), null, 0.3f, 40f, 200f, false);   // the whole world hears it, under the room's own sounds
        if (UdonSharpEditorUtility.GetUdonSharpProgramAsset(typeof(EmuWeather)) == null)
        {
            CreateProgramAssets();
            Debug.LogWarning("[ShaderEmu] EmuWeather's program was just made: run this command once more for the lightning");
        }
        else
        {
            EmuWeather storm = Udon<EmuWeather>(weather.gameObject);
            storm.thunder = thunder;
            storm.claps = new[] { AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "Thunder1.wav"), AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "Thunder2.wav"),
                                  AssetDatabase.LoadAssetAtPath<AudioClip>(sounds + "Thunder3.wav") };
            storm.clapDelay = new[] { 0.7f, 2.4f, 5.0f };
            storm.clapFlash = new[] { 1.0f, 0.6f, 0.3f };
            RoomSwitches(world, weather, storm);
            Apply(storm);
        }

        Screens(world, computer);
        RoomLightVolume(world);
    }

    // LTCGI: the display is an area light with the machine's own picture on it.
    static void Screens(Transform world, Transform computer)
    {
        foreach (LTCGI_Controller existing in Object.FindObjectsOfType<LTCGI_Controller>()) Object.DestroyImmediate(existing.gameObject);
        GameObject prefab = AssetDatabase.LoadAssetAtPath<GameObject>("Packages/at.pimaker.ltcgi/LTCGI Controller.prefab");
        GameObject instance = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
        instance.transform.SetParent(world, false);
        LTCGI_Controller controller = instance.GetComponent<LTCGI_Controller>();
        RenderTexture picture = AssetDatabase.LoadAssetAtPath<RenderTexture>(Generated + "/DisplayPicture.renderTexture");
        controller.VideoTexture = picture;
        // in the editor the picture holds whatever was last drawn into it: the machine is off, so black
        RenderTexture before = RenderTexture.active;
        RenderTexture.active = picture;
        GL.Clear(false, true, Color.black);
        RenderTexture.active = before;

        Transform display = computer.Find("Display screen");
        LTCGI_Screen screen = display.GetComponent<LTCGI_Screen>();
        if (screen == null) screen = display.gameObject.AddComponent<LTCGI_Screen>();
        screen.ColorMode = pi.LTCGI.ColorMode.Texture;
        screen.TextureIndex = 0;   // the video texture
        screen.Color = Color.white * 2f;   // a wall of light two metres wide
        screen.Diffuse = true;
        screen.Specular = true;
        screen.RendererMode = RendererMode.Distance;
        screen.RendererDistance = 9f;
        screen.AffectLightVolumes = false;
        controller.UpdateMaterials();
    }

    // One light volume the size of the room, baked with the lightmaps.
    static void RoomLightVolume(Transform world)
    {
        Transform old = world.Find("Light volume");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        GameObject holder = new GameObject("Light volume");
        holder.transform.SetParent(world, false);
        holder.transform.localPosition = new Vector3(0, 1.6f, 0);
        holder.transform.localScale = new Vector3(9f, 3.2f, 11f);
        VRCLightVolumes.LightVolume volume = holder.AddComponent<VRCLightVolumes.LightVolume>();
        volume.Bake = true;
        volume.AdaptiveResolution = false;
        volume.VoxelsPerUnit = 2f;
        volume.Recalculate();
    }
}
