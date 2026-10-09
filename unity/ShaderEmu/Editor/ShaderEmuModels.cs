using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

// The room as modelled in Blender (docs/world.md: world/build.py writes Models/*.fbx, textures.py
// and fetch.py the textures). The builder's own boxes stay as colliders only. Surfaces are on
// Mochie's Standard shader, which takes the screens' light (LTCGI) and light volumes; the
// camera is HDR, so lamps, LEDs and the city's lights are brighter than white and bloom shows it.
public static partial class ShaderEmuBuilder
{
    const string ModelsPath = Root + "/Models";
    const string WorldTextures = Root + "/Textures/World";
    const string AssetTextures = Root + "/Textures/Assets";
    const int PostLayer = 23;

    struct Surface
    {
        public string name, texture;
        public Color tint;
        public float rough, metallic;
        public bool normal, matte;
    }

    static Surface S(string name, string texture, float r, float g, float b, float rough, bool normal = true, bool matte = false, float metallic = 0f)
    {
        return new Surface { name = name, texture = texture, tint = new Color(r, g, b), rough = rough, normal = normal, matte = matte, metallic = metallic };
    }

    // Tints as in world/materials.py. A textured surface's roughness is its packed map's (TEXTURE_p.png);
    // `rough` is for one without a texture.
    static readonly Surface[] Surfaces =
    {
        S("Wall", "Plaster", 1f, 1f, 1f, 1f, true, true),
        S("Ceiling", "Ceiling", 1f, 1f, 1f, 1f, true, true),
        S("Parquet", "Parquet", 1f, 1f, 1f, 1f),
        S("WoodOak", "Wood", 1f, 1f, 1f, 1f),
        S("WoodDark", "Walnut", 0.62f, 0.55f, 0.50f, 1f),
        S("TrimWhite", null, 0.80f, 0.79f, 0.75f, 0.65f),
        S("PlasticBeige", "Plastic", 0.84f, 0.80f, 0.68f, 1f),
        S("PlasticGrey", "Plastic", 0.45f, 0.45f, 0.44f, 1f),
        S("PlasticDark", "Plastic", 0.17f, 0.17f, 0.18f, 1f),
        S("GamepadBody", "Plastic", 0.78f, 0.79f, 0.82f, 1f),
        S("GamepadBlue", "Plastic", 0.12f, 0.30f, 0.75f, 1f),
        S("GamepadRed", "Plastic", 0.80f, 0.10f, 0.08f, 1f),
        S("GamepadYellow", "Plastic", 0.95f, 0.75f, 0.10f, 1f),
        S("MetalSteel", "Metal", 0.55f, 0.56f, 0.58f, 1f, true, false, 0.85f),
        S("MetalBlack", "Metal", 0.15f, 0.15f, 0.16f, 1f, true, false, 0.4f),
        S("Brass", "Metal", 0.80f, 0.62f, 0.30f, 1f, true, false, 1f),
        S("RackSteel", "Metal", 0.21f, 0.22f, 0.24f, 1f),   // painted, not bare: its shading shows
        S("FabricCushion", "Velvet", 0.72f, 0.60f, 0.62f, 1f, true, true),
        S("FabricChair", "Linen", 0.45f, 0.45f, 0.50f, 1f, true, true),
        S("Cardboard", "Cardboard", 1f, 1f, 1f, 1f, true, true),
        S("Terracotta", "Plaster", 0.70f, 0.40f, 0.28f, 1f, true, true),
        S("Ceramic", null, 0.90f, 0.90f, 0.88f, 0.25f),
        S("Paper", null, 0.93f, 0.91f, 0.84f, 0.95f, false, true),
        S("Rubber", null, 0.10f, 0.10f, 0.10f, 0.8f),
        S("CableBlue", null, 0.08f, 0.20f, 0.50f, 0.55f),
        S("CableYellow", null, 0.78f, 0.60f, 0.08f, 0.55f),
        S("Books", "Books", 1f, 1f, 1f, 1f, false),
        S("Keyboard", "Keyboard", 1f, 1f, 1f, 1f, false),
        S("Rug", "Rug", 1f, 1f, 1f, 1f, true, true),
        S("RoundRug", "RoundRug", 1f, 1f, 1f, 1f, false, true),
        S("Carpet", "Carpet", 1f, 1f, 1f, 1f, true, true),
        S("RedPaint", null, 0.62f, 0.05f, 0.04f, 0.35f),
        S("PlasticCase", "Plastic", 0.74f, 0.71f, 0.62f, 1f),
    };

    static Texture2D ImportedTexture(string path, bool colour, bool normal, int aniso)
    {
        TextureImporter importer = AssetImporter.GetAtPath(path) as TextureImporter;
        if (importer == null) throw new System.Exception(path + " is missing: run world/textures.py and world/fetch.py, and copy unity/ShaderEmu over");
        TextureImporterType type = normal ? TextureImporterType.NormalMap : TextureImporterType.Default;
        if (importer.textureType != type || importer.anisoLevel != aniso || importer.alphaIsTransparency || importer.sRGBTexture != colour || importer.maxTextureSize < 4096)
        {
            importer.maxTextureSize = 4096;   // a baked picture may be that large (world/bake_pc.py)
            importer.textureType = type;
            importer.sRGBTexture = colour;   // a packed map's numbers are not colours
            importer.anisoLevel = aniso;
            importer.alphaIsTransparency = false;
            importer.mipmapEnabled = true;
            importer.wrapMode = TextureWrapMode.Repeat;
            importer.SaveAndReimport();
        }
        return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
    }

    static Texture2D WorldTexture(string name, bool normal = false, bool colour = true)
    {
        return ImportedTexture(WorldTextures + "/" + name + ".png", colour && !normal, normal, name.StartsWith("City") ? 4 : 8);
    }

    static void Transparent(Material m, Color colour, float smooth)
    {
        m.color = colour;
        m.SetFloat("_Mode", 3f);
        m.SetInt("_SrcBlend", (int)BlendMode.One);
        m.SetInt("_DstBlend", (int)BlendMode.OneMinusSrcAlpha);
        m.SetInt("_ZWrite", 0);
        m.EnableKeyword("_ALPHAPREMULTIPLY_ON");
        m.renderQueue = (int)RenderQueue.Transparent;
        m.SetFloat("_Glossiness", smooth);
    }

    // A light that is seen: far brighter than white, and (if it lights the room) part of the bake.
    static Material Emitter(string name, Color colour, float strength, bool lightsRoom, Texture2D map = null)
    {
        Material m = Mat(name, "Standard");
        m.color = Color.black;
        m.SetFloat("_Glossiness", 0.4f);
        m.EnableKeyword("_EMISSION");
        m.SetTexture("_EmissionMap", map);
        m.SetColor("_EmissionColor", colour * strength);
        m.globalIlluminationFlags = lightsRoom ? MaterialGlobalIlluminationFlags.BakedEmissive : MaterialGlobalIlluminationFlags.None;
        return m;
    }

    // A lit surface on Mochie's Standard. Its keywords are what its own inspector would set
    // from these properties (StandardEditor.SetKeywords, SetBlendMode).
    static Material Surfaced(string name, Color tint, Texture albedo, Texture normal, Texture packed, float metallic, float rough,
                        bool matte, bool cutout = false, bool bothSides = false, Texture emission = null, float glow = 0f)
    {
        Material m = Mat(name, "Mochie/Standard");
        m.shaderKeywords = new string[0];
        m.SetColor("_Color", tint);
        m.SetTexture("_MainTex", albedo);
        m.SetTextureScale("_MainTex", Vector2.one);
        m.SetTexture("_NormalMap", normal);
        m.SetTexture("_PackedMap", packed);
        m.SetInt("_PrimaryWorkflow", packed != null ? 1 : 0);
        m.SetFloat("_PackedMetallicStrength", metallic);
        m.SetFloat("_PackedRoughnessStrength", 1f);
        m.SetFloat("_PackedOcclusionStrength", 1f);
        m.SetFloat("_MetallicStrength", metallic);
        m.SetFloat("_RoughnessStrength", rough);
        foreach (string sample in new[] { "_SampleMetallic", "_SampleRoughness", "_SampleOcclusion", "_SampleSpecular" }) m.SetInt(sample, 0);
        m.SetInt("_ReflectionsToggle", matte ? 0 : 1);
        m.SetInt("_SpecularHighlightsToggle", matte ? 0 : 1);
        m.SetInt("_BlendMode", cutout ? 1 : 0);
        m.SetFloat("_Cutoff", 0.4f);
        m.SetInt("_Culling", bothSides ? (int)CullMode.Off : (int)CullMode.Back);
        m.SetInt("_LTCGI", 1);              // the screens light it (ShaderEmuWeather.cs: Screens)
        m.SetInt("_LightVolumesToggle", 1);
        m.SetInt("_BicubicSampling", 1);
        // a reflection is dimmed where the lightmap says little light falls: without that every
        // glossy thing in a dark corner shone with the lamps of the room's one reflection probe
        m.SetInt("_SpecularOcclusionToggle", 1);
        m.SetFloat("_SpecularOcclusionStrength", 1f);
        m.SetFloat("_SpecularOcclusionBrightness", 5f);
        m.SetFloat("_IndirectSpecularOcclusionStrength", 0.35f);
        m.SetInt("_MaterialResetCheck", 1);
        m.SetTexture("_EmissionMap", emission);
        m.SetColor("_EmissionColor", glow > 0f ? Color.white : Color.black);
        m.SetFloat("_EmissionStrength", glow);
        m.globalIlluminationFlags = glow > 0f ? MaterialGlobalIlluminationFlags.None : MaterialGlobalIlluminationFlags.EmissiveIsBlack;

        if (glow > 0f) m.EnableKeyword("_EMISSION_ON");
        if (!matte) { m.EnableKeyword("_REFLECTIONS_ON"); m.EnableKeyword("_SPECULAR_HIGHLIGHTS_ON"); }
        if (packed != null) m.EnableKeyword("_WORKFLOW_PACKED_ON");
        if (normal != null) m.EnableKeyword("_NORMALMAP_ON");
        m.EnableKeyword("_BICUBIC_SAMPLING_ON");
        m.EnableKeyword("LTCGI");
        m.SetOverrideTag("RenderType", cutout ? "TransparentCutout" : "Opaque");
        m.SetInt("_SrcBlend", (int)BlendMode.One);
        m.SetInt("_DstBlend", (int)BlendMode.Zero);
        m.SetInt("_ZWrite", 1);
        m.SetInt("_AlphaToMask", cutout ? 1 : 0);
        if (cutout) m.EnableKeyword("_ALPHATEST_ON");
        m.renderQueue = cutout ? (int)RenderQueue.AlphaTest : (int)RenderQueue.Geometry;
        return m;
    }

    // Poly Haven's materials, as world/fetch.py lists them: name, kind (0 solid, 1 with holes,
    // 2 no picture), then metallic and "has a normal map", or for kind 2 its colour.
    static void AssetMaterials(Dictionary<string, Material> made)
    {
        List<string> listed = new List<string>(File.ReadAllLines(ModelsPath + "/materials.txt"));
        if (File.Exists(ModelsPath + "/materials_extra.txt")) listed.AddRange(File.ReadAllLines(ModelsPath + "/materials_extra.txt"));   // models from elsewhere (world/import_retro_pc.py)
        foreach (string line in listed)
        {
            string[] f = line.Split('\t');
            if (f.Length < 6) continue;
            string name = f[0];
            System.Func<int, float> number = i => float.Parse(f[i], System.Globalization.CultureInfo.InvariantCulture);
            if (name.ToLowerInvariant().Contains("glass"))   // not "lass": that made a classic_laptop of glass
            {
                Material glass = Mat(name, "Standard");
                Transparent(glass, new Color(0.02f, 0.03f, 0.04f, 0.05f), 0.92f);
                made[name] = glass;
                continue;
            }
            if (f[1] == "2")   // no picture: a plain colour (a laptop's dark screen)
            {
                made[name] = Surfaced(name, new Color(number(2), number(3), number(4)), null, null, null, 0f, 0.15f, false);
                continue;
            }
            string stem = AssetTextures + "/" + name;
            Texture albedo = ImportedTexture(stem + "_d.png", true, false, 4);
            Texture normal = f[3] == "1" ? ImportedTexture(stem + "_n.jpg", false, true, 4) : null;
            Texture packed = File.Exists(stem + "_p.jpg") ? ImportedTexture(stem + "_p.jpg", false, false, 4) : null;
            bool leaves = f[1] == "1" || name.Contains("leaves");
            if (name.EndsWith("_light") || name.EndsWith("_globe"))   // a lamp's bulb
            {
                made[name] = Emitter(name, new Color(1f, 0.74f, 0.42f), 9f, true);
                continue;
            }
            // world/assets.py gives leaves a second face turned over, so culling stays on
            // a model with lamps of its own has a picture of their light (world/bake_pc.py)
            Texture light = File.Exists(stem + "_e.png") ? ImportedTexture(stem + "_e.png", true, false, 4) : null;
            made[name] = Surfaced(name, Color.white, albedo, normal, packed, packed != null ? 1f : number(2), 0.6f, false, f[1] == "1", false, light, light != null ? 5f : 0f);
            if (leaves) made[name].SetInt("_LTCGI", 0);
            if (leaves) made[name].DisableKeyword("LTCGI");
        }
    }

    static Dictionary<string, Material> ModelMaterials()
    {
        Dictionary<string, Material> made = new Dictionary<string, Material>();
        foreach (Surface s in Surfaces)
        {
            Texture albedo = s.texture != null ? WorldTexture(s.texture) : null;
            Texture normal = s.texture != null && s.normal ? WorldTexture(s.texture + "_n", true) : null;
            Texture packed = s.texture != null ? WorldTexture(s.texture + "_p", false, false) : null;
            made[s.name] = Surfaced(s.name, s.tint, albedo, normal, packed, s.metallic, s.rough, s.matte);
        }
        // the small printed things: their LEDs and read-outs are lamps
        made["Details"] = Surfaced("Details", Color.white, WorldTexture("Details"), null, WorldTexture("Details_p", false, false), 0f, 0.7f, false,
                              false, false, WorldTexture("Details_e"), 6f);

        // the holodeck's grid: its lines are lamps; and what marks where a program's world shows
        made["HoloGrid"] = Surfaced("HoloGrid", Color.white, WorldTexture("HoloGrid"), null, WorldTexture("HoloGrid_p", false, false), 0f, 0.5f, false,
                               false, false, WorldTexture("HoloGrid_e"), 2.5f);
        made["HoloMask"] = Mat("VolumeMask", "ShaderEmu/VolumeMask");
        Material tube = Mat("TubeGlass", "ShaderEmu/CRT");   // a station's glass until Stations() gives each its own
        tube.SetVector("_Size", Vector4.zero);
        made["TubeGlass"] = tube;

        made["PropScreen"] = Emitter("PropScreen", Color.white, 2.5f, false, WorldTexture("PropScreen"));
        made["Lamp"] = Emitter("Lamp", new Color(1f, 0.86f, 0.66f), 7f, true);
        made["LampLow"] = Emitter("LampLow", new Color(1f, 0.84f, 0.62f), 2.2f, true);   // the row over the screens, turned down
        made["Shade"] = Emitter("Shade", new Color(1f, 0.70f, 0.40f), 3.5f, true);
        made["Led"] = Emitter("Led", new Color(0.15f, 1f, 0.35f), 5f, false);
        made["Amber"] = Emitter("Amber", new Color(1f, 0.5f, 0.08f), 5f, false);
        foreach (string poster in new[] { "PosterDie", "PosterGrid", "PosterWords" })   // the builder's own textures (Decorate)
        {
            Material old = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/" + poster + ".mat");
            Texture picture = old != null ? old.mainTexture : null;
            made[poster] = Surfaced(poster, Color.white, picture, null, null, 0f, 0.6f, false);
        }

        foreach (string city in new[] { "CityOffice", "CityFlats", "CityGlass", "CityStreet", "CityGlow" })
        {
            Material m = Mat(city, "ShaderEmu/City");
            m.mainTexture = WorldTexture(city == "CityGlow" ? "White" : city);
            m.SetColor("_Haze", new Color(0.040f, 0.036f, 0.048f));
            m.SetFloat("_Dark", city == "CityStreet" ? 0.8f : 0.6f);
            m.SetFloat("_Bright", city == "CityStreet" ? 3f : 5f);
            made[city] = m;
        }
        AssetMaterials(made);
        WeatherMaterials(made);   // the window's pane, the rain (ShaderEmuWeather.cs)
        return made;
    }

    static GameObject ImportModel(string name, Dictionary<string, Material> materials, bool lightmapped)
    {
        string path = ModelsPath + "/" + name + ".fbx";
        ModelImporter importer = AssetImporter.GetAtPath(path) as ModelImporter;
        if (importer == null) throw new System.Exception(path + " is missing: run world/build.py and copy unity/ShaderEmu over");
        importer.importNormals = ModelImporterNormals.Import;
        importer.importTangents = ModelImporterTangents.CalculateMikk;
        importer.generateSecondaryUV = lightmapped;
        importer.secondaryUVPackMargin = 8f;
        importer.importCameras = false;
        importer.importLights = false;
        importer.importAnimation = false;
        importer.animationType = ModelImporterAnimationType.None;
        importer.meshCompression = ModelImporterMeshCompression.Off;
        importer.isReadable = false;
        importer.materialImportMode = ModelImporterMaterialImportMode.ImportStandard;
        foreach (KeyValuePair<string, Material> pair in materials)
            if (pair.Value != null) importer.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), pair.Key), pair.Value);
        importer.SaveAndReimport();
        return AssetDatabase.LoadAssetAtPath<GameObject>(path);
    }

    // What the builder made of boxes: gone if it only showed, kept unseen if it stops a player.
    static void StripBoxes(Transform root, System.Predicate<Transform> which)
    {
        List<GameObject> gone = new List<GameObject>();
        foreach (MeshRenderer renderer in root.GetComponentsInChildren<MeshRenderer>(true))
        {
            Transform t = renderer.transform;
            if (t.GetComponentInParent<Canvas>() != null || !which(t)) continue;
            if (t.GetComponent<Collider>() != null)
            {
                Object.DestroyImmediate(renderer);
                Object.DestroyImmediate(t.GetComponent<MeshFilter>());
                GameObjectUtility.SetStaticEditorFlags(t.gameObject, 0);
            }
            else gone.Add(t.gameObject);
        }
        foreach (GameObject go in gone)
            if (go != null) Object.DestroyImmediate(go);
    }

    static readonly HashSet<string> ComputerBoxes = new HashSet<string>
    {
        "Wall panel", "Terminal bezel", "Monitor bezel", "Monitor neck", "Monitor foot", "Memory bezel", "Desk top", "Desk side",
        "Desk back", "Tower", "Tower front", "Tower led", "Tower vents", "Volume bezel",
    };

    // More lightmap for what is full of holes and corners, where occlusion gives it depth.
    static float LightmapScale(string name)
    {
        if (name == "Books" || name == "Shelf" || name == "Rack" || name.StartsWith("book_")) return 2f;
        if (name == "Screen housings" || name == "Desk" || name == "Consoles" || name == "Tower" || name == "Radiator") return 1.5f;
        if (name == "Panelling") return 0.7f;
        if (name == "Desk things" || name == "Table things" || name == "Wall things" || name == "Cables") return 4f;   // small: or a mouse is three texels
        if (name.StartsWith("pachira") || name.StartsWith("potted_plant")) return 0.35f;   // leaves: many small pieces
        return 1f;
    }

    // models whose lightmap islands overlap (Unity's bake lists them): lit by the volume instead
    static readonly string[] ProbeLit = { "wicker_basket", "korean_public_payphone", "ornate_mirror", "boombox", "industrial_wall_sconce",
                                          "korean_fire_extinguisher", "desk_lamp_arm", "modern_ceiling_lamp", "alarm_clock", "fire_alarm" };

    static void Remodel(Transform world)
    {
        Dictionary<string, Material> materials = ModelMaterials();
        Transform room = world.Find("Room"), decor = world.Find("Decor"), computer = world.Find("Computer");
        Transform oldCity = decor.Find("City");
        if (oldCity != null) Object.DestroyImmediate(oldCity.gameObject);
        StripBoxes(room, t => true);
        StripBoxes(decor, t => true);
        StripBoxes(computer, t => ComputerBoxes.Contains(t.name));
        foreach (string empty in new[] { "Laptop", "Chair" })
        {
            Transform t = decor.Find(empty);
            if (t != null && t.childCount == 0) Object.DestroyImmediate(t.gameObject);
        }

        Transform old = world.Find("Models");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        Transform models = new GameObject("Models").transform;
        models.SetParent(world, false);
        foreach (string name in new[] { "Room", "Computer", "Furniture", "Annex", "City" })
        {
            bool city = name == "City";
            GameObject instance = (GameObject)PrefabUtility.InstantiatePrefab(ImportModel(name, materials, !city));
            instance.transform.SetParent(models, false);
            foreach (MeshRenderer renderer in instance.GetComponentsInChildren<MeshRenderer>())
            {
                GameObject go = renderer.gameObject;
                if (city || go.name == "Window glass")
                {
                    renderer.shadowCastingMode = ShadowCastingMode.Off;
                    renderer.receiveShadows = false;
                    if (city)
                    {
                        renderer.lightProbeUsage = LightProbeUsage.Off;
                        renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
                    }
                    continue;
                }
                if (go.name == "Holodeck mask" || go.name == "Holodeck control")   // Annex() takes them
                {
                    renderer.shadowCastingMode = ShadowCastingMode.Off;
                    continue;
                }
                // every model has a lightmap of its own surface: shadow and occlusion give it depth
                GameObjectUtility.SetStaticEditorFlags(go, StaticEditorFlags.ContributeGI | StaticEditorFlags.BatchingStatic |
                                                           StaticEditorFlags.OccludeeStatic | StaticEditorFlags.ReflectionProbeStatic);
                renderer.receiveGI = ReceiveGI.Lightmaps;
                renderer.scaleInLightmap = LightmapScale(go.name);
                // A downloaded thing whose lightmap islands overlap (Unity's bake lists them) is in
                // the bake, and throws its shadow, but is lit by the room's volume: its own
                // lightmap came out specked.
                bool plant = go.name.StartsWith("pachira") || go.name.StartsWith("potted_plant");
#if BAKERY_INCLUDED
                if (plant) renderer.receiveGI = ReceiveGI.LightProbes;   // Bakery lays out a lightmap of its own for the rest
#else
                if (plant || System.Array.Exists(ProbeLit, word => go.name.StartsWith(word))) renderer.receiveGI = ReceiveGI.LightProbes;
#endif
            }
        }

        if (decor.Find("Desk lamp light") == null)
            BakedLight(decor, "Desk lamp light", new Vector3(-3.3f, 1.25f, 5.1f), new Color(1f, 0.78f, 0.5f), 1f, 2.5f);
        Mood(decor);
        foreach (string screen in new[] { "DisplayShow", "Terminal" })
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/" + screen + ".mat");
            if (m != null && m.HasProperty("_Glow")) m.SetFloat("_Glow", 1.35f);
        }
        PostProcessing(world);
        foreach (string pad in new[] { "Shooter controller", "Strategy controller" })
            if (world.Find(pad) != null) GamepadModel(world.Find(pad), pad.StartsWith("Shooter") ? "Gamepad shooter" : "Gamepad strategy", materials);
        Weather(world, computer);   // the storm, the screens' light, the light volume (ShaderEmuWeather.cs)
        Annex(world, models.Find("Annex"));   // the holodeck and the corridor (ShaderEmuHolodeck.cs)
        Stations(world);                      // the classroom's computers (ShaderEmuStations.cs)
        PcSounds(world);                      // the computer's own sounds (ShaderEmuPcSound.cs)
    }

    // A controller's shape (world/gamepad.py), in place of the builder's boxes. It moves: no lightmap.
    static void GamepadModel(Transform pad, string which, Dictionary<string, Material> materials)
    {
        for (int i = pad.childCount - 1; i >= 0; i--)
            if (pad.GetChild(i).GetComponent<MeshRenderer>() != null) Object.DestroyImmediate(pad.GetChild(i).gameObject);
        GameObject all = Object.Instantiate(ImportModel("Gamepad", materials, false));
        Transform model = all.transform.Find(which);
        model.SetParent(pad, false);
        model.localPosition = Vector3.zero;
        model.localRotation = Quaternion.identity;
        model.name = "Model";
        Object.DestroyImmediate(all);
    }

    // An evening: the ceiling's back row of lamps is what lights the room, with the lamps that
    // stand, the screens and the window. The point lights only help the lamps that are seen.
    static void Mood(Transform decor)
    {
        foreach (Light light in decor.GetComponentsInChildren<Light>(true))
        {
            switch (light.name)
            {
                case "Lamp light": light.intensity = light.transform.localPosition.z < 0 ? 0.35f : 0.12f; light.color = new Color(1f, 0.86f, 0.68f); break;
                case "Floor lamp light": light.intensity = 1.6f; break;
                case "Desk lamp light": light.intensity = 1.5f; light.range = 3.5f; light.transform.localPosition = new Vector3(-3.3f, 1.25f, 5.1f); break;
                case "Window light": light.intensity = 0.35f; break;
                case "Screen glow": light.intensity = 0f; break;   // the display lights the room itself, when it is on (LTCGI)
            }
        }
        RenderSettings.ambientLight = new Color(0.012f, 0.014f, 0.02f);
        foreach (ReflectionProbe probe in decor.GetComponentsInChildren<ReflectionProbe>(true))
        {
            probe.resolution = 512;   // box-projected to the room's walls: sharp enough for the floor's varnish
            probe.hdr = true;
            probe.boxProjection = true;
        }
    }

    static T Effect<T>(PostProcessProfile profile) where T : PostProcessEffectSettings
    {
        T settings;
        if (profile.TryGetSettings(out settings)) return settings;
        settings = profile.AddSettings<T>();
        AssetDatabase.AddObjectToAsset(settings, profile);
        return settings;
    }

    // Bloom and exposure, for every camera that takes the scene's reference camera as its model
    // (VRChat's own does). No tone curve beyond a neutral one: the machine's screens keep their colours.
    static void PostProcessing(Transform world)
    {
        NameLayer(PostLayer, "PostProcessing");
        PostProcessProfile profile = LoadOrCreate(Generated + "/PostProcessing.asset", () => ScriptableObject.CreateInstance<PostProcessProfile>());
        Bloom bloom = Effect<Bloom>(profile);
        bloom.enabled.Override(true);
        bloom.intensity.Override(0.3f);
        bloom.threshold.Override(1.2f);
        bloom.softKnee.Override(0.5f);
        bloom.diffusion.Override(6.5f);
        bloom.fastMode.Override(true);
        ColorGrading grade = Effect<ColorGrading>(profile);
        grade.enabled.Override(true);
        grade.gradingMode.Override(GradingMode.HighDefinitionRange);
        grade.tonemapper.Override(Tonemapper.Neutral);
        grade.postExposure.Override(0f);
        EditorUtility.SetDirty(profile);

        Transform old = world.Find("Post processing");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        GameObject holder = new GameObject("Post processing");
        holder.transform.SetParent(world, false);
        holder.layer = PostLayer;
        PostProcessVolume volume = holder.AddComponent<PostProcessVolume>();
        volume.isGlobal = true;
        volume.sharedProfile = profile;

        // the scene's reference camera (the default scene's Main Camera) gains HDR and the effects
        VRC.SDK3.Components.VRCSceneDescriptor descriptor = Object.FindObjectOfType<VRC.SDK3.Components.VRCSceneDescriptor>();
        Camera camera = descriptor != null && descriptor.ReferenceCamera != null ? descriptor.ReferenceCamera.GetComponent<Camera>() : null;
        if (camera == null)
        {
            camera = new GameObject("Reference camera").AddComponent<Camera>();
            camera.transform.SetParent(holder.transform, false);
            camera.enabled = false;
            if (descriptor != null) descriptor.ReferenceCamera = camera.gameObject;
        }
        camera.allowHDR = true;
        camera.nearClipPlane = 0.05f;
        camera.farClipPlane = 3000f;
        PostProcessLayer layer = camera.GetComponent<PostProcessLayer>();
        if (layer == null) layer = camera.gameObject.AddComponent<PostProcessLayer>();
        layer.volumeLayer = 1 << PostLayer;
        layer.volumeTrigger = camera.transform;
        string[] resources = AssetDatabase.FindAssets("t:PostProcessResources");
        if (resources.Length > 0) layer.Init(AssetDatabase.LoadAssetAtPath<PostProcessResources>(AssetDatabase.GUIDToAssetPath(resources[0])));
        if (descriptor != null) EditorUtility.SetDirty(descriptor);
    }

    [MenuItem("ShaderEmu/Put the modelled room into the open scene")]
    public static void RemodelOpenScene()
    {
        GameObject world = GameObject.Find("ShaderEmu");
        if (world == null) { Debug.LogError("[ShaderEmu] no ShaderEmu object: run ShaderEmu/Build world first"); return; }
        Remodel(world.transform);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(world.scene);
        Debug.Log("[ShaderEmu] the modelled room is in the scene. Now run ShaderEmu/Bake lighting.");
    }
}
