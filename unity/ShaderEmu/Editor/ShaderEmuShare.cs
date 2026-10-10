using TMPro;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// Players see each other's machines (docs/share.md): the textures the stream is packed and
// unpacked in, the hub, the object VRChat copies for every player, and the panel's buttons.
public static partial class ShaderEmuBuilder
{
    // A capture of the display at the stream's size; the half and quarter size blocks are cut
    // from its mipmaps.
    static RenderTexture MipTarget(string name, int width, int height)
    {
        RenderTexture rt = LoadOrCreate(Generated + "/" + name + ".renderTexture",
            () => new RenderTexture(width, height, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear));
        rt.Release();
        rt.width = width;
        rt.height = height;
        rt.antiAliasing = 1;
        rt.useMipMap = true;
        rt.autoGenerateMips = true;
        rt.filterMode = FilterMode.Point;
        rt.wrapMode = TextureWrapMode.Clamp;
        EditorUtility.SetDirty(rt);
        return rt;
    }

    // What arrives, for every screen that shows a player: a slot a sender (EmuStreams).
    static EmuStreams MakeStreams(Transform world, EmuMachine machine)
    {
        if (world.Find("Streams") != null) Object.DestroyImmediate(world.Find("Streams").gameObject);
        const int tiles = 300, rowTexels = 128, rowCount = tiles * 5;   // a row a tile, then a row a quarter of a tile at the fine level
        EmuStreams streams = Udon<EmuStreams>(new GameObject("Streams"));
        streams.transform.SetParent(world, false);
        streams.terminal = machine.terminal;
        streams.decodeMaterial = Mat("ShareDecode", "ShaderEmu/ShareDecode");
        Material consoleMat = Mat("StreamConsole", "ShaderEmu/Terminal");
        consoleMat.CopyPropertiesFromMaterial(AssetDatabase.LoadAssetAtPath<Material>(Generated + "/Terminal.mat"));   // the font, the columns and rows
        streams.consoleMaterial = consoleMat;
        streams.blackTexture = machine.blackTexture;
        const int slots = 12;   // as EmuShare's `ask`
        streams.stores = new Texture2D[slots];
        streams.pictures = new RenderTexture[slots];
        streams.grids = new Texture2D[slots];
        streams.consoles = new RenderTexture[slots];
        for (int s = 0; s < slots; s++)
        {
            Texture2D store = LoadOrCreate(Generated + "/StreamStore" + s + ".asset", () => new Texture2D(rowTexels, rowCount, TextureFormat.RGBA32, false, true));
            store.filterMode = FilterMode.Point;
            store.wrapMode = TextureWrapMode.Clamp;
            streams.stores[s] = store;
            streams.pictures[s] = PictureTexture("StreamPicture" + s, 1280, 960);
            int cols = machine.terminal.cols, rows = machine.terminal.rows;
            Texture2D grid = LoadOrCreate(Generated + "/StreamGrid" + s + ".asset", () => new Texture2D(cols, rows, TextureFormat.RGBA32, false, true));
            grid.filterMode = FilterMode.Point;
            grid.wrapMode = TextureWrapMode.Clamp;
            streams.grids[s] = grid;
            streams.consoles[s] = PictureTexture("StreamConsole" + s, 640, 480);
        }
        return streams;
    }

    // `at`: the middle of the players' panel, between the display and the memory view.
    static void Sharing(Transform world, Transform computer, RectTransform panel, EmuMachine machine, Vector3 at)
    {
        if (computer.Find("Players panel") != null) Object.DestroyImmediate(computer.Find("Players panel").gameObject);
        foreach (string old in new[] { "Share", "Share player", "Streams" })
            if (world.Find(old) != null) Object.DestroyImmediate(world.Find(old).gameObject);
        for (int i = panel.childCount - 1; i >= 0; i--)
            if (panel.GetChild(i).name.StartsWith("Share ")) Object.DestroyImmediate(panel.GetChild(i).gameObject);

        const int tiles = 300, rowTexels = 128, rowCount = tiles * 5;
        EmuStreams streams = MakeStreams(world, machine);

        EmuShareHub hub = Udon<EmuShareHub>(new GameObject("Share"));
        hub.transform.SetParent(world, false);
        hub.machine = machine;
        hub.terminal = machine.terminal;
        hub.consoleKeyboard = machine.consoleKeyboard;
        hub.gpuKeyboard = machine.gpuKeyboard;
        hub.encodeMaterial = Mat("ShareEncode", "ShaderEmu/ShareEncode");
        hub.captureA = MipTarget("ShareCaptureA", 640, 480);
        hub.captureB = MipTarget("ShareCaptureB", 640, 480);
        hub.atlas = Target("ShareAtlas", rowTexels, rowCount, 0);
        // the fine level: the display's own pixels, which tiles changed, and its blocks
        hub.fineA = Target("ShareFineA", 1280, 960, 0);
        hub.fineB = Target("ShareFineB", 1280, 960, 0);
        hub.changed = Target("ShareChanged", 20, 15, 0);
        hub.fineBlocks = Target("ShareFineBlocks", 640, 240, 0);
        hub.streams = streams;
        streams.hub = hub;
        Apply(streams);
        hub.displayShowMaterial = machine.displayShowMaterial;

        Color dim = new Color(0.62f, 0.68f, 0.76f);
        const float wide = 500, rowWide = 470, left = 15;
        RectTransform list = Panel(computer, "Players panel", at, Vector3.zero, wide, 1000, new Color(0.05f, 0.055f, 0.07f));
        Label(list, "Title", "Whose computer is on these screens?", left, 8, rowWide, 66, 26, TextAnchor.MiddleLeft, Color.white);
        hub.watchLabel = Label(list, "Showing", "", left, 76, rowWide, 84, 20, TextAnchor.UpperLeft, new Color(0.75f, 0.95f, 0.8f));
        string[] methods = { "PickAutomatic", "PickMine", "Pick0", "Pick1", "Pick2", "Pick3", "Pick4", "Pick5", "Pick6", "Pick7" };
        string[] texts = { "Automatic: the first that is on", "My own" };
        hub.rowObjects = new GameObject[10];
        hub.rowPlates = new Image[10];
        hub.rowLabels = new TextMeshProUGUI[10];
        Button[] rowButtons = new Button[10];
        for (int i = 0; i < 10; i++)
        {
            float y = i < 2 ? 166 + i * 68 : 318 + (i - 2) * 66;
            rowButtons[i] = MakeButton(list, "Row " + i, i < 2 ? texts[i] : "", left, y, rowWide, 60, 24, out hub.rowLabels[i]);
            hub.rowObjects[i] = rowButtons[i].gameObject;
            hub.rowPlates[i] = rowButtons[i].GetComponent<Image>();
            if (i >= 2) rowButtons[i].gameObject.SetActive(false);   // until a player is there
        }
        Label(list, "Others", "Other players:", left, 300 - 2, rowWide, 20, 18, TextAnchor.MiddleLeft, dim);
        TextMeshProUGUI unused;
        Button more = MakeButton(list, "More", "More players", left, 848, rowWide, 46, 22, out unused);
        more.gameObject.SetActive(false);
        hub.moreButton = more.gameObject;
        Button allow = MakeButton(list, "Allow", "Others may use my computer: no", left, 904, rowWide, 60, 22, out hub.allowLabel);
        Label(list, "Hint", "Whoever picks you sees your console and display.", left, 966, rowWide, 28, 17, TextAnchor.MiddleLeft, dim);
        hub.rowColour = KeyColour;
        hub.pickedColour = new Color(0.15f, 0.45f, 0.95f);
        Apply(hub);
        for (int i = 0; i < 10; i++) OnClick(rowButtons[i], hub, methods[i]);
        OnClick(more, hub, "More");
        OnClick(allow, hub, "ToggleAllow");

        GameObject template = new GameObject("Share player");
        template.transform.SetParent(world, false);
        template.AddComponent<VRC.SDK3.Components.VRCPlayerObject>();
        EmuShare share = Udon<EmuShare>(template);
        share.hub = hub;
        Apply(share);
    }

    // Into the scene as it is, without building the world again (which would need a bake).
    [MenuItem("ShaderEmu/Add sharing to the open scene")]
    public static void AddSharing()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panel = GameObject.Find("Control panel");
        if (machine == null || panel == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        Transform computer = GameObject.Find("Computer").transform;
        CreateProgramAssets();
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(machine);
        Sharing(machine.transform.parent, computer, panel.GetComponent<RectTransform>(), machine,
                new Vector3(1.28f, 1.82f, panel.transform.localPosition.z));
        foreach (TextMeshProUGUI label in panel.GetComponentsInChildren<TextMeshProUGUI>())
            if (label.name == "Help") label.text = label.text.Replace("other players have their own.", "other players have their own, and can show you theirs.");
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] sharing added");
    }

    // The receiver alone, into a scene that has sharing and the classroom: no panel is made
    // again, so none loses its plate or its place in the lightmap.
    [MenuItem("ShaderEmu/Update the streams in the open scene")]
    public static void UpdateStreams()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        EmuShareHub hub = Object.FindObjectOfType<EmuShareHub>();
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        if (machine == null || hub == null || stations == null) throw new System.Exception("no machine, sharing or classroom in the open scene");
        CreateProgramAssets();
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(machine);
        EmuStreams streams = MakeStreams(machine.transform.parent, machine);
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(hub);
        hub.streams = streams;
        Apply(hub);
        streams.hub = hub;
        Apply(streams);
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(stations);
        stations.streams = streams;
        stations.blackTexture = machine.blackTexture;
        stations.ownConsole = PictureTexture("StationOwnConsole", 640, 480);
        Apply(stations);
        foreach (string old in new[] { "ShareStore.asset", "RemotePicture.renderTexture", "StationDecode.mat", "StationConsole.mat" })
            AssetDatabase.DeleteAsset(Generated + "/" + old);
        for (int s = 0; s < 8; s++)
            foreach (string old in new[] { "StationStore" + s + ".asset", "StationPicture" + s + ".renderTexture", "StationGrid" + s + ".asset",
                                           "StationConsole" + s + ".renderTexture" })
                AssetDatabase.DeleteAsset(Generated + "/" + old);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] streams updated");
    }
}
