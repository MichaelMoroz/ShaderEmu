using System.IO;
using UdonSharp;
using UdonSharp.Compiler;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Experimental.Rendering;
using UnityEngine.UI;

// The machine alone (unity/gpuvm/README.md): the world's builder (ShaderEmuBuilder.cs and the room's files) needs Bakery,
// LTCGI, Mochie's shaders and the models, so this project has none of it. These are the few of its helpers that
// ShaderEmuGvm.cs uses, and a scene of the machine, a display and a console, the spawn in front of them.
public static partial class ShaderEmuBuilder
{
    const string Root = ShaderEmuImages.Root;
    const string Generated = Root + "/Generated";
    const string ScenePath = Root + "/GvmMachine.unity";
    const int GpuLayer = 22;
    const int TickLayer = 23, PointsLayer = 24;
    const int MachineCores = 256;
    const int GpuTriangles = 65536;

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

    static RenderTexture StateTexture(string name, int width, int height)
    {
        RenderTexture rt = LoadOrCreate(Generated + "/" + name + ".renderTexture",
            () => new RenderTexture(width, height, 0, GraphicsFormat.R32G32B32A32_UInt));
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
        rt.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(rt);
        return rt;
    }

    static Mesh Indexed(string name, int count, MeshTopology topology)
    {
        Mesh mesh = LoadOrCreate(Generated + "/" + name + ".asset", () => new Mesh { name = name });
        if (mesh.vertexCount != count)
        {
            int[] indices = new int[count];
            for (int i = 0; i < count; i++) indices[i] = i;
            mesh.Clear();
            mesh.indexFormat = UnityEngine.Rendering.IndexFormat.UInt32;
            mesh.vertices = new Vector3[count];
            mesh.SetIndices(indices, topology, 0, false);
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one);
            EditorUtility.SetDirty(mesh);
        }
        return mesh;
    }

    static Camera PassCamera(Transform device, string name, int layer, Mesh mesh, Material material)
    {
        Camera camera = new GameObject(name + " camera").AddComponent<Camera>();
        camera.transform.SetParent(device, false);
        camera.orthographic = true;
        camera.orthographicSize = 1f;
        camera.nearClipPlane = 0.1f;
        camera.farClipPlane = 4f;
        camera.cullingMask = 1 << layer;
        camera.clearFlags = CameraClearFlags.Nothing;
        camera.allowHDR = false;
        camera.allowMSAA = false;
        camera.useOcclusionCulling = false;
        camera.depth = -11;
        camera.stereoTargetEye = StereoTargetEyeMask.None;
        camera.enabled = false;   // EmuMachine renders it
        PassMesh(device, name, layer, mesh, material);
        return camera;
    }

    static MeshRenderer PassMesh(Transform device, string name, int layer, Mesh mesh, Material material)
    {
        GameObject meshObject = new GameObject(name + " mesh", typeof(MeshFilter), typeof(MeshRenderer));
        meshObject.transform.SetParent(device, false);
        meshObject.transform.localPosition = new Vector3(0, 0, 2f);
        meshObject.layer = layer;
        meshObject.GetComponent<MeshFilter>().sharedMesh = mesh;
        MeshRenderer renderer = meshObject.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        renderer.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
        renderer.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
        return renderer;
    }

    static void NameLayer(int layer, string name)
    {
        SerializedObject tags = new SerializedObject(AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/TagManager.asset")[0]);
        SerializedProperty slot = tags.FindProperty("layers").GetArrayElementAtIndex(layer);
        if (slot.stringValue == name) return;
        slot.stringValue = name;
        tags.ApplyModifiedProperties();
    }

    static void Apply(UdonSharpBehaviour behaviour)
    {
        UdonSharpEditorUtility.CopyProxyToUdon(behaviour);
        EditorUtility.SetDirty(behaviour);
    }

    static GameObject Screen(Transform parent, string name, Vector3 centre, float width, float height, Material material)
    {
        GameObject go = GameObject.CreatePrimitive(PrimitiveType.Quad);
        go.name = name;
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        go.transform.localScale = new Vector3(width, height, 1f);
        Object.DestroyImmediate(go.GetComponent<Collider>());
        MeshRenderer renderer = go.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        return go;
    }

    static Slider HiddenSlider(Transform parent, string name, int max, int value)
    {
        GameObject go = new GameObject(name, typeof(RectTransform));
        go.transform.SetParent(parent, false);
        Slider slider = go.AddComponent<Slider>();
        slider.minValue = 0;
        slider.maxValue = max;
        slider.wholeNumbers = true;
        slider.value = value;
        return slider;
    }

    // Every UdonSharpBehaviour of Scripts gets its program asset, and they are compiled.
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

    // (CreateProgramAssets first, and its compile finished: GvmAuto's "programs".)
    // The scene: the spawn (VRCWorld) faces +z, the display and the console a metre and a half ahead; the GPU device,
    // the tick's and the commit's cameras far below, on layers the player's camera does not see. Then the boot image
    // if it is not imported yet, and ShaderEmuGvm.cs's machine.
    [MenuItem("ShaderEmu/GPU-VM: make the machine's scene")]
    public static void GvmScene()
    {
        Directory.CreateDirectory(Generated);
        NameLayer(GpuLayer, "GpuDevice");
        NameLayer(TickLayer, "MachineTick");
        NameLayer(PointsLayer, "MachineCommit");
        if (AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath("gvm", "ram", 0)) == null) GvmImportImage();

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        int hidden = (1 << GpuLayer) | (1 << TickLayer) | (1 << PointsLayer);

        // the spawn and the player's camera (ClientSim takes the reference camera's settings)
        GameObject vrcWorld = (GameObject)PrefabUtility.InstantiatePrefab(
            AssetDatabase.LoadAssetAtPath<GameObject>("Packages/com.vrchat.worlds/Samples/UdonExampleScene/Prefabs/VRCWorld.prefab"));
        vrcWorld.transform.SetPositionAndRotation(Vector3.zero, Quaternion.identity);
        Camera reference = new GameObject("Reference camera").AddComponent<Camera>();
        reference.enabled = false;
        reference.cullingMask = ~hidden;
        reference.clearFlags = CameraClearFlags.SolidColor;
        reference.backgroundColor = new Color(0.05f, 0.05f, 0.06f);
        reference.nearClipPlane = 0.05f;
        var descriptor = vrcWorld.GetComponent<VRC.SDK3.Components.VRCSceneDescriptor>();
        descriptor.ReferenceCamera = reference.gameObject;
        EditorUtility.SetDirty(descriptor);
        GameObject floor = GameObject.CreatePrimitive(PrimitiveType.Plane);
        floor.name = "Floor";
        floor.transform.localScale = new Vector3(2, 1, 2);

        Material machineMat = Mat("GvmMachine", "ShaderEmu/GvmMachine");
        Material gpuMat = Mat("GvmGpuDraw", "ShaderEmu/GvmGpuDraw");
        Material displayMat = Mat("Display", "ShaderEmu/Display");
        Material showMat = Mat("DisplayShow", "ShaderEmu/DisplayShow");
        Material terminalMat = Mat("Terminal", "ShaderEmu/Terminal");
        Material nothing = Mat("Nothing", "Unlit/Color");   // the room's views (heat, volume) that this scene has not

        RenderTexture gpuTarget = Target("GpuTarget", 1280, 720, 32);
        RenderTexture readback = Target("Readback", 1032, 32, 0);
        RenderTexture picture = PictureTexture("DisplayPicture", 2048, 1024);
        showMat.mainTexture = picture;
        showMat.SetVector("_TexSize", new Vector4(picture.width, picture.height, 0, 0));
        displayMat.SetFloat("_Raw", 1f);
        displayMat.SetVector("_RawSize", new Vector4(picture.width, picture.height, 0, 0));
        displayMat.SetTexture("_GpuTarget", gpuTarget);
        machineMat.SetTexture("_GpuTarget", gpuTarget);

        const int cols = 80, rows = 30;
        Texture2D grid = LoadOrCreate(Generated + "/TerminalGrid.asset", () => new Texture2D(cols, rows, TextureFormat.RGBA32, false, true));
        grid.filterMode = FilterMode.Point;
        grid.wrapMode = TextureWrapMode.Clamp;
        Texture2D hostData = LoadOrCreate(Generated + "/HostData.asset", () => new Texture2D(256, 256, TextureFormat.RGBA32, false, true));
        hostData.filterMode = FilterMode.Point;
        hostData.wrapMode = TextureWrapMode.Clamp;
        Texture2D netData = LoadOrCreate(Generated + "/NetData.asset", () => new Texture2D(160, 4, TextureFormat.RGBA32, false, true));
        netData.filterMode = FilterMode.Point;
        netData.wrapMode = TextureWrapMode.Clamp;
        Texture2D black = LoadOrCreate(Generated + "/Black.asset", () =>
        {
            Texture2D t = new Texture2D(4, 4, TextureFormat.RGBA32, false, true);
            t.SetPixels32(new Color32[16]);
            t.Apply();
            return t;
        });
        terminalMat.SetTexture("_Grid", grid);
        terminalMat.SetTexture("_Font", AssetDatabase.LoadAssetAtPath<Texture2D>(Root + "/Textures/TerminalFont.png"));
        terminalMat.SetInt("_Cols", cols);
        terminalMat.SetInt("_Rows", rows);

        GameObject root = new GameObject("ShaderEmu");
        Transform world = root.transform;
        const float dispW = 1.6f, dispH = 1.2f, termW = 1.36f, termH = 1.02f;
        GameObject displayScreen = Screen(world, "Display screen", new Vector3(-0.1f, 1.5f, 1.6f), dispW, dispH, showMat);
        showMat.SetFloat("_Aspect", dispW / dispH);
        GameObject terminalScreen = Screen(world, "Terminal screen", new Vector3(-1.75f, 1.5f, 1.6f), termW, termH, terminalMat);
        terminalScreen.transform.localRotation = Quaternion.Euler(0, -20f, 0);

        // the GPU device: its camera draws the guest's lists into GpuTarget
        Transform device = new GameObject("GPU device").transform;
        device.SetParent(world, false);
        device.localPosition = new Vector3(0, -100f, 0);
        Camera camera = new GameObject("GPU camera").AddComponent<Camera>();
        camera.transform.SetParent(device, false);
        camera.orthographic = true;
        camera.orthographicSize = 1f;
        camera.nearClipPlane = 0.1f;
        camera.farClipPlane = 4f;
        camera.cullingMask = 1 << GpuLayer;
        camera.clearFlags = CameraClearFlags.Depth;
        camera.targetTexture = gpuTarget;
        camera.allowHDR = false;
        camera.allowMSAA = false;
        camera.useOcclusionCulling = false;
        camera.depth = -10;
        camera.stereoTargetEye = StereoTargetEyeMask.None;
        camera.enabled = false;
        PassMesh(device, "GPU", GpuLayer, Indexed("GpuPoints", GpuTriangles, MeshTopology.Points), gpuMat);
        // the tick's camera and the pass camera (ShaderEmuGvm.cs gives them their meshes); the RISC-V machine's
        // points and gather meshes, which it turns off
        Material placeholder = nothing;
        Camera tickCamera = PassCamera(device, "Tick", TickLayer, Indexed("GvmTickQuads", 6 * MachineCores, MeshTopology.Triangles), placeholder);
        Camera pointsCamera = PassCamera(device, "Points", PointsLayer, Indexed("PlaceholderPoints", 1, MeshTopology.Points), placeholder);
        MeshRenderer pointsRenderer = device.Find("Points mesh").GetComponent<MeshRenderer>();
        MeshRenderer gatherRenderer = PassMesh(device, "Gather", PointsLayer, Indexed("PlaceholderPoints", 1, MeshTopology.Points), placeholder);
        pointsRenderer.enabled = false;
        gatherRenderer.enabled = false;

        // behaviours: the console and its keyboard, a display keyboard, the machine
        EmuTerminal terminal = terminalScreen.AddUdonSharpComponent<EmuTerminal>();
        terminal.gridTexture = grid;
        terminal.screenMaterial = terminalMat;
        terminal.cols = cols;
        terminal.rows = rows;
        GameObject keys = new GameObject("Keyboards");
        keys.transform.SetParent(world, false);
        EmuKeyboard consoleKeys = new GameObject("Console keyboard").AddUdonSharpComponent<EmuKeyboard>();
        EmuKeyboard gpuKeys = new GameObject("Display keyboard").AddUdonSharpComponent<EmuKeyboard>();
        consoleKeys.transform.SetParent(keys.transform, false);
        gpuKeys.transform.SetParent(keys.transform, false);
        gpuKeys.rawKeys = true;
        consoleKeys.other = gpuKeys;
        gpuKeys.other = consoleKeys;
        terminal.keyboard = consoleKeys;
        Apply(consoleKeys);
        Apply(gpuKeys);
        Apply(terminal);

        GameObject machineObject = new GameObject("Machine");
        machineObject.transform.SetParent(world, false);
        EmuMachine machine = machineObject.AddUdonSharpComponent<EmuMachine>();
        machine.machineMaterial = machineMat;
        machine.gpuMaterial = gpuMat;
        machine.gpuCamera = camera;
        machine.tickCamera = tickCamera;
        machine.pointsCamera = pointsCamera;
        machine.pointsRenderer = pointsRenderer;
        machine.gatherRenderer = gatherRenderer;
        machine.volumeMaterial = nothing;
        machine.heatMaterial = nothing;
        machine.displayMaterial = displayMat;
        machine.displayTexture = picture;
        machine.displayShowMaterial = showMat;
        machine.readbackTexture = readback;
        machine.terminal = terminal;
        machine.consoleKeyboard = consoleKeys;
        machine.gpuKeyboard = gpuKeys;
        machine.blackTexture = black;
        machine.hostData = hostData;
        machine.netData = netData;
        machine.siteAddresses = new string[0];
        machine.siteUrls = new VRC.SDKBase.VRCUrl[0];
        machine.wantedLinks = new InputField[0];
        machine.typedUrls = new VRC.SDK3.Components.VRCUrlInputField[0];
        GameObject links = new GameObject("Links (none)");
        links.transform.SetParent(world, false);
        machine.linksButton = new GameObject("Links button");
        machine.linksPanel = new GameObject("Links panel");
        machine.linksButton.transform.SetParent(links.transform, false);
        machine.linksPanel.transform.SetParent(links.transform, false);
        // the panel's two sliders, unseen: 2,048 instructions a frame doubled per step (5: 65,536, eight passes of
        // 8,192), and a pass's instructions, 1,024 doubled per step (3: 8,192, EmuMachine's TicksPerRound)
        machine.speedSlider = HiddenSlider(world, "Speed (2048 << value a frame)", 7, 5);
        machine.tickSlider = HiddenSlider(world, "Tick (1024 << value a pass)", 3, 3);
        machine.powerOnAtStart = true;
        Apply(machine);

        // the measuring script (Measure/GvmProbe.cs): a play-mode MonoBehaviour, not a part of a VRChat world
        System.Type probe = System.Type.GetType("GvmProbe, Assembly-CSharp");
        if (probe != null) machineObject.AddComponent(probe);

        EditorSceneManager.SaveScene(scene, ScenePath);
        GvmBuild();
        machine.hostWants = 0;   // the console's shell alone, as the RISC-V scene and the harness's --no-desktop: nx is typed
        machine.powerOnAtStart = true;   // (GvmBuild leaves the world's machine off until Power: this scene measures from play)
        Apply(machine);
        EditorSceneManager.SaveScene(scene, ScenePath);
        AssetDatabase.SaveAssets();
        Debug.Log("[GVM] the machine's scene is " + ScenePath);
    }
}
