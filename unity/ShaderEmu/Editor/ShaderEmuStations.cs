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
    const float StationDeskTop = 0.765f, StationSetZ = 0.06f, StationGlassWide = 0.576f, StationGlassHigh = 0.436f;
    const float TowerX = 0.38f, TowerFront = 0.05f - 0.15f, TowerBlankBay = 0.43f - 0.088f;   // the tower's place, its front, its spare bay

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
        stations.decodeMaterial = Mat("StationDecode", "ShaderEmu/ShareDecode");
        foreach (EmuTerminal terminal in world.GetComponentsInChildren<EmuTerminal>(true)) stations.terminal = terminal;
        Material terminalMat = AssetDatabase.LoadAssetAtPath<Material>(Generated + "/Terminal.mat");
        Material consoleMat = Mat("StationConsole", "ShaderEmu/Terminal");
        consoleMat.CopyPropertiesFromMaterial(terminalMat);   // the font, the columns and rows
        stations.consoleMaterial = consoleMat;
        int cols = stations.terminal.cols, rows = stations.terminal.rows;
        Button[] take = new Button[count];
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
                Solid(root, "Monitor", at.x - 0.11f - 0.372f, at.x - 0.11f + 0.372f, at.y, at.y + 0.8f, at.z - 0.12f, at.z + 0.36f);
                Solid(root, "Tower", at.x + 0.38f - 0.1f, at.x + 0.38f + 0.1f, at.y, at.y + 0.44f, at.z - 0.1f, at.z + 0.32f);
                Material screen = Mat("Station " + s, "ShaderEmu/CRT");
                screen.SetFloat("_Aspect", StationGlassWide / StationGlassHigh);
                screen.SetFloat("_Curve", 0f);   // the glass is the model's, and bulges itself
                screen.SetColor("_Off", new Color(0.045f, 0.05f, 0.047f));
                screen.SetFloat("_Glossiness", 0.86f);
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
                take[s] = MakeButton(panel, "Take", "FREE", 2, 2, 140, 34, 18, out stations.labels[s]);
            }
        }
        // each model's glass draws its place's tube
        int glazed = 0;
        foreach (MeshRenderer renderer in world.Find("Models").GetComponentsInChildren<MeshRenderer>(true))
        {
            Material[] materials = renderer.sharedMaterials;
            int glass = System.Array.FindIndex(materials, m => m != null && (m.name == "TubeGlass" || m.shader.name == "ShaderEmu/CRT"));
            int number;
            if (glass < 0 || !renderer.name.StartsWith("Station ") || !int.TryParse(renderer.name.Substring(8), out number) || number >= count) continue;
            materials[glass] = stations.screens[number];
            renderer.sharedMaterials = materials;
            glazed++;
        }
        if (glazed != count) Debug.LogWarning("[ShaderEmu] " + glazed + " of " + count + " stations have a model with glass (world/pc.py, world/build.py)");
        foreach (EmuMachine machine in world.GetComponentsInChildren<EmuMachine>(true)) stations.machine = machine;
        foreach (EmuShareHub hub in world.GetComponentsInChildren<EmuShareHub>(true))
        {
            stations.hub = hub;
            UdonSharpEditorUtility.CopyUdonToProxy(hub);
            hub.stations = stations;
            Apply(hub);
        }
        Apply(stations);
        for (int s = 0; s < count; s++) OnClick(take[s], stations, "Take" + s);
    }
}
