using System.Collections.Generic;
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.Events;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.UI;

// The classroom in the den (world/furniture.py, docs/world/stations.md): twelve computers on six wooden
// tables, one a visitor whose machine is on; and one in each of the three side rooms off the corridor
// (world/rooms.py), places 12 to 14. The models are the furniture's; this gives each
// one's glass its tube, its keyboard and tube their beams' plates, the keys modelled on its
// monitor and tower their work, its tower's window the speed of its owner's machine, and its
// small lamps (disk, network, the hubs' ports) their light.
public static partial class ShaderEmuBuilder
{
    // Models/stations.json, which world/furniture.py writes with the models (world/pc.py's numbers)
    [System.Serializable]
    class StationPlace
    {
        public float x, z, turn;       // the set on its table, and its turn about the upright
        public float[] keyboard;       // its keyboard off its place: x, z, turn
    }
    [System.Serializable]
    class StationHub
    {
        public float x, y, z, turn;    // a hub in the room, and its turn about the upright
        public float[] ports;          // its lamps' lenses in its own frame, facing its -z: x, y, z, the place served (-1: another hub)
        public float[] lamp;           // a lens: wide, high
    }
    [System.Serializable]
    class StationsPlan
    {
        public StationPlace[] places;
        public float table, keyScale, keyboardZ, setX, reset;
        public float[] glass;          // wide, high, its middle's height, its front
        public float[] tower;          // x, z, its front in its own frame, its height
        public float[] speed;          // the speed's window: x, down from the top, wide, high
        public float[] power;          // the power key: x, down from the top
        public float[] osd;            // the monitor's six keys: the first's x, their pitch, their height
        public float led;              // the monitor's lamp: x
        public float[] lamps;          // the tower's three: x of power, turbo, disk
        public float[] tables;         // the tables' middles: x, z each
        public float keyboardHigh, keyboardSlope, keyboardTops;   // world/pc.py's keyboard(): its plane, its tilt, its caps' tops
        public float[] nic, nicSize;   // the network socket's lamp on the tower's back, in the place's frame; wide, high
        public StationHub[] hubs;
    }

    const float TubeFill = 0.97f;      // the picture's share of the glass: above 0.973 the bezel's rounded corners cut its own
    // A classroom computer's shell is flat where the model has lenses and keys: what is lit lies this far over it.
    const float OnShell = 0.0004f;
    const float TowerBlankBay = 0.36f - 0.088f;   // the tower's spare bay

    // The name of the key that hands over the visitor's real keyboard, on its cap as the console
    // keyboard's canvas has it: the words change while it is on, so they are not the caps' print.
    static TextMeshPro CaptureSign(EmuKeyboard keyboard)
    {
        int k = System.Array.IndexOf(keyboard.keyLinux, EmuKeyboard.LinuxCapture);
        if (k < 0) return null;
        Transform old = keyboard.transform.Find("Capture sign");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        TMP_FontAsset font = RetroFont("IBMPlexSansCondensed-Bold");
        TextMeshPro sign = new GameObject("Capture sign").AddComponent<TextMeshPro>();
        sign.transform.SetParent(keyboard.transform, false);
        sign.transform.localPosition = new Vector3(keyboard.keyX[k] + keyboard.keyW[k] * 0.5f - keyboard.panelWidth * 0.5f,
                                                   keyboard.panelHeight * 0.5f - keyboard.keyY[k] - keyboard.keyH[k] * 0.5f, -0.5f);
        sign.rectTransform.sizeDelta = new Vector2(keyboard.keyW[k], keyboard.keyH[k]);
        sign.font = font;
        sign.fontSharedMaterial = LitFont(font);   // ink, in the room's light: the font's own material came out blue-grey
        sign.fontSize = 180f;   // the console keyboard's 18, in a canvas's units
        sign.alignment = TextAlignmentOptions.Center;
        sign.enableWordWrapping = false;
        sign.color = new Color(0.05f, 0.05f, 0.06f);
        sign.text = "Use my keyboard";
        sign.GetComponent<MeshRenderer>().shadowCastingMode = ShadowCastingMode.Off;
        return sign;
    }

    static StationsPlan ReadStations()
    {
        StationsPlan plan = JsonUtility.FromJson<StationsPlan>(System.IO.File.ReadAllText(ModelsPath + "/stations.json"));
        // the side rooms' computers (world/rooms.py) are places too, after the classroom's: N13 to N15
        SideRoomsPlan sides = SideRooms();
        if (sides != null && sides.places != null && sides.first == plan.places.Length)
        {
            List<StationPlace> all = new List<StationPlace>(plan.places);
            all.AddRange(sides.places);
            plan.places = all.ToArray();
        }
        return plan;
    }

    // Keys with nothing to show, over keys the model has: a canvas the beams find. `keys` are
    // x, y, width, height in millimetres from the canvas's top left, and the event each sends.
    static void HotKeys(Transform parent, string name, Vector3 at, float wide, float high, EmuPlace place, float[] keys, string[] events)
    {
        RectTransform panel = Panel(parent, name, at, Vector3.zero, wide, high, Color.clear);
        TextMeshProUGUI unused;
        for (int i = 0; i < events.Length; i++)
        {
            Button key = MakeButton(panel, events[i], "", keys[i * 4], keys[i * 4 + 1], keys[i * 4 + 2], keys[i * 4 + 3], 8, out unused);
            key.GetComponent<Image>().color = Color.clear;
            OnClick(key, place, events[i]);
        }
    }

    // The pointer's switches on the monitor's bezel, beside the volume: this visitor's own (EmuPointer).
    static void StationSwitches(Transform place, int s, EmuPlace own, StationsPlan plan, List<TextMeshProUGUI> physical, List<TextMeshProUGUI> steady)
    {
        const float wide = 124, high = 18, volumeHalf = 100, gap = 6;
        float y = (plan.glass[2] - (plan.glass[1] + 0.024f) / 2f + plan.osd[2] + 0.011f) / 2f, x = (volumeHalf + gap + wide / 2f) * 0.001f;
        string[] events = { "PhysicalButtons", "SteadyPointer" }, texts = { "Physical buttons: OFF", "Steady pointer: ON" };
        for (int i = 0; i < 2; i++)
        {
            string name = "Station " + s + (i == 0 ? " physical" : " steady");
            Gone(place, name);
            RectTransform panel = Panel(place, name, new Vector3(plan.setX + (i == 0 ? -x : x), y, plan.glass[3] - 0.002f), Vector3.zero,
                                        wide, high, new Color(0.05f, 0.055f, 0.07f));
            TextMeshProUGUI label;
            OnClick(MakeButton(panel, events[i], texts[i], 3, 2, wide - 6, high - 4, 8, out label), own, events[i]);
            (i == 0 ? physical : steady).Add(label);
        }
    }

    // The tower's reset key, on the hot panel's place of before (it held turbo too).
    static void TowerHotKeys(Transform place, int s, EmuPlace own, StationsPlan plan)
    {
        string name = "Station " + s + " tower hot";
        Gone(place, name);
        HotKeys(place, name, new Vector3(plan.tower[0] + 0.012f, plan.tower[3] - plan.speed[1], plan.tower[1] + plan.tower[2] - 0.004f), 34, 30, own,
                new float[] { 18, 2, 15, 26 }, new[] { "ResetKey" });
    }

    static Material SpeedWindow(Transform parent, string name, Vector3 at, float wide, float high)
    {
        GameObject window = GameObject.CreatePrimitive(PrimitiveType.Quad);
        window.name = name;
        Object.DestroyImmediate(window.GetComponent<Collider>());
        window.transform.SetParent(parent, false);
        window.transform.localPosition = at;
        window.transform.localScale = new Vector3(wide, high, 1f);
        Material material = Mat(name, "ShaderEmu/SevenSeg");
        MeshRenderer renderer = window.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        renderer.lightProbeUsage = LightProbeUsage.Off;
        renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        return material;
    }

    // The same window, large, on the den's wall between the console and the display: this
    // visitor's machine's speed, to be read from across the room. It is of the tower's own
    // material, so whatever sets that sets it. No frame, and its light is its own: no bake.
    const float WallSpeedWide = 0.62f, WallSpeedHigh = 0.365f;
    static void WallSpeed(Transform computer)
    {
        Gone(computer, "Speed wall");
        Gone(computer, "Speed sign");
        Transform display = computer.Find("Display screen");
        Vector3 at = new Vector3(-1.23f, 2.0f, (display != null ? display.localPosition.z : 5.41f) - 0.002f);
        GameObject window = GameObject.CreatePrimitive(PrimitiveType.Quad);
        window.name = "Speed wall";
        Object.DestroyImmediate(window.GetComponent<Collider>());
        window.transform.SetParent(computer, false);
        window.transform.localPosition = at;
        window.transform.localScale = new Vector3(WallSpeedWide, WallSpeedHigh, 1f);
        MeshRenderer renderer = window.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = Mat("Speed own", "ShaderEmu/SevenSeg");
        renderer.shadowCastingMode = ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        renderer.lightProbeUsage = LightProbeUsage.Off;
        renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;

        // what its two rows are, on strips above and below it, as the tower's case has them
        const float strip = 0.042f;
        RectTransform sign = Panel(computer, "Speed sign", at + new Vector3(0, 0, -0.002f), Vector3.zero, WallSpeedWide * 1000, (WallSpeedHigh + 2 * strip) * 1000,
                                   new Color(0, 0, 0, 0));
        Object.DestroyImmediate(sign.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(sign.GetComponent<BoxCollider>());
        Object.DestroyImmediate(sign.GetComponent<GraphicRaycaster>());
        TMP_FontAsset bold = RetroFont("IBMPlexMono-Bold");
        string[] names = { "MIPS  ALL CORES", "MIPS  CORE 0" };
        for (int row = 0; row < 2; row++)
        {
            RectTransform plate = Child(sign, row == 0 ? "All" : "Core", 0, row == 0 ? 0 : (WallSpeedHigh + strip) * 1000, WallSpeedWide * 1000, strip * 1000);
            Plate(plate, new Color(0.012f, 0.012f, 0.014f, 1f)).raycastTarget = false;
            TextMeshProUGUI label = Label(plate, "Text", names[row], 0, 0, WallSpeedWide * 1000, strip * 1000, 26, TextAnchor.MiddleCenter, new Color(1f, 0.62f, 0.2f));
            label.font = bold;
            label.fontSharedMaterial = bold.material;
            label.raycastTarget = false;
        }
    }



    // A lamp that flickers (Blink.shader), a hair before a model's dull lens: a material of its
    // own, whose sixteen bits EmuStations writes. `turn` about the upright: 180 for a tower's back.
    static Material BlinkLamp(Transform parent, string name, Vector3 at, float turn, float wide, float high, Color colour)
    {
        GameObject lamp = GameObject.CreatePrimitive(PrimitiveType.Quad);
        lamp.name = name;
        Object.DestroyImmediate(lamp.GetComponent<Collider>());
        lamp.transform.SetParent(parent, false);
        lamp.transform.localPosition = at;
        lamp.transform.localEulerAngles = new Vector3(0, turn, 0);
        lamp.transform.localScale = new Vector3(wide, high, 1f);
        Material material = Mat(name, "ShaderEmu/Blink");
        material.SetColor("_Color", colour);
        material.SetFloat("_Pattern", 0f);
        MeshRenderer renderer = lamp.GetComponent<MeshRenderer>();
        renderer.sharedMaterial = material;
        renderer.shadowCastingMode = ShadowCastingMode.Off;
        renderer.receiveShadows = false;
        renderer.lightProbeUsage = LightProbeUsage.Off;
        renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        return material;
    }

    // An address to open and text to type, for a visitor at their own place: one strip, which
    // EmuStations stands between that place's keyboard and its monitor. Its third line says
    // what came of the link.
    static void StationFields(Transform root, EmuStations stations, EmuMachine machine)
    {
        const float wide = 1120, high = 84, gap = 5, key = 190, row = high - 2 * gap;
        Gone(root, "Station fields");
        Color back = new Color(0.05f, 0.055f, 0.07f);
        RectTransform panel = Panel(root, "Station fields", Vector3.zero, Vector3.zero, wide, high, back);
        panel.localScale = Vector3.one * 0.0005f;
        // the hit box stands 40 mm proud of the face: VRChat's laser, coming low over the
        // table, met the table's collider before the strip and was not drawn
        BoxCollider hit = panel.GetComponent<BoxCollider>();
        hit.size = new Vector3(wide, high, 80f);
        hit.center = new Vector3(0, 0, -40f);
        Font font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        TextMeshProUGUI unused;
        float fieldWidth = wide - 20 - 3 * (key + 10);
        RectTransform box = Child(panel, "Field", 10, gap, fieldWidth, row);
        Plate(box, new Color(0.14f, 0.16f, 0.22f));
        Text text = Child(box, "Text", 10, 2, fieldWidth - 20, row - 26).gameObject.AddComponent<Text>();
        text.font = font;
        text.material = UiMaterial();
        text.fontSize = 24;
        text.color = Color.white;
        text.alignment = TextAnchor.MiddleLeft;
        text.supportRichText = false;
        // one field: an address to open, or text typed at this place's keyboard
        VRC.SDK3.Components.VRCUrlInputField field = box.gameObject.AddComponent<VRC.SDK3.Components.VRCUrlInputField>();
        field.textComponent = text;
        field.targetGraphic = box.GetComponent<Image>();
        machine.placeField = field;
        EmuPaste paste = Udon<EmuPaste>(panel.gameObject);
        paste.field = null;
        paste.urlField = field;
        float x = 10 + fieldWidth + 10;
        OnClick(MakeButton(panel, "Open", "Open Link", x, gap, key, row, 26, out unused), machine, "OpenPlaceLink");
        OnClick(MakeButton(panel, "Type", "Type as Text", x + key + 10, gap, key, row, 26, out unused), paste, "Send");
        OnClick(MakeButton(panel, "Type line", "+Enter", x + 2 * (key + 10), gap, key, row, 26, out unused), paste, "SendLine");
        // (the link's news in the field's lowest line: the strip has no row to spare)
        machine.placeLabel = Label(panel, "Link says", "", 20, gap + row - 24, fieldWidth - 20, 22, 16, TextAnchor.MiddleLeft,
                                   new Color(1f, 0.8f, 0.3f));
        machine.placeLabel.raycastTarget = false;
        machine.placeLabel.enableWordWrapping = false;
        Apply(paste);
        Apply(machine);
        stations.fields = panel.gameObject;
        stations.paste = paste;
        panel.gameObject.SetActive(false);
    }

    // The lamps that are lit while a machine is on: the monitor's (`monitor`: its place, or none)
    // and the tower's power lamp, a hair before the model's own dull lenses.
    static GameObject PowerLamps(Transform parent, StationsPlan plan, bool monitor, Vector3 tower, float glassZ, float proud)
    {
        Material green = Mat("Power lamp", "Unlit/Color");
        green.color = new Color(0.1f, 1f, 0.3f) * 2.5f;   // (lit: brighter than white, it blooms)
        GameObject lamps = new GameObject("Power lamps");
        lamps.transform.SetParent(parent, false);
        for (int which = monitor ? 0 : 1; which < 2; which++)
        {
            GameObject lamp = GameObject.CreatePrimitive(which == 0 ? PrimitiveType.Sphere : PrimitiveType.Quad);
            lamp.name = which == 0 ? "Monitor lamp" : "Tower lamp";
            Object.DestroyImmediate(lamp.GetComponent<Collider>());
            lamp.transform.SetParent(lamps.transform, false);
            if (which == 0)
            {
                lamp.transform.localPosition = new Vector3(plan.setX + plan.led, plan.osd[2], glassZ - proud);
                lamp.transform.localScale = new Vector3(0.0066f, 0.0066f, 0.0006f);
            }
            else
            {
                lamp.transform.localPosition = tower + new Vector3(plan.lamps[0], plan.tower[3] - plan.speed[1] + 0.005f, -proud);
                lamp.transform.localScale = new Vector3(0.0062f, 0.0024f, 1f);
            }
            MeshRenderer renderer = lamp.GetComponent<MeshRenderer>();
            renderer.sharedMaterial = green;
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
        }
        lamps.SetActive(false);
        return lamps;
    }

    // The speed's slider on a tower, where its floppy drive was (under the place's key): its
    // owner's machine's ticks a frame, Snail to Turbo, 1 to 16 (EmuMachine's speed steps), a few of them named.
    const float TowerSpeedBay = 0.36f - 0.131f;
    static readonly int[] SpeedNamed = { 1, 4, 8, 12, 16 };

    // A slider of a place's panel, in the panel's millimetres from its top-left corner.
    static Slider PlaceSlider(RectTransform panel, string name, float x, float y, float wide, float high, int min, int max, int value)
    {
        GameObject sliderObject = DefaultControls.CreateSlider(new DefaultControls.Resources());
        sliderObject.name = name;
        RectTransform rect = sliderObject.GetComponent<RectTransform>();
        rect.SetParent(panel, false);
        rect.anchorMin = rect.anchorMax = new Vector2(0, 1);
        rect.pivot = new Vector2(0, 1);
        rect.anchoredPosition = new Vector2(x, -y);
        rect.sizeDelta = new Vector2(wide, high);
        Slider slider = sliderObject.GetComponent<Slider>();
        slider.minValue = min;
        slider.maxValue = max;
        slider.wholeNumbers = true;
        slider.value = value;
        foreach (Image part in sliderObject.GetComponentsInChildren<Image>())
        {
            part.material = UiMaterial();
            part.color = part.name == "Handle" ? Color.white : part.name == "Fill" ? new Color(0.3f, 0.65f, 1f) : new Color(0.2f, 0.21f, 0.25f);
        }
        slider.handleRect.sizeDelta = new Vector2(9, 0);
        DeafToWalking(slider);
        return slider;
    }
    static Slider StationSpeed(Transform place, int s, EmuPlace own, StationsPlan plan)
    {
        const float wide = 146, high = 40, left = 8, right = 8, top = 12, row = 14;
        string name = "Station " + s + " speed";
        Gone(place, name);
        RectTransform panel = Panel(place, name, new Vector3(plan.tower[0], TowerSpeedBay, plan.tower[1] + plan.tower[2] - 0.003f), Vector3.zero,
                                    wide, high, new Color(0.05f, 0.055f, 0.07f));
        Color dim = new Color(0.62f, 0.68f, 0.76f);
        TextMeshProUGUI snail = Label(panel, "Snail", "Snail", left, 1, 50, 11, 8, TextAnchor.MiddleLeft, dim);
        TextMeshProUGUI turbo = Label(panel, "Turbo", "Turbo", wide - right - 50, 1, 50, 11, 8, TextAnchor.MiddleLeft, dim);
        turbo.alignment = TextAlignmentOptions.Right;
        Slider slider = PlaceSlider(panel, "Speed slider", left, top, wide - left - right, row, 1, 16, 3);
        // a few steps' numbers under the handle's places (its travel is the slider less a handle)
        float travel = wide - left - right - 9;
        foreach (int n in SpeedNamed)
            Label(panel, "Step " + n, n.ToString(), left + 4.5f + travel * (n - 1) / 15f - 12, top + row + 1, 24, 11, 7, TextAnchor.MiddleCenter, dim);
        own.speed = slider;
        UnityEventTools.AddStringPersistentListener(slider.onValueChanged, UdonSharpEditorUtility.GetBackingUdonBehaviour(own).SendCustomEvent, "SpeedMoved");
        return slider;
    }

    // The sound's volume on a monitor, on the bezel between its row of keys and the glass: its
    // owner's machine's, as the control panel's Volume (EmuSound.volumeSlider, 0 to 100).
    static Slider StationVolume(Transform place, int s, EmuPlace own, StationsPlan plan)
    {
        const float wide = 200, high = 18, left = 4, label = 34;
        string name = "Station " + s + " volume";
        Gone(place, name);
        float glassBottom = plan.glass[2] - (plan.glass[1] + 0.024f) / 2f, keysTop = plan.osd[2] + 0.011f;
        RectTransform panel = Panel(place, name, new Vector3(plan.setX, (glassBottom + keysTop) / 2f, plan.glass[3] - 0.002f), Vector3.zero,
                                    wide, high, new Color(0.05f, 0.055f, 0.07f));
        Label(panel, "Volume", "VOLUME", left, 3, label, 12, 8, TextAnchor.MiddleLeft, new Color(0.62f, 0.68f, 0.76f));
        Slider slider = PlaceSlider(panel, "Volume slider", left + label, 3, wide - left * 2 - label, 12, 0, 100, 10);
        own.volume = slider;
        UnityEventTools.AddStringPersistentListener(slider.onValueChanged, UdonSharpEditorUtility.GetBackingUdonBehaviour(own).SendCustomEvent, "VolumeMoved");
        return slider;
    }

    static void Stations(Transform world)
    {
        StationsPlan plan = ReadStations();
        float glassWide = plan.glass[0], glassHigh = plan.glass[1], glassMid = plan.glass[2], glassZ = plan.glass[3];
        float towerX = plan.tower[0], towerFront = plan.tower[1] + plan.tower[2], towerHigh = plan.tower[3];
        float keyUnit = plan.keyScale * 0.01905f / 60f;
        float lampY = towerHigh - plan.speed[1] + 0.005f;   // the tower's three lamps
        Transform decor = world.Find("Decor"), computer = world.Find("Computer");
        foreach (string gone in new[] { "Round table top", "Round table post", "Stool seat" }) Gone(decor, gone);
        Gone(world, "Stations");
        Transform root = new GameObject("Stations").transform;
        root.SetParent(world, false);
        EmuStations stations = Udon<EmuStations>(root.gameObject);
        int count = plan.places.Length;
        stations.screens = new Material[count];
        stations.labels = new TextMeshProUGUI[count];
        stations.modes = new TextMeshProUGUI[count];
        stations.speeds = new Material[count];
        stations.linkLamps = new GameObject[count];
        stations.powerLamps = new GameObject[count];
        stations.linkSpots = new Transform[count];
        stations.diskLamps = new Material[count];
        stations.netLamps = new Material[count];
        stations.beacons = new Material[count];
        stations.fieldSpots = new Transform[count];
        stations.speedSliders = new Slider[count];
        stations.volumeSliders = new Slider[count];
        List<TextMeshProUGUI> physicalLabels = new List<TextMeshProUGUI>(), steadyLabels = new List<TextMeshProUGUI>();
        Color red = new Color(1f, 0.08f, 0.04f), green = new Color(0.1f, 1f, 0.3f), amber = new Color(1f, 0.6f, 0.1f);
        TMP_FontAsset signFont = RetroFont("IBMPlexSansCondensed-Bold");
        foreach (EmuTerminal terminal in world.GetComponentsInChildren<EmuTerminal>(true)) stations.terminal = terminal;
        stations.ownConsole = PictureTexture("StationOwnConsole", 640, 480);
        Button[] take = new Button[count], leave = new Button[count];
        TextMeshProUGUI unused;
        // a place's keyboard is the console's own layout at the model's size, and its tube a pointer's plate
        EmuMachine theMachine = null;
        foreach (EmuMachine machine in world.GetComponentsInChildren<EmuMachine>(true)) theMachine = machine;
        UdonSharpEditorUtility.CopyUdonToProxy(theMachine);
        EmuKeyboard layout = theMachine.consoleKeyboard;
        UdonSharpEditorUtility.CopyUdonToProxy(layout);
        // (every key of it, the one that hands over the visitor's real keyboard too)
        List<int> real = new List<int>();
        for (int i = 0; i < layout.keyLinux.Length; i++) real.Add(i);
        Material over = Mat("Station key over", "Sprites/Default"), held = Mat("Station key held", "Sprites/Default");
        over.color = new Color(1f, 0.75f, 0.2f, 0.35f);
        held.color = new Color(0.2f, 0.55f, 1f, 0.7f);
        Material lamp = Mat("Links lamp", "Unlit/Color");
        lamp.color = new Color(1f, 0.55f, 0.1f) * 3f;   // (lit: brighter than white, it blooms)
        AudioClip[] strokes = KeyStrokes();
        stations.keyboards = new EmuKeyboard[count];
        Collider[] tubes = new Collider[count], plates = new Collider[count];
        // a table stops a visitor (the tables stand as world/furniture.py has them)
        for (int t = 0; plan.tables != null && t + 1 < plan.tables.Length; t += 2)
            Solid(root, "Table", plan.tables[t] - 1f, plan.tables[t] + 1f, 0, plan.table, plan.tables[t + 1] - 0.45f, plan.tables[t + 1] + 0.45f);
        List<EmuPlace> placed = new List<EmuPlace>();
        for (int s = 0; s < count; s++)
        {
            // the place's own frame: x to the sitter's right, y up from the table, the sitter at -z
            StationPlace where = plan.places[s];
            Transform place = new GameObject("Station " + s).transform;
            place.SetParent(root, false);
            place.localPosition = new Vector3(where.x, plan.table, where.z);
            place.localEulerAngles = new Vector3(0, where.turn, 0);
            // the monitor and the tower stop a visitor's hand, and a beam meant for the wall's display
            Solid(place, "Monitor", plan.setX - 0.3225f, plan.setX + 0.3225f, 0, 0.61f, -0.14f, 0.39f);
            Solid(place, "Tower", towerX - 0.092f, towerX + 0.092f, 0, 0.37f, towerFront, towerFront + 0.42f);
            Material screen = Mat("Station " + s, "ShaderEmu/CRT");
            screen.SetFloat("_Aspect", glassWide / glassHigh);
            screen.SetFloat("_Curve", 0f);   // the glass is the model's, and bulges itself
            screen.SetFloat("_Triads", 560f);   // stripes 0.95 mm apart at the middle: 549 mm of them, coarse enough to see from the chair
            screen.SetFloat("_Fill", TubeFill);
            screen.SetColor("_Off", new Color(0.045f, 0.05f, 0.047f));
            screen.SetFloat("_Glossiness", 0.84f);   // the television's glass is 0.89 to 0.72 smooth; this 0.84 to 0.71
            screen.SetTexture("_Smudge", WorldTexture("Smudges"));   // the television's own film, a little thinner
            screen.SetFloat("_Smudged", 0.16f);
            screen.SetFloat("_Film", 0.6f);
            screen.SetVector("_Size", Vector4.zero);
            stations.screens[s] = screen;

            EmuPlace own = Udon<EmuPlace>(place.gameObject);
            own.stations = stations;
            own.place = s;
            placed.Add(own);
            stations.speedSliders[s] = StationSpeed(place, s, own, plan);
            stations.volumeSliders[s] = StationVolume(place, s, own, plan);
            StationSwitches(place, s, own, plan, physicalLabels, steadyLabels);

            // whose it is, on a key in the tower's spare bay
            RectTransform panel = Panel(place, "Station " + s + " panel", new Vector3(towerX, TowerBlankBay, towerFront - 0.003f), Vector3.zero,
                                        144, 38, new Color(0.05f, 0.055f, 0.07f));
            take[s] = MakeButton(panel, "Take", "FREE", 2, 2, 96, 34, 18, out stations.labels[s]);
            leave[s] = MakeButton(panel, "Leave", "EXIT", 100, 2, 42, 34, 11, out unused);
            // the name above, what a press does below it, both the key's own lettering
            stations.labels[s].alignment = TextAlignmentOptions.Top;
            stations.labels[s].fontSize = 14;
            stations.labels[s].margin = new Vector4(0, 3, 0, 0);
            stations.modes[s] = Label((RectTransform)take[s].transform, "Mode", "press POWER: yours starts here", 0, 21, 96, 11, 6, TextAnchor.MiddleCenter, stations.labels[s].color);
            stations.modes[s].raycastTarget = false;

            // the tower's own keys (reset; power under it), larger to a beam than their caps
            float panelY = towerHigh - plan.speed[1], powerY = towerHigh - plan.power[1];
            TowerHotKeys(place, s, own, plan);
            HotKeys(place, "Station " + s + " power hot", new Vector3(towerX + plan.power[0], powerY, towerFront - 0.008f), 64, 48, own,
                    new float[] { 1, 1, 62, 46 }, new[] { "Power" });
            // what that key is, in lit letters over it, and a bar under it that beckons while the place is free
            TextMeshPro sign = new GameObject("Power sign").AddComponent<TextMeshPro>();
            sign.transform.SetParent(place, false);
            sign.transform.localPosition = new Vector3(towerX + plan.power[0], powerY + 0.021f, towerFront - OnShell);
            sign.rectTransform.sizeDelta = new Vector2(0.06f, 0.012f);
            sign.font = signFont;
            sign.fontSharedMaterial = signFont.material;
            sign.fontSize = 0.085f;
            sign.alignment = TextAlignmentOptions.Center;
            sign.color = new Color(1f, 0.70f, 0.20f);
            sign.text = "POWER";
            stations.beacons[s] = BlinkLamp(place, "Beacon " + s, new Vector3(towerX + plan.power[0], powerY - 0.019f, towerFront - OnShell), 0, 0.034f, 0.0035f, amber);
            stations.diskLamps[s] = BlinkLamp(place, "Disk " + s, new Vector3(towerX + plan.lamps[2], lampY, towerFront - OnShell), 0, 0.0062f, 0.0024f, red);
            if (plan.nic != null && plan.nic.Length == 3)
                stations.netLamps[s] = BlinkLamp(place, "Net " + s, new Vector3(plan.nic[0], plan.nic[1], plan.nic[2] - 0.001f + OnShell), 180, plan.nicSize[0], plan.nicSize[1], green);
            // and the monitor's six, under the glass, left to right as their legend names them
            float pitch = plan.osd[1] * 1000f;
            float[] six = new float[24];
            for (int i = 0; i < 6; i++) { six[i * 4] = i * pitch + 1; six[i * 4 + 1] = 1; six[i * 4 + 2] = pitch - 2; six[i * 4 + 3] = 28; }
            HotKeys(place, "Station " + s + " monitor hot", new Vector3(plan.setX + plan.osd[0] + 2.5f * plan.osd[1], plan.osd[2], glassZ - 0.006f), 6 * pitch, 30, own,
                    six, new[] { "NextTerminal", "LastTerminal", "ShowConsole", "ShowDisplay", "CloseLinks", "OpenLinks" });
            // a lamp over the last of them, lit while the browser wants addresses pasted
            GameObject links = GameObject.CreatePrimitive(PrimitiveType.Quad);
            links.name = "Links lamp";
            Object.DestroyImmediate(links.GetComponent<Collider>());
            links.transform.SetParent(place, false);
            links.transform.localPosition = new Vector3(plan.setX + plan.osd[0] + 5f * plan.osd[1], plan.osd[2] + 0.011f, glassZ - OnShell);
            links.transform.localScale = new Vector3(0.014f, 0.004f, 1f);
            links.GetComponent<MeshRenderer>().sharedMaterial = lamp;
            links.GetComponent<MeshRenderer>().shadowCastingMode = ShadowCastingMode.Off;
            links.SetActive(false);
            stations.linkLamps[s] = links;
            stations.powerLamps[s] = PowerLamps(place, plan, true, new Vector3(towerX, 0, towerFront), glassZ, OnShell);
            Transform spot = new GameObject("Links spot").transform;
            spot.SetParent(place, false);
            spot.localPosition = new Vector3(plan.setX, glassMid, glassZ - 0.06f);
            stations.linkSpots[s] = spot;

            // the speed's window on the tower: every core's, and core 0's
            stations.speeds[s] = SpeedWindow(place, "Speed " + s, new Vector3(towerX + plan.speed[0], panelY, towerFront - OnShell), plan.speed[2], plan.speed[3]);

            // its keys: the layout's rectangles over the caps' tops, a beam's plate, a mark a hand
            GameObject keysGo = new GameObject("Station " + s + " keyboard");
            keysGo.transform.SetParent(place, false);
            Quaternion lie = Quaternion.Euler(0f, where.keyboard[2], 0f) * Quaternion.Euler(90f - plan.keyboardSlope, 0f, 0f);
            keysGo.transform.localPosition = new Vector3(plan.setX + where.keyboard[0], plan.keyboardHigh, plan.keyboardZ + where.keyboard[1]) + lie * new Vector3(0f, 0f, -plan.keyboardTops);
            // where the fields stand: between the keyboard and the monitor's foot, leaning back
            Transform fieldSpot = new GameObject("Fields spot").transform;
            fieldSpot.SetParent(place, false);
            fieldSpot.localPosition = new Vector3(plan.setX + where.keyboard[0], 0.045f, plan.keyboardZ + where.keyboard[1] + 0.150f);
            fieldSpot.localEulerAngles = new Vector3(50f, where.keyboard[2], 0f);
            stations.fieldSpots[s] = fieldSpot;
            keysGo.transform.localRotation = lie;
            keysGo.transform.localScale = Vector3.one * keyUnit;
            BoxCollider plate = keysGo.AddComponent<BoxCollider>();
            plate.size = new Vector3(layout.panelWidth + 60f, layout.panelHeight + 60f, 6f);   // half a key beyond the outer rows: no gap to miss into
            plate.isTrigger = true;
            plates[s] = plate;
            EmuKeyboard keyboard = Udon<EmuKeyboard>(keysGo);
            keyboard.panelWidth = layout.panelWidth;
            keyboard.panelHeight = layout.panelHeight;
            StationCaps(keyboard);
            keyboard.keyX = real.ConvertAll(i => layout.keyX[i]).ToArray();
            keyboard.keyY = real.ConvertAll(i => layout.keyY[i]).ToArray();
            keyboard.keyW = real.ConvertAll(i => layout.keyW[i]).ToArray();
            keyboard.keyH = real.ConvertAll(i => layout.keyH[i]).ToArray();
            keyboard.keyLinux = real.ConvertAll(i => layout.keyLinux[i]).ToArray();
            keyboard.keyNormal = real.ConvertAll(i => layout.keyNormal[i]).ToArray();
            keyboard.keyShifted = real.ConvertAll(i => layout.keyShifted[i]).ToArray();
            keyboard.hostKeys = layout.hostKeys;      // the visitor's real keyboard, while "Use my keyboard" is on
            keyboard.hostLinux = layout.hostLinux;
            keyboard.hostNormal = layout.hostNormal;
            keyboard.hostShifted = layout.hostShifted;
            keyboard.other = layout;                  // only one keyboard has the real one
            keyboard.marks = new Transform[2];
            keyboard.markRenderers = new Renderer[2];
            keyboard.markOver = over;
            keyboard.markHeld = held;
            keyboard.click = ClickSource(keysGo.transform);
            keyboard.strokes = strokes;
            for (int h = 0; h < 2; h++)
            {
                GameObject mark = GameObject.CreatePrimitive(PrimitiveType.Quad);
                mark.name = "Mark " + h;
                Object.DestroyImmediate(mark.GetComponent<Collider>());
                mark.transform.SetParent(keysGo.transform, false);
                MeshRenderer markRenderer = mark.GetComponent<MeshRenderer>();
                markRenderer.sharedMaterial = over;
                markRenderer.shadowCastingMode = ShadowCastingMode.Off;
                markRenderer.receiveShadows = false;
                mark.SetActive(false);
                keyboard.marks[h] = mark.transform;
                keyboard.markRenderers[h] = markRenderer;
            }
            keyboard.captureSign = CaptureSign(keyboard);
            Apply(keyboard);
            stations.keyboards[s] = keyboard;

            // its tube, to a beam: flat, a little before the glass and the monitor's own box
            GameObject tubeGo = new GameObject("Station " + s + " tube");
            tubeGo.transform.SetParent(place, false);
            tubeGo.transform.localPosition = new Vector3(plan.setX, glassMid, glassZ - 0.012f);
            tubeGo.transform.localScale = new Vector3(glassWide, glassHigh, 1f);
            BoxCollider tube = tubeGo.AddComponent<BoxCollider>();
            tube.size = new Vector3(1f, 1f, 0.004f);
            tube.isTrigger = true;
            tubes[s] = tube;
        }
        // each place's glass draws its tube: the furniture's "Station N rest" has it
        int glazed = 0;
        foreach (MeshRenderer renderer in world.Find("Models").GetComponentsInChildren<MeshRenderer>(true))
        {
            if (!renderer.name.StartsWith("Station ") || !renderer.name.EndsWith(" rest")) continue;
            int s;
            if (!int.TryParse(renderer.name.Substring(8, renderer.name.Length - 13), out s) || s < 0 || s >= count) continue;
            Material[] materials = renderer.sharedMaterials;
            int glass = System.Array.FindIndex(materials, m => m != null && (m.name == "TubeGlass" || m.shader.name == "ShaderEmu/CRT"));
            if (glass < 0) continue;
            materials[glass] = stations.screens[s];
            renderer.sharedMaterials = materials;
            glazed++;
        }
        if (glazed != count) Debug.LogWarning("[ShaderEmu] " + glazed + " of " + count + " stations have a model with glass (world/build.py)");

        // the hubs' ports: a lamp each, lit and flickering as its place's network socket is
        List<Material> hubLamps = new List<Material>();
        List<int> hubPlaces = new List<int>();
        for (int h = 0; plan.hubs != null && h < plan.hubs.Length; h++)
        {
            StationHub hub = plan.hubs[h];
            Transform frame = new GameObject("Hub " + h).transform;
            frame.SetParent(root, false);
            frame.localPosition = new Vector3(hub.x, hub.y, hub.z);
            frame.localEulerAngles = new Vector3(0, hub.turn, 0);
            for (int p = 0; p + 3 < hub.ports.Length; p += 4)
            {
                hubLamps.Add(BlinkLamp(frame, "Hub " + h + " port " + p / 4, new Vector3(hub.ports[p], hub.ports[p + 1], hub.ports[p + 2] - 0.0008f), 0,
                                       hub.lamp[0], hub.lamp[1], green));
                hubPlaces.Add(Mathf.RoundToInt(hub.ports[p + 3]));
            }
        }
        stations.hubLamps = hubLamps.ToArray();
        stations.hubPlaces = hubPlaces.ToArray();
        StationFields(root, stations, theMachine);

        // the den's own tower is the same case: its window and lamps show the machine on the wall
        Gone(computer, "Speed own");
        Gone(computer, "Power lamps");
        Gone(computer, "Disk own");
        Vector3 den = new Vector3(1.55f, 0f, 5.5f - 0.4f + 0.17f - (plan.tower[2] + 0.42f));   // world/computer.py's TOWER, its case's own origin
        stations.ownSpeed = SpeedWindow(computer, "Speed own", den + new Vector3(plan.speed[0], towerHigh - plan.speed[1], plan.tower[2] - 0.0022f), plan.speed[2], plan.speed[3]);
        WallSpeed(computer);

        stations.ownLamps = PowerLamps(computer, plan, false, den + new Vector3(0, 0, plan.tower[2]), 0, 0.0028f);   // the den's tower is modelled whole
        stations.ownDisk = BlinkLamp(computer, "Disk own", den + new Vector3(plan.lamps[2], lampY, plan.tower[2] - 0.0028f), 0, 0.0062f, 0.0024f, red);
        stations.machine = theMachine;
        stations.blackTexture = theMachine.blackTexture;
        stations.click = ClickSource(root);
        stations.clickSound = strokes.Length > 0 ? strokes[0] : null;
        foreach (EmuShareHub hub in world.GetComponentsInChildren<EmuShareHub>(true)) stations.hub = hub;
        foreach (EmuStreams streams in world.GetComponentsInChildren<EmuStreams>(true)) stations.streams = streams;
        foreach (EmuPointer pointer in world.GetComponentsInChildren<EmuPointer>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(pointer);
            int kept = pointer.stationKeysFrom >= 0 ? pointer.stationKeysFrom : pointer.keyboards.Length;   // the machine's own two
            List<EmuKeyboard> all = new List<EmuKeyboard>(pointer.keyboards).GetRange(0, kept);
            List<Collider> allPlates = new List<Collider>(pointer.keyboardPlates).GetRange(0, kept);
            // the den's two keyboards click too
            foreach (EmuKeyboard desk in all)
            {
                UdonSharpEditorUtility.CopyUdonToProxy(desk);
                desk.click = ClickSource(desk.transform);
                desk.strokes = strokes;
                Apply(desk);
            }
            all.AddRange(stations.keyboards);
            allPlates.AddRange(plates);
            pointer.keyboards = all.ToArray();
            pointer.keyboardPlates = allPlates.ToArray();
            pointer.stationKeysFrom = kept;
            pointer.tubes = tubes;
            pointer.tubeAspect = glassWide / glassHigh;
            pointer.tubeBulge = 1.8f;      // world/pc.py's BULGE
            pointer.tubeGap = 0.024f;      // the plate is 12 mm before the glass's rim, the glass's middle 12 mm behind that
            pointer.tubeFill = TubeFill;
            pointer.physicalLabels = physicalLabels.ToArray();
            pointer.steadyLabels = steadyLabels.ToArray();
            TouchPanels(world, pointer);   // the places' keys made anew: a fingertip's list too
            Apply(pointer);
            stations.pointer = pointer;
        }
        Apply(stations);
        foreach (EmuPlace own in placed) Apply(own);
        for (int s = 0; s < count; s++) OnClick(take[s], stations, "Take" + s);
        for (int s = 0; s < count; s++) OnClick(leave[s], placed[s], "Leave");
    }

    // The keys' strokes (world/sounds.py cut them from a recording of an old keyboard).
    static AudioClip[] KeyStrokes()
    {
        List<AudioClip> clips = new List<AudioClip>();
        for (int i = 1; i <= 4; i++)
        {
            AudioClip clip = AssetDatabase.LoadAssetAtPath<AudioClip>(Root + "/Sounds/Key" + i + ".wav");
            if (clip != null) clips.Add(clip);
        }
        return clips.ToArray();
    }

    // Where a key's stroke sounds from: at the thing itself, heard a few metres round it.
    static AudioSource ClickSource(Transform at)
    {
        Transform old = at.Find("Click");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        AudioSource source = new GameObject("Click").AddComponent<AudioSource>();
        source.transform.SetParent(at, false);
        source.playOnAwake = false;
        source.spatialBlend = 1f;
        source.minDistance = 0.4f;
        source.maxDistance = 6f;
        source.rolloffMode = AudioRolloffMode.Linear;
        source.volume = 0.7f;
        Spatial(source, source.minDistance, source.maxDistance);
        return source;
    }
}
