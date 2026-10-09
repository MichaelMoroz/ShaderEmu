using System.Collections.Generic;
using System.IO;
using System.Reflection;
using TMPro;
using UdonSharp;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.TextCore.LowLevel;
using UnityEngine.UI;

// The room's panels as hardware of its decade, and as things in the room: dark faceplates with
// lettering printed on them, push buttons with caps, a board of fanfold paper, label tape over
// the screens. A canvas becomes a plate with a lightmap where it is (Plate.shader): its picture,
// and what its TextMesh Pro labels print as a distance field in the same surface. The canvas
// stays for the beams and for what changes: a read-out or a lamp, which glow.
// The keyboards' caps are models (world/computer.py). Nothing moves.
public static partial class ShaderEmuBuilder
{
    const string UiTextures = Root + "/Textures/UI";
    const string PanelTextures = Root + "/Textures/Panels";
    const int CaptureLayer = 31;
    static readonly Color Ink = new Color(0.05f, 0.05f, 0.06f), InkDim = new Color(0.27f, 0.27f, 0.29f);
    static readonly Color Printed = new Color(0.93f, 0.90f, 0.80f), PrintedDim = new Color(0.93f, 0.90f, 0.80f);
    static readonly Color Amber = new Color(1f, 0.70f, 0.20f), Phosphor = new Color(0.35f, 1f, 0.45f);
    static readonly Color Faceplate = new Color(0.07f, 0.07f, 0.08f), CapColour = new Color(0.86f, 0.83f, 0.74f);
    static readonly Color Liquid = new Color(0.62f, 0.68f, 0.56f);
    static readonly Color Over = new Color(1f, 0.95f, 0.55f, 0.30f), Held = new Color(0f, 0f, 0f, 0.45f), LitUp = new Color(1f, 0.62f, 0.10f, 0.50f);

    static Sprite UiSprite(string name, int border, bool cutout = false)
    {
        string path = UiTextures + "/" + name + ".png";
        TextureImporter importer = AssetImporter.GetAtPath(path) as TextureImporter;
        if (importer == null) throw new System.Exception(path + " is missing: run world/textures.py and copy unity/ShaderEmu over");
        Vector4 borders = Vector4.one * border;
        if (importer.textureType != TextureImporterType.Sprite || importer.spriteBorder != borders || importer.alphaIsTransparency != cutout)
        {
            importer.textureType = TextureImporterType.Sprite;
            importer.spriteImportMode = SpriteImportMode.Single;
            importer.spriteBorder = borders;
            importer.alphaIsTransparency = cutout;
            importer.mipmapEnabled = true;
            importer.wrapMode = TextureWrapMode.Clamp;
            importer.SaveAndReimport();
        }
        return AssetDatabase.LoadAssetAtPath<Sprite>(path);
    }

    // IBM Plex (SIL Open Font Licence; Fonts/OFL.txt) as a TextMesh Pro font with the characters the panels use.
    static TMP_FontAsset RetroFont(string file)
    {
        string path = Generated + "/" + file + " SDF.asset";
        TMP_FontAsset asset = AssetDatabase.LoadAssetAtPath<TMP_FontAsset>(path);
        if (asset != null) return asset;
        Font font = AssetDatabase.LoadAssetAtPath<Font>(Root + "/Fonts/" + file + ".ttf");
        if (font == null) throw new System.Exception(file + ".ttf is missing from " + Root + "/Fonts");
        asset = TMP_FontAsset.CreateFontAsset(font, 72, 8, GlyphRenderMode.SDFAA, 1024, 1024, AtlasPopulationMode.Dynamic, false);
        AssetDatabase.CreateAsset(asset, path);
        asset.material.name = file + " material";
        asset.atlasTexture.name = file + " atlas";
        AssetDatabase.AddObjectToAsset(asset.material, asset);
        AssetDatabase.AddObjectToAsset(asset.atlasTexture, asset);
        System.Text.StringBuilder characters = new System.Text.StringBuilder();
        for (char c = ' '; c <= '~'; c++) characters.Append(c);
        characters.Append("←↑→↓…·×");
        string missing;
        asset.TryAddCharacters(characters.ToString(), out missing);
        asset.atlasPopulationMode = AtlasPopulationMode.Static;   // the atlas is whole: nothing is drawn into it in the world
        EditorUtility.SetDirty(asset);
        AssetDatabase.SaveAssets();
        return asset;
    }

    // A font's material for print: lit by the room. The font's own is for what glows.
    static Material LitFont(TMP_FontAsset font)
    {
        string path = Generated + "/" + font.name + " lit.mat";
        Material material = AssetDatabase.LoadAssetAtPath<Material>(path);
        if (material == null)
        {
            material = new Material(font.material);
            AssetDatabase.CreateAsset(material, path);
        }
        material.shader = Shader.Find("ShaderEmu/LitText");
        material.SetTexture("_MainTex", font.atlasTexture);
        EditorUtility.SetDirty(material);
        return material;
    }

    static void Letters(TextMeshProUGUI label, TMP_FontAsset font, Color colour, bool glows = false)
    {
        label.enabled = true;
        label.font = font;
        label.fontSharedMaterial = glows ? font.material : LitFont(font);
        label.color = colour;
        EditorUtility.SetDirty(label);
    }

    static void Picture(Image image, Sprite sprite, Color colour)
    {
        image.enabled = true;
        image.sprite = sprite;
        image.type = sprite != null ? Image.Type.Sliced : Image.Type.Simple;
        image.color = colour;
        image.material = UiMaterial();
        EditorUtility.SetDirty(image);
    }

    static float Brightness(Color c) { return (c.r + c.g + c.b) / 3f; }

    static float Saturation(Color c)
    {
        float high = Mathf.Max(c.r, Mathf.Max(c.g, c.b)), low = Mathf.Min(c.r, Mathf.Min(c.g, c.b));
        return high <= 0f ? 0f : (high - low) / high;
    }

    // What a text's colour meant, as print on a dark plate: plain, dimmer, or marked out in amber.
    static Color OnPlate(Color c)
    {
        if (Saturation(c) > 0.35f && Brightness(c) > 0.12f) return Amber;
        float b = Brightness(c);
        return (b > 0.75f || b < 0.15f) ? Printed : PrintedDim;
    }

    // Another font is another width: a paragraph that no longer fits its box is set smaller until it does.
    static void Fit(TextMeshProUGUI label)
    {
        if (!label.enableWordWrapping) return;
        float least = label.fontSize * 0.6f;
        label.ForceMeshUpdate();
        if (label.GetPreferredValues(label.text, 100000f, 100000f).x <= label.rectTransform.rect.width)
        {
            label.overflowMode = TextOverflowModes.Overflow;   // one line: a line is a little higher than some of these boxes, at any size
            return;
        }
        while (label.preferredHeight > label.rectTransform.rect.height + 2f && label.fontSize > least)
        {
            label.fontSize -= 1f;
            label.ForceMeshUpdate();
        }
    }

    static Image Behind(RectTransform label, string name, float grow, Sprite sprite, Color colour)
    {
        Transform parent = label.parent;
        Transform old = parent.Find(name);
        if (old != null) Object.DestroyImmediate(old.gameObject);
        GameObject go = new GameObject(name, typeof(RectTransform));
        RectTransform rect = go.GetComponent<RectTransform>();
        rect.SetParent(parent, false);
        rect.anchorMin = label.anchorMin;
        rect.anchorMax = label.anchorMax;
        rect.pivot = label.pivot;
        rect.anchoredPosition = label.anchoredPosition + new Vector2(-grow, grow);
        rect.sizeDelta = label.sizeDelta + new Vector2(grow * 2, grow * 2);
        rect.SetSiblingIndex(label.GetSiblingIndex());
        Image image = rect.gameObject.AddComponent<Image>();
        image.raycastTarget = false;
        Picture(image, sprite, colour);
        return image;
    }

    // What a behaviour writes or paints while the world runs: every label and image its fields name.
    static void Changing(Transform world, HashSet<Object> labels, HashSet<Object> plates)
    {
        foreach (UdonSharpBehaviour behaviour in world.GetComponentsInChildren<UdonSharpBehaviour>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(behaviour);
            foreach (FieldInfo field in behaviour.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance))
            {
                object value = field.GetValue(behaviour);
                if (value is TextMeshProUGUI || value is Text) labels.Add((Object)value);
                else if (value is TextMeshProUGUI[]) foreach (TextMeshProUGUI label in (TextMeshProUGUI[])value) { if (label != null) labels.Add(label); }
                else if (value is Image && field.Name != "keyPlate") plates.Add((Object)value);
                else if (value is Image[] && field.Name != "keyPlate") foreach (Image image in (Image[])value) { if (image != null) plates.Add(image); }
            }
        }
    }

    static readonly Dictionary<Canvas, bool> Hidden = new Dictionary<Canvas, bool>();

    static bool WillShow(Canvas canvas)
    {
        bool hidden;
        return !(Hidden.TryGetValue(canvas, out hidden) && hidden);
    }

    static Camera PanelCamera(RectTransform rect)
    {
        Vector2 size = rect.sizeDelta;
        Camera camera = new GameObject("panel camera").AddComponent<Camera>();
        camera.transform.SetPositionAndRotation(rect.position - rect.forward * 0.5f, rect.rotation);
        camera.orthographic = true;
        camera.orthographicSize = size.y * rect.lossyScale.y / 2f;
        camera.aspect = size.x / size.y;
        camera.nearClipPlane = 0.01f;
        camera.farClipPlane = 1f;
        camera.cullingMask = 1 << CaptureLayer;
        camera.clearFlags = CameraClearFlags.SolidColor;
        camera.backgroundColor = new Color(0, 0, 0, 0);
        camera.allowHDR = false;
        camera.allowMSAA = false;
        return camera;
    }

    static Texture2D Shot(Camera camera, int wide, int high, bool colour, int samples, Shader with)
    {
        RenderTexture target = new RenderTexture(wide, high, 24, RenderTextureFormat.ARGB32, colour ? RenderTextureReadWrite.sRGB : RenderTextureReadWrite.Linear);
        target.antiAliasing = samples;
        camera.targetTexture = target;
        if (with != null) camera.RenderWithShader(with, ""); else camera.Render();
        RenderTexture.active = target;
        Texture2D picture = new Texture2D(wide, high, TextureFormat.RGBA32, false, !colour);
        picture.ReadPixels(new Rect(0, 0, wide, high), 0, 0);
        picture.Apply();
        RenderTexture.active = null;
        camera.targetTexture = null;
        target.Release();
        Object.DestroyImmediate(target);
        return picture;
    }

    // A picture written among the panels' textures. kind 1: a map of glyph numbers, exact; 2: the fonts' sheet.
    static Texture2D Kept(string name, Texture2D picture, int kind)
    {
        Directory.CreateDirectory(PanelTextures);
        string path = PanelTextures + "/" + name + ".png";
        File.WriteAllBytes(path, picture.EncodeToPNG());
        Object.DestroyImmediate(picture);
        AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate);
        TextureImporter importer = (TextureImporter)AssetImporter.GetAtPath(path);
        importer.wrapMode = TextureWrapMode.Clamp;
        importer.alphaIsTransparency = false;
        importer.maxTextureSize = 4096;
        importer.npotScale = TextureImporterNPOTScale.None;
        importer.anisoLevel = kind == 1 ? 0 : 16;
        importer.mipmapEnabled = kind == 0;
        importer.sRGBTexture = kind == 0;
        importer.filterMode = kind == 1 ? FilterMode.Point : FilterMode.Trilinear;
        if (kind != 0) importer.textureCompression = TextureImporterCompression.Uncompressed;
        TextureImporterPlatformSettings pc = importer.GetPlatformTextureSettings("Standalone");
        pc.overridden = kind != 0;
        pc.maxTextureSize = 4096;
        pc.format = kind == 1 ? TextureImporterFormat.RGBA32 : kind == 2 ? TextureImporterFormat.R8 : TextureImporterFormat.Automatic;
        importer.SetPlatformTextureSettings(pc);
        importer.SaveAndReimport();
        return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
    }

    static Dictionary<GameObject, int> ToCaptureLayer(Canvas canvas)
    {
        Dictionary<GameObject, int> layers = new Dictionary<GameObject, int>();
        foreach (Transform t in canvas.GetComponentsInChildren<Transform>(true)) { layers[t.gameObject] = t.gameObject.layer; t.gameObject.layer = CaptureLayer; }
        return layers;
    }

    const int GlyphsWide = 1024, SheetCell = 1024;
    static readonly List<TMP_FontAsset> SheetFonts = new List<TMP_FontAsset>();
    static Texture2D Sheet;

    // The panels' fonts' distance field atlases side by side in one texture, for Plate.shader.
    static void FontSheet(params TMP_FontAsset[] fonts)
    {
        SheetFonts.Clear();
        SheetFonts.AddRange(fonts);
        int side = 1;
        while (side * side < fonts.Length) side++;
        Texture2D sheet = new Texture2D(side * SheetCell, side * SheetCell, TextureFormat.RGBA32, false, true);
        Color32[] all = new Color32[sheet.width * sheet.height];
        RenderTexture target = new RenderTexture(SheetCell, SheetCell, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
        Texture2D one = new Texture2D(SheetCell, SheetCell, TextureFormat.RGBA32, false, true);
        for (int k = 0; k < fonts.Length; k++)
        {
            Texture atlas = fonts[k].atlasTexture;
            if (atlas.width != SheetCell || atlas.height != SheetCell) throw new System.Exception(fonts[k].name + ": its atlas is not " + SheetCell + " square");
            Graphics.Blit(atlas, target);
            RenderTexture.active = target;
            one.ReadPixels(new Rect(0, 0, SheetCell, SheetCell), 0, 0);
            RenderTexture.active = null;
            Color32[] pixels = one.GetPixels32();
            int x0 = (k % side) * SheetCell, y0 = (k / side) * SheetCell;
            for (int y = 0; y < SheetCell; y++)
                for (int x = 0; x < SheetCell; x++)
                {
                    byte a = pixels[y * SheetCell + x].a;
                    all[(y0 + y) * sheet.width + x0 + x] = new Color32(a, a, a, 255);
                }
        }
        target.Release();
        Object.DestroyImmediate(target);
        Object.DestroyImmediate(one);
        sheet.SetPixels32(all);
        sheet.Apply();
        Sheet = Kept("Fonts", sheet, 2);
    }

    // What is printed on a surface, as that surface's own (Plate.shader): every glyph of the
    // labels is a box on the surface, a box in the fonts' sheet and a colour (NAME glyphs), and
    // a map of the surface says which glyph a place belongs to (NAME index).
    static void Lettering(Canvas canvas, string name, List<Graphic> print, Material onto)
    {
        RectTransform rect = (RectTransform)canvas.transform;
        Vector2 size = rect.sizeDelta;
        int side = 1;
        while (side * side < SheetFonts.Count) side++;
        List<Color> data = new List<Color>();
        List<Vector4> inner = new List<Vector4>();   // a glyph without its field's margin, in canvas units
        List<Vector4> outer = new List<Vector4>();
        if (print != null)
            foreach (Graphic graphic in print)
            {
                TextMeshProUGUI label = graphic as TextMeshProUGUI;
                if (label == null) continue;
                label.enabled = true;
                label.ForceMeshUpdate(true, true);
                TMP_TextInfo text = label.textInfo;
                for (int c = 0; c < text.characterCount; c++)
                {
                    TMP_CharacterInfo info = text.characterInfo[c];
                    if (!info.isVisible) continue;
                    int font = SheetFonts.IndexOf(info.fontAsset);
                    if (font < 0) { Debug.LogWarning("[ShaderEmu] " + label.name + ": a glyph of " + info.fontAsset.name + ", which is not in the sheet"); continue; }
                    Vector3 low = canvas.transform.InverseTransformPoint(label.transform.TransformPoint(info.vertex_BL.position));
                    Vector3 top = canvas.transform.InverseTransformPoint(label.transform.TransformPoint(info.vertex_TR.position));
                    Vector3 lowIn = canvas.transform.InverseTransformPoint(label.transform.TransformPoint(info.bottomLeft));
                    Vector3 topIn = canvas.transform.InverseTransformPoint(label.transform.TransformPoint(info.topRight));
                    Vector2 uvLow = info.vertex_BL.uv, uvTop = info.vertex_TR.uv;
                    Vector2 cell = new Vector2(font % side, font / side);
                    Material material = info.fontAsset.material;
                    bool bold = (info.style & FontStyles.Bold) != 0;
                    float weight = material.GetFloat(bold ? "_WeightBold" : "_WeightNormal") / 4f * material.GetFloat("_ScaleRatioA") * 0.5f;
                    Color colour = ((Color)info.vertex_BL.color).linear;
                    data.Add(new Color(low.x / size.x + 0.5f, low.y / size.y + 0.5f, top.x / size.x + 0.5f, top.y / size.y + 0.5f));
                    data.Add(new Color((uvLow.x + cell.x) / side, (uvLow.y + cell.y) / side, (uvTop.x + cell.x) / side, (uvTop.y + cell.y) / side));
                    data.Add(new Color(colour.r, colour.g, colour.b, weight));
                    outer.Add(new Vector4(low.x, low.y, top.x, top.y));
                    inner.Add(new Vector4(lowIn.x, lowIn.y, topIn.x, topIn.y));
                }
                label.enabled = false;
            }
        if (inner.Count == 0) { onto.SetTexture("_Index", null); onto.SetTexture("_Glyphs", null); onto.SetTexture("_Fonts", null); onto.SetTexture("_Far", null); EditorUtility.SetDirty(onto); return; }
        if (inner.Count > 65535) throw new System.Exception(name + ": more glyphs than the index can name");

        // A place of the map names the two glyphs nearest to it whose boxes reach it: a glyph's
        // edge is then whole on both sides of the line where one glyph's places end.
        float scale = Mathf.Min(1f, (size.x > 3000f ? 4096f : 2048f) / Mathf.Max(size.x, size.y));
        int wide = Mathf.CeilToInt(size.x * scale), high = Mathf.CeilToInt(size.y * scale);
        int[] first = new int[wide * high], second = new int[wide * high];
        float[] firstAway = new float[wide * high], secondAway = new float[wide * high];
        float reach = 0.75f / scale;   // a place is a square: a box that touches it counts
        for (int g = 0; g < inner.Count; g++)
        {
            Vector4 box = outer[g], core = inner[g];
            int x0 = Mathf.Max(0, Mathf.FloorToInt((box.x - reach + size.x / 2) * scale)), x1 = Mathf.Min(wide - 1, Mathf.CeilToInt((box.z + reach + size.x / 2) * scale));
            int y0 = Mathf.Max(0, Mathf.FloorToInt((box.y - reach + size.y / 2) * scale)), y1 = Mathf.Min(high - 1, Mathf.CeilToInt((box.w + reach + size.y / 2) * scale));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                {
                    float px = (x + 0.5f) / scale - size.x / 2, py = (y + 0.5f) / scale - size.y / 2;
                    if (px < box.x - reach || px > box.z + reach || py < box.y - reach || py > box.w + reach) continue;
                    float dx = Mathf.Max(core.x - px, 0, px - core.z), dy = Mathf.Max(core.y - py, 0, py - core.w);
                    float away = dx * dx + dy * dy;
                    int at = y * wide + x;
                    if (first[at] == 0 || away < firstAway[at])
                    {
                        second[at] = first[at]; secondAway[at] = firstAway[at];
                        first[at] = g + 1; firstAway[at] = away;
                    }
                    else if (second[at] == 0 || away < secondAway[at]) { second[at] = g + 1; secondAway[at] = away; }
                }
        }
        Texture2D index = new Texture2D(wide, high, TextureFormat.RGBA32, false, true);
        Color32[] named = new Color32[wide * high];
        for (int i = 0; i < named.Length; i++) named[i] = new Color32((byte)(first[i] & 255), (byte)(first[i] >> 8), (byte)(second[i] & 255), (byte)(second[i] >> 8));
        index.SetPixels32(named);
        index.Apply();

        // the lettering's usual size: the sheet's texels across the plate, for places with no glyph
        List<float> across = new List<float>(), up = new List<float>();
        for (int g = 0; g < inner.Count; g++)
        {
            Color here = data[g * 3], there = data[g * 3 + 1];
            across.Add(Mathf.Abs((there.b - there.r) / (here.b - here.r)) * side * SheetCell);
            up.Add(Mathf.Abs((there.a - there.g) / (here.a - here.g)) * side * SheetCell);
        }
        across.Sort();
        up.Sort();
        onto.SetVector("_Usual", new Vector4(across[across.Count / 2], up[up.Count / 2], 0, 0));

        int rows = (data.Count + GlyphsWide - 1) / GlyphsWide;
        Texture2D glyphs = new Texture2D(GlyphsWide, rows, TextureFormat.RGBAFloat, false, true);
        Color[] table = new Color[GlyphsWide * rows];
        data.CopyTo(table);
        glyphs.SetPixels(table);
        glyphs.Apply();
        glyphs.filterMode = FilterMode.Point;
        glyphs.wrapMode = TextureWrapMode.Clamp;
        string path = PanelTextures + "/" + name + " glyphs.asset";
        AssetDatabase.DeleteAsset(path);
        AssetDatabase.CreateAsset(glyphs, path);

        // from far off a pixel holds whole glyphs: there the lettering is a picture of itself with mipmaps
        Graphic[] all = canvas.GetComponentsInChildren<Graphic>(true);
        bool[] was = new bool[all.Length];
        for (int i = 0; i < all.Length; i++) { was[i] = all[i].enabled; all[i].enabled = print.Contains(all[i]) && all[i] is TextMeshProUGUI; }
        Dictionary<GameObject, int> layers = ToCaptureLayer(canvas);
        Canvas.ForceUpdateCanvases();
        Camera camera = PanelCamera(rect);
        float farScale = Mathf.Min(2f, 4096f / Mathf.Max(size.x, size.y));   // no coarser than where the glyphs' fields give way to it
        Texture2D far = Shot(camera, Mathf.CeilToInt(size.x * farScale), Mathf.CeilToInt(size.y * farScale), true, 8, null);
        Object.DestroyImmediate(camera.gameObject);
        foreach (KeyValuePair<GameObject, int> pair in layers) pair.Key.layer = pair.Value;
        for (int i = 0; i < all.Length; i++) all[i].enabled = was[i];
        onto.SetTexture("_Far", Kept(name + " far", far, 0));

        onto.SetTexture("_Index", Kept(name + " index", index, 1));
        onto.SetTexture("_Glyphs", glyphs);
        onto.SetTexture("_Fonts", Sheet);
        onto.SetFloat("_GradientScale", SheetFonts[0].material.GetFloat("_GradientScale"));
        EditorUtility.SetDirty(onto);
    }

    // The canvas as it stands, without its print, drawn into a picture and hung on a plate with a
    // lightmap just behind it; the print is the plate's lettering, and leaves the canvas.
    static void Plate(Canvas canvas, string name, bool shown = true, List<Graphic> print = null)
    {
        RectTransform rect = (RectTransform)canvas.transform;
        string plateName = canvas.name + " plate";
        Transform old = canvas.transform.Find(plateName);
        if (old != null) Object.DestroyImmediate(old.gameObject);
        Vector2 size = rect.sizeDelta;
        float scale = Mathf.Min(2f, (size.x > 3000f ? 4096f : 2048f) / Mathf.Max(size.x, size.y));   // the strip of names is seven metres
        int wide = Mathf.CeilToInt(size.x * scale), high = Mathf.CeilToInt(size.y * scale);
        if (print != null) foreach (Graphic graphic in print) graphic.enabled = false;
        Dictionary<GameObject, int> layers = ToCaptureLayer(canvas);
        Camera camera = PanelCamera(rect);
        Canvas.ForceUpdateCanvases();
        Texture2D picture = Shot(camera, wide, high, true, 8, null);
        Object.DestroyImmediate(camera.gameObject);
        foreach (KeyValuePair<GameObject, int> pair in layers) pair.Key.layer = pair.Value;

        bool holes = false;
        foreach (Color32 pixel in picture.GetPixels32()) if (pixel.a < 128) { holes = true; break; }
        Material material = Mat("Plate " + name, "ShaderEmu/Plate");
        material.SetTexture("_MainTex", Kept(name, picture, 0));
        material.SetFloat("_Cutoff", holes ? 0.5f : 0f);
        // print shines a little on a matt plate; on paper it is as dull as the paper
        // a panel is to be read, not admired: nearly all of it shown as drawn, a little left to the room's light
        bool paper = name == "About";
        material.SetFloat("_Glossiness", 0.08f);
        material.SetFloat("_InkGlossiness", 0.08f);
        material.SetFloat("_InkMetallic", 0f);
        material.SetFloat("_InkGlow", paper ? 0f : 0.2f);   // a little over the plate, short of glowing
        material.SetFloat("_Flat", paper ? 0.5f : 0.8f);
        material.SetFloat("_Bold", paper ? 0.07f : 0.05f);   // thin strokes were hard to read across the room
        Lettering(canvas, name, print, material);
        if (print != null) foreach (Graphic graphic in print) graphic.enabled = false;

        GameObject plate = GameObject.CreatePrimitive(PrimitiveType.Quad);   // it faces -z, as a canvas does
        plate.name = plateName;
        Object.DestroyImmediate(plate.GetComponent<Collider>());
        plate.transform.SetParent(canvas.transform, false);   // the canvas's own: it opens and closes with it
        plate.transform.localPosition = new Vector3(0, 0, 0.6f);
        plate.transform.localScale = new Vector3(size.x, size.y, 1f);
        MeshRenderer renderer = plate.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = ShadowCastingMode.Off;
        if (!shown) return;   // a panel that opens later has no part in the bake: the probes light its plate
        GameObjectUtility.SetStaticEditorFlags(plate, StaticEditorFlags.ContributeGI | StaticEditorFlags.BatchingStatic | StaticEditorFlags.ReflectionProbeStatic);
        renderer.receiveGI = ReceiveGI.Lightmaps;
        renderer.scaleInLightmap = 3f;
    }

    // A panel: draw it whole and still, hang that on a plate, then leave on the canvas only what changes.
    static void PanelLook(Canvas canvas, Sprite raised, Sprite sunken, Sprite cap, TMP_FontAsset sans, TMP_FontAsset bold, TMP_FontAsset mono,
                          HashSet<Object> changing, HashSet<Object> painted)
    {
        Transform bar = canvas.transform.Find("Title bar");
        if (bar != null) Object.DestroyImmediate(bar.gameObject);
        List<Graphic> still = new List<Graphic>();     // drawn on the plate, then not by the canvas
        List<Graphic> live = new List<Graphic>();      // not on the plate: the canvas draws them
        List<Graphic> print = new List<Graphic>();     // lettering: the plate's own
        Transform backTransform = canvas.transform.Find("Back");
        if (backTransform != null) { Image back = backTransform.GetComponent<Image>(); Picture(back, raised, Faceplate); still.Add(back); }
        HashSet<Object> done = new HashSet<Object>();
        foreach (Button button in canvas.GetComponentsInChildren<Button>(true))
        {
            Image image = button.targetGraphic as Image;
            bool tinted = image != null && Saturation(image.color) > 0.4f && image.color.a > 0.9f && !painted.Contains(image);   // made to stand out
            if (image != null)
            {
                Picture(image, cap, tinted ? image.color : CapColour);
                // a button tints what it draws, and after a first restyle that tint is "unseen":
                // drawn like that, the caps were missing from the picture
                ColorBlock plain = button.colors;
                plain.normalColor = Color.white;
                button.colors = plain;
                image.canvasRenderer.SetColor(Color.white);
                image.pixelsPerUnitMultiplier = 1.3f;
                still.Add(image);
                done.Add(image);
            }
            foreach (TextMeshProUGUI label in button.GetComponentsInChildren<TextMeshProUGUI>(true))
            {
                Letters(label, bold, tinted ? Color.white : Ink);
                (changing.Contains(label) ? live : print).Add(label);
                done.Add(label);
            }
        }
        foreach (Slider slider in canvas.GetComponentsInChildren<Slider>(true))
            foreach (Image part in slider.GetComponentsInChildren<Image>(true))
            {
                // a fader: its slot is on the plate; the strip of light and the cap move
                if (part.name == "Fill") { Picture(part, null, Amber); live.Add(part); }
                else if (part.name == "Handle") { Picture(part, cap, CapColour); part.material = Mat("LitUI", "ShaderEmu/LitUI"); live.Add(part); }
                else { Picture(part, sunken, new Color(0.10f, 0.10f, 0.11f)); still.Add(part); }
                done.Add(part);
            }
        foreach (Selectable field in canvas.GetComponentsInChildren<Selectable>(true))
        {
            if (!(field is InputField)) continue;   // VRChat's address field is one too
            Image image = field.targetGraphic as Image;
            if (image != null) { Picture(image, sunken, Liquid); still.Add(image); done.Add(image); }   // a liquid crystal strip
            foreach (Text text in field.GetComponentsInChildren<Text>(true)) { text.color = Ink; text.material = Mat("LitUI", "ShaderEmu/LitUI"); live.Add(text); done.Add(text); EditorUtility.SetDirty(text); }
        }
        foreach (TextMeshProUGUI label in canvas.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            if (done.Contains(label)) continue;
            if (label.name == "Stats")   // the machine's own read-out: a display let into the panel
            {
                still.Add(Behind(label.rectTransform, "Stats field", 10f, sunken, new Color(0.05f, 0.08f, 0.06f)));
                Letters(label, mono, Phosphor, true);
                live.Add(label);
            }
            else if (changing.Contains(label)) { Letters(label, mono, Amber, true); live.Add(label); }   // a small display of its own
            else
            {
                Letters(label, label.name == "Title" ? bold : sans, label.name == "Title" ? Printed : OnPlate(label.color));
                print.Add(label);
            }
            Fit(label);
        }
        foreach (Text text in canvas.GetComponentsInChildren<Text>(true))
        {
            if (done.Contains(text)) continue;
            bool glows = changing.Contains(text);
            text.enabled = true;
            text.color = glows ? Amber : OnPlate(text.color);
            (glows ? live : print).Add(text);
            EditorUtility.SetDirty(text);
        }

        foreach (Graphic graphic in live) graphic.enabled = false;
        Plate(canvas, canvas.name, WillShow(canvas), print);
        foreach (Graphic graphic in live) graphic.enabled = true;
        foreach (Graphic graphic in still)
        {
            Image image = graphic as Image;
            if (image == null) continue;
            // an image stays, unseen, for a beam to find; a button shows the beam on it
            image.sprite = null;
            image.type = Image.Type.Simple;
            image.color = painted.Contains(image) ? Color.clear : image.GetComponent<Button>() != null ? Color.white : Color.clear;
        }
        foreach (Button button in canvas.GetComponentsInChildren<Button>(true))
        {
            ColorBlock colours = button.colors;
            bool own = painted.Contains(button.targetGraphic);   // its behaviour paints it: lit from within while picked
            colours.normalColor = own ? Color.white : Color.clear;
            colours.highlightedColor = own ? Color.white : Over;
            colours.pressedColor = own ? Color.white : Held;
            colours.selectedColor = colours.normalColor;
            colours.colorMultiplier = 1f;
            button.colors = colours;
            if (button.targetGraphic != null) button.targetGraphic.canvasRenderer.SetColor(colours.normalColor);
            EditorUtility.SetDirty(button);
        }
    }

    // The caps are a model. The canvas keeps the rectangles, and shows a key under a beam, held or latched.
    static void KeyboardLook(EmuKeyboard keyboard, TMP_FontAsset legend, TMP_FontAsset mono)
    {
        UdonSharpEditorUtility.CopyUdonToProxy(keyboard);
        Transform back = keyboard.transform.Find("Back");
        if (back != null) back.GetComponent<Image>().enabled = false;
        Color[] colours = new Color[keyboard.keyPlate.Length];
        List<Graphic> legends = new List<Graphic>();
        for (int i = 0; i < keyboard.keyPlate.Length; i++)
        {
            Image plate = keyboard.keyPlate[i];
            plate.sprite = null;
            plate.type = Image.Type.Simple;
            plate.material = UiMaterial();
            plate.color = colours[i] = Color.clear;
            EditorUtility.SetDirty(plate);
            TextMeshProUGUI label = plate.GetComponentInChildren<TextMeshProUGUI>(true);
            if (label == null) continue;
            // a legend is printed in its cap's top left corner, as on the keyboards of the time
            label.gameObject.SetActive(true);
            Letters(label, legend, Ink);
            label.enableWordWrapping = false;
            label.overflowMode = TextOverflowModes.Overflow;
            if (label == keyboard.captureLabel) { label.alignment = TextAlignmentOptions.Center; label.margin = Vector4.zero; label.fontSize = 18; continue; }
            int length = label.text.Length;
            label.alignment = TextAlignmentOptions.TopLeft;
            label.margin = new Vector4(11, 7, 0, 0);
            label.fontSize = length <= 1 ? 24 : length <= 3 ? 17 : 14;
            legends.Add(label);
        }
        // the legends are the caps' own: the model's picture is the panel seen from above, and so is their field
        Material caps = Mat("Keyboard caps", "ShaderEmu/Plate");
        caps.SetFloat("_Glossiness", 0.32f);
        caps.SetFloat("_InkGlossiness", 0.6f);
        foreach (MeshRenderer renderer in keyboard.transform.root.GetComponentsInChildren<MeshRenderer>(true))
        {
            Material[] materials = renderer.sharedMaterials;
            bool any = false;
            for (int i = 0; i < materials.Length; i++)
                if (materials[i] != null && materials[i].name == "Keyboard") { caps.SetTexture("_MainTex", materials[i].mainTexture); materials[i] = caps; any = true; }
            if (any) renderer.sharedMaterials = materials;
        }
        Lettering(keyboard.GetComponent<Canvas>(), "Keyboard", legends, caps);
        foreach (Graphic graphic in legends) graphic.enabled = false;
        foreach (TextMeshProUGUI label in keyboard.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            if (label.transform.parent != keyboard.transform) continue;
            if (label.name == "Modifiers") Letters(label, mono, Amber, true);   // a row of lamps, in effect
            else Letters(label, legend, Printed);   // its name, printed on the tray
        }
        Transform oldPlate = keyboard.transform.Find(keyboard.name + " plate");   // from when its name was a picture
        if (oldPlate != null) Object.DestroyImmediate(oldPlate.gameObject);
        keyboard.keyColour = colours;
        keyboard.plateColour = Color.clear;
        keyboard.hoverColour = Over;
        keyboard.pressedColour = Held;
        keyboard.latchedColour = LitUp;
        Apply(keyboard);
    }

    static void PaperLook(Canvas canvas, Sprite paper, TMP_FontAsset mono, TMP_FontAsset bold)
    {
        Image back = canvas.transform.Find("Back").GetComponent<Image>();
        Picture(back, paper, Color.white);
        back.type = Image.Type.Simple;
        List<Graphic> print = new List<Graphic>();
        foreach (TextMeshProUGUI label in canvas.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            bool head = label.name.EndsWith(" head") || label.name == "Title";
            Letters(label, head ? bold : mono, label.name == "Title" ? Ink : head ? new Color(0.50f, 0.10f, 0.08f) : label.name == "Note" ? InkDim : Ink);
            if (head)   // one line: its box is a line high, and this font's line is a little higher
            {
                label.enableWordWrapping = false;
                label.overflowMode = TextOverflowModes.Overflow;
            }
            Fit(label);
            print.Add(label);
        }
        Plate(canvas, canvas.name, WillShow(canvas), print);
        back.enabled = false;
    }

    static void TapeLook(Canvas canvas, TMP_FontAsset bold, HashSet<Object> changing)
    {
        Color tape = new Color(0.06f, 0.06f, 0.07f), raised = new Color(0.95f, 0.95f, 0.92f);
        Image back = canvas.transform.Find("Back").GetComponent<Image>();
        bool whole = canvas.name.EndsWith(" name");   // a small sign is one strip; the long one has a strip a name
        List<Graphic> still = new List<Graphic>(), live = new List<Graphic>(), print = new List<Graphic>();
        if (whole) { Picture(back, null, tape); still.Add(back); }
        foreach (TextMeshProUGUI label in canvas.GetComponentsInChildren<TextMeshProUGUI>(true))
        {
            Letters(label, bold, raised);
            label.characterSpacing = 6f;
            label.ForceMeshUpdate();
            (changing.Contains(label) ? live : print).Add(label);
            if (whole) continue;
            Image strip = Behind(label.rectTransform, "Tape of " + label.name, 0f, null, tape);
            RectTransform rect = strip.rectTransform;
            float wide = Mathf.Min(label.rectTransform.sizeDelta.x, label.preferredWidth + 70f);
            rect.anchoredPosition += new Vector2((label.rectTransform.sizeDelta.x - wide) / 2f, 0);
            rect.sizeDelta = new Vector2(wide, label.rectTransform.sizeDelta.y);
            still.Add(strip);
        }
        foreach (Graphic graphic in live) graphic.enabled = false;
        Plate(canvas, canvas.name, WillShow(canvas), print);
        foreach (Graphic graphic in live) graphic.enabled = true;
        foreach (Graphic graphic in still) graphic.enabled = false;
    }

    static void Retro(Transform world)
    {
        Sprite raised = UiSprite("Raised", 6), sunken = UiSprite("Sunken", 6), cap = UiSprite("Keycap", 16, true), paper = UiSprite("Paper", 0);
        TMP_FontAsset sans = RetroFont("IBMPlexSansCondensed-Medium"), bold = RetroFont("IBMPlexSansCondensed-Bold");
        TMP_FontAsset mono = RetroFont("IBMPlexMono-Regular"), monoBold = RetroFont("IBMPlexMono-Bold");
        HashSet<Object> changing = new HashSet<Object>(), painted = new HashSet<Object>();
        Changing(world, changing, painted);
        FontSheet(sans, bold, mono, monoBold);
        foreach (Canvas canvas in world.GetComponentsInChildren<Canvas>(true))
        {
            if (canvas.transform.parent != null && canvas.transform.parent.GetComponentInParent<Canvas>() != null) continue;   // a slider's own
            bool shown = canvas.gameObject.activeSelf;
            Hidden[canvas] = !shown;
            canvas.gameObject.SetActive(true);   // one that opens later is drawn as it will be
            EmuKeyboard keyboard = canvas.GetComponent<EmuKeyboard>();
            if (keyboard != null) KeyboardLook(keyboard, bold, monoBold);
            else if (canvas.name == "About") PaperLook(canvas, paper, mono, monoBold);
            else if (canvas.name == "Signs" || canvas.name.EndsWith(" name")) TapeLook(canvas, monoBold, changing);
            else if (canvas.name == "Memory legend" || canvas.name == "CPU sign")   // drawn over a screen: part of a display
                foreach (TextMeshProUGUI label in canvas.GetComponentsInChildren<TextMeshProUGUI>(true)) { label.font = monoBold; label.fontSharedMaterial = monoBold.material; EditorUtility.SetDirty(label); }
            else PanelLook(canvas, raised, sunken, cap, sans, bold, mono, changing, painted);
            canvas.gameObject.SetActive(shown);
        }
        // what the behaviours paint their own buttons with: nothing, or lit from within
        foreach (EmuScope scope in world.GetComponentsInChildren<EmuScope>(true))
        {
            scope.plain = Color.clear;
            scope.picked = LitUp;
            Apply(scope);
        }
        foreach (EmuShareHub hub in world.GetComponentsInChildren<EmuShareHub>(true))
        {
            hub.rowColour = Color.clear;
            hub.pickedColour = LitUp;
            Apply(hub);
        }
        foreach (EmuTerminal terminal in world.GetComponentsInChildren<EmuTerminal>(true))
        {
            terminal.tabColour = Color.clear;
            terminal.tabShownColour = LitUp;
            terminal.tabNewsColour = new Color(0.45f, 1f, 0.45f, 0.40f);
            Apply(terminal);
        }
    }

    [MenuItem("ShaderEmu/Restyle the panels in the open scene")]
    public static void RetroOpenScene()
    {
        GameObject world = GameObject.Find("ShaderEmu");
        if (world == null) { Debug.LogError("[ShaderEmu] no ShaderEmu object: run ShaderEmu/Build world first"); return; }
        Retro(world.transform);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(world.scene);
        Debug.Log("[ShaderEmu] the panels are restyled: bake the lighting again, for their plates");
    }
}
