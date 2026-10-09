using System.Collections.Generic;
using TMPro;
using UdonSharpEditor;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

// The classroom in the den (world/furniture.py, docs/stations.md): eight computers on four wooden
// tables, one a visitor. The models are the furniture's; this gives each one's glass its tube,
// the textures its owner's display and console are drawn into, and the key that takes a free
// one (and, at a visitor's own, turns the tube between display and console).
public static partial class ShaderEmuBuilder
{
    // as world/furniture.py and world/pc.py
    static readonly float[] StationRowsZ = { 1.5f, -0.9f }, StationPlacesX = { -1.5f, -0.5f, 0.5f, 1.5f };
    const float StationDeskTop = 0.765f, StationSetZ = 0f, StationGlassWide = 0.549f, StationGlassHigh = 0.411f;
    // world/pc.py: SET_X, GLASS's middle and face, the keyboard's place, slope, key pitch and how far its caps' tops stand off
    const float StationSetX = -0.11f, StationGlassMid = 0.3625f, StationGlassZ = -0.14f;
    const float KeyboardHigh = 0.016f, KeyboardZ = -0.31f, KeyboardSlope = 7f, KeyUnit = 0.01905f / 60f, KeyboardTops = 0.0225f;
    const float TowerX = 0.38f, TowerFront = 0.05f - 0.15f, TowerBlankBay = 0.36f - 0.088f;   // the tower's place, its front, its spare bay

    static void Stations(Transform world)
    {
        Transform decor = world.Find("Decor");
        foreach (string gone in new[] { "Round table top", "Round table post", "Stool seat" }) Gone(decor, gone);
        Gone(world, "Stations");
        Transform root = new GameObject("Stations").transform;
        root.SetParent(world, false);
        EmuStations stations = Udon<EmuStations>(root.gameObject);
        int count = StationRowsZ.Length * StationPlacesX.Length;
        stations.screens = new Material[count];
        stations.stores = new Texture2D[count];
        stations.pictures = new RenderTexture[count];
        stations.grids = new Texture2D[count];
        stations.consoles = new RenderTexture[count];
        stations.labels = new TextMeshProUGUI[count];
        stations.modes = new TextMeshProUGUI[count];
        stations.decodeMaterial = Mat("StationDecode", "ShaderEmu/ShareDecode");
        foreach (EmuTerminal terminal in world.GetComponentsInChildren<EmuTerminal>(true)) stations.terminal = terminal;
        Material terminalMat = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/Terminal.mat");
        Material consoleMat = Mat("StationConsole", "ShaderEmu/Terminal");
        consoleMat.CopyPropertiesFromMaterial(terminalMat);   // the font, the columns and rows
        stations.consoleMaterial = consoleMat;
        int cols = stations.terminal.cols, rows = stations.terminal.rows;
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
        stations.keyboards = new EmuKeyboard[count];
        stations.consoleKeys = theMachine.consoleKeyboard;
        stations.displayKeys = theMachine.gpuKeyboard;
        Collider[] tubes = new Collider[count], plates = new Collider[count];
        const int rowTexels = 128, rowCount = 1500;   // as the share's store (ShaderEmuShare.cs)
        for (int row = 0; row < StationRowsZ.Length; row++)
        {
            foreach (float x in new[] { -1f, 1f })   // a table stops a visitor
                Solid(root, "Table", x - 1f, x + 1f, 0, StationDeskTop, StationRowsZ[row] - 0.45f, StationRowsZ[row] + 0.45f);
            for (int place = 0; place < StationPlacesX.Length; place++)
            {
                int s = row * StationPlacesX.Length + place;
                Vector3 at = new Vector3(StationPlacesX[place], StationDeskTop, StationRowsZ[row] + StationSetZ);
                // the monitor and the tower stop a visitor's hand, and a beam meant for the wall's display
                Solid(root, "Monitor", at.x - 0.11f - 0.3225f, at.x - 0.11f + 0.3225f, at.y, at.y + 0.61f, at.z - 0.14f, at.z + 0.39f);
                Solid(root, "Tower", at.x + 0.38f - 0.092f, at.x + 0.38f + 0.092f, at.y, at.y + 0.37f, at.z - 0.1f, at.z + 0.32f);
                Material screen = Mat("Station " + s, "ShaderEmu/CRT");
                screen.SetFloat("_Aspect", StationGlassWide / StationGlassHigh);
                screen.SetFloat("_Curve", 0f);   // the glass is the model's, and bulges itself
                screen.SetFloat("_Triads", 880f);   // stripes 0.60 mm apart at the middle, 0.78 in the corners: 549 mm of them
                screen.SetColor("_Off", new Color(0.045f, 0.05f, 0.047f));
                screen.SetFloat("_Glossiness", 0.84f);   // the television's glass is 0.89 to 0.72 smooth; this 0.84 to 0.71
                screen.SetTexture("_Smudge", WorldTexture("Smudges"));   // the television's own film, a little thinner
                screen.SetFloat("_Smudged", 0.16f);
                screen.SetFloat("_Film", 0.6f);
                screen.SetVector("_Size", Vector4.zero);
                stations.screens[s] = screen;

                int index = s;
                Texture2D store = LoadOrCreate(Generated + "/StationStore" + s + ".asset", () => new Texture2D(rowTexels, rowCount, TextureFormat.RGBA32, false, true));
                store.filterMode = FilterMode.Point;
                store.wrapMode = TextureWrapMode.Clamp;
                stations.stores[s] = store;
                stations.pictures[s] = PictureTexture("StationPicture" + index, 1280, 960);
                Texture2D grid = LoadOrCreate(Generated + "/StationGrid" + s + ".asset", () => new Texture2D(cols, rows, TextureFormat.RGBA32, false, true));
                grid.filterMode = FilterMode.Point;
                grid.wrapMode = TextureWrapMode.Clamp;
                stations.grids[s] = grid;
                stations.consoles[s] = PictureTexture("StationConsole" + index, 640, 480);

                // whose it is, on a key in the tower's spare bay
                RectTransform panel = Panel(root, "Station " + s + " panel", at + new Vector3(TowerX, TowerBlankBay, TowerFront - 0.003f), Vector3.zero,
                                            144, 38, new Color(0.05f, 0.055f, 0.07f));
                take[s] = MakeButton(panel, "Take", "FREE", 2, 2, 96, 34, 18, out stations.labels[s]);
                keys[s] = MakeButton(panel, "Keys", "Use my\nkeyboard", 100, 2, 42, 34, 7, out captions[s]);
                // the name above, what a press does below it, both the key's own lettering
                stations.labels[s].alignment = TextAlignmentOptions.Top;
                stations.labels[s].fontSize = 14;
                stations.labels[s].margin = new Vector4(0, 3, 0, 0);
                stations.modes[s] = Label((RectTransform)take[s].transform, "Mode", "press to sit here", 0, 21, 96, 11, 6, TextAnchor.MiddleCenter, stations.labels[s].color);
                stations.modes[s].raycastTarget = false;

                // its keys: the layout's rectangles over the caps' tops, a beam's plate, a mark a hand
                GameObject keysGo = new GameObject("Station " + s + " keyboard");
                keysGo.transform.SetParent(root, false);
                Quaternion lie = Quaternion.Euler(90f - KeyboardSlope, 0f, 0f);
                keysGo.transform.localPosition = at + new Vector3(StationSetX, KeyboardHigh, KeyboardZ) + lie * new Vector3(0f, 0f, -KeyboardTops);
                keysGo.transform.localRotation = lie;
                keysGo.transform.localScale = Vector3.one * KeyUnit;
                BoxCollider plate = keysGo.AddComponent<BoxCollider>();
                plate.size = new Vector3(layout.panelWidth, layout.panelHeight, 6f);
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
                for (int h = 0; h < 2; h++)
                {
                    GameObject mark = GameObject.CreatePrimitive(PrimitiveType.Quad);
                    mark.name = "Mark " + h;
                    Object.DestroyImmediate(mark.GetComponent<Collider>());
                    mark.transform.SetParent(keysGo.transform, false);
                    MeshRenderer markRenderer = mark.GetComponent<MeshRenderer>();
                    markRenderer.sharedMaterial = over;
                    markRenderer.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
                    markRenderer.receiveShadows = false;
                    mark.SetActive(false);
                    keyboard.marks[h] = mark.transform;
                    keyboard.markRenderers[h] = markRenderer;
                }
                Apply(keyboard);
                stations.keyboards[s] = keyboard;

                // its tube, to a beam: flat, a little before the glass and the monitor's own box
                GameObject tubeGo = new GameObject("Station " + s + " tube");
                tubeGo.transform.SetParent(root, false);
                tubeGo.transform.localPosition = at + new Vector3(StationSetX, StationGlassMid, StationGlassZ - 0.012f);
                tubeGo.transform.localScale = new Vector3(StationGlassWide, StationGlassHigh, 1f);
                BoxCollider tube = tubeGo.AddComponent<BoxCollider>();
                tube.size = new Vector3(1f, 1f, 0.004f);
                tube.isTrigger = true;
                tubes[s] = tube;
            }
        }
        // each model's glass draws its place's tube
        int glazed = 0;
        foreach (MeshRenderer renderer in world.Find("Models").GetComponentsInChildren<MeshRenderer>(true))
        {
            Material[] materials = renderer.sharedMaterials;
            int glass = System.Array.FindIndex(materials, m => m != null && (m.name == "TubeGlass" || m.shader.name == "ShaderEmu/CRT"));
            if (glass < 0) continue;
            Vector3 p = world.InverseTransformPoint(renderer.transform.position);   // the set's own origin is its place on the table
            int row = Mathf.Abs(p.z - StationRowsZ[0]) < Mathf.Abs(p.z - StationRowsZ[1]) ? 0 : 1, place = 0;
            for (int i = 1; i < StationPlacesX.Length; i++)
                if (Mathf.Abs(p.x - StationPlacesX[i]) < Mathf.Abs(p.x - StationPlacesX[place])) place = i;
            materials[glass] = stations.screens[row * StationPlacesX.Length + place];
            renderer.sharedMaterials = materials;
            glazed++;
        }
        if (glazed != count) Debug.LogWarning("[ShaderEmu] " + glazed + " of " + count + " stations have a model with glass (world/bake_pc.py, world/build.py)");
        foreach (EmuMachine machine in world.GetComponentsInChildren<EmuMachine>(true)) stations.machine = machine;
        foreach (EmuShareHub hub in world.GetComponentsInChildren<EmuShareHub>(true))
        {
            stations.hub = hub;
            UdonSharpEditorUtility.CopyUdonToProxy(hub);
            hub.stations = stations;
            Apply(hub);
        }
        foreach (EmuPointer pointer in world.GetComponentsInChildren<EmuPointer>(true))
        {
            UdonSharpEditorUtility.CopyUdonToProxy(pointer);
            int kept = pointer.stationKeysFrom >= 0 ? pointer.stationKeysFrom : pointer.keyboards.Length;   // the machine's own two
            List<EmuKeyboard> all = new List<EmuKeyboard>(pointer.keyboards).GetRange(0, kept);
            List<Collider> allPlates = new List<Collider>(pointer.keyboardPlates).GetRange(0, kept);
            all.AddRange(stations.keyboards);
            allPlates.AddRange(plates);
            pointer.keyboards = all.ToArray();
            pointer.keyboardPlates = allPlates.ToArray();
            pointer.stationKeysFrom = kept;
            pointer.tubes = tubes;
            pointer.tubeAspect = StationGlassWide / StationGlassHigh;
            pointer.tubeBulge = 1.8f;      // world/pc.py's BULGE
            pointer.tubeGap = 0.024f;      // the plate is 12 mm before the glass's rim, the glass's middle 12 mm behind that
            Apply(pointer);
            stations.pointer = pointer;
        }
        Apply(stations);
        for (int s = 0; s < count; s++) OnClick(take[s], stations, "Take" + s);
        for (int s = 0; s < count; s++) OnClick(keys[s], stations, "Keys" + s);
    }
}
