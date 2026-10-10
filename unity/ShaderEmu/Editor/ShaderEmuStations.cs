using System.Collections.Generic;
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.UI;

// The classroom in the den (world/furniture.py, docs/stations.md): eight computers on four wooden
// tables, one a visitor. The models are the furniture's; this gives each one's glass its tube,
// its keyboard and tube their beams' plates, the keys modelled on its monitor and tower their
// work, and its tower's window the speed of its owner's machine.
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
    class StationsPlan
    {
        public StationPlace[] places;
        public float table, keyScale, keyboardZ, setX, turbo, reset;
        public float[] glass;          // wide, high, its middle's height, its front
        public float[] tower;          // x, z, its front in its own frame, its height
        public float[] speed;          // the speed's window: x, down from the top, wide, high
        public float[] power;          // the power key: x, down from the top
        public float[] osd;            // the monitor's six keys: the first's x, their pitch, their height
        public float led;              // the monitor's lamp: x
        public float[] lamps;          // the tower's three: x of power, turbo, disk
    }

    const float KeyboardHigh = 0.016f, KeyboardSlope = 7f, KeyboardTops = 0.0225f;   // world/pc.py's keyboard()
    const float TowerBlankBay = 0.36f - 0.088f;   // the tower's spare bay

    static StationsPlan ReadStations()
    {
        return JsonUtility.FromJson<StationsPlan>(System.IO.File.ReadAllText(ModelsPath + "/stations.json"));
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

    // The lamps that are lit while a machine is on: the monitor's (`monitor`: its place, or none)
    // and the tower's power lamp, a hair before the model's own dull lenses.
    static GameObject PowerLamps(Transform parent, StationsPlan plan, bool monitor, Vector3 tower, float glassZ)
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
                lamp.transform.localPosition = new Vector3(plan.setX + plan.led, plan.osd[2], glassZ - 0.0021f);
                lamp.transform.localScale = new Vector3(0.0066f, 0.0066f, 0.0016f);
            }
            else
            {
                lamp.transform.localPosition = tower + new Vector3(plan.lamps[0], plan.tower[3] - plan.speed[1] + 0.005f, -0.0028f);
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

    static void Stations(Transform world)
    {
        StationsPlan plan = ReadStations();
        float glassWide = plan.glass[0], glassHigh = plan.glass[1], glassMid = plan.glass[2], glassZ = plan.glass[3];
        float towerX = plan.tower[0], towerFront = plan.tower[1] + plan.tower[2], towerHigh = plan.tower[3];
        float keyUnit = plan.keyScale * 0.01905f / 60f;
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
        foreach (EmuTerminal terminal in world.GetComponentsInChildren<EmuTerminal>(true)) stations.terminal = terminal;
        stations.ownConsole = PictureTexture("StationOwnConsole", 640, 480);
        Button[] take = new Button[count], keys = new Button[count];
        TextMeshProUGUI[] captions = new TextMeshProUGUI[count];
        // a place's keyboard is the console's own layout at the model's size, and its tube a pointer's plate
        EmuMachine theMachine = null;
        foreach (EmuMachine machine in world.GetComponentsInChildren<EmuMachine>(true)) theMachine = machine;
        UdonSharpEditorUtility.CopyUdonToProxy(theMachine);
        EmuKeyboard layout = theMachine.consoleKeyboard;
        UdonSharpEditorUtility.CopyUdonToProxy(layout);
        List<int> real = new List<int>();
        for (int i = 0; i < layout.keyLinux.Length; i++)
            if (layout.keyLinux[i] != EmuKeyboard.LinuxCapture) real.Add(i);
        Material over = Mat("Station key over", "Sprites/Default"), held = Mat("Station key held", "Sprites/Default");
        over.color = new Color(1f, 0.75f, 0.2f, 0.35f);
        held.color = new Color(0.2f, 0.55f, 1f, 0.7f);
        Material lamp = Mat("Links lamp", "Unlit/Color");
        lamp.color = new Color(1f, 0.55f, 0.1f) * 3f;   // (lit: brighter than white, it blooms)
        AudioClip[] strokes = KeyStrokes();
        stations.keyboards = new EmuKeyboard[count];
        Collider[] tubes = new Collider[count], plates = new Collider[count];
        // a table stops a visitor (the tables stand as world/furniture.py has them)
        foreach (float z in new[] { 1.5f, -0.9f })
            foreach (float x in new[] { -1f, 1f })
                Solid(root, "Table", x - 1f, x + 1f, 0, plan.table, z - 0.45f, z + 0.45f);
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
            screen.SetFloat("_Triads", 880f);   // stripes 0.60 mm apart at the middle, 0.78 in the corners: 549 mm of them
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

            // whose it is, on a key in the tower's spare bay
            RectTransform panel = Panel(place, "Station " + s + " panel", new Vector3(towerX, TowerBlankBay, towerFront - 0.003f), Vector3.zero,
                                        144, 38, new Color(0.05f, 0.055f, 0.07f));
            take[s] = MakeButton(panel, "Take", "FREE", 2, 2, 96, 34, 18, out stations.labels[s]);
            keys[s] = MakeButton(panel, "Keys", "Use my\nkeyboard", 100, 2, 42, 34, 7, out captions[s]);
            // the name above, what a press does below it, both the key's own lettering
            stations.labels[s].alignment = TextAlignmentOptions.Top;
            stations.labels[s].fontSize = 14;
            stations.labels[s].margin = new Vector4(0, 3, 0, 0);
            stations.modes[s] = Label((RectTransform)take[s].transform, "Mode", "press to sit here", 0, 21, 96, 11, 6, TextAnchor.MiddleCenter, stations.labels[s].color);
            stations.modes[s].raycastTarget = false;

            // the tower's own keys (turbo, reset; power under them), larger to a beam than their caps
            float panelY = towerHigh - plan.speed[1], powerY = towerHigh - plan.power[1];
            HotKeys(place, "Station " + s + " tower hot", new Vector3(towerX + 0.012f, panelY, towerFront - 0.004f), 34, 30, own,
                    new float[] { 1, 2, 15, 26, 18, 2, 15, 26 }, new[] { "Turbo", "ResetKey" });
            HotKeys(place, "Station " + s + " power hot", new Vector3(towerX + plan.power[0], powerY, towerFront - 0.008f), 42, 32, own,
                    new float[] { 1, 1, 40, 30 }, new[] { "Power" });
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
            links.transform.localPosition = new Vector3(plan.setX + plan.osd[0] + 5f * plan.osd[1], plan.osd[2] + 0.011f, glassZ - 0.0045f);
            links.transform.localScale = new Vector3(0.014f, 0.004f, 1f);
            links.GetComponent<MeshRenderer>().sharedMaterial = lamp;
            links.GetComponent<MeshRenderer>().shadowCastingMode = ShadowCastingMode.Off;
            links.SetActive(false);
            stations.linkLamps[s] = links;
            stations.powerLamps[s] = PowerLamps(place, plan, true, new Vector3(towerX, 0, towerFront), glassZ);
            Transform spot = new GameObject("Links spot").transform;
            spot.SetParent(place, false);
            spot.localPosition = new Vector3(plan.setX, glassMid, glassZ - 0.06f);
            stations.linkSpots[s] = spot;

            // the speed's window on the tower: every core's, and core 0's
            stations.speeds[s] = SpeedWindow(place, "Speed " + s, new Vector3(towerX + plan.speed[0], panelY, towerFront - 0.0022f), plan.speed[2], plan.speed[3]);

            // its keys: the layout's rectangles over the caps' tops, a beam's plate, a mark a hand
            GameObject keysGo = new GameObject("Station " + s + " keyboard");
            keysGo.transform.SetParent(place, false);
            Quaternion lie = Quaternion.Euler(0f, where.keyboard[2], 0f) * Quaternion.Euler(90f - KeyboardSlope, 0f, 0f);
            keysGo.transform.localPosition = new Vector3(plan.setX + where.keyboard[0], KeyboardHigh, plan.keyboardZ + where.keyboard[1]) + lie * new Vector3(0f, 0f, -KeyboardTops);
            keysGo.transform.localRotation = lie;
            keysGo.transform.localScale = Vector3.one * keyUnit;
            BoxCollider plate = keysGo.AddComponent<BoxCollider>();
            plate.size = new Vector3(layout.panelWidth + 60f, layout.panelHeight + 60f, 6f);   // half a key beyond the outer rows: no gap to miss into
            plate.isTrigger = true;
            plates[s] = plate;
            EmuKeyboard keyboard = Udon<EmuKeyboard>(keysGo);
            keyboard.panelWidth = layout.panelWidth;
            keyboard.panelHeight = layout.panelHeight;
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
            keyboard.captureLabel = captions[s];
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

        // the den's own tower is the same case: its window shows this visitor's machine
        Gone(computer, "Speed own");
        Gone(computer, "Power lamps");
        Vector3 den = new Vector3(1.55f, 0f, 5.5f - 0.4f + 0.17f - (plan.tower[2] + 0.42f));   // world/computer.py's TOWER, its case's own origin
        stations.ownSpeed = SpeedWindow(computer, "Speed own", den + new Vector3(plan.speed[0], towerHigh - plan.speed[1], plan.tower[2] - 0.0022f), plan.speed[2], plan.speed[3]);

        stations.ownLamps = PowerLamps(computer, plan, false, den + new Vector3(0, 0, plan.tower[2]), 0);
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
            Apply(pointer);
            stations.pointer = pointer;
        }
        Apply(stations);
        foreach (EmuPlace own in placed) Apply(own);
        for (int s = 0; s < count; s++) OnClick(take[s], stations, "Take" + s);
        for (int s = 0; s < count; s++) OnClick(keys[s], stations, "Keys" + s);
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
        return source;
    }
}
