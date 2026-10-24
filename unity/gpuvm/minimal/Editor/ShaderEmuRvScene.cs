using System.IO;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The RISC-V machine beside gpu_vm's (unity/gpuvm/README.md, "RISC-V beside it"): the world's EmuMachine as EmuRvMachine,
// its shaders as ShaderEmuRv/... (ShadersRv), what the world's builder makes for it (ShaderEmuBuilder.Build and
// MachineCameras), and the no-MMU image of ShaderEmuImages (build/images/nommu), in a scene of its own laid out as the
// gpu_vm one, with its own Generated assets (Rv...).
public static partial class ShaderEmuBuilder
{
    const string RvScenePath = Root + "/RvMachine.unity";
    const int FieldPoints = 2 * 3968;   // the lanes' field's buckets (FIELD_MAX in lean/tick.hlsl)

    [MenuItem("ShaderEmu/RISC-V: make the machine's scene")]
    public static void RvScene()
    {
        Directory.CreateDirectory(Generated);
        NameLayer(GpuLayer, "GpuDevice");
        NameLayer(TickLayer, "MachineTick");
        NameLayer(PointsLayer, "MachineCommit");
        if (AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath("linux", "ram", 0)) == null) ShaderEmuImages.ImportImages();

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        int hidden = (1 << GpuLayer) | (1 << TickLayer) | (1 << PointsLayer);
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

        // what the world's Build makes for the machine
        Material machineMat = Mat("RvMachine", "ShaderEmuRv/Machine");
        Material tickMat = Mat("RvMachineTick", "ShaderEmuRv/MachineTickLean");
        Material pointsMat = Mat("RvCommitPoints", "ShaderEmuRv/MachineCommitPoints");
        Material gatherMat = Mat("RvGather", "ShaderEmuRv/MachineGather");
        Material gpuMat = Mat("RvGpuDraw", "ShaderEmuRv/GpuDraw");
        Material displayMat = Mat("RvDisplay", "ShaderEmuRv/Display");
        Material readbackMat = Mat("RvReadback", "ShaderEmuRv/Readback");
        Material showMat = Mat("RvDisplayShow", "ShaderEmu/DisplayShow");
        Material terminalMat = Mat("RvTerminal", "ShaderEmu/Terminal");
        Material nothing = Mat("Nothing", "Unlit/Color");

        RenderTexture state = StateTexture("RvStateA", 2048, 9280);
        RenderTexture tickState = StateTexture("RvTickState", 2048, 64);
        RenderTexture stage = StateTexture("RvStage", 2048, 21);      // STAGE_ROWS (src/ram.cginc)
        RenderTexture window = StateTexture("RvWindow", 2048, 11);    // WINDOW_ROWS (MachineWindow.cginc)
        RenderTexture[] tickTargets = new RenderTexture[8];
        for (int i = 0; i < 8; i++) tickTargets[i] = StateTexture("RvTickOut" + i, 1040, 32);
        RenderTexture tickDepth = Target("RvTickDepth", 1040, 32, 16);
        RenderTexture gpuTarget = Target("RvGpuTarget", 1280, 720, 32);
        RenderTexture readback = Target("RvReadback", 1032, 32, 0);
        RenderTexture picture = PictureTexture("RvDisplayPicture", 2048, 1024);
        showMat.mainTexture = picture;
        showMat.SetVector("_TexSize", new Vector4(picture.width, picture.height, 0, 0));
        displayMat.SetFloat("_Raw", 1f);
        displayMat.SetVector("_RawSize", new Vector4(picture.width, picture.height, 0, 0));
        machineMat.SetTexture("_GpuTarget", gpuTarget);
        machineMat.SetTexture("_TickState", tickState);
        machineMat.SetTexture("_Stage", stage);
        machineMat.SetTexture("_Window", window);
        gpuMat.SetTexture("_State", state);
        displayMat.SetTexture("_State", state);
        displayMat.SetTexture("_GpuTarget", gpuTarget);
        readbackMat.SetTexture("_State", state);
        pointsMat.SetTexture("_TickState", tickState);
        pointsMat.SetTexture("_Stage", stage);
        pointsMat.SetTexture("_GpuTarget", gpuTarget);
        gatherMat.SetTexture("_Ram", state);
        gatherMat.SetTexture("_TickState", tickState);
        gatherMat.SetTexture("_GpuTarget", gpuTarget);

        const int cols = 80, rows = 30;
        Texture2D grid = LoadOrCreate(Generated + "/RvTerminalGrid.asset", () => new Texture2D(cols, rows, TextureFormat.RGBA32, false, true));
        grid.filterMode = FilterMode.Point;
        grid.wrapMode = TextureWrapMode.Clamp;
        Texture2D hostData = LoadOrCreate(Generated + "/HostData.asset", () => new Texture2D(256, 256, TextureFormat.RGBA32, false, true));
        Texture2D netData = LoadOrCreate(Generated + "/NetData.asset", () => new Texture2D(160, 4, TextureFormat.RGBA32, false, true));
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
        Screen(world, "Display screen", new Vector3(-0.1f, 1.5f, 1.6f), dispW, dispH, showMat);
        showMat.SetFloat("_Aspect", dispW / dispH);
        GameObject terminalScreen = Screen(world, "Terminal screen", new Vector3(-1.75f, 1.5f, 1.6f), termW, termH, terminalMat);
        terminalScreen.transform.localRotation = Quaternion.Euler(0, -20f, 0);

        // the GPU device, the tick's camera (a triangle for every sixteen cores and one for the lanes' field) and the
        // points camera (the commit's points, the gather's)
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
        int groups = (MachineCores + 15) / 16 + 1;
        Camera tickCamera = PassCamera(device, "Tick", TickLayer, Indexed("TickTriangle", 3 * groups, MeshTopology.Triangles), tickMat);
        Camera pointsCamera = PassCamera(device, "Points", PointsLayer,
                                         Indexed("CommitPoints", 1 + 258 + (MachineCores - 1) * 130 + FieldPoints + 1, MeshTopology.Points), pointsMat);
        MeshRenderer pointsRenderer = device.Find("Points mesh").GetComponent<MeshRenderer>();
        MeshRenderer gatherRenderer = PassMesh(device, "Gather", PointsLayer, Indexed("GatherPoints", 1 + MachineCores, MeshTopology.Points), gatherMat);
        gatherRenderer.enabled = false;

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
        EmuRvMachine machine = machineObject.AddUdonSharpComponent<EmuRvMachine>();
        machine.state = state;
        machine.tickState = tickState;
        machine.stage = stage;
        machine.window = window;
        machine.tickTargets = tickTargets;
        machine.tickDepth = tickDepth;
        machine.tickMaterial = tickMat;
        machine.tickCamera = tickCamera;
        machine.pointsCamera = pointsCamera;
        machine.pointsMaterial = pointsMat;
        machine.pointsRenderer = pointsRenderer;
        machine.gatherMaterial = gatherMat;
        machine.gatherRenderer = gatherRenderer;
        machine.machineMaterial = machineMat;
        machine.gpuMaterial = gpuMat;
        machine.gpuCamera = camera;
        machine.volumeMaterial = nothing;
        machine.heatMaterial = nothing;
        machine.displayMaterial = displayMat;
        machine.displayTexture = picture;
        machine.displayShowMaterial = showMat;
        machine.readbackMaterial = readbackMat;
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
        machine.speedSlider = HiddenSlider(world, "Speed (2048 << value a frame)", 7, 5);
        machine.tickSlider = HiddenSlider(world, "Tick (1024 << value a pass)", 3, 3);
        machine.powerOnAtStart = true;
        machine.hostWants = 0;   // the console's shell alone, as the harness's --no-desktop: nx is typed
        Texture2D[] textures = new Texture2D[12];
        string[] parts = { "ram", "rom", "tree" };
        for (int i = 0; i < 12; i++) textures[i] = AssetDatabase.LoadAssetAtPath<Texture2D>(ShaderEmuImages.LanePath("linux", parts[i / 4], i % 4));
        machine.imageTextures = textures;
        Apply(machine);

        System.Type probe = System.Type.GetType("GvmProbe, Assembly-CSharp");
        if (probe != null) machineObject.AddComponent(probe);
        EditorSceneManager.SaveScene(scene, RvScenePath);
        AssetDatabase.SaveAssets();
        Debug.Log("[RV] the machine's scene is " + RvScenePath + " (ROM " + (textures[4] != null ? textures[4].height + " rows" : "missing") + ")");
    }
}
