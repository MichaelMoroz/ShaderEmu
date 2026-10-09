#if BAKERY_INCLUDED
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

// The room's light baked by Bakery, when the project has it (it defines BAKERY_INCLUDED): Unity's
// own lightmapper left blots under cornices and behind plants, and hung on a large page.
// "ShaderEmu/Bake lighting" comes here; without Bakery it is Unity's bake as before.
public static partial class ShaderEmuBuilder
{
    const float BakeryTexels = 60f;   // a metre, as Unity's bake had

    // A lamp as Bakery reads it: the builder's light with a component of Bakery's beside it.
    static void BakeryLamp(Light light)
    {
        BakeryPointLight lamp = light.GetComponent<BakeryPointLight>();
        if (lamp == null) lamp = light.gameObject.AddComponent<BakeryPointLight>();
        Color colour = light.color;
        float intensity = light.intensity;
        if (PlayerSettings.colorSpace == ColorSpace.Linear && !GraphicsSettings.lightsUseLinearIntensity)
        {
            // as Bakery's own "match lightmapped to real-time" does
            float r = Mathf.Pow(colour.r * intensity, 2.2f), g = Mathf.Pow(colour.g * intensity, 2.2f), b = Mathf.Pow(colour.b * intensity, 2.2f);
            intensity = Mathf.Max(r, Mathf.Max(g, b));
            colour = intensity > 0 ? new Color(r / intensity, g / intensity, b / intensity) : Color.black;
        }
        lamp.color = colour;
        lamp.intensity = intensity;
        lamp.cutoff = light.range;
        lamp.shadowSpread = Mathf.Max(0.05f, light.shadowRadius);   // a lamp has a size: soft shadows
        lamp.samples = 32;
        lamp.projMode = BakeryPointLight.ftLightProjectionMode.Omni;
        lamp.realisticFalloff = false;
        lamp.bakeToIndirect = false;
        lamp.enabled = light.intensity > 0;
        EditorUtility.SetDirty(lamp);
    }

    static void BakeWithBakery()
    {
        GameObject decor = GameObject.Find("ShaderEmu/Decor");
        if (decor != null) Mood(decor.transform);   // the lamps as the bake wants them (the look without a bake changes them)
        Batched(true);
        QualitySettings.pixelLightCount = 8;
        foreach (Light light in RoomLights())
        {
            light.lightmapBakeType = LightmapBakeType.Baked;
            light.enabled = true;
            if (light.type == LightType.Point) BakeryLamp(light);
        }
        // the night outside: what little the sky gives, as the scene's own ambient light has it
        GameObject skyHolder = GameObject.Find("ShaderEmu/Bakery sky");
        if (skyHolder == null)
        {
            skyHolder = new GameObject("Bakery sky");
            skyHolder.transform.SetParent(GameObject.Find("ShaderEmu").transform, false);
            skyHolder.tag = "EditorOnly";
        }
        BakerySkyLight sky = skyHolder.GetComponent<BakerySkyLight>();
        if (sky == null) sky = skyHolder.AddComponent<BakerySkyLight>();
        SphericalHarmonicsL2 ambient = RenderSettings.ambientProbe;
        Color mean = new Color(ambient[0, 0], ambient[1, 0], ambient[2, 0]) * 0.2821f;   // the first band is the mean
        float most = Mathf.Max(mean.r, Mathf.Max(mean.g, mean.b));
        sky.color = most > 0 ? new Color(mean.r / most, mean.g / most, mean.b / most) : Color.black;
        sky.intensity = most * RenderSettings.ambientIntensity;
        sky.samples = 32;
        EditorUtility.SetDirty(sky);

        // the room's light volumes are Bakery's volumes now
        foreach (VRCLightVolumes.LightVolumeSetup setup in Object.FindObjectsOfType<VRCLightVolumes.LightVolumeSetup>(true))
        {
            setup.BakingMode = VRCLightVolumes.LightVolumeSetup.Baking.Bakery;
            EditorUtility.SetDirty(setup);
        }
        foreach (VRCLightVolumes.LightVolume volume in Object.FindObjectsOfType<VRCLightVolumes.LightVolume>(true))
        {
            volume.SetupBakeryDependencies();
            EditorUtility.SetDirty(volume);
        }

        // Bakery's own window: its settings reach the bake through it
        ftRenderLightmap bakery = (ftRenderLightmap)EditorWindow.GetWindow(typeof(ftRenderLightmap));
        bakery.LoadRenderSettings();
        ftGlobalStorage found = ftLightmaps.GetGlobalStorage();   // what Bakery's own test of this card found to run
        if (!found.runsNonRTX && (found.runsRTX6 || found.runsRTX9)) ftRenderLightmap.rtxMode = true;
        bakery.userRenderMode = ftRenderLightmap.RenderMode.FullLighting;
        ftRenderLightmap.renderDirMode = ftRenderLightmap.RenderDirMode.None;
        ftRenderLightmap.lightProbeMode = ftRenderLightmap.LightProbeMode.L1;   // the light probes with the lightmaps, in the one render
        bakery.texelsPerUnit = BakeryTexels;
        ftBuildGraphics.texelsPerUnit = BakeryTexels;
        ftRenderLightmap.bounces = 5;
        bakery.giSamples = 32;
        bakery.denoise = true;
        bakery.fixSeams = true;
        // what stands on the floor is lit from straight above: occlusion grounds it
        ftRenderLightmap.hackAOIntensity = 1f;
        ftRenderLightmap.hackAORadius = 0.3f;
        ftRenderLightmap.hackAOSamples = 32;
        // Bakery asks in a window when its working folder is not there yet
        if (!string.IsNullOrEmpty(ftRenderLightmap.scenePath)) System.IO.Directory.CreateDirectory(ftRenderLightmap.scenePath);
        bakery.SaveRenderSettings();
        ftRenderLightmap.OnFinishedFullRender -= BakeryDone;
        ftRenderLightmap.OnFinishedFullRender += BakeryDone;
        bakery.RenderButton(false);
        Debug.Log("[ShaderEmu] lighting bake started (Bakery)");
    }

    // Lightmaps, volumes and light probes are done in one render: Bakery's lightmaps. The
    // reflection probes are Unity's own bake, which blurs them by roughness as it should; but a
    // baked probe's picture is kept in Unity's lighting data, which Bakery replaces. So each is
    // baked into a file of its own and the probe told to use that file.
    static void BakeryDone(object sender, System.EventArgs e)
    {
        ftRenderLightmap.OnFinishedFullRender -= BakeryDone;
        EditorApplication.delayCall += () =>
        {
            foreach (Light light in RoomLights()) light.enabled = false;   // the lightmap has their light already
            foreach (ReflectionProbe probe in Object.FindObjectsOfType<ReflectionProbe>(true))
            {
                string path = Generated + "/" + probe.name + " baked.exr";
                probe.mode = ReflectionProbeMode.Custom;
                probe.customBakedTexture = null;
                if (!Lightmapping.BakeReflectionProbe(probe, path)) Debug.LogWarning("[ShaderEmu] " + probe.name + " was not baked");
                AssetDatabase.ImportAsset(path);
                probe.customBakedTexture = AssetDatabase.LoadAssetAtPath<Cubemap>(path);
                EditorUtility.SetDirty(probe);
            }
            Baked();
        };
    }
}
#endif
