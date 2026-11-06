using System.Collections.Generic;
using pi.LTCGI;
using UdonSharpEditor;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;
using UnityEngine.Rendering;
using VRCLightVolumes;

// What the room has from its three shader packages (world/install_packages.py) and the storm
// outside: rain on the window's pane (Silent's Fake Glass), sheets of rain and a drifting sky that
// lightning lights (EmuWeather), the display's own light on the room (LTCGI), and a light
// volume, so that what moves is lit as the room is.
public static partial class ShaderEmuBuilder
{

    // The outside's own light at night, against what it was (4 November 2026): a third. Not its lit
    // windows and lamps, not lightning: City.shader, Sky.shader and Rain.shader keep those apart.
    const float Night = 1f / 3f;

    // The fog over the city (HeightFog.cginc), one for the towers and the sky: thickest at the street
    // (world/city.py: GROUND, 42 m under the window), thinning by e every 20 m up; its colour a
    // pale mist's, near white, the night's third of it (by eye: a warm one read as smog), and lightning lights it.
    static void HeightFog(Material m)
    {
        m.SetColor("_FogColour", new Color(0.50f, 0.50f, 0.53f) * Night);
        m.SetColor("_FogFlash", new Color(0.05f, 0.05f, 0.06f));
        m.SetFloat("_FogDensity", 0.125f);
        m.SetFloat("_FogBase", WindowY - 42f);
        m.SetFloat("_FogFalloff", 20f);
    }

    // the window probe's picture as a flash of lightning of 1 lights the city (WindowProbe)
    const string LightningPath = Generated + "/Reflection probe window lightning.cubemap";
    // the room as the pane sees it, the panes put away: what a drop reflects (WindowProbe)
    const string RoomPath = Generated + "/Reflection probe window room.cubemap";

    static void WeatherMaterials(Dictionary<string, Material> made)
    {
        made["Glass"] = GlassMaterial();

        Material rain = Mat("Rain", "ShaderEmu/Rain");
        rain.mainTexture = WorldTexture("RainStreaks");
        rain.SetFloat("_Bright", 0.05f * Night);   // (the city's light on it; lightning's as it was)
        made["Rain"] = rain;
    }

    // The pane: clear, with water on it that bends the city as water does (WindowRain.shader:
    // BigWings's "Heartfelt" drops as the water's height, traced into the window probe's picture). Silent's Fake Glass had it before: its drops barely showed.
    static Material GlassMaterial()
    {
        Material glass = Mat("Glass", "ShaderEmu/WindowRain");
        glass.shaderKeywords = new string[0];
        glass.SetTexture("_Sky", AssetDatabase.LoadAssetAtPath<Cubemap>(Generated + "/Reflection probe window picture.cubemap"));   // (WindowProbe makes it again)
        glass.SetTexture("_SkyFlash", AssetDatabase.LoadAssetAtPath<Cubemap>(LightningPath));
        glass.SetTexture("_Room", AssetDatabase.LoadAssetAtPath<Cubemap>(RoomPath));
        // every number set here: a material keeps another shader's numbers of the same names (Fake Glass's _IOR of 1 bent nothing)
        // Heartfelt's drops (WindowRain.shader: BigWings's, CC BY-NC-SA 3.0), on the pane in metres
        glass.SetFloat("_Scale", 0.84f);   // (as tuned in the editor, 4 November 2026: these, _Rim and the mist)
        glass.SetFloat("_Rain", 1f);
        // (2.7 mm, a puddle's height at glass's contact angle, was tried on 4 November 2026: from afar the
        // drops bent the city more, and what shows through them blurred into the city behind; no more seen)
        glass.SetFloat("_Height", 0.83f);
        glass.SetFloat("_IOR", 1.333f);
        glass.SetColor("_Clear", new Color(0.93f, 0.96f, 1f, 1f));
        glass.SetFloat("_Rim", 1f);
        glass.SetFloat("_Mist", 0.5f);       // Heartfelt's fog on the dry glass, which drops and trails clear
        glass.SetFloat("_MistBlur", 4f);
        glass.SetFloat("_Debug", 0f);
        glass.renderQueue = -1;
        glass.globalIlluminationFlags = MaterialGlobalIlluminationFlags.EmissiveIsBlack;
        return glass;
    }

    // The glass's own reflection probe, just outside the pane: a drop shows the city bent, and
    // the room's probe has only the lit room and a blank window. Its picture is a camera's (six
    // faces, the sheets of rain put away meanwhile), and every pane takes it by its anchor.
    // The city's pictures have the city and the sky alone; the room is a picture of its own (RoomPath),
    // what a drop's surface reflects back into the room (before, the lit room was an eighth of the
    // city's picture, and the reflection was turned back out to the city: the drops blended in).

    // The camera's six faces as they are in the scene: linear and HDR. (RenderToCubemap straight into a
    // Cubemap asset went through an 8-bit sRGB target: every value clamped at 1 and kept sRGB-encoded,
    // which the glass then read as linear, the city's dark walls 0.006 coming out 0.071: washed out.)
    static Color[][] CaptureHdr(Camera camera, CubemapFace[] faces, int size)
    {
        RenderTexture cube = new RenderTexture(size, size, 24, RenderTextureFormat.ARGBHalf, RenderTextureReadWrite.Linear);
        cube.dimension = UnityEngine.Rendering.TextureDimension.Cube;
        cube.Create();
        camera.RenderToCubemap(cube);
        Texture2D face = new Texture2D(size, size, TextureFormat.RGBAHalf, false, true);
        Color[][] result = new Color[faces.Length][];
        RenderTexture before = RenderTexture.active;
        for (int f = 0; f < faces.Length; f++)
        {
            Graphics.SetRenderTarget(cube, 0, faces[f]);
            face.ReadPixels(new Rect(0, 0, size, size), 0, 0);
            face.Apply();
            result[f] = face.GetPixels();   // (in a Cubemap's own row order: checked against RenderToCubemap's)
        }
        RenderTexture.active = before;
        Object.DestroyImmediate(face);
        cube.Release();
        Object.DestroyImmediate(cube);
        return result;
    }

    // The city's two pictures are 1024 a face (a texel 0.09 degrees): its lamps are tiny and many times
    // brighter than white (City.shader, "lamps"), and at 256 a face a drop showed each as a smear of a third
    // of a degree. Kept as BC6H, 8 MB a picture with its mips (RGBAHalf would be 50); the room's stays 256.
    const int CitySize = 1024, RoomSize = 256;

    // the pictures into the cubemap asset at `path`, made anew (a compressed one takes no SetPixels) and
    // copied over the asset so what refers to it keeps it
    static Cubemap StoreCube(string path, Color[][] pictures, CubemapFace[] faces, int size, bool compress)
    {
        Cubemap made = new Cubemap(size, TextureFormat.RGBAHalf, true);
        made.name = System.IO.Path.GetFileNameWithoutExtension(path);   // (CopySerialized brings the name too)
        for (int f = 0; f < faces.Length; f++) made.SetPixels(pictures[f], faces[f]);
        made.Apply(true);
        if (compress) EditorUtility.CompressCubemapTexture(made, TextureFormat.BC6H, UnityEditor.TextureCompressionQuality.Best);
        Cubemap asset = AssetDatabase.LoadAssetAtPath<Cubemap>(path);
        if (asset == null) { AssetDatabase.CreateAsset(made, path); return made; }
        EditorUtility.CopySerialized(made, asset);
        Object.DestroyImmediate(made);
        EditorUtility.SetDirty(asset);
        return asset;
    }

    static void WindowProbe(Transform world, Material glass)
    {
        const string name = "Reflection probe window";
        Transform parent = world.Find("Decor") != null ? world.Find("Decor") : world;
        Transform old = parent.Find(name);
        if (old != null) Object.DestroyImmediate(old.gameObject);
        ReflectionProbe probe = new GameObject(name).AddComponent<ReflectionProbe>();
        probe.transform.SetParent(parent, false);
        probe.transform.position = new Vector3(4.95f, WindowY, WindowZ);
        probe.size = new Vector3(0.3f, 2.2f, 4.2f);
        probe.boxProjection = false;
        probe.importance = 2;
        probe.resolution = CitySize;
        probe.hdr = true;
        probe.nearClipPlane = 0.05f;
        probe.farClipPlane = 3000f;

        string path = Generated + "/" + name + " picture.cubemap";
        Transform rain = world.Find("Models/City/Rain");
        bool rainOn = rain != null && rain.gameObject.activeSelf;
        if (rain != null) rain.gameObject.SetActive(false);
        Camera camera = new GameObject("probe camera").AddComponent<Camera>();
        camera.transform.position = probe.transform.position;
        camera.allowHDR = true;
        camera.nearClipPlane = 0.05f;
        camera.farClipPlane = 3000f;
        CubemapFace[] faces = { CubemapFace.PositiveX, CubemapFace.NegativeX, CubemapFace.PositiveY, CubemapFace.NegativeY, CubemapFace.PositiveZ, CubemapFace.NegativeZ };
        // the room (and the city through the other panes' frames), the panes themselves put away
        List<Renderer> panesAway = new List<Renderer>();
        foreach (Renderer renderer in Object.FindObjectsOfType<Renderer>())
            if (renderer.enabled && System.Array.IndexOf(renderer.sharedMaterials, glass) >= 0) { renderer.enabled = false; panesAway.Add(renderer); }
        Color[][] whole = CaptureHdr(camera, faces, RoomSize);
        foreach (Renderer renderer in panesAway) renderer.enabled = true;
        // again with nothing but the city and the sky
        Transform city = world.Find("Models/City");
        List<Renderer> hidden = new List<Renderer>();
        foreach (Renderer renderer in Object.FindObjectsOfType<Renderer>())
            if (renderer.enabled && (city == null || !renderer.transform.IsChildOf(city))) { renderer.enabled = false; hidden.Add(renderer); }
        Color[][] cityOnly = CaptureHdr(camera, faces, CitySize);
        // and the city and the sky as a flash of 1 lights them (_UdonWeatherFlash, EmuWeather): the
        // drops show lightning as the window does. Everything lightning lights is linear in the flash,
        // so the glass mixes the two pictures by the flash and has what any flash shows
        int flashId = Shader.PropertyToID("_UdonWeatherFlash");
        float flashWas = Shader.GetGlobalFloat(flashId);
        Shader.SetGlobalFloat(flashId, 1f);
        Color[][] cityLit = CaptureHdr(camera, faces, CitySize);
        Shader.SetGlobalFloat(flashId, flashWas);
        foreach (Renderer renderer in hidden) renderer.enabled = true;
        Cubemap picture = StoreCube(path, cityOnly, faces, CitySize, true);
        Cubemap lightning = StoreCube(LightningPath, cityLit, faces, CitySize, true);
        Cubemap room = StoreCube(RoomPath, whole, faces, RoomSize, false);
        Object.DestroyImmediate(camera.gameObject);
        if (rain != null) rain.gameObject.SetActive(rainOn);
        probe.mode = ReflectionProbeMode.Custom;
        probe.customBakedTexture = picture;
        probe.boxProjection = false;   // (a direction's colour: the city is far)
        glass.SetTexture("_SkyFlash", lightning);
        glass.SetTexture("_Room", room);
        glass.SetTexture("_Sky", picture);   // what the drops bend (WindowRain.shader takes it itself: the probe Unity binds did not reach it)
        EditorUtility.SetDirty(glass);

        foreach (MeshRenderer renderer in world.GetComponentsInChildren<MeshRenderer>(true))
        {
            if (System.Array.IndexOf(renderer.sharedMaterials, glass) < 0) continue;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Simple;
            // (no light probes: with the room's, a drop was white paint in the room's light)
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.probeAnchor = probe.transform;
            EditorUtility.SetDirty(renderer);
        }
    }

    [MenuItem("ShaderEmu/Give the windows' glass its rain again in the open scene")]
    public static void GlassAgain()
    {
        GameObject world = GameObject.Find("ShaderEmu");
        if (world == null) { Debug.LogError("[ShaderEmu] open the world's scene first"); return; }
        WindowProbe(world.transform, GlassMaterial());
        AssetDatabase.SaveAssets();
        UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(world.scene);
        Debug.Log("[ShaderEmu] the windows' glass is Fake Glass with rain, with a probe of its own outside the pane");
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
        Spatial(source, near, far);
        return source;
    }

    // VRChat's own component on a source in the room: without it the SDK's panel asks for one, and adds its defaults.
    static void Spatial(AudioSource source, float near, float far)
    {
        VRC.SDK3.Components.VRCSpatialAudioSource spatial = source.GetComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
        if (spatial == null) spatial = source.gameObject.AddComponent<VRC.SDK3.Components.VRCSpatialAudioSource>();
        spatial.Gain = 0f;
        spatial.Near = near;
        spatial.Far = far;
        spatial.VolumetricRadius = near;
        spatial.EnableSpatialization = true;
        spatial.UseAudioSourceVolumeCurve = false;
        EditorUtility.SetDirty(spatial);
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
        WindowProbe(world, GlassMaterial());   // (the panes are new ones: their probe and its anchors again)
        // ---- the sky: a panorama under drifting cloud
        Material sky = Mat("NightSky", "ShaderEmu/Sky");
        sky.SetTexture("_MainTex", WorldTexture("NightSky"));
        sky.SetTexture("_Clouds", WorldTexture("CloudNoise", false, false));
        sky.SetFloat("_Exposure", 0.5f * Night);
        sky.SetFloat("_Cover", 0.8f);
        sky.SetFloat("_CityGlow", Night);   // (lightning in the clouds as it was)
        HeightFog(sky);
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
