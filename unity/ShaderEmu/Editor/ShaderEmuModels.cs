using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.PostProcessing;

// The room as modelled in Blender (docs/world/world.md: world/build.py writes Models/*.fbx, textures.py
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
        S("FabricChair", "ChairWeave", 0.66f, 0.70f, 0.82f, 1f, true, true),   // the chairs' wool herringbone (world/fetch.py)
        S("Cardboard", "Cardboard", 1f, 1f, 1f, 1f, true, true),
        S("Terracotta", "Plaster", 0.70f, 0.40f, 0.28f, 1f, true, true),
        S("Ceramic", null, 0.90f, 0.90f, 0.88f, 0.25f),
        S("Paper", null, 0.93f, 0.91f, 0.84f, 0.95f, false, true),
        S("LabelYellow", null, 0.92f, 0.78f, 0.25f, 0.95f, false, true),   // a floppy's coloured labels (world/toys.py): paper
        S("LabelRed", null, 0.78f, 0.16f, 0.12f, 0.95f, false, true),
        S("LabelBlue", null, 0.22f, 0.38f, 0.72f, 0.95f, false, true),
        S("Rubber", null, 0.10f, 0.10f, 0.10f, 0.8f),
        S("CableBlue", null, 0.08f, 0.20f, 0.50f, 0.55f),
        S("CableYellow", null, 0.78f, 0.60f, 0.08f, 0.55f),
        S("Books", "Books", 1f, 1f, 1f, 1f, false),
        // world/gameboxes.py: scans of boxes and magazines, fetched when the world is made (Textures/Fetched is not in git)
        S("GameBoxes", "Fetched/GameBoxes", 1f, 1f, 1f, 1f, false),
        S("GameBoxes2", "Fetched/GameBoxes2", 1f, 1f, 1f, 1f, false),
        S("Magazines", "Fetched/Magazines", 1f, 1f, 1f, 1f, false),
        S("Keyboard", "Keyboard", 1f, 1f, 1f, 1f, false),
        S("Rug", "Rug", 1f, 1f, 1f, 1f, true, true),
        S("RoundRug", "RoundRug", 1f, 1f, 1f, 1f, false, true),
        S("Carpet", "Carpet", 1f, 1f, 1f, 1f, true, true),
        S("RedPaint", null, 0.62f, 0.05f, 0.04f, 0.35f),
        S("ToyGreen", "Plastic", 0.08f, 0.55f, 0.20f, 1f),   // world/toys.py: the cube puzzle's tiles
        S("ToyOrange", "Plastic", 0.95f, 0.42f, 0.05f, 1f),
        S("Posters", "Fetched/Posters", 1f, 1f, 1f, 1f, false),   // world/posters.py: fetched when the world is made
        S("RoomSigns", "RoomSigns", 1f, 1f, 1f, 1f, false),     // the side rooms' number plates
        S("PlasticCase", "Plastic", 0.74f, 0.71f, 0.62f, 1f),
        S("PlasticWhite", "Plastic", 0.86f, 0.85f, 0.80f, 1f),   // wall switches and sockets (world/room.py)
        S("CanCoke", "CanCoke", 1f, 1f, 1f, 1f, false, false, 0.75f),   // printed aluminium (world/textures.py's soda_cans)
        S("CanDiet", "CanDiet", 1f, 1f, 1f, 1f, false, false, 0.85f),
        S("CanMetal", "Metal", 0.80f, 0.81f, 0.83f, 1f, true, false, 0.9f),
        // a thing's own material with its use laid over it (world/wear.py's recipes)
        S("FloppyShell", "Plastic", 0.17f, 0.17f, 0.18f, 1f),
        S("FloppyShutter", "Metal", 0.55f, 0.56f, 0.58f, 1f, true, false, 0.85f),
        S("DeskWalnut", "Walnut", 0.62f, 0.55f, 0.50f, 1f),
        S("DeskWalnutSides", "Walnut", 0.62f, 0.55f, 0.50f, 1f),   // its upright faces: no rings, no dust
        // the holodecks' corridor and seats
        S("HoloPanel", "HoloPanel", 1f, 1f, 1f, 1f, false),   // a holodeck's panels; the bars between them are HoloLine
        S("SciPanel", "Plastic", 0.70f, 0.68f, 0.63f, 1f),
        S("SciTrim", "Plastic", 0.30f, 0.31f, 0.34f, 1f),
        S("SciCarpet", "Linen", 0.47f, 0.51f, 0.64f, 1f, true, true),
        S("SciSeat", "Linen", 0.15f, 0.14f, 0.15f, 1f, true, true),
    };

    // Pictures seen from afar or small, smaller and crunched (DXT with a lossy pass on top: smaller to download,
    // as large in memory): the fetched atlases of posters, boxes and magazines (4096 each, five of them: 53 MB),
    // the cans. Everything else 4096 at most and plainly compressed.
    static readonly string[] Lesser = { "/Fetched/", "/CanCoke.", "/CanDiet." };
    const int LesserSize = 2048, CanSize = 1024, LesserQuality = 50;

    static int MostSize(string path)
    {
        if (path.Contains("/CanCoke.") || path.Contains("/CanDiet.")) return CanSize;
        foreach (string l in Lesser) if (path.Contains(l)) return LesserSize;
        return 4096;   // a baked picture may be that large (world/bake_pc.py)
    }

    static Texture2D ImportedTexture(string path, bool colour, bool normal, int aniso)
    {
        TextureImporter importer = AssetImporter.GetAtPath(path) as TextureImporter;
        if (importer == null) throw new System.Exception(path + " is missing: run world/textures.py and world/fetch.py, and copy unity/ShaderEmu over");
        TextureImporterType type = normal ? TextureImporterType.NormalMap : TextureImporterType.Default;
        int most = MostSize(path);
        bool crunch = most < 4096;
        if (importer.textureType != type || importer.anisoLevel != aniso || importer.alphaIsTransparency || importer.sRGBTexture != colour || importer.maxTextureSize != most
            || importer.crunchedCompression != crunch || (crunch && importer.compressionQuality != LesserQuality))
        {
            importer.maxTextureSize = most;
            importer.crunchedCompression = crunch;
            if (crunch) importer.compressionQuality = LesserQuality;
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
        string path = name.StartsWith("Fetched/") ? Root + "/Textures/" + name + ".png" : WorldTextures + "/" + name + ".png";   // (world/gameboxes.py's)
        return ImportedTexture(path, colour && !normal, normal, name.StartsWith("City") ? 4 : 8);
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

    // Moulded plastic seen close to, over a surface's own maps: a scan's grain and wear
    // (world/fetch.py: plastic_detail). Laid by where a place is in the room, along the three
    // axes: one size on every face, and no seam where a model's own picture has one.
    // `mask`: white where the surface is plastic (none: all of it).
    const float PlasticDetailTile = 0.2f;   // metres a repeat: the grain's bumps come out a millimetre apart
    const float PlasticGrainStrength = 1f;   // the scan as it is
    static void PlasticDetail(Material m, Texture mask = null) { Detail(m, "PlasticDetail", PlasticDetailTile, mask); }

    // A thing's use laid over its material (world/wear.py: its recipes, Wear.txt): the same detail
    // slot, a set of its own, so many repeats a metre as its recipe says.
    static Dictionary<string, KeyValuePair<string, float>> WearSets()
    {
        var sets = new Dictionary<string, KeyValuePair<string, float>>();
        string listing = WorldTextures + "/Wear.txt";
        if (!File.Exists(listing)) return sets;
        foreach (string line in File.ReadAllLines(listing))
        {
            string[] f = line.Split('\t');
            if (f.Length >= 3) sets[f[0]] = new KeyValuePair<string, float>(f[1], float.Parse(f[2], System.Globalization.CultureInfo.InvariantCulture));
        }
        return sets;
    }

    static void Detail(Material m, string set, float tile, Texture mask = null)
    {
        Texture colour = WorldTexture(set, false, false);
        m.SetTexture("_DetailMainTex", colour);
        m.SetTextureScale("_DetailMainTex", Vector2.one / tile);
        m.SetTexture("_DetailNormalMap", WorldTexture(set + "_n", true));
        m.SetTexture("_DetailRoughnessMap", WorldTexture(set + "_r", false, false));
        m.SetTexture("_DefaultDetailSampler", colour);   // (its wrap and filter are the detail's)
        m.SetTexture("_DetailMask", mask);
        m.SetInt("_DetailWorkflow", 0);
        m.SetInt("_DetailSampleMode", 3);     // triplanar
        m.SetInt("_TriplanarCoordSpace", 1);  // the room's axes (nothing of plastic moves)
        m.SetInt("_DetailMainTexBlend", 4);   // overlays: 0.5 changes nothing
        m.SetInt("_DetailRoughnessBlend", 4);
        m.SetFloat("_DetailMainTexStrength", 1f);
        m.SetFloat("_DetailRoughnessStrength", 1f);
        m.SetFloat("_DetailNormalStrength", PlasticGrainStrength);
        m.SetInt("_UVDetailSet", 0);
        m.DisableKeyword("_STOCHASTIC_DETAIL_ON");
        foreach (string keyword in new[] { "_TRIPLANAR_DETAIL_ON", "_DETAIL_MAINTEX_ON", "_DETAIL_NORMAL_ON", "_DETAIL_ROUGHNESS_ON" }) m.EnableKeyword(keyword);
        EditorUtility.SetDirty(m);
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
            // the classroom's computers: plastic but for their plates and sockets (world/bake_pc.py's mask)
            if (name == "PH_pc_station") PlasticDetail(made[name], ImportedTexture(stem + "_m.png", false, false, 4));
            if (leaves) made[name].SetInt("_LTCGI", 0);
            if (leaves) made[name].DisableKeyword("LTCGI");
        }
    }

    static Dictionary<string, Material> ModelMaterials()
    {
        Dictionary<string, Material> made = new Dictionary<string, Material>();
        var wear = WearSets();
        foreach (Surface s in Surfaces)
        {
            Texture albedo = s.texture != null ? WorldTexture(s.texture) : null;
            Texture normal = s.texture != null && s.normal ? WorldTexture(s.texture + "_n", true) : null;
            Texture packed = s.texture != null ? WorldTexture(s.texture + "_p", false, false) : null;
            made[s.name] = Surfaced(s.name, s.tint, albedo, normal, packed, s.metallic, s.rough, s.matte);
            if (wear.ContainsKey(s.name)) Detail(made[s.name], wear[s.name].Key, wear[s.name].Value);   // (its own grain is in its set)
            else if (s.texture == "Plastic") PlasticDetail(made[s.name]);
        }
        // the small printed things: their LEDs and read-outs are lamps
        made["Details"] = Surfaced("Details", Color.white, WorldTexture("Details"), null, WorldTexture("Details_p", false, false), 0f, 0.7f, false,
                              false, false, WorldTexture("Details_e"), 6f);

        // the holodeck's grid: its lines are lamps; and what marks where a program's world shows
        made["HoloGrid"] = Surfaced("HoloGrid", Color.white, WorldTexture("HoloGrid"), null, WorldTexture("HoloGrid_p", false, false), 0f, 0.5f, false,
                               false, false, WorldTexture("HoloGrid_e"), 2.5f);
        made["HoloMask"] = Mat("VolumeMask", "ShaderEmu/VolumeMask");
        made["HoloLine"] = Emitter("HoloLine", new Color(1f, 0.72f, 0.10f), 2.5f, true);   // the holodecks' grid: lit bars, which light the panels in the bake
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
            // the night's own light a third (Night): the haze and all that is not a bright light; a
            // bright light and what lightning lights (_HazeFlash, the haze as it was) as they were
            m.SetColor("_Haze", new Color(0.040f, 0.036f, 0.048f) * Night);
            m.SetColor("_HazeFlash", new Color(0.040f, 0.036f, 0.048f));
            m.SetFloat("_Dark", city == "CityStreet" ? 0.8f : 0.6f);
            m.SetFloat("_Night", Night);
            m.SetFloat("_Bright", city == "CityStreet" ? 3f : 5f);
            HeightFog(m);
            made[city] = m;
        }
        AssetMaterials(made);
        WeatherMaterials(made);   // the window's pane, the rain (ShaderEmuWeather.cs)
        return made;
    }

    // unwrap: Unity lays out the lightmap's coordinates. No model of ours asks it now: Blender lays
    // them out (world/unwrap.py, the shell world/shell.py), and Unity's charts of a mug were slivers.
    static GameObject ImportModel(string name, Dictionary<string, Material> materials, bool unwrap)
    {
        string path = ModelsPath + "/" + name + ".fbx";
        ModelImporter importer = AssetImporter.GetAtPath(path) as ModelImporter;
        if (importer == null) throw new System.Exception(path + " is missing: run world/build.py and copy unity/ShaderEmu over");
        importer.importNormals = ModelImporterNormals.Import;
        importer.importTangents = ModelImporterTangents.CalculateMikk;
        importer.generateSecondaryUV = unwrap;
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
        // (world/unwrap.py keeps this table too, for the margins it packs at)
        if (name == "Desk things" || name == "Table things" || name == "Room desk things") return 8f;   // a mug at 4 had its neighbours' light on its rim
        if (name.EndsWith(" things")) return 4f;   // small: or a mouse is three texels
        if (name == "Cables" || name == "Network" || name == "Hubs") return 4f;
        if (name.StartsWith("Station ") && name.EndsWith(" rest")) return 6f;   // a key's top is a centimetre, and each cap a chart with a margin
        if (name.StartsWith("pachira") || name.StartsWith("potted_plant")) return 0.35f;   // leaves: many small pieces
        return 1f;
    }

    // models whose lightmap islands overlap (Unity's bake lists them): lit by the volume instead
    static readonly string[] ProbeLit = { "wicker_basket", "korean_public_payphone", "ornate_mirror", "boombox", "industrial_wall_sconce",
                                          "korean_fire_extinguisher", "desk_lamp_arm", "modern_ceiling_lamp", "alarm_clock", "fire_alarm" };

    const float BakeTexels = 20f;   // Bakery's texels a metre (its settings' Texels per unit), before a renderer's scale
    const float TinyChart = 9f;     // texels: a chart under 3 x 3 holds no light of its own
    const float TinyShare = 0.3f;   // of a model's surface in such charts: lit by the volume instead
    const float MostScale = 8f;     // the most a model's lightmap is made larger for its small charts
    static readonly List<string> probeLitByCharts = new List<string>(), moreLightmap = new List<string>();

    // The share of a mesh's surface whose lightmap chart (the triangles joined by shared vertices
    // in its second coordinates, as Unity's unwrap and ours split them) is under TinyChart texels.
    static float TinyChartShare(Mesh mesh, Transform t, float texelsPerMetre)
    {
        if (mesh == null || mesh.uv2 == null || mesh.uv2.Length == 0) return 0f;
        int[] tris = mesh.triangles;
        Vector3[] v = mesh.vertices;
        int[] parent = new int[v.Length];
        for (int i = 0; i < parent.Length; i++) parent[i] = i;
        System.Func<int, int> find = null;
        find = i => { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
        for (int k = 0; k < tris.Length; k += 3)
        {
            int a = find(tris[k]), b = find(tris[k + 1]), c = find(tris[k + 2]);
            parent[b] = a; parent[find(c)] = a;
        }
        var area = new Dictionary<int, float>();
        float total = 0f;
        for (int k = 0; k < tris.Length; k += 3)
        {
            Vector3 p = t.TransformPoint(v[tris[k]]), q = t.TransformPoint(v[tris[k + 1]]), r = t.TransformPoint(v[tris[k + 2]]);
            float s = Vector3.Cross(q - p, r - p).magnitude / 2f;
            int root = find(tris[k]);
            area[root] = (area.TryGetValue(root, out float was) ? was : 0f) + s;
            total += s;
        }
        if (total <= 0f) return 0f;
        float small = 0f, texel = 1f / (texelsPerMetre * texelsPerMetre);
        foreach (float a in area.Values) if (a / texel < TinyChart) small += a;
        return small / total;
    }

    static void Remodel(Transform world)
    {
        probeLitByCharts.Clear();
        moreLightmap.Clear();
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
        // (FurnitureCharted: what of the furniture brings its own place in the lightmap, the classroom's keyboards, mice and leads)
        // (AnnexCharted: the side rooms' keyboards, mice and leads, world/rooms.py)
        foreach (string name in new[] { "Room", "Shell", "Computer", "Furniture", "FurnitureCharted", "Annex", "AnnexCharted", "City" })
        {
            bool city = name == "City";
            GameObject instance = (GameObject)PrefabUtility.InstantiatePrefab(ImportModel(name, materials, false));
            instance.transform.SetParent(models, false);
            foreach (MeshRenderer renderer in instance.GetComponentsInChildren<MeshRenderer>())
            {
                GameObject go = renderer.gameObject;
                // (any pane: built in another order Blender names the room's "Window glass.001", and lit by the lightmap its drops were white paint)
                if (city || go.name.StartsWith("Window glass"))
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
                if (go.name.StartsWith("Clock hand "))
                {
                    // (ClockHand.shader turns it about its own middle: it must stay an object of its own)
                    string hand = go.name.Split(' ')[2];
                    Material body = renderer.sharedMaterial, turning = Mat("Clock " + hand + " " + (body != null ? body.name : ""), "ShaderEmu/ClockHand");
                    if (body != null && body.HasProperty("_MainTex")) turning.SetTexture("_MainTex", body.GetTexture("_MainTex"));
                    if (body != null && body.HasProperty("_BumpMap")) turning.SetTexture("_BumpMap", body.GetTexture("_BumpMap"));
                    turning.SetFloat("_Period", hand == "hour" ? 43200f : hand == "minute" ? 3600f : 60f);
                    turning.SetFloat("_Steps", hand == "second" ? 60f : 0f);
                    renderer.sharedMaterial = turning;
                    renderer.shadowCastingMode = ShadowCastingMode.Off;
                    GameObjectUtility.SetStaticEditorFlags(go, 0);
                    continue;
                }
                if (go.name == "Holodeck mask")   // Annex() takes it
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
                // A model whose lightmap is mostly charts too small to hold light (a die, a can, a tape: a share
                // of its surface in charts under TinyChart texels, each chart's texels the dark of what is under or
                // beside it) is given more lightmap, doubled up to MostScale times; only one that is still mostly
                // such charts there is lit by the volume, whose light is coarser and casts no shadow of its own.
                MeshFilter filter = go.GetComponent<MeshFilter>();
                if (!plant && filter != null)
                {
                    float scale = renderer.scaleInLightmap, tiny = TinyChartShare(filter.sharedMesh, go.transform, BakeTexels * scale);
                    while (tiny > TinyShare && scale < LightmapScale(go.name) * MostScale)
                    {
                        scale *= 2f;
                        tiny = TinyChartShare(filter.sharedMesh, go.transform, BakeTexels * scale);
                    }
                    if (scale != renderer.scaleInLightmap)
                        moreLightmap.Add(go.name + string.Format(" (x{0:0.#})", scale / renderer.scaleInLightmap));
                    renderer.scaleInLightmap = scale;
                    if (tiny > TinyShare)
                    {
                        plant = true;
                        probeLitByCharts.Add(go.name + string.Format(" ({0:0}% in charts under {1} texels)", tiny * 100f, TinyChart));
                    }
                }
#if BAKERY_INCLUDED
                if (plant) renderer.receiveGI = ReceiveGI.LightProbes;   // Bakery lays out a lightmap of its own for the rest
#else
                if (plant || System.Array.Exists(ProbeLit, word => go.name.StartsWith(word))) renderer.receiveGI = ReceiveGI.LightProbes;
#endif
            }
        }

        Debug.Log("[ShaderEmu] more lightmap for their small charts: " + (moreLightmap.Count == 0 ? "none" : string.Join(", ", moreLightmap)));
        Debug.Log("[ShaderEmu] lit by the volume for their tiny lightmap charts: " + (probeLitByCharts.Count == 0 ? "none" : string.Join(", ", probeLitByCharts)));
        if (decor.Find("Desk lamp light") == null)
            BakedLight(decor, "Desk lamp light", new Vector3(-3.3f, 1.25f, 5.1f), new Color(1f, 0.78f, 0.5f), 1f, 2.5f);
        Mood(decor);
        foreach (string screen in new[] { "DisplayShow", "Terminal" })
        {
            Material m = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/" + screen + ".mat");
            if (m != null && m.HasProperty("_Glow")) m.SetFloat("_Glow", 1.35f);
        }
        PostProcessing(world);
        NoDeskControllers(world, computer);   // they are in the holodecks now (ShaderEmuGamepad.cs)
        Weather(world, computer);   // the storm, the screens' light, the light volume (ShaderEmuWeather.cs)
        Annex(world, models.Find("Annex"), materials);   // the holodecks and the corridor (ShaderEmuHolodeck.cs)
        Stations(world);                      // the classroom's computers (ShaderEmuStations.cs)
        PcSounds(world);                      // the computer's own sounds (ShaderEmuPcSound.cs)
        // in a scene whose panels are styled the caps' legends are made already: the new models take them
        Material caps = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/Keyboard caps.mat");
        if (caps != null && caps.GetTexture("_Index") != null) CapsOnModels(world, caps);
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
            // (the panes' own is a picture of the city from outside the window: box-projected to its
            // 30 cm box it was a still picture of the room on the glass; ShaderEmuWeather.cs)
            if (probe.name == "Reflection probe window") continue;
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
        camera.nearClipPlane = 0.01f;   // the world's camera: always the least VRChat takes (AGENTS.md), or a hand or a key held near the eye is cut
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
        RetroNewPanels();   // the stations' panels are made again: never left as bare canvases
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(world.scene);
        Debug.Log("[ShaderEmu] the modelled room is in the scene. Now run ShaderEmu/Bake lighting.");
    }
}
