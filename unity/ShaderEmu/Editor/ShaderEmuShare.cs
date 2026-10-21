using TMPro;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// Players see each other's machines (docs/world/share.md): the textures the stream is packed and
// unpacked in, the hub, the object VRChat copies for every player, and the panel's buttons.
public static partial class ShaderEmuBuilder
{
    // What arrives, for every screen that shows a player: a slot a sender (EmuStreams).
    static EmuStreams MakeStreams(Transform world, EmuMachine machine)
    {
        if (world.Find("Streams") != null) Object.DestroyImmediate(world.Find("Streams").gameObject);
        EmuStreams streams = Udon<EmuStreams>(new GameObject("Streams"));
        streams.transform.SetParent(world, false);
        streams.terminal = machine.terminal;
        Material consoleMat = Mat("StreamConsole", "ShaderEmu/Terminal");
        consoleMat.CopyPropertiesFromMaterial(AssetDatabase.LoadAssetAtPath<Material>(Generated + "/Terminal.mat"));   // the font, the columns and rows
        streams.consoleMaterial = consoleMat;
        streams.blackTexture = machine.blackTexture;
        UnitsReceiver(streams);
        const int slots = 16;   // as EmuShare's `ask`
        streams.pictures = new RenderTexture[slots];
        streams.grids = new Texture2D[slots];
        streams.consoles = new RenderTexture[slots];
        for (int s = 0; s < slots; s++)
        {
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
        // (wherever one is: a panel left behind lies over the new one and takes its clicks)
        foreach (Canvas old in world.GetComponentsInChildren<Canvas>(true))
            if (old != null && old.name == "Players panel") Object.DestroyImmediate(old.gameObject);
        foreach (string old in new[] { "Share", "Share player", "Streams" })
            if (world.Find(old) != null) Object.DestroyImmediate(world.Find(old).gameObject);
        for (int i = panel.childCount - 1; i >= 0; i--)
            if (panel.GetChild(i).name.StartsWith("Share ")) Object.DestroyImmediate(panel.GetChild(i).gameObject);

        EmuStreams streams = MakeStreams(world, machine);

        EmuShareHub hub = Udon<EmuShareHub>(new GameObject("Share"));
        hub.transform.SetParent(world, false);
        hub.machine = machine;
        hub.terminal = machine.terminal;
        hub.consoleKeyboard = machine.consoleKeyboard;
        hub.gpuKeyboard = machine.gpuKeyboard;
        UnitsSender(hub);
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
            float y = i < 2 ? 166 + i * 68 : 318 + (i - 2) * 58;
            rowButtons[i] = MakeButton(list, "Row " + i, i < 2 ? texts[i] : "", left, y, rowWide, i < 2 ? 60 : 52, 22, out hub.rowLabels[i]);
            hub.rowObjects[i] = rowButtons[i].gameObject;
            hub.rowPlates[i] = rowButtons[i].GetComponent<Image>();
            if (i >= 2) rowButtons[i].gameObject.SetActive(false);   // until a player is there
        }
        Label(list, "Others", "Other players:", left, 300 - 2, rowWide, 20, 18, TextAnchor.MiddleLeft, dim);
        // who has the shown machine on their own wall
        hub.watchersLabel = Label(list, "Watchers", "", left, 784, rowWide, 62, 17, TextAnchor.UpperLeft, new Color(0.75f, 0.95f, 0.8f));
        Button more = MakeButton(list, "More", "More players", left, 848, rowWide, 46, 22, out hub.moreLabel);
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
        NetPlayer(world, hub);
        ShareProbe(hub, streams);
        ShareUsers(hub, streams);
    }

    // The network's packets go in a player object of their own (EmuNet, docs/system/lan.md).
    static void NetPlayer(Transform world, EmuShareHub hub)
    {
        if (world.Find("Net player") != null) Object.DestroyImmediate(world.Find("Net player").gameObject);
        GameObject template = new GameObject("Net player");
        template.transform.SetParent(world, false);
        template.AddComponent<VRC.SDK3.Components.VRCPlayerObject>();
        EmuNet net = Udon<EmuNet>(template);
        net.hub = hub;
        Apply(net);
        UdonSharpEditor.UdonSharpEditorUtility.GetBackingUdonBehaviour(net).SyncMethod = VRC.SDKBase.Networking.SyncType.Manual;
        template.SetActive(false);   // (as the SDK leaves "Share player")
    }

    [MenuItem("ShaderEmu/Add the network's player object to the open scene")]
    public static void AddNetPlayer()
    {
        EmuShareHub hub = Object.FindObjectOfType<EmuShareHub>();
        if (hub == null) throw new System.Exception("no sharing in the open scene");
        NetPlayer(hub.transform.parent, hub);
        EditorSceneManager.MarkSceneDirty(hub.gameObject.scene);
        EditorSceneManager.SaveScene(hub.gameObject.scene);
        Debug.Log("[ShaderEmu] the network's player object added");
    }

    // The display as units the GPU picks (docs/world/share.md, "Display"): the captures and the
    // textures of ShareUnits.shader's passes, and of the receiver's two.
    static void UnitsSender(EmuShareHub hub)
    {
        hub.fineA = Target("ShareFineA", 1280, 960, 0);
        hub.fineB = Target("ShareFineB", 1280, 960, 0);
        hub.unitsMaterial = Mat("ShareUnits", "ShaderEmu/ShareUnits");
        hub.unitsBlocks = Target("UnitsBlocks", 640, 450, 0);
        hub.unitsExact = Target("UnitsExact", 160, 210, 0);
        hub.unitsNeed = Target("UnitsNeed", 160, 120, 0);
        hub.unitsStats = Target("UnitsStats", 1, 1, 0);
        hub.unitsPlan = Target("UnitsPlan", 160, 120, 0);
        hub.unitsTable = Target("UnitsTable", 80, 113, 0);
        hub.unitsPrefix = Target("UnitsPrefix", 256, 1, 0);
        hub.unitsCut = Target("UnitsCut", 2, 1, 0);
        hub.unitsPacket = Target("UnitsPacket", 292, 1, 0);
        hub.unitsState = new[] { Target("UnitsState0", 160, 120, 0), Target("UnitsState1", 160, 120, 0), Target("UnitsState2", 160, 120, 0) };
    }

    static void UnitsReceiver(EmuStreams streams)
    {
        streams.unitsMaterial = Mat("ShareUnitsDecode", "ShaderEmu/ShareUnitsDecode");
        Texture2D packet = LoadOrCreate(Generated + "/UnitsReceived.asset", () => new Texture2D(288, 1, TextureFormat.RGBA32, false, true));
        packet.filterMode = FilterMode.Point;
        packet.wrapMode = TextureWrapMode.Clamp;
        streams.unitsPacket = packet;
        streams.unitsIndex = Target("UnitsIndex", 80, 113, 0);
    }

    // For the test with two clients: what each client's screen is, coarsely, in its log.
    static void ShareProbe(EmuShareHub hub, EmuStreams streams)
    {
        Transform world = hub.transform.parent;
        if (world.Find("Share probe") != null) Object.DestroyImmediate(world.Find("Share probe").gameObject);
        EmuShareProbe probe = Udon<EmuShareProbe>(new GameObject("Share probe"));
        probe.transform.SetParent(world, false);
        probe.hub = hub;
        probe.machine = hub.machine;
        probe.streams = streams;
        probe.material = Mat("ShareSignature", "ShaderEmu/ShareUnits");
        probe.signature = Target("ShareSignature", 24, 18, 0);
        Apply(probe);
    }

    // Into a scene that has sharing: nothing of it is made again; the display stream's textures
    // are, and what the tiles of before 13 October 2026 were kept in is deleted.
    [MenuItem("ShaderEmu/Update the display stream in the open scene")]
    public static void AddUnits()
    {
        EmuShareHub hub = Object.FindObjectOfType<EmuShareHub>();
        EmuStreams streams = Object.FindObjectOfType<EmuStreams>();
        if (hub == null || streams == null) throw new System.Exception("no sharing in the open scene");
        CreateProgramAssets();
        foreach (string old in new[] { "ShareEncode.mat", "ShareDecode.mat", "ShareCaptureA.renderTexture", "ShareCaptureB.renderTexture", "ShareAtlas.renderTexture",
                                       "ShareChanged.renderTexture", "ShareFineBlocks.renderTexture" })
            AssetDatabase.DeleteAsset(Generated + "/" + old);
        for (int s = 0; s < 16; s++) AssetDatabase.DeleteAsset(Generated + "/StreamStore" + s + ".asset");
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(hub);
        UnitsSender(hub);
        Apply(hub);
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(streams);
        UnitsReceiver(streams);
        Apply(streams);
        ShareProbe(hub, streams);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(hub.gameObject.scene);
        EditorSceneManager.SaveScene(hub.gameObject.scene);
        Debug.Log("[ShaderEmu] display stream updated");
    }

    // Whatever else in the scene uses the hub and the receiver is told of the new ones.
    static void ShareUsers(EmuShareHub hub, EmuStreams streams)
    {
        EmuHolodeck deck = Object.FindObjectOfType<EmuHolodeck>();
        EmuGamepad gamepad = Object.FindObjectOfType<EmuGamepad>();
        EmuStations stations = Object.FindObjectOfType<EmuStations>();
        EmuLan lan = Object.FindObjectOfType<EmuLan>();
        if (lan != null)
        {
            UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(lan);
            lan.hub = hub;
            Apply(lan);
        }
        if (deck != null)
        {
            UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(deck);
            deck.hub = hub;
            deck.streams = streams;
            Apply(deck);
        }
        if (gamepad != null)
        {
            UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(gamepad);
            gamepad.hub = hub;
            Apply(gamepad);
        }
        if (stations != null)
        {
            UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(stations);
            stations.hub = hub;
            stations.streams = streams;
            Apply(stations);
        }
    }

    // Into the scene as it is, without building the world again (which would need a bake).
    [MenuItem("ShaderEmu/Add sharing to the open scene")]
    public static void AddSharing()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        GameObject panel = GameObject.Find("Control panel");
        if (machine == null || panel == null) throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        Transform computer = Den();
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

    // A scene where "Add sharing" hung the panel on the model Models/Computer and left the one
    // before it, whose buttons call nothing, in the same place: that one goes, and the hub's
    // own comes to the den. Nothing is made again, so no plate loses its place in the lightmap.
    [MenuItem("ShaderEmu/Remove the dead players' panel in the open scene")]
    public static void RemoveDeadPlayersPanel()
    {
        EmuShareHub hub = Object.FindObjectOfType<EmuShareHub>();
        Transform computer = Den();
        if (hub == null || computer == null) throw new System.Exception("no sharing in the open scene");
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(hub);
        if (hub.watchLabel == null) throw new System.Exception("the hub has no panel: run ShaderEmu/Add sharing to the open scene");
        Transform live = hub.watchLabel.transform.parent;
        int removed = 0;
        foreach (Canvas canvas in hub.transform.parent.GetComponentsInChildren<Canvas>(true))
        {
            if (canvas == null || canvas.name != "Players panel" || canvas.transform == live) continue;
            Object.DestroyImmediate(canvas.gameObject);
            removed++;
        }
        bool moved = live.parent != computer;
        if (moved) live.SetParent(computer, true);
        EditorSceneManager.MarkSceneDirty(hub.gameObject.scene);
        EditorSceneManager.SaveScene(hub.gameObject.scene);
        Debug.Log("[ShaderEmu] players' panels removed: " + removed + (moved ? "; the hub's own moved to the den" : ""));
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
        ShareUsers(hub, streams);
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
