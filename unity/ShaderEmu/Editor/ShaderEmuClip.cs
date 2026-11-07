using System.Collections.Generic;
using TMPro;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

// The text field in front of the screen (docs/system/clipboard.md): EmuClip, its readback's target and material,
// its panel before the wall's display (a classroom place's tube has it before itself: EmuClip.Place), and the
// Clipboard keys of the console's and the display's strips and of a classroom place's (ShaderEmuStations.cs).
public static partial class ShaderEmuBuilder
{
    // Keys made before the field: wired to it when it is made (the console's strip comes before the machine).
    static readonly List<Button> ClipKeys = new List<Button>();

    static Button ClipKey(RectTransform panel, float x, float y, float w, float h, int size)
    {
        TextMeshProUGUI unused;
        Button key = MakeButton(panel, "Clipboard", "Clipboard", x, y, w, h, size, out unused);
        ClipKeys.Add(key);
        return key;
    }

    // The cube before the panel that VRChat's camera is put into for a photo of a file (docs/apps/export.md, "A photo"): one
    // renderer of two submeshes, the cube everyone sees (PhotoCube.shader) and a triangle Photo.shader makes the whole of the
    // camera's picture while the camera is inside the cube. A child of the panel, so it goes with it to a classroom place.
    static void PhotoCube(EmuClip clip, RectTransform panel)
    {
        const float Side = 0.45f;   // metres: a camera held up to the panel fits, a passer-by's head does not reach in by chance
        string path = Generated + "/PhotoCube.asset";
        Mesh mesh = AssetDatabase.LoadAssetAtPath<Mesh>(path);
        if (mesh == null)
        {
            mesh = new Mesh { name = "PhotoCube" };
            AssetDatabase.CreateAsset(mesh, path);
        }
        mesh.Clear();
        var vertices = new List<Vector3>();
        var cube = new List<int>();
        Vector3[] faces = { Vector3.right, Vector3.left, Vector3.up, Vector3.down, Vector3.forward, Vector3.back };
        foreach (Vector3 n in faces)
        {
            Vector3 u = Vector3.Cross(n, Mathf.Abs(n.y) > 0.5f ? Vector3.forward : Vector3.up), v = Vector3.Cross(n, u);
            int b = vertices.Count;
            vertices.Add(0.5f * (n - u - v)); vertices.Add(0.5f * (n + u - v)); vertices.Add(0.5f * (n + u + v)); vertices.Add(0.5f * (n - u + v));
            cube.AddRange(new[] { b, b + 1, b + 2, b, b + 2, b + 3 });
        }
        int t = vertices.Count;   // 24: the triangle's indices are 24 to 26, which Photo.shader takes by their two low bits
        vertices.Add(Vector3.zero); vertices.Add(Vector3.zero); vertices.Add(Vector3.zero);
        mesh.SetVertices(vertices);
        mesh.subMeshCount = 2;
        mesh.SetTriangles(cube, 0);
        mesh.SetTriangles(new[] { t, t + 1, t + 2 }, 1, false);
        mesh.bounds = new Bounds(Vector3.zero, Vector3.one);   // (the cube: a camera inside it sees it, so it is not culled)
        EditorUtility.SetDirty(mesh);

        Transform old = panel.Find("Photo cube");
        if (old != null) Object.DestroyImmediate(old.gameObject);
        GameObject go = new GameObject("Photo cube");
        go.transform.SetParent(panel, false);
        go.transform.localPosition = new Vector3(0, 0, -Side * 0.5f / panel.localScale.z - 30f);   // (just before the panel's face)
        go.transform.localRotation = Quaternion.identity;
        go.transform.localScale = Vector3.one * (Side / panel.localScale.x);
        go.AddComponent<MeshFilter>().sharedMesh = mesh;
        MeshRenderer r = go.AddComponent<MeshRenderer>();
        Material photo = Mat("Photo", "ShaderEmu/Photo");
        r.sharedMaterials = new[] { Mat("PhotoCube", "ShaderEmu/PhotoCube"), photo };
        r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        r.receiveShadows = false;
        r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
        r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
        r.allowOcclusionWhenDynamic = false;
        r.enabled = false;
        clip.photo = r;
        clip.photoMaterial = photo;
    }

    static void Clip(Transform computer, EmuMachine machine, Vector3 dispAt)
    {
        Gone(computer, "Clip panel");
        Gone(machine.transform.parent, "Clip");
        Color dim = new Color(0.62f, 0.68f, 0.76f), amber = new Color(1f, 0.8f, 0.3f);
        TextMeshProUGUI unused;
        EmuClip clip = Udon<EmuClip>(new GameObject("Clip"));
        clip.transform.SetParent(machine.transform.parent, false);
        clip.machine = machine;
        clip.readMaterial = Mat("ClipRead", "ShaderEmu/PersistRead");
        clip.readTexture = Target("ClipRead", 256, 256, 0);

        // (a hair before the links panel, which may be open too)
        RectTransform panel = Panel(computer, "Clip panel", dispAt + new Vector3(0, 0, -0.32f), Vector3.zero, 1700, 900,
                                    new Color(0.05f, 0.055f, 0.07f, 0.97f));
        clip.titleLabel = Label(panel, "Title", "", 30, 14, 1420, 70, 24, TextAnchor.MiddleLeft, Color.white);
        OnClick(MakeButton(panel, "Close", "Close", 1490, 16, 180, 70, 26, out unused), clip, "Close");
        RectTransform box = Child(panel, "Field", 30, 96, 1640, 700);
        Plate(box, new Color(0.14f, 0.16f, 0.22f));
        Text text = Child(box, "Text", 12, 8, 1616, 684).gameObject.AddComponent<Text>();
        text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
        text.material = UiMaterial();
        text.fontSize = 16;
        text.color = Color.white;
        text.alignment = TextAnchor.UpperLeft;
        text.horizontalOverflow = HorizontalWrapMode.Wrap;
        text.verticalOverflow = VerticalWrapMode.Truncate;
        text.supportRichText = false;
        InputField field = box.gameObject.AddComponent<InputField>();
        field.textComponent = text;
        field.targetGraphic = box.GetComponent<Image>();
        field.lineType = InputField.LineType.MultiLineNewline;
        field.characterLimit = 0;
        clip.field = field;
        Button back = MakeButton(panel, "Last page", "< Page", 30, 810, 180, 70, 26, out unused);
        Button on = MakeButton(panel, "Next page", "Page >", 220, 810, 180, 70, 26, out unused);
        OnClick(back, clip, "LastPage");
        OnClick(on, clip, "NextPage");
        clip.pageLabel = Label(panel, "Page", "", 420, 810, 560, 70, 22, TextAnchor.MiddleLeft, dim);
        clip.giveLabel = Label(panel, "Given", "", 990, 810, 420, 70, 20, TextAnchor.MiddleRight, amber);
        Button give = MakeButton(panel, "Give", "To the machine", 1430, 810, 240, 70, 26, out unused);
        OnClick(give, clip, "Give");
        clip.panel = panel.gameObject;
        PhotoCube(clip, panel);
        clip.backButton = back.gameObject;
        clip.onButton = on.gameObject;
        clip.giveButton = give.gameObject;
        Apply(clip);
        machine.clip = clip;
        Apply(machine);
        foreach (Button key in ClipKeys)
            if (key != null) OnClick(key, clip, "ShowBoard");
        ClipKeys.Clear();
    }
}
