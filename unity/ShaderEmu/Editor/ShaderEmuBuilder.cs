using System.Collections.Generic;
using System.IO;
using TMPro;
using UdonSharp;
using UdonSharp.Compiler;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Events;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.UI;
using VRC.Udon;

// Builds the ShaderEmu world from nothing: the render textures and materials the machine
// runs on, a room with a computer in it, the screens, keyboards and control panel, and the
// Udon behaviours wired together. Running it again replaces what it built before.
public static partial class ShaderEmuBuilder
{
    const string Root = ShaderEmuImages.Root;
    const string Generated = Root + "/Generated";
    const string ScenePath = Root + "/ShaderEmuWorld.unity";
    const int GpuLayer = 22;
    const int GpuTriangles = 65536;

    // ---------------------------------------------------------------- keys

    class Key
    {
        public string label;
        public KeyCode host;
        public int linux, normal, shifted;
        public float x, y, width, height;   // in key units, from the layout's top left corner
    }

    const int None = EmuKeyboard.KeyNone;

    static Key K(string label, KeyCode host, int linux, int normal, int shifted, float width = 1f, float height = 1f)
    {
        return new Key { label = label, host = host, linux = linux, normal = normal, shifted = shifted, width = width, height = height };
    }

    static Key Letter(char c, int linux)
    {
        return K(char.ToUpper(c).ToString(), KeyCode.A + (c - 'a'), linux, c, char.ToUpper(c));
    }

    static Key Symbol(char normal, char shifted, KeyCode host, int linux)
    {
        return K(shifted + " " + normal, host, linux, normal, shifted);
    }

    // A key that types nothing at the console: function keys, locks, modifiers.
    static Key Plain(string label, KeyCode host, int linux, float width = 1f)
    {
        return K(label, host, linux, None, None, width);
    }

    // A key that types the same with and without shift.
    static Key Same(string label, KeyCode host, int linux, int character, float width = 1f, float height = 1f)
    {
        return K(label, host, linux, character, character, width, height);
    }

    static void Row(List<Key> keys, float x, float y, params Key[] row)
    {
        foreach (Key key in row)
        {
            key.x = x;
            key.y = y;
            keys.Add(key);
            x += key.width;
        }
    }

    const float LayoutWidth = 22.5f, LayoutHeight = 6.25f;

    // A full-size keyboard: the main block, the navigation keys and the number pad.
    static List<Key> Layout()
    {
        List<Key> keys = new List<Key>();
        const float nav = 15.25f, pad = 18.5f;
        float[] y = { 0f, 1.25f, 2.25f, 3.25f, 4.25f, 5.25f };

        Row(keys, 0, y[0], Same("Esc", KeyCode.Escape, 1, 27));
        int[] function = { 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88 };
        for (int i = 0; i < 12; i++)
            Row(keys, 2f + i + (i / 4) * 0.5f, y[0], Plain("F" + (i + 1), KeyCode.F1 + i, function[i]));
        Row(keys, nav, y[0], Plain("PrtSc", KeyCode.Print, 99), Plain("ScrLk", KeyCode.ScrollLock, 70), Plain("Pause", KeyCode.Pause, 119));

        Row(keys, 0, y[1],
            Symbol('`', '~', KeyCode.BackQuote, 41),
            Symbol('1', '!', KeyCode.Alpha1, 2), Symbol('2', '@', KeyCode.Alpha2, 3), Symbol('3', '#', KeyCode.Alpha3, 4),
            Symbol('4', '$', KeyCode.Alpha4, 5), Symbol('5', '%', KeyCode.Alpha5, 6), Symbol('6', '^', KeyCode.Alpha6, 7),
            Symbol('7', '&', KeyCode.Alpha7, 8), Symbol('8', '*', KeyCode.Alpha8, 9), Symbol('9', '(', KeyCode.Alpha9, 10),
            Symbol('0', ')', KeyCode.Alpha0, 11), Symbol('-', '_', KeyCode.Minus, 12), Symbol('=', '+', KeyCode.Equals, 13),
            Same("Bksp", KeyCode.Backspace, 14, 127, 2f));
        Row(keys, nav, y[1], Plain("Ins", KeyCode.Insert, 110), Same("Home", KeyCode.Home, 102, EmuKeyboard.KeyHome),
            Same("PgUp", KeyCode.PageUp, 104, EmuKeyboard.KeyPageUp));
        Row(keys, pad, y[1], Plain("Num", KeyCode.Numlock, 69), Same("/", KeyCode.KeypadDivide, 98, '/'),
            Same("*", KeyCode.KeypadMultiply, 55, '*'), Same("-", KeyCode.KeypadMinus, 74, '-'));

        Row(keys, 0, y[2],
            Same("Tab", KeyCode.Tab, 15, 9, 1.5f),
            Letter('q', 16), Letter('w', 17), Letter('e', 18), Letter('r', 19), Letter('t', 20), Letter('y', 21),
            Letter('u', 22), Letter('i', 23), Letter('o', 24), Letter('p', 25),
            Symbol('[', '{', KeyCode.LeftBracket, 26), Symbol(']', '}', KeyCode.RightBracket, 27),
            K("| \\", KeyCode.Backslash, 43, '\\', '|', 1.5f));
        Row(keys, nav, y[2], Same("Del", KeyCode.Delete, 111, EmuKeyboard.KeyDelete), Same("End", KeyCode.End, 107, EmuKeyboard.KeyEnd),
            Same("PgDn", KeyCode.PageDown, 109, EmuKeyboard.KeyPageDown));
        Row(keys, pad, y[2], Same("7", KeyCode.Keypad7, 71, '7'), Same("8", KeyCode.Keypad8, 72, '8'), Same("9", KeyCode.Keypad9, 73, '9'),
            Same("+", KeyCode.KeypadPlus, 78, '+', 1f, 2f));

        Row(keys, 0, y[3],
            Plain("Caps", KeyCode.CapsLock, 58, 1.75f),
            Letter('a', 30), Letter('s', 31), Letter('d', 32), Letter('f', 33), Letter('g', 34), Letter('h', 35),
            Letter('j', 36), Letter('k', 37), Letter('l', 38),
            Symbol(';', ':', KeyCode.Semicolon, 39), Symbol('\'', '"', KeyCode.Quote, 40),
            Same("Enter", KeyCode.Return, 28, 13, 2.25f));
        Row(keys, pad, y[3], Same("4", KeyCode.Keypad4, 75, '4'), Same("5", KeyCode.Keypad5, 76, '5'), Same("6", KeyCode.Keypad6, 77, '6'));

        Row(keys, 0, y[4],
            Plain("Shift", KeyCode.LeftShift, 42, 2.25f),
            Letter('z', 44), Letter('x', 45), Letter('c', 46), Letter('v', 47), Letter('b', 48), Letter('n', 49), Letter('m', 50),
            Symbol(',', '<', KeyCode.Comma, 51), Symbol('.', '>', KeyCode.Period, 52), Symbol('/', '?', KeyCode.Slash, 53),
            Plain("Shift", KeyCode.RightShift, 54, 2.75f));
        Row(keys, nav + 1f, y[4], Same("Up", KeyCode.UpArrow, 103, EmuKeyboard.KeyUp));
        Row(keys, pad, y[4], Same("1", KeyCode.Keypad1, 79, '1'), Same("2", KeyCode.Keypad2, 80, '2'), Same("3", KeyCode.Keypad3, 81, '3'),
            Same("Enter", KeyCode.KeypadEnter, 96, 13, 1f, 2f));

        Row(keys, 0, y[5],
            Plain("Ctrl", KeyCode.LeftControl, 29, 1.25f), Plain("Win", KeyCode.LeftWindows, 125, 1.25f),
            Plain("Alt", KeyCode.LeftAlt, 56, 1.25f), Same("", KeyCode.Space, 57, ' ', 6.25f),
            Plain("Alt", KeyCode.RightAlt, 100, 1.25f), Plain("Win", KeyCode.RightWindows, 126, 1.25f),
            Plain("Menu", KeyCode.Menu, 127, 1.25f), Plain("Ctrl", KeyCode.RightControl, 97, 1.25f));
        Row(keys, nav, y[5], Same("Left", KeyCode.LeftArrow, 105, EmuKeyboard.KeyLeft), Same("Down", KeyCode.DownArrow, 108, EmuKeyboard.KeyDown),
            Same("Right", KeyCode.RightArrow, 106, EmuKeyboard.KeyRight));
        Row(keys, pad, y[5], Same("0", KeyCode.Keypad0, 82, '0', 2f), Same(".", KeyCode.KeypadPeriod, 83, '.'));
        return keys;
    }

    // ---------------------------------------------------------------- assets

    static T LoadOrCreate<T>(string path, System.Func<T> create) where T : Object
    {
        T asset = AssetDatabase.LoadAssetAtPath<T>(path);
        if (asset != null) return asset;
        asset = create();
        AssetDatabase.CreateAsset(asset, path);
        return asset;
    }

    static Material Mat(string name, string shader)
    {
        Shader s = Shader.Find(shader);
        if (s == null) throw new System.Exception("shader " + shader + " not found (does it compile?)");
        Material m = LoadOrCreate(Generated + "/" + name + ".mat", () => new Material(s));
        m.shader = s;
        EditorUtility.SetDirty(m);
        return m;
    }

    static Material Lit(string name, Color colour, Texture2D texture = null, float tiling = 1f, float smooth = 0.15f)
    {
        Material m = Mat(name, "Standard");
        m.color = colour;
        m.mainTexture = texture;
        m.mainTextureScale = new Vector2(tiling, tiling);
        m.SetFloat("_Glossiness", smooth);
        return m;
    }

    // Paint, plaster, cloth: no highlights and no mirror image of the room.
    static Material Matte(Material m)
    {
        m.SetFloat("_Glossiness", 0.02f);
        m.SetFloat("_SpecularHighlights", 0f);
        m.EnableKeyword("_SPECULARHIGHLIGHTS_OFF");
        m.SetFloat("_GlossyReflections", 0f);
        m.EnableKeyword("_GLOSSYREFLECTIONS_OFF");
        return m;
    }

    static Material Glow(string name, Color colour)
    {
        Material m = Mat(name, "Standard");
        m.color = Color.black;
        m.EnableKeyword("_EMISSION");
        m.SetColor("_EmissionColor", colour);
        m.globalIlluminationFlags = MaterialGlobalIlluminationFlags.BakedEmissive;   // it lights the room in the bake
        return m;
    }

    // A tiling texture computed from a function of (u, v), saved as a PNG.
    static Texture2D Pattern(string name, int size, System.Func<float, float, Color> at)
    {
        string path = Root + "/Textures/" + name + ".png";
        Texture2D tex = new Texture2D(size, size, TextureFormat.RGBA32, false);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                tex.SetPixel(x, y, at((x + 0.5f) / size, (y + 0.5f) / size));
        File.WriteAllBytes(path, tex.EncodeToPNG());
        Object.DestroyImmediate(tex);
        AssetDatabase.ImportAsset(path);
        return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
    }

    static float Noise(float u, float v, float scale)
    {
        // a few octaves; the period keeps the texture tiling
        float n = 0, weight = 0.5f;
        for (int o = 0; o < 4; o++)
        {
            float s = scale * (1 << o);
            float a = Mathf.PerlinNoise(u * s, v * s), b = Mathf.PerlinNoise((u - 1) * s, v * s);
            float c = Mathf.PerlinNoise(u * s, (v - 1) * s), d = Mathf.PerlinNoise((u - 1) * s, (v - 1) * s);
            n += weight * Mathf.Lerp(Mathf.Lerp(a, b, u), Mathf.Lerp(c, d, u), v);
            weight *= 0.5f;
        }
        return n;
    }

    // The machine's state: four 32-bit words a texel.
    static RenderTexture StateTexture(string name, int width, int height)
    {
        RenderTexture rt = LoadOrCreate(Generated + "/" + name + ".renderTexture",
            () => new RenderTexture(width, height, 0, GraphicsFormat.R32G32B32A32_UInt));
        if (rt.width != width || rt.height != height)
        {
            // an asset from before the size changed
            rt.Release();
            rt.width = width;
            rt.height = height;
        }
        rt.antiAliasing = 1;
        rt.filterMode = FilterMode.Point;
        rt.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(rt);
        return rt;
    }

    static RenderTexture Target(string name, int width, int height, int depth)
    {
        RenderTexture rt = LoadOrCreate(Generated + "/" + name + ".renderTexture",
            () => new RenderTexture(width, height, depth, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear));
        if (rt.width != width || rt.height != height)
        {
            rt.Release();
            rt.width = width;
            rt.height = height;
        }
        rt.antiAliasing = 1;
        rt.filterMode = FilterMode.Point;
        rt.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(rt);
        return rt;
    }

    static Mesh GpuMesh()
    {
        // One point per triangle the GPU can draw. A point carries nothing but its number:
        // the geometry shader makes the triangle from the guest's list.
        AssetDatabase.DeleteAsset(Generated + "/GpuMesh.asset");
        return LoadOrCreate(Generated + "/GpuPoints.asset", () =>
        {
            int[] indices = new int[GpuTriangles];
            for (int i = 0; i < GpuTriangles; i++) indices[i] = i;
            Mesh mesh = new Mesh { name = "GpuPoints", indexFormat = UnityEngine.Rendering.IndexFormat.UInt32 };
            mesh.vertices = new Vector3[GpuTriangles];
            mesh.SetIndices(indices, MeshTopology.Points, 0, false);
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one);
            return mesh;
        });
    }

    [MenuItem("ShaderEmu/Create Udon program assets")]
    public static void CreateProgramAssets()
    {
        bool created = false;
        foreach (string guid in AssetDatabase.FindAssets("t:MonoScript", new[] { Root + "/Scripts" }))
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            MonoScript script = AssetDatabase.LoadAssetAtPath<MonoScript>(path);
            System.Type type = script.GetClass();
            if (type == null || !typeof(UdonSharpBehaviour).IsAssignableFrom(type)) continue;
            if (UdonSharpEditorUtility.GetUdonSharpProgramAsset(type) != null) continue;
            UdonSharpProgramAsset program = ScriptableObject.CreateInstance<UdonSharpProgramAsset>();
            program.sourceCsScript = script;
            AssetDatabase.CreateAsset(program, Path.ChangeExtension(path, ".asset"));
            created = true;
        }
        if (created) AssetDatabase.Refresh();
        UdonSharpCompilerV1.CompileSync();
    }

    // ---------------------------------------------------------------- scene helpers

    static GameObject Box(Transform parent, string name, Vector3 centre, Vector3 size, Material material, bool collide = true)
    {
        GameObject go = GameObject.CreatePrimitive(PrimitiveType.Cube);
        go.name = name;
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        go.transform.localScale = size;
        go.GetComponent<MeshRenderer>().sharedMaterial = material;
        if (!collide) Object.DestroyImmediate(go.GetComponent<Collider>());
        go.isStatic = true;
        return go;
    }

    // A quad seen from -z, `width` by `height`, with a thin box collider.
    static GameObject Screen(Transform parent, string name, Vector3 centre, float width, float height, Material material)
    {
        GameObject go = GameObject.CreatePrimitive(PrimitiveType.Quad);
        go.name = name;
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        go.transform.localScale = new Vector3(width, height, 1f);
        Object.DestroyImmediate(go.GetComponent<Collider>());
        go.AddComponent<BoxCollider>().size = new Vector3(1f, 1f, 0.01f);
        MeshRenderer renderer = go.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        return go;
    }

    // VRChat's super-sampled UI shader: sharper in a headset than Unity's own.
    static Material UiMaterial()
    {
        return AssetDatabase.LoadAssetAtPath<Material>("Packages/com.vrchat.worlds/Editor/VRCSDK/SDK3/VRCSuperSampledUIMaterial.mat");
    }

    static Image Plate(RectTransform rect, Color colour)
    {
        Image image = rect.gameObject.AddComponent<Image>();
        image.color = colour;
        image.material = UiMaterial();
        return image;
    }

    // A world-space canvas, one unit a millimetre, that VRChat's laser can use.
    static RectTransform Panel(Transform parent, string name, Vector3 centre, Vector3 euler, float width, float height, Color back)
    {
        GameObject go = new GameObject(name, typeof(RectTransform), typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
        go.transform.SetParent(parent, false);
        go.GetComponent<Canvas>().renderMode = RenderMode.WorldSpace;
        go.GetComponent<CanvasScaler>().dynamicPixelsPerUnit = 6f;
        RectTransform rect = go.GetComponent<RectTransform>();
        rect.sizeDelta = new Vector2(width, height);
        rect.localPosition = centre;
        rect.localEulerAngles = euler;
        rect.localScale = Vector3.one * 0.001f;
        go.AddComponent<VRC.SDK3.Components.VRCUiShape>();
        go.AddComponent<BoxCollider>().size = new Vector3(width, height, 1f);
        Plate(Child(rect, "Back", 0, 0, width, height), back).raycastTarget = false;
        return rect;
    }

    // A child rectangle placed from the panel's top-left corner.
    static RectTransform Child(RectTransform parent, string name, float x, float y, float width, float height)
    {
        GameObject go = new GameObject(name, typeof(RectTransform));
        RectTransform rect = go.GetComponent<RectTransform>();
        rect.SetParent(parent, false);
        rect.anchorMin = rect.anchorMax = new Vector2(0, 1);
        rect.pivot = new Vector2(0, 1);
        rect.anchoredPosition = new Vector2(x, -y);
        rect.sizeDelta = new Vector2(width, height);
        return rect;
    }

    static TextMeshProUGUI Label(RectTransform parent, string name, string text, float x, float y, float width, float height, int size,
                                 TextAnchor anchor, Color colour)
    {
        TextMeshProUGUI label = Child(parent, name, x, y, width, height).gameObject.AddComponent<TextMeshProUGUI>();
        label.fontSize = size;
        label.text = text;
        label.alignment = anchor == TextAnchor.MiddleCenter ? TextAlignmentOptions.Center
                        : anchor == TextAnchor.UpperLeft ? TextAlignmentOptions.TopLeft : TextAlignmentOptions.Left;
        label.color = colour;
        label.raycastTarget = false;
        label.richText = false;
        label.enableWordWrapping = true;
        label.overflowMode = TextOverflowModes.Truncate;
        return label;
    }

    static readonly Color KeyColour = new Color(0.17f, 0.18f, 0.21f), KeyText = new Color(0.9f, 0.92f, 0.95f);

    static Button MakeButton(RectTransform parent, string name, string text, float x, float y, float width, float height, int size,
                             out TextMeshProUGUI label)
    {
        RectTransform rect = Child(parent, name, x, y, width, height);
        Image image = Plate(rect, KeyColour);
        Button button = rect.gameObject.AddComponent<Button>();
        button.targetGraphic = image;
        ColorBlock colours = button.colors;
        colours.normalColor = Color.white;
        colours.highlightedColor = new Color(1.6f, 1.6f, 1.7f);
        colours.pressedColor = new Color(0.4f, 0.8f, 1.6f);
        colours.selectedColor = Color.white;
        colours.colorMultiplier = 1.6f;
        button.colors = colours;
        Navigation navigation = button.navigation;
        navigation.mode = Navigation.Mode.None;
        button.navigation = navigation;
        label = Label(rect, "Text", text, 0, 0, width, height, size, TextAnchor.MiddleCenter, KeyText);
        return button;
    }

    // A slider that was touched stays selected, and Unity then moves it with the keys and
    // stick a player walks with. Sent to itself in every direction, it does not move.
    public static void DeafToWalking(Slider slider)
    {
        Navigation navigation = slider.navigation;
        navigation.mode = Navigation.Mode.Explicit;
        navigation.selectOnLeft = navigation.selectOnRight = navigation.selectOnUp = navigation.selectOnDown = slider;
        slider.navigation = navigation;
        EditorUtility.SetDirty(slider);
    }

    static void OnClick(Button button, UdonSharpBehaviour target, string method)
    {
        UdonBehaviour udon = UdonSharpEditorUtility.GetBackingUdonBehaviour(target);
        UnityEventTools.AddStringPersistentListener(button.onClick, udon.SendCustomEvent, method);
    }

    static T Udon<T>(GameObject go) where T : UdonSharpBehaviour
    {
        return go.AddUdonSharpComponent<T>();
    }

    static void Apply(UdonSharpBehaviour behaviour)
    {
        UdonSharpEditorUtility.CopyProxyToUdon(behaviour);
        EditorUtility.SetDirty(behaviour);
    }

    // A panel of key plates. It is not a VRChat canvas: the players' beams (EmuPointer) find the
    // key under them from the rectangles, so each hand can hold one.
    static EmuKeyboard Keyboard(Transform parent, string name, string title, Vector3 centre, Vector3 euler, bool raw)
    {
        const float unit = 60f, gap = 6f, top = 56f;
        List<Key> keys = Layout();
        float width = LayoutWidth * unit + gap, height = top + LayoutHeight * unit + gap;
        keys.Add(new Key { label = "Use my keyboard", linux = EmuKeyboard.LinuxCapture, normal = None, shifted = None,
                           x = (width - 276 - gap) / unit, y = (6 - top) / unit, width = 276 / unit, height = 50 / unit });
        RectTransform panel = Panel(parent, name, centre, euler, width, height, new Color(0.07f, 0.075f, 0.09f));
        Object.DestroyImmediate(panel.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(panel.GetComponent<GraphicRaycaster>());
        EmuKeyboard keyboard = Udon<EmuKeyboard>(panel.gameObject);
        keyboard.rawKeys = raw;
        keyboard.panelWidth = width;
        keyboard.panelHeight = height;

        Label(panel, "Title", title, 12, 6, 520, 44, 26, TextAnchor.MiddleLeft, new Color(0.6f, 0.75f, 0.95f));
        keyboard.modifierLabel = Label(panel, "Modifiers", "", 540, 6, 420, 44, 24, TextAnchor.MiddleCenter, new Color(1f, 0.8f, 0.3f));

        Image[] plates = new Image[keys.Count];
        float[] kx = new float[keys.Count], ky = new float[keys.Count], kw = new float[keys.Count], kh = new float[keys.Count];
        for (int i = 0; i < keys.Count; i++)
        {
            Key key = keys[i];
            kx[i] = gap + key.x * unit;
            ky[i] = top + key.y * unit;
            kw[i] = key.width * unit - gap;
            kh[i] = key.height * unit - gap;
            RectTransform rect = Child(panel, "Key " + (key.label == "" ? "Space" : key.label), kx[i], ky[i], kw[i], kh[i]);
            plates[i] = Plate(rect, KeyColour);
            plates[i].raycastTarget = false;
            TextMeshProUGUI text = Label(rect, "Text", key.label, 0, 0, kw[i], kh[i], key.label.Length > 3 ? 18 : 22, TextAnchor.MiddleCenter, KeyText);
            if (key.linux == EmuKeyboard.LinuxCapture) keyboard.captureLabel = text;
        }
        keyboard.keyX = kx;
        keyboard.keyY = ky;
        keyboard.keyW = kw;
        keyboard.keyH = kh;
        keyboard.keyLinux = keys.ConvertAll(k => k.linux).ToArray();
        keyboard.keyNormal = keys.ConvertAll(k => k.normal).ToArray();
        keyboard.keyShifted = keys.ConvertAll(k => k.shifted).ToArray();
        keyboard.keyPlate = plates;
        keyboard.plateColour = KeyColour;
        keyboard.hoverColour = new Color(0.30f, 0.33f, 0.40f);
        keyboard.pressedColour = new Color(0.15f, 0.45f, 0.95f);
        keyboard.latchedColour = new Color(0.62f, 0.40f, 0.10f);

        List<Key> host = keys.FindAll(k => k.linux != EmuKeyboard.LinuxCapture);
        keyboard.hostKeys = host.ConvertAll(k => (int)k.host).ToArray();
        keyboard.hostLinux = host.ConvertAll(k => k.linux).ToArray();
        keyboard.hostNormal = host.ConvertAll(k => k.normal).ToArray();
        keyboard.hostShifted = host.ConvertAll(k => k.shifted).ToArray();
        return keyboard;
    }

    // A hand's beam and the dot where it lands; EmuPointer moves both every frame.
    static void Beam(Transform parent, string name, Material material, out LineRenderer line, out Transform dot)
    {
        GameObject go = new GameObject(name + " beam");
        go.transform.SetParent(parent, false);
        line = go.AddComponent<LineRenderer>();
        line.useWorldSpace = true;
        line.positionCount = 2;
        line.startWidth = 0.004f;
        line.endWidth = 0.002f;
        line.sharedMaterial = material;
        line.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        line.receiveShadows = false;
        line.enabled = false;
        GameObject sphere = GameObject.CreatePrimitive(PrimitiveType.Sphere);
        sphere.name = name + " dot";
        sphere.transform.SetParent(parent, false);
        sphere.transform.localScale = Vector3.one * 0.014f;
        Object.DestroyImmediate(sphere.GetComponent<Collider>());
        MeshRenderer renderer = sphere.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        sphere.SetActive(false);
        dot = sphere.transform;
    }

    // The decoded display: 8-bit sRGB with mipmaps, made again after every frame of the machine.
    static RenderTexture PictureTexture(string name, int width, int height)
    {
        RenderTexture rt = LoadOrCreate(Generated + "/" + name + ".renderTexture",
            () => new RenderTexture(width, height, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB));
        rt.Release();
        rt.width = width;
        rt.height = height;
        rt.antiAliasing = 1;
        rt.useMipMap = true;
        rt.autoGenerateMips = true;
        rt.filterMode = FilterMode.Trilinear;
        rt.anisoLevel = 8;
        rt.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(rt);
        return rt;
    }

    // The volume display (docs/volume.md): a third screen, on the left wall by the console, that is a window
    // onto the guest's 3D frame, drawn behind it as the visitor's own eyes see it.
    static Material Volume(Transform parent, float halfW, RenderTexture state, Material frameMat)
    {
        const float wide = 1.28f, high = 0.8f;
        Transform root = new GameObject("Volume display").transform;
        root.SetParent(parent, false);
        root.localPosition = new Vector3(-halfW + 0.04f, 1.65f, 3.9f);
        root.localEulerAngles = new Vector3(0, -90f, 0);   // its face towards the room
        Box(root, "Volume bezel", new Vector3(0, 0, 0.02f), new Vector3(wide + 0.08f, high + 0.08f, 0.03f), frameMat, false);

        Material maskMat = Mat("VolumeMask", "ShaderEmu/VolumeMask");
        Material sealMat = Mat("VolumeSeal", "ShaderEmu/VolumeSeal");
        Material sceneMat = Mat("GpuVolume", "ShaderEmu/GpuVolume");
        Material darkMat = Mat("VolumeOff", "Unlit/Color");
        darkMat.color = new Color(0.01f, 0.011f, 0.014f);
        sceneMat.SetTexture("_State", state);
        sceneMat.SetVector("_ScreenSize", new Vector4(wide, high, 0, 0));
        Transform content = new GameObject("Volume content").transform;
        content.SetParent(root, false);
        System.Func<string, Material, Transform, GameObject> face = (name, material, under) =>
        {
            GameObject go = GameObject.CreatePrimitive(PrimitiveType.Quad);
            go.name = name;
            go.transform.SetParent(under, false);
            go.transform.localScale = new Vector3(wide, high, 1f);
            Object.DestroyImmediate(go.GetComponent<Collider>());
            MeshRenderer r = go.GetComponent<MeshRenderer>();
            r.sharedMaterial = material;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
            r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
            return go;
        };
        face("Volume mask", maskMat, content);
        face("Volume seal", sealMat, content);
        GameObject dark = face("Volume off", darkMat, root);
        // one point a triangle, as the GPU's own mesh; its bounds are the screen, so it is
        // drawn whenever the screen is in view
        Mesh points = LoadOrCreate(Generated + "/VolumePoints.asset", () =>
        {
            int[] indices = new int[GpuTriangles];
            for (int i = 0; i < GpuTriangles; i++) indices[i] = i;
            Mesh mesh = new Mesh { name = "VolumePoints", indexFormat = UnityEngine.Rendering.IndexFormat.UInt32 };
            mesh.vertices = new Vector3[GpuTriangles];
            mesh.SetIndices(indices, MeshTopology.Points, 0, false);
            return mesh;
        });
        points.bounds = new Bounds(Vector3.zero, new Vector3(wide, high, 0.1f));
        EditorUtility.SetDirty(points);
        GameObject scene = new GameObject("Volume scene", typeof(MeshFilter), typeof(MeshRenderer));
        scene.transform.SetParent(content, false);
        scene.GetComponent<MeshFilter>().sharedMesh = points;
        MeshRenderer sceneRenderer = scene.GetComponent<MeshRenderer>();
        sceneRenderer.sharedMaterial = sceneMat;
        sceneRenderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        sceneRenderer.receiveShadows = false;
        sceneRenderer.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
        sceneRenderer.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;

        // its switch, under the screen
        RectTransform panel = Panel(root, "Volume panel", new Vector3(0, -high / 2 - 0.15f, 0.03f), Vector3.zero, 900, 180,
                                    new Color(0.05f, 0.055f, 0.07f));
        EmuVolume volume = Udon<EmuVolume>(panel.gameObject);
        volume.content = content.gameObject;
        volume.dark = dark;
        volume.sceneMaterial = sceneMat;
        Button power = MakeButton(panel, "Power", "3D screen: on", 14, 14, 260, 76, 26, out volume.powerLabel);
        Label(panel, "Hint", "A window onto 3D programs: what glxgears or Doom draws, seen in depth from where you stand. Start one from the Start menu.",
              290, 10, 596, 84, 20, TextAnchor.MiddleLeft, new Color(0.62f, 0.68f, 0.76f));
        // where the screen lies in the program's view: its near plane, or further out
        Label(panel, "Plane title", "Screen plane: near ... far", 14, 104, 270, 60, 20, TextAnchor.MiddleLeft, Color.white);
        GameObject planeObject = DefaultControls.CreateSlider(new DefaultControls.Resources());
        RectTransform planeRect = planeObject.GetComponent<RectTransform>();
        planeRect.SetParent(panel, false);
        planeRect.anchorMin = planeRect.anchorMax = new Vector2(0, 1);
        planeRect.pivot = new Vector2(0, 1);
        planeRect.anchoredPosition = new Vector2(290, -110);
        planeRect.sizeDelta = new Vector2(596, 48);
        Slider plane = planeObject.GetComponent<Slider>();
        plane.minValue = 0;
        plane.maxValue = 1;
        plane.value = 0;
        foreach (Image part in planeObject.GetComponentsInChildren<Image>())
        {
            part.material = UiMaterial();
            part.color = part.name == "Handle" ? Color.white : part.name == "Fill" ? new Color(0.3f, 0.65f, 1f) : new Color(0.2f, 0.21f, 0.25f);
        }
        plane.handleRect.sizeDelta = new Vector2(36, 0);
        DeafToWalking(plane);
        volume.planeSlider = plane;
        Apply(volume);
        OnClick(power, volume, "Toggle");
        UnityEventTools.AddStringPersistentListener(plane.onValueChanged,
            UdonSharpEditorUtility.GetBackingUdonBehaviour(volume).SendCustomEvent, "PlaneChanged");
        return sceneMat;
    }

    // What the guest's browser may not open by itself (docs/fetch.md): a button by the display
    // while something waits, and a panel in front of the display with four addresses to copy
    // and four fields to paste them into. A page opens only on a visitor's own say-so.
    public static void WebLinks(Transform computer, EmuMachine machine, Vector3 dispAt, float dispW, float dispH)
    {
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        TextMeshProUGUI unused;
        RectTransform corner = Panel(computer, "Links button", dispAt + new Vector3(-dispW / 2 - 0.17f, -dispH / 2 + 0.12f, 0), Vector3.zero,
                                     220, 220, new Color(0.05f, 0.055f, 0.07f));
        Button open = MakeButton(corner, "Open", "Links\nneeded", 10, 10, 200, 200, 30, out unused);
        open.GetComponent<Image>().color = new Color(0.55f, 0.30f, 0.08f);
        OnClick(open, machine, "ToggleLinks");

        RectTransform panel = Panel(computer, "Links panel", dispAt + new Vector3(0, 0, -0.30f), Vector3.zero, 1700, 640,
                                    new Color(0.05f, 0.055f, 0.07f, 0.97f));
        Label(panel, "Title", "The browser asks for these. VRChat lets a world open only an address a visitor gives it:", 30, 14, 1400, 40, 26,
              TextAnchor.MiddleLeft, Color.white);
        Label(panel, "How", "copy each address on the left and paste it into the field on its right. What was asked for then loads by itself.",
              30, 54, 1400, 36, 22, TextAnchor.MiddleLeft, dim);
        Button close = MakeButton(panel, "Close", "Close", 1490, 16, 180, 70, 26, out unused);
        OnClick(close, machine, "ToggleLinks");
        string[] rows = { "The page", "Picture or style 1", "Picture or style 2", "Picture or style 3" };
        machine.wantedLinks = new InputField[4];
        machine.typedUrls = new VRC.SDK3.Components.VRCUrlInputField[4];
        for (int i = 0; i < 4; i++)
        {
            float y = 112 + i * 128;
            Label(panel, "Row " + i, rows[i], 30, y, 400, 30, 22, TextAnchor.MiddleLeft, dim);
            Label(panel, "Copy " + i, "copy", 30, y + 34, 80, 60, 20, TextAnchor.MiddleLeft, dim);
            Label(panel, "Paste " + i, "paste", 870, y + 34, 90, 60, 20, TextAnchor.MiddleLeft, dim);
            for (int side = 0; side < 2; side++)
            {
                RectTransform box = Child(panel, (side == 0 ? "Link " : "Address ") + i, side == 0 ? 110 : 960, y + 34, 710, 60);
                Plate(box, side == 0 ? new Color(0.10f, 0.11f, 0.13f) : new Color(0.14f, 0.16f, 0.22f));
                Text text = Child(box, "Text", 12, 6, 686, 48).gameObject.AddComponent<Text>();
                text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
                text.fontSize = 22;
                text.color = Color.white;
                text.alignment = TextAnchor.MiddleLeft;
                text.supportRichText = false;
                if (side == 0)
                {
                    InputField link = box.gameObject.AddComponent<InputField>();
                    link.textComponent = text;
                    link.targetGraphic = box.GetComponent<Image>();
                    machine.wantedLinks[i] = link;
                }
                else
                {
                    VRC.SDK3.Components.VRCUrlInputField address = box.gameObject.AddComponent<VRC.SDK3.Components.VRCUrlInputField>();
                    address.textComponent = text;
                    address.targetGraphic = box.GetComponent<Image>();
                    machine.typedUrls[i] = address;
                }
            }
        }
        machine.linksButton = corner.gameObject;
        machine.linksPanel = panel.gameObject;
    }

    static void NameLayer(int layer, string name)
    {
        SerializedObject tags = new SerializedObject(AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/TagManager.asset")[0]);
        SerializedProperty slot = tags.FindProperty("layers").GetArrayElementAtIndex(layer);
        if (slot.stringValue == name) return;
        slot.stringValue = name;
        tags.ApplyModifiedProperties();
    }

    // ---------------------------------------------------------------- the world

    [MenuItem("ShaderEmu/Build world")]
    public static void Build()
    {
        Directory.CreateDirectory(Generated);
        if (!Directory.Exists("Assets/TextMesh Pro")) TMP_PackageResourceImporter.ImportResources(true, false, false);
        Directory.CreateDirectory(Root + "/Textures");
        NameLayer(GpuLayer, "GpuDevice");

        if (!File.Exists(ScenePath)) AssetDatabase.CopyAsset("Assets/Scenes/VRCDefaultWorldScene.unity", ScenePath);
        UnityEngine.SceneManagement.Scene scene = EditorSceneManager.OpenScene(ScenePath, OpenSceneMode.Single);
        foreach (GameObject old in scene.GetRootGameObjects())
            if (old.name == "ShaderEmu" || old.name == "Floor") Object.DestroyImmediate(old);

        // ---- what the machine runs on
        Material machineMat = Mat("Machine", "ShaderEmu/Machine");
        Material tickMat = Mat("MachineTick", "ShaderEmu/MachineTick");
        Material gpuMat = Mat("GpuDraw", "ShaderEmu/GpuDraw");
        Material displayMat = Mat("Display", "ShaderEmu/Display");
        Material showMat = Mat("DisplayShow", "ShaderEmu/DisplayShow");
        Material heatMat = Mat("MemHeat", "ShaderEmu/MemHeat");
        Material memMat = Mat("MemView", "ShaderEmu/MemView");
        Material readbackMat = Mat("Readback", "ShaderEmu/Readback");
        Material terminalMat = Mat("Terminal", "ShaderEmu/Terminal");

        RenderTexture state = StateTexture("StateA", 2048, 4096);
        RenderTexture stateB = StateTexture("StateB", 2048, 4096);
        RenderTexture tickState = StateTexture("TickState", 64, 16);   // STATE_ROWS: what the tick draws
        AssetDatabase.DeleteAsset(Generated + "/MachineState.asset");
        RenderTexture gpuTarget = Target("GpuTarget", 1280, 720, 32);   // the picture is 720p at most
        RenderTexture readback = Target("Readback", 448, 1, 0);
        RenderTexture picture = PictureTexture("DisplayPicture", 2048, 1024);
        showMat.mainTexture = picture;
        showMat.SetVector("_TexSize", new Vector4(picture.width, picture.height, 0, 0));
        displayMat.SetFloat("_Raw", 1f);
        displayMat.SetVector("_RawSize", new Vector4(picture.width, picture.height, 0, 0));
        CustomRenderTexture heat = LoadOrCreate(Generated + "/MemHeat.asset",
            () => new CustomRenderTexture(1024, 512, RenderTextureFormat.ARGBFloat, RenderTextureReadWrite.Linear));
        heat.material = heatMat;
        heat.filterMode = FilterMode.Point;
        heat.wrapMode = TextureWrapMode.Clamp;
        heat.doubleBuffered = true;
        heat.updateMode = CustomRenderTextureUpdateMode.Realtime;
        heat.initializationMode = CustomRenderTextureUpdateMode.OnLoad;
        heat.initializationColor = Color.clear;
        EditorUtility.SetDirty(heat);

        const int cols = 80, rows = 30;
        Texture2D grid = LoadOrCreate(Generated + "/TerminalGrid.asset", () => new Texture2D(cols, rows, TextureFormat.RGBA32, false, true));
        grid.filterMode = FilterMode.Point;
        grid.wrapMode = TextureWrapMode.Clamp;
        // an answer to the guest's browser: 256 KB, four bytes a texel (docs/fetch.md)
        Texture2D hostData = LoadOrCreate(Generated + "/HostData.asset", () => new Texture2D(256, 256, TextureFormat.RGBA32, false, true));
        hostData.filterMode = FilterMode.Point;
        hostData.wrapMode = TextureWrapMode.Clamp;
        Texture2D black = LoadOrCreate(Generated + "/Black.asset", () =>
        {
            Texture2D t = new Texture2D(4, 4, TextureFormat.RGBA32, false, true);
            t.SetPixels32(new Color32[16]);
            t.Apply();
            return t;
        });

        machineMat.SetTexture("_GpuTarget", gpuTarget);
        machineMat.SetTexture("_TickState", tickState);
        gpuMat.SetTexture("_State", state);
        displayMat.SetTexture("_State", state);
        displayMat.SetTexture("_GpuTarget", gpuTarget);
        heatMat.SetTexture("_State", state);
        heatMat.SetInt("_Strips", 2);
        memMat.SetTexture("_Heat", heat);
        readbackMat.SetTexture("_State", state);
        terminalMat.SetTexture("_Grid", grid);
        terminalMat.SetTexture("_Font", AssetDatabase.LoadAssetAtPath<Texture2D>(Root + "/Textures/TerminalFont.png"));
        terminalMat.SetInt("_Cols", cols);
        terminalMat.SetInt("_Rows", rows);

        // ---- the room
        Texture2D floorTex = Pattern("Floor", 512, (u, v) =>
        {
            float tu = Mathf.Repeat(u * 2f, 1f), tv = Mathf.Repeat(v * 2f, 1f);
            bool grout = tu < 0.012f || tv < 0.012f || tu > 0.988f || tv > 0.988f;
            float n = Noise(u, v, 6f);
            bool dark = ((int)(u * 2f) + (int)(v * 2f)) % 2 == 0;
            float shade = grout ? 0.08f : (dark ? 0.26f : 0.34f) + (n - 0.5f) * 0.10f;
            return new Color(shade * 0.96f, shade, shade * 1.06f, 1f);
        });
        Texture2D wallTex = Pattern("Wall", 512, (u, v) =>
        {
            float n = Noise(u, v, 10f);
            float seam = Mathf.Abs(Mathf.Repeat(u * 4f, 1f) - 0.5f) > 0.492f ? -0.10f : 0f;
            float shade = 0.74f + (n - 0.5f) * 0.10f + seam;
            return new Color(shade * 0.97f, shade * 0.98f, shade, 1f);
        });
        Texture2D woodTex = Pattern("Wood", 512, (u, v) =>
        {
            float grain = Noise(u * 0.25f, v, 24f) + 0.25f * Mathf.Sin((u * 6f + Noise(u, v, 3f) * 2.5f) * Mathf.PI * 2f);
            float shade = 0.5f + (grain - 0.5f) * 0.35f;
            return new Color(0.46f * shade + 0.16f, 0.30f * shade + 0.09f, 0.17f * shade + 0.05f, 1f);
        });
        Material floorMat = Lit("Floor", Color.white, floorTex, 1f, 0.22f);
        floorMat.mainTextureScale = new Vector2(4.5f, 5.5f);
        Material wallMat = Matte(Lit("Wall", Color.white, wallTex));
        wallMat.mainTextureScale = new Vector2(3f, 1f);
        Material ceilingMat = Matte(Lit("Ceiling", new Color(0.82f, 0.82f, 0.80f)));
        Material woodMat = Lit("Wood", Color.white, woodTex, 1f, 0.3f);
        Material plasticMat = Lit("DarkPlastic", new Color(0.06f, 0.065f, 0.075f), null, 1f, 0.45f);
        Material metalMat = Lit("Metal", new Color(0.32f, 0.34f, 0.37f), null, 1f, 0.6f);
        metalMat.SetFloat("_Metallic", 0.8f);
        Material panelMat = Matte(Lit("WallPanel", new Color(0.36f, 0.39f, 0.44f), null, 1f, 0.12f));
        Material ledMat = Glow("Led", new Color(0.2f, 1.6f, 0.5f));
        Material lampMat = Glow("Lamp", new Color(1.5f, 1.45f, 1.3f));

        GameObject root = new GameObject("ShaderEmu");
        foreach (GameObject go in scene.GetRootGameObjects())
            if (go.name == "VRCWorld") go.transform.SetPositionAndRotation(new Vector3(0, 0, 1.6f), Quaternion.identity);   // the spawn
        Transform world = root.transform;
        const float halfW = 4.5f, halfD = 5.5f, height = 3.2f, wall = 0.2f;

        Transform room = new GameObject("Room").transform;
        room.SetParent(world, false);
        Box(room, "Floor", new Vector3(0, -0.05f, 0), new Vector3(halfW * 2, 0.1f, halfD * 2), floorMat);
        Box(room, "Ceiling", new Vector3(0, height + 0.05f, 0), new Vector3(halfW * 2, 0.1f, halfD * 2), ceilingMat);
        Box(room, "Wall front", new Vector3(0, height / 2, halfD + wall / 2), new Vector3(halfW * 2 + wall * 2, height, wall), wallMat);
        Box(room, "Wall back", new Vector3(0, height / 2, -halfD - wall / 2), new Vector3(halfW * 2 + wall * 2, height, wall), wallMat);
        Box(room, "Wall left", new Vector3(-halfW - wall / 2, height / 2, 0), new Vector3(wall, height, halfD * 2), wallMat);
        // the right wall, in four pieces round the window's opening
        float wallX = halfW + wall / 2, winZ0 = WindowZ - WindowWide / 2, winZ1 = WindowZ + WindowWide / 2;
        float winY0 = WindowY - WindowHigh / 2, winY1 = WindowY + WindowHigh / 2;
        Box(room, "Wall right low", new Vector3(wallX, winY0 / 2, 0), new Vector3(wall, winY0, halfD * 2), wallMat);
        Box(room, "Wall right high", new Vector3(wallX, (winY1 + height) / 2, 0), new Vector3(wall, height - winY1, halfD * 2), wallMat);
        Box(room, "Wall right back", new Vector3(wallX, WindowY, (winZ0 - halfD) / 2), new Vector3(wall, WindowHigh, winZ0 + halfD), wallMat);
        Box(room, "Wall right front", new Vector3(wallX, WindowY, (winZ1 + halfD) / 2), new Vector3(wall, WindowHigh, halfD - winZ1), wallMat);
        Decorate(world, room, halfW, halfD, height, woodMat, plasticMat, metalMat, ledMat, lampMat);

        // ---- the computer: a wall of screens over a desk
        Transform computer = new GameObject("Computer").transform;
        computer.SetParent(world, false);
        const float screenZ = halfD - 0.09f;
        Box(computer, "Wall panel", new Vector3(0, 1.75f, halfD - 0.03f), new Vector3(7.6f, 2.0f, 0.06f), panelMat, false);

        // console
        const float termW = 1.36f, termH = 1.02f;   // 4:3, as its 80 x 30 cells of 8 x 16 are
        Vector3 termAt = new Vector3(-2.35f, 1.80f, screenZ);
        Box(computer, "Terminal bezel", termAt + new Vector3(0, 0, 0.025f), new Vector3(termW + 0.08f, termH + 0.08f, 0.04f), plasticMat, false);
        GameObject terminalScreen = Screen(computer, "Terminal screen", termAt, termW, termH, terminalMat);

        // display
        const float dispW = 1.6f, dispH = 1.2f;     // 4:3: the desktop starts at 800 x 600
        Vector3 dispAt = new Vector3(0f, 1.82f, screenZ);
        Box(computer, "Monitor bezel", dispAt + new Vector3(0, 0, 0.025f), new Vector3(dispW + 0.08f, dispH + 0.08f, 0.04f), plasticMat, false);
        Box(computer, "Monitor neck", new Vector3(0, 1.0075f, halfD - 0.07f), new Vector3(0.12f, 0.465f, 0.05f), metalMat, false);   // ends under the bezel
        Box(computer, "Monitor foot", new Vector3(0, 0.775f, halfD - 0.22f), new Vector3(0.5f, 0.02f, 0.28f), metalMat, false);
        GameObject displayScreen = Screen(computer, "Display screen", dispAt, dispW, dispH, showMat);
        showMat.SetFloat("_Aspect", dispW / dispH);

        // memory
        const float memW = 1.7f, memH = 0.85f;
        Vector3 memAt = new Vector3(2.45f, 2.13f, screenZ);
        Box(computer, "Memory bezel", memAt + new Vector3(0, 0, 0.025f), new Vector3(memW + 0.08f, memH + 0.08f, 0.04f), plasticMat, false);
        GameObject memoryScreen = Screen(computer, "Memory screen", memAt, memW, memH, memMat);
        Object.DestroyImmediate(memoryScreen.GetComponent<Collider>());

        // desk and tower
        RBox(computer, "Desk top", new Vector3(-1.15f, 0.74f, halfD - 0.5f), new Vector3(4.7f, 0.05f, 1.0f), woodMat, 0.02f, true);
        foreach (float x in new[] { -3.4f, 1.1f })
            Box(computer, "Desk side", new Vector3(x, 0.36f, halfD - 0.5f), new Vector3(0.05f, 0.72f, 0.95f), woodMat);
        Box(computer, "Desk back", new Vector3(-1.15f, 0.45f, halfD - 0.06f), new Vector3(4.5f, 0.5f, 0.03f), woodMat, false);
        // the classroom's case (world/pc.py: 184 x 372 x 420 mm), its front where world/computer.py puts it
        Vector3 towerAt = new Vector3(1.55f, 0.186f, halfD - 0.4f - 0.04f);
        Box(computer, "Tower", towerAt, new Vector3(0.184f, 0.372f, 0.42f), plasticMat);

        // ---- the GPU device: a camera of its own, under the floor, that sees only the mesh
        Transform device = new GameObject("GPU device").transform;
        device.SetParent(world, false);
        device.localPosition = new Vector3(0, -20f, 0);
        Camera camera = new GameObject("GPU camera").AddComponent<Camera>();
        camera.transform.SetParent(device, false);
        camera.orthographic = true;
        camera.orthographicSize = 1f;
        camera.nearClipPlane = 0.1f;
        camera.farClipPlane = 4f;
        camera.cullingMask = 1 << GpuLayer;
        camera.clearFlags = CameraClearFlags.Depth;   // the picture stays until the next list
        camera.targetTexture = gpuTarget;
        camera.allowHDR = false;
        camera.allowMSAA = false;
        camera.useOcclusionCulling = false;
        camera.depth = -10;
        camera.enabled = false;   // EmuMachine renders it, once a round
        GameObject meshObject = new GameObject("GPU mesh", typeof(MeshFilter), typeof(MeshRenderer));
        meshObject.transform.SetParent(device, false);
        meshObject.transform.localPosition = new Vector3(0, 0, 2f);
        meshObject.layer = GpuLayer;
        meshObject.GetComponent<MeshFilter>().sharedMesh = GpuMesh();
        MeshRenderer meshRenderer = meshObject.GetComponent<MeshRenderer>();
        meshRenderer.sharedMaterial = gpuMat;
        meshRenderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        meshRenderer.receiveShadows = false;
        meshRenderer.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
        meshRenderer.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;

        // ---- behaviours
        EmuTerminal terminal = Udon<EmuTerminal>(terminalScreen);
        terminal.gridTexture = grid;
        terminal.screenMaterial = terminalMat;
        terminal.cols = cols;
        terminal.rows = rows;

        Vector3 tilt = new Vector3(58f, 0, 0);
        EmuKeyboard consoleKeys = Keyboard(computer, "Console keyboard", "Console keyboard", new Vector3(termAt.x, 0.95f, halfD - 0.62f), tilt, false);
        EmuKeyboard gpuKeys = Keyboard(computer, "Display keyboard", "Display keyboard (window system)", new Vector3(dispAt.x, 0.95f, halfD - 0.62f), tilt, true);
        consoleKeys.other = gpuKeys;
        gpuKeys.other = consoleKeys;
        Apply(consoleKeys);
        Apply(gpuKeys);
        Console(computer, terminal, consoleKeys);

        GameObject machineObject = new GameObject("Machine");
        machineObject.transform.SetParent(world, false);
        EmuMachine machine = Udon<EmuMachine>(machineObject);
        machine.stateA = state;
        machine.stateB = stateB;
        machine.tickState = tickState;
        machine.tickMaterial = tickMat;
        machine.machineMaterial = machineMat;
        machine.gpuMaterial = gpuMat;
        machine.volumeMaterial = Volume(computer, halfW, state, plasticMat);
        machine.gpuCamera = camera;
        machine.displayMaterial = displayMat;
        machine.displayTexture = picture;
        machine.displayShowMaterial = showMat;
        machine.heatMaterial = heatMat;
        machine.readbackMaterial = readbackMat;
        machine.readbackTexture = readback;
        machine.terminal = terminal;
        machine.consoleKeyboard = consoleKeys;
        machine.gpuKeyboard = gpuKeys;
        machine.blackTexture = black;
        machine.hostData = hostData;
        // the sites the browser's home page lists: a world can load no address it was not built with
        List<string> sites = new List<string>();
        foreach (string line in File.ReadAllLines(ShaderEmuImages.Repo + "/linux/apps/web/sites.txt"))
            if (line.Contains("\t")) sites.Add(line.Split('\t')[0]);
        machine.siteAddresses = sites.ToArray();
        machine.siteUrls = sites.ConvertAll(s => new VRC.SDKBase.VRCUrl(s)).ToArray();

        List<Texture2D> textures = new List<Texture2D>();
        ShaderEmuImages.BootImage boot = ShaderEmuImages.Images[0];
        foreach (string part in new[] { "ram", "rom", "tree" })
            for (int lane = 0; lane < 4; lane++)
                textures.Add(AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath(boot.name, part, lane)));
        if (textures[0] == null) Debug.LogError("[ShaderEmu] no boot image: run ShaderEmu/Import boot images first");
        machine.imageTextures = textures.ToArray();

        Transform beamRoot = new GameObject("Beams").transform;
        beamRoot.SetParent(world, false);
        Material beamMat = Mat("Beam", "ShaderEmu/Beam");
        beamMat.color = new Color(0.35f, 0.85f, 1f, 0.22f);
        Material beamSolidMat = Mat("BeamSolid", "ShaderEmu/Beam");
        beamSolidMat.color = new Color(0.45f, 0.9f, 1f, 1f);
        EmuPointer pointer = Udon<EmuPointer>(beamRoot.gameObject);
        pointer.machine = machine;
        pointer.screen = displayScreen.GetComponent<Collider>();
        pointer.aspect = dispW / dispH;
        pointer.keyboards = new[] { consoleKeys, gpuKeys };
        pointer.keyboardPlates = new Collider[] { consoleKeys.GetComponent<Collider>(), gpuKeys.GetComponent<Collider>() };
        pointer.beams = new LineRenderer[2];
        pointer.dots = new Transform[2];
        pointer.faintMaterial = beamMat;
        pointer.solidMaterial = beamSolidMat;
        Beam(beamRoot, "Left", beamMat, out pointer.beams[0], out pointer.dots[0]);
        Beam(beamRoot, "Right", beamMat, out pointer.beams[1], out pointer.dots[1]);
        pointer.dotRenderers = new Renderer[] { pointer.dots[0].GetComponent<Renderer>(), pointer.dots[1].GetComponent<Renderer>() };

        // ---- control panel, under the memory view
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        RectTransform panel = Panel(computer, "Control panel", new Vector3(memAt.x, 1.22f, screenZ), Vector3.zero, 1700, 760,
                                    new Color(0.05f, 0.055f, 0.07f));
        Label(panel, "Title", "ShaderEmu: a RISC-V computer in a pixel shader", 30, 16, 1640, 56, 38, TextAnchor.MiddleLeft, Color.white);
        machine.statsText = Label(panel, "Stats", "off", 30, 84, 1000, 420, 30, TextAnchor.UpperLeft, new Color(0.75f, 0.95f, 0.8f));
        Label(panel, "Help",
              "This machine runs on your own graphics card; other players have their own, and can show you theirs.\n" +
              "Each hand has a beam. On the big screen it is the mouse: trigger = left, grip = right, right stick = wheel.\n" +
              "On a keyboard a key stays down while the trigger is held; Shift, Ctrl and Alt also latch.\n" +
              "Desktop: click 'Use my keyboard' on a keyboard to type with the real one.",
              30, 580, 1000, 170, 22, TextAnchor.UpperLeft, dim);

        TextMeshProUGUI unusedText;
        float bx = 1060;
        Button power = MakeButton(panel, "Power", "Power on", bx, 84, 190, 80, 28, out machine.powerLabel);
        Button reset = MakeButton(panel, "Reset", "Reset", bx + 205, 84, 190, 80, 28, out unusedText);
        Button pause = MakeButton(panel, "Pause", "Pause", bx + 410, 84, 190, 80, 28, out machine.pauseLabel);
        Label(panel, "Speed title", "Speed", bx, 196, 600, 36, 26, TextAnchor.MiddleLeft, dim);
        machine.speedLabel = Label(panel, "Speed", "", bx, 294, 600, 40, 26, TextAnchor.MiddleLeft, Color.white);

        GameObject sliderObject = DefaultControls.CreateSlider(new DefaultControls.Resources());
        RectTransform sliderRect = sliderObject.GetComponent<RectTransform>();
        sliderRect.SetParent(panel, false);
        sliderRect.anchorMin = sliderRect.anchorMax = new Vector2(0, 1);
        sliderRect.pivot = new Vector2(0, 1);
        sliderRect.anchoredPosition = new Vector2(bx, -238);
        sliderRect.sizeDelta = new Vector2(600, 48);
        Slider slider = sliderObject.GetComponent<Slider>();
        slider.minValue = 0;
        slider.maxValue = 7;
        slider.wholeNumbers = true;
        slider.value = 4;   // 32,768 instructions a frame
        foreach (Image part in sliderObject.GetComponentsInChildren<Image>())
        {
            part.material = UiMaterial();
            part.color = part.name == "Handle" ? Color.white : part.name == "Fill" ? new Color(0.3f, 0.65f, 1f) : new Color(0.2f, 0.21f, 0.25f);
        }
        RectTransform handle = slider.handleRect;
        handle.sizeDelta = new Vector2(36, 0);
        DeafToWalking(slider);
        machine.speedSlider = slider;

        // an address of the visitor's own for the guest's browser: VRChat only loads what is
        // typed into a field of this kind
        WebLinks(computer, machine, dispAt, dispW, dispH);
        machine.fetchLabel = Label(panel, "Web status", "", 30, 520, 1000, 50, 24, TextAnchor.MiddleLeft, new Color(0.75f, 0.85f, 1f));
        Apply(machine);

        OnClick(power, machine, "Power");
        PowerSign(computer, machine);
        Gamepad(world, computer, machine, gpuKeys, pointer);
        OnClick(reset, machine, "ResetMachine");
        OnClick(pause, machine, "Pause");

        // the beams' angle, for hands and controllers that point elsewhere than the avatar's
        pointer.pitchLabel = Label(panel, "Beam angle", "", bx, 360, 600, 36, 26, TextAnchor.MiddleLeft, dim);
        Button beamUp = MakeButton(panel, "Beam up", "Beam up", bx, 404, 190, 70, 26, out unusedText);
        Button beamDown = MakeButton(panel, "Beam down", "Beam down", bx + 205, 404, 190, 70, 26, out unusedText);
        Apply(pointer);
        OnClick(beamUp, pointer, "PitchUp");
        OnClick(beamDown, pointer, "PitchDown");
        Sharing(world, computer, panel, machine, new Vector3(1.28f, 1.82f, screenZ));
        Sound(world, panel, machine);
        UnityEventTools.AddStringPersistentListener(slider.onValueChanged,
            UdonSharpEditorUtility.GetBackingUdonBehaviour(machine).SendCustomEvent, "SpeedChanged");

        Apply(terminal);

        // labels over the screens
        RectTransform signs = Panel(computer, "Signs", new Vector3(0, 2.62f, screenZ), Vector3.zero, 7000, 120, new Color(0, 0, 0, 0));
        Object.DestroyImmediate(signs.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(signs.GetComponent<BoxCollider>());
        Object.DestroyImmediate(signs.GetComponent<GraphicRaycaster>());
        Label(signs, "Console", "CONSOLE", 3500 + termAt.x * 1000 - 750, 20, 1500, 80, 54, TextAnchor.MiddleCenter, dim);
        Label(signs, "Display", "DISPLAY", 3500 + dispAt.x * 1000 - 750, 20, 1500, 80, 54, TextAnchor.MiddleCenter, dim);
        Label(signs, "Memory", "MEMORY (writes glow)", 3500 + memAt.x * 1000 - 850, 20, 1700, 80, 54, TextAnchor.MiddleCenter, dim);
        Scope(world, computer, panel, machine);   // after the signs: it renames the memory screen's
        Remodel(world);   // last: the boxes above are where Blender's models go (ShaderEmuModels.cs)
        Retro(world);     // and the panels in the manner of the room's decade (ShaderEmuRetro.cs)

        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(scene);
        EditorSceneManager.SaveScene(scene);
        Debug.Log("[ShaderEmu] world built: " + ScenePath + ". Now run ShaderEmu/Bake lighting.");
    }
}
