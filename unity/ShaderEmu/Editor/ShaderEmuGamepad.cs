using TMPro;
using UdonSharp;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using VRC.Udon;
using UdonSharpEditor;

// The game controllers (docs/gamepad.md): two models on the desk by the display's keyboard,
// a name lying on the desk before each, and the station that holds whoever has one where
// they stand. What their controls do is on the board (About, in ShaderEmuDecor.cs).
public static partial class ShaderEmuBuilder
{
    // The board's "what you can do", here because the controllers' part of it is the longest.
    public const int DoSize = 25;
    public const string DoText =
        "Press POWER on the computer's tower, right of the desk (or on the control panel). Linux boots in a few " +
        "seconds and starts the desktop.\n\n" +
        "DISPLAY: each hand's beam is the mouse (trigger = left, grip = right, right stick = wheel).\n" +
        "Keyboards on the desk: both hands type, or press 'Use my keyboard' to use your real one.\n" +
        "Start menu (or the Win key): games, tools, the Web (a fixed list of sites: VRChat lets a world load " +
        "only addresses it was built with).\n" +
        "CONSOLE: the machine's serial line. Four terminals (the buttons above it), and a field over its " +
        "keyboard to paste text into.\n" +
        "MEMORY: all of the machine's memory at once; what is being written glows.\n\n" +
        "GAME CONTROLLERS, on the desk right of the keyboards. Click one to take it; you then stand still and " +
        "your sticks are the game's. Hold BOTH GRIPS for a second to put it back, and again to take it " +
        "again from anywhere (desktop: G puts it back).\n" +
        "White, SHOOTER (Quake, Doom): left stick walks and steps aside, right stick turns and looks, right " +
        "trigger fires, left trigger jumps or opens, right grip next weapon, left grip Esc, jump Enter.\n" +
        "Blue, STRATEGY (Red Alert, Command & Conquer): right stick moves the pointer, the triggers are the " +
        "mouse's buttons, left stick scrolls the map, right grip with the right stick is the wheel, left grip " +
        "Esc, jump Enter.";

    static GameObject Part(Transform parent, PrimitiveType type, string name, Vector3 centre, Vector3 size, Vector3 euler, Material material)
    {
        GameObject go = GameObject.CreatePrimitive(type);
        go.name = name;
        go.transform.SetParent(parent, false);
        go.transform.localPosition = centre;
        go.transform.localEulerAngles = euler;
        go.transform.localScale = type == PrimitiveType.Cylinder ? new Vector3(size.x, size.y * 0.5f, size.z) : size;
        go.GetComponent<MeshRenderer>().sharedMaterial = material;
        Object.DestroyImmediate(go.GetComponent<Collider>());
        return go;   // (not static: it moves, and is not in the baked light)
    }

    // One controller at `at` on the desk (the computer's coordinates), with its name before it.
    static EmuGamepad OneGamepad(Transform world, Transform computer, string name, string title, Vector3 at, Material body, Material dark,
                                 Material mark, int mapping)
    {
        Transform dock = new GameObject(name + " dock").transform;
        dock.SetParent(computer, false);
        dock.localPosition = at;
        dock.localEulerAngles = new Vector3(0, 180f, 0);   // its sticks towards the room

        GameObject pad = new GameObject(name);
        pad.transform.SetParent(world, false);
        pad.transform.SetPositionAndRotation(dock.position, dock.rotation);
        Transform model = pad.transform;
        Part(model, PrimitiveType.Cube, "Body", Vector3.zero, new Vector3(0.150f, 0.034f, 0.080f), Vector3.zero, body);
        foreach (float side in new[] { -1f, 1f })
        {
            Part(model, PrimitiveType.Cube, "Grip", new Vector3(side * 0.066f, -0.012f, -0.045f), new Vector3(0.042f, 0.040f, 0.085f),
                 new Vector3(-20f, side * 14f, 0), body);
            Part(model, PrimitiveType.Cylinder, "Stick", new Vector3(side * 0.036f, 0.024f, side < 0 ? 0.012f : -0.012f),
                 new Vector3(0.024f, 0.018f, 0.024f), Vector3.zero, dark);
            Part(model, PrimitiveType.Cube, "Trigger", new Vector3(side * 0.055f, 0.004f, 0.044f), new Vector3(0.030f, 0.018f, 0.014f),
                 Vector3.zero, dark);
        }
        for (int i = 0; i < 4; i++)
            Part(model, PrimitiveType.Sphere, "Button", new Vector3(0.040f + (i == 0 ? -0.014f : i == 1 ? 0.014f : 0f), 0.019f,
                 0.016f + (i == 2 ? -0.014f : i == 3 ? 0.014f : 0f)), Vector3.one * 0.012f, Vector3.zero, i == 0 ? mark : dark);
        BoxCollider reach = pad.AddComponent<BoxCollider>();
        reach.size = new Vector3(0.20f, 0.08f, 0.16f);
        reach.isTrigger = true;

        // its name, flat on the desk before it: nothing stands in front of anything
        RectTransform sign = Panel(computer, name + " name", at + new Vector3(0, -0.024f, -0.105f), new Vector3(90f, 0, 0), 190, 46,
                                   new Color(0.05f, 0.055f, 0.07f));
        Object.DestroyImmediate(sign.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(sign.GetComponent<BoxCollider>());
        Object.DestroyImmediate(sign.GetComponent<UnityEngine.UI.GraphicRaycaster>());
        Label(sign, "Text", title, 0, 0, 190, 46, 22, TextAnchor.MiddleCenter, Color.white);

        EmuGamepad gamepad = Udon<EmuGamepad>(pad);
        gamepad.model = model;
        gamepad.dock = dock;
        gamepad.mapping = mapping;
        gamepad.last = mapping == 0;
        return gamepad;
    }

    static void Gamepad(Transform world, Transform computer, EmuMachine machine, EmuKeyboard keys, EmuPointer pointer)
    {
        foreach (string old in new[] { "Gamepad", "Shooter controller", "Strategy controller", "Gamepad station" })
            if (world.Find(old) != null) Object.DestroyImmediate(world.Find(old).gameObject);
        foreach (string old in new[] { "Gamepad dock", "Gamepad panel", "Shooter controller dock", "Strategy controller dock",
                                       "Shooter controller name", "Strategy controller name" })
            if (computer.Find(old) != null) Object.DestroyImmediate(computer.Find(old).gameObject);

        GameObject seat = new GameObject("Gamepad station");
        seat.transform.SetParent(world, false);
        VRC.SDK3.Components.VRCStation station = seat.AddComponent<VRC.SDK3.Components.VRCStation>();
        station.PlayerMobility = VRC.SDKBase.VRCStation.Mobility.Immobilize;
        station.seated = false;
        station.disableStationExit = true;
        station.canUseStationFromStation = false;
        station.stationEnterPlayerLocation = seat.transform;
        station.stationExitPlayerLocation = seat.transform;

        // on the desk, to the right of the display's keyboard
        Vector3 at = keys.transform.localPosition + new Vector3(0.84f, -0.158f, -0.08f);
        Material dark = Lit("DarkPlastic", new Color(0.06f, 0.065f, 0.075f), null, 1f, 0.45f);
        EmuGamepad shooter = OneGamepad(world, computer, "Shooter controller", "SHOOTER", at,
            Lit("GamepadBody", new Color(0.78f, 0.79f, 0.82f), null, 1f, 0.5f), dark, Lit("GamepadRed", new Color(0.80f, 0.10f, 0.08f), null, 1f, 0.5f), 0);
        EmuGamepad strategy = OneGamepad(world, computer, "Strategy controller", "STRATEGY", at + new Vector3(0.24f, 0, 0),
            Lit("GamepadBlue", new Color(0.12f, 0.30f, 0.75f), null, 1f, 0.5f), dark, Lit("GamepadYellow", new Color(0.95f, 0.75f, 0.10f), null, 1f, 0.5f), 1);
        foreach (EmuGamepad gamepad in new[] { shooter, strategy })
        {
            gamepad.machine = machine;
            gamepad.keys = keys;
            gamepad.pointer = pointer;
            gamepad.station = station;
            gamepad.other = gamepad == shooter ? strategy : shooter;
            Apply(gamepad);
            UdonBehaviour udon = UdonSharpEditorUtility.GetBackingUdonBehaviour(gamepad);
            udon.interactText = gamepad == shooter ? "Take the shooter controller" : "Take the strategy controller";
            EditorUtility.SetDirty(udon);
        }

        // the board says what they do: its text in a scene built before this
        GameObject board = GameObject.Find("About");
        Transform does = board != null ? board.transform.Find("Do") : null;
        if (does != null)
        {
            TextMeshProUGUI text = does.GetComponent<TextMeshProUGUI>();
            text.text = DoText;
            text.fontSize = DoSize;
            EditorUtility.SetDirty(text);
        }
    }

    [MenuItem("ShaderEmu/Add the game controllers to the open scene")]
    public static void AddGamepad()
    {
        EmuMachine machine = Object.FindObjectOfType<EmuMachine>();
        EmuPointer pointer = Object.FindObjectOfType<EmuPointer>();
        GameObject computer = GameObject.Find("Computer");
        EmuKeyboard keys = null;
        foreach (EmuKeyboard k in Object.FindObjectsOfType<EmuKeyboard>())
            if (k.rawKeys) keys = k;
        if (machine == null || pointer == null || computer == null || keys == null)
            throw new System.Exception("no machine in the open scene: run ShaderEmu/Build world");
        CreateProgramAssets();
        Gamepad(machine.transform.parent, computer.transform, machine, keys, pointer);
        AssetDatabase.SaveAssets();
        EditorSceneManager.MarkSceneDirty(machine.gameObject.scene);
        EditorSceneManager.SaveScene(machine.gameObject.scene);
        Debug.Log("[ShaderEmu] game controllers added");
    }
}
