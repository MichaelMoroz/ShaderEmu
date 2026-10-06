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
        public float width;
    }

    static Key K(string label, KeyCode host, int linux, int normal, int shifted, float width = 1f)
    {
        return new Key { label = label, host = host, linux = linux, normal = normal, shifted = shifted, width = width };
    }

    static Key Letter(char c, int linux)
    {
        return K(char.ToUpper(c).ToString(), KeyCode.A + (c - 'a'), linux, c, char.ToUpper(c));
    }

    static Key Symbol(char normal, char shifted, KeyCode host, int linux)
    {
        return K(shifted + " " + normal, host, linux, normal, shifted);
    }

    const int None = EmuKeyboard.KeyNone;

    static Key[][] Layout()
    {
        return new[]
        {
            new[]
            {
                K("Esc", KeyCode.Escape, 1, 27, 27), Symbol('`', '~', KeyCode.BackQuote, 41),
                Symbol('1', '!', KeyCode.Alpha1, 2), Symbol('2', '@', KeyCode.Alpha2, 3), Symbol('3', '#', KeyCode.Alpha3, 4),
                Symbol('4', '$', KeyCode.Alpha4, 5), Symbol('5', '%', KeyCode.Alpha5, 6), Symbol('6', '^', KeyCode.Alpha6, 7),
                Symbol('7', '&', KeyCode.Alpha7, 8), Symbol('8', '*', KeyCode.Alpha8, 9), Symbol('9', '(', KeyCode.Alpha9, 10),
                Symbol('0', ')', KeyCode.Alpha0, 11), Symbol('-', '_', KeyCode.Minus, 12), Symbol('=', '+', KeyCode.Equals, 13),
                K("Bksp", KeyCode.Backspace, 14, 127, 127),
            },
            new[]
            {
                K("Tab", KeyCode.Tab, 15, 9, 9, 1.5f),
                Letter('q', 16), Letter('w', 17), Letter('e', 18), Letter('r', 19), Letter('t', 20), Letter('y', 21),
                Letter('u', 22), Letter('i', 23), Letter('o', 24), Letter('p', 25),
                Symbol('[', '{', KeyCode.LeftBracket, 26), Symbol(']', '}', KeyCode.RightBracket, 27),
                K("| \\", KeyCode.Backslash, 43, '\\', '|', 1.5f),
            },
            new[]
            {
                K("Caps", KeyCode.CapsLock, 58, None, None, 1.75f),
                Letter('a', 30), Letter('s', 31), Letter('d', 32), Letter('f', 33), Letter('g', 34), Letter('h', 35),
                Letter('j', 36), Letter('k', 37), Letter('l', 38),
                Symbol(';', ':', KeyCode.Semicolon, 39), Symbol('\'', '"', KeyCode.Quote, 40),
                K("Enter", KeyCode.Return, 28, 13, 13, 2.25f),
            },
            new[]
            {
                K("Shift", KeyCode.LeftShift, 42, None, None, 2f),
                Letter('z', 44), Letter('x', 45), Letter('c', 46), Letter('v', 47), Letter('b', 48), Letter('n', 49), Letter('m', 50),
                Symbol(',', '<', KeyCode.Comma, 51), Symbol('.', '>', KeyCode.Period, 52), Symbol('/', '?', KeyCode.Slash, 53),
                K("Home", KeyCode.Home, 102, EmuKeyboard.KeyHome, EmuKeyboard.KeyHome),
                K("Up", KeyCode.UpArrow, 103, EmuKeyboard.KeyUp, EmuKeyboard.KeyUp),
                K("End", KeyCode.End, 107, EmuKeyboard.KeyEnd, EmuKeyboard.KeyEnd),
            },
            new[]
            {
                K("Ctrl", KeyCode.LeftControl, 29, None, None, 1.5f), K("Alt", KeyCode.LeftAlt, 56, None, None, 1.5f),
                K("", KeyCode.Space, 57, ' ', ' ', 7f),
                K("Del", KeyCode.Delete, 111, EmuKeyboard.KeyDelete, EmuKeyboard.KeyDelete),
                K("PgUp", KeyCode.PageUp, 104, EmuKeyboard.KeyPageUp, EmuKeyboard.KeyPageUp),
                K("Left", KeyCode.LeftArrow, 105, EmuKeyboard.KeyLeft, EmuKeyboard.KeyLeft),
                K("Down", KeyCode.DownArrow, 108, EmuKeyboard.KeyDown, EmuKeyboard.KeyDown),
                K("Right", KeyCode.RightArrow, 106, EmuKeyboard.KeyRight, EmuKeyboard.KeyRight),
            },
        };
    }

    // Keys of a real keyboard that have no on-screen key.
    static Key[] HostOnly()
    {
        List<Key> keys = new List<Key>
        {
            K("", KeyCode.RightShift, 54, None, None), K("", KeyCode.RightControl, 97, None, None),
            K("", KeyCode.PageDown, 109, EmuKeyboard.KeyPageDown, EmuKeyboard.KeyPageDown),
            K("", KeyCode.KeypadEnter, 28, 13, 13),
        };
        int[] function = { 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88 };
        for (int i = 0; i < 12; i++) keys.Add(K("", KeyCode.F1 + i, function[i], None, None));
        return keys.ToArray();
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

    static EmuKeyboard Keyboard(Transform parent, string name, string title, Vector3 centre, Vector3 euler, bool raw)
    {
        const float unit = 60f, gap = 6f, top = 56f;
        Key[][] layout = Layout();
        RectTransform panel = Panel(parent, name, centre, euler, 15 * unit + gap, top + layout.Length * unit + gap,
                                    new Color(0.07f, 0.075f, 0.09f));
        EmuKeyboard keyboard = Udon<EmuKeyboard>(panel.gameObject);
        keyboard.rawKeys = raw;

        Label(panel, "Title", title, 12, 6, 420, 44, 26, TextAnchor.MiddleLeft, new Color(0.6f, 0.75f, 0.95f));
        keyboard.modifierLabel = Label(panel, "Modifiers", "", 430, 6, 200, 44, 24, TextAnchor.MiddleCenter, new Color(1f, 0.8f, 0.3f));
        TextMeshProUGUI captureText;
        Button capture = MakeButton(panel, "Capture", "Use my keyboard", 15 * unit + gap - 276, 6, 270, 44, 22, out captureText);
        keyboard.captureLabel = captureText;
        OnClick(capture, keyboard, "ToggleCapture");

        List<Key> host = new List<Key>();
        for (int r = 0; r < layout.Length; r++)
        {
            float x = gap;
            foreach (Key key in layout[r])
            {
                TextMeshProUGUI unused;
                Button button = MakeButton(panel, "Key " + (key.label == "" ? "Space" : key.label), key.label,
                                           x, top + r * unit, key.width * unit - gap, unit - gap, key.label.Length > 3 ? 18 : 22, out unused);
                EmuKey behaviour = Udon<EmuKey>(button.gameObject);
                behaviour.keyboard = keyboard;
                behaviour.linuxCode = key.linux;
                behaviour.normal = key.normal;
                behaviour.shifted = key.shifted;
                Apply(behaviour);
                OnClick(button, behaviour, "Press");
                x += key.width * unit;
                host.Add(key);
            }
        }
        host.AddRange(HostOnly());
        keyboard.hostKeys = host.ConvertAll(k => (int)k.host).ToArray();
        keyboard.hostLinux = host.ConvertAll(k => k.linux).ToArray();
        keyboard.hostNormal = host.ConvertAll(k => k.normal).ToArray();
        keyboard.hostShifted = host.ConvertAll(k => k.shifted).ToArray();
        return keyboard;
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
        Material heatMat = Mat("MemHeat", "ShaderEmu/MemHeat");
        Material memMat = Mat("MemView", "ShaderEmu/MemView");
        Material readbackMat = Mat("Readback", "ShaderEmu/Readback");
        Material terminalMat = Mat("Terminal", "ShaderEmu/Terminal");

        RenderTexture state = StateTexture("StateA", 2048, 4096);
        RenderTexture stateB = StateTexture("StateB", 2048, 4096);
        RenderTexture tickState = StateTexture("TickState", 64, 64);
        AssetDatabase.DeleteAsset(Generated + "/MachineState.asset");
        RenderTexture gpuTarget = Target("GpuTarget", 1280, 720, 32);   // the picture is 720p at most
        RenderTexture readback = Target("Readback", 320, 1, 0);
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
        Box(room, "Wall right", new Vector3(halfW + wall / 2, height / 2, 0), new Vector3(wall, height, halfD * 2), wallMat);
        Decorate(world, room, halfW, halfD, height, woodMat, plasticMat, metalMat, ledMat, lampMat);

        // ---- the computer: a wall of screens over a desk
        Transform computer = new GameObject("Computer").transform;
        computer.SetParent(world, false);
        const float screenZ = halfD - 0.09f;
        Box(computer, "Wall panel", new Vector3(0, 1.75f, halfD - 0.03f), new Vector3(7.6f, 2.0f, 0.06f), panelMat, false);

        // console
        const float termW = 1.5f, termH = 0.9375f;
        Vector3 termAt = new Vector3(-2.35f, 1.80f, screenZ);
        Box(computer, "Terminal bezel", termAt + new Vector3(0, 0, 0.025f), new Vector3(termW + 0.08f, termH + 0.08f, 0.04f), plasticMat, false);
        GameObject terminalScreen = Screen(computer, "Terminal screen", termAt, termW, termH, terminalMat);

        // display
        const float dispW = 1.92f, dispH = 1.08f;
        Vector3 dispAt = new Vector3(0f, 1.82f, screenZ);
        Box(computer, "Monitor bezel", dispAt + new Vector3(0, 0, 0.025f), new Vector3(dispW + 0.08f, dispH + 0.08f, 0.04f), plasticMat, false);
        Box(computer, "Monitor neck", new Vector3(0, 1.0075f, halfD - 0.07f), new Vector3(0.12f, 0.465f, 0.05f), metalMat, false);   // ends under the bezel
        Box(computer, "Monitor foot", new Vector3(0, 0.775f, halfD - 0.22f), new Vector3(0.5f, 0.02f, 0.28f), metalMat, false);
        GameObject displayScreen = Screen(computer, "Display screen", dispAt, dispW, dispH, displayMat);
        displayMat.SetFloat("_Aspect", dispW / dispH);

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
        Vector3 towerAt = new Vector3(1.55f, 0.26f, halfD - 0.4f);
        Box(computer, "Tower", towerAt, new Vector3(0.24f, 0.52f, 0.52f), plasticMat);
        Box(computer, "Tower front", towerAt + new Vector3(0, 0, -0.262f), new Vector3(0.2f, 0.46f, 0.006f), metalMat, false);
        Box(computer, "Tower led", towerAt + new Vector3(0.06f, 0.18f, -0.267f), new Vector3(0.03f, 0.012f, 0.004f), ledMat, false);
        Box(computer, "Tower vents", towerAt + new Vector3(0, -0.08f, -0.267f), new Vector3(0.14f, 0.16f, 0.004f), plasticMat, false);

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

        GameObject machineObject = new GameObject("Machine");
        machineObject.transform.SetParent(world, false);
        EmuMachine machine = Udon<EmuMachine>(machineObject);
        machine.stateA = state;
        machine.stateB = stateB;
        machine.tickState = tickState;
        machine.tickMaterial = tickMat;
        machine.machineMaterial = machineMat;
        machine.gpuMaterial = gpuMat;
        machine.gpuCamera = camera;
        machine.displayMaterial = displayMat;
        machine.heatMaterial = heatMat;
        machine.readbackMaterial = readbackMat;
        machine.readbackTexture = readback;
        machine.terminal = terminal;
        machine.consoleKeyboard = consoleKeys;
        machine.gpuKeyboard = gpuKeys;
        machine.blackTexture = black;

        List<Texture2D> textures = new List<Texture2D>();
        ShaderEmuImages.BootImage boot = ShaderEmuImages.Images[0];
        foreach (string part in new[] { "ram", "rom", "tree" })
            for (int lane = 0; lane < 4; lane++)
                textures.Add(AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath(boot.name, part, lane)));
        if (textures[0] == null) Debug.LogError("[ShaderEmu] no boot image: run ShaderEmu/Import boot images first");
        machine.imageTextures = textures.ToArray();

        EmuPointer pointer = Udon<EmuPointer>(displayScreen);
        pointer.machine = machine;
        pointer.screen = displayScreen.GetComponent<Collider>();
        pointer.aspect = dispW / dispH;
        Apply(pointer);

        // ---- control panel, under the memory view
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        RectTransform panel = Panel(computer, "Control panel", new Vector3(memAt.x, 1.22f, screenZ), Vector3.zero, 1700, 760,
                                    new Color(0.05f, 0.055f, 0.07f));
        Label(panel, "Title", "ShaderEmu: a RISC-V computer in a pixel shader", 30, 16, 1640, 56, 38, TextAnchor.MiddleLeft, Color.white);
        machine.statsText = Label(panel, "Stats", "off", 30, 84, 1000, 420, 30, TextAnchor.UpperLeft, new Color(0.75f, 0.95f, 0.8f));
        Label(panel, "Help",
              "This machine runs on your own graphics card; other players have their own.\n" +
              "Point at the big screen for the mouse: trigger = left button, grip = right.\n" +
              "Desktop: press 'Use my keyboard' on a keyboard to type with the real one.",
              30, 600, 1640, 150, 26, TextAnchor.UpperLeft, dim);

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
        machine.speedSlider = slider;
        Apply(machine);

        OnClick(power, machine, "Power");
        OnClick(reset, machine, "ResetMachine");
        OnClick(pause, machine, "Pause");
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

        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(scene);
        EditorSceneManager.SaveScene(scene);
        Debug.Log("[ShaderEmu] world built: " + ScenePath + ". Now run ShaderEmu/Bake lighting.");
    }
}
