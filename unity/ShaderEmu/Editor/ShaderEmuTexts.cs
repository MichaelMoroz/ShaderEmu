using System.Collections.Generic;
using System.Text;
using TMPro;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

// Texts over each other (docs/world/world.md, "Texts that overlap"): every text of every panel of the open scene, those
// switched off or empty when the world is built too (a key's label, a line a behaviour fills in later), measured where it
// is drawn and compared with every other text of its canvas. A text is its letters' box as its font lays them out in its
// rectangle (wrapped to its width, aligned as it is aligned, past its rectangle where it overflows); an empty one, its
// whole rectangle, as what a behaviour writes there may fill it. A field's placeholder and its text are one place by design.
// A text is also compared with every other graphic of its canvas that is drawn (a key, a field, a strip): one it lies across
// part of is an overlap; one it lies wholly inside is its ground, and one that holds it (its own key) is its own.
// Panels apart are compared too where they lie in nearly one plane (within 5 cm of each other's plane, facing within 15
// degrees): a text of one over a text or a graphic of the other (a sign before a panel's help, two panels that open at one
// place). A panel's baked plate is skipped: its picture is of the panel's own graphics, which are compared themselves.
public static class ShaderEmuTexts
{
    const float Edge = 2f;    // canvas units: boxes that meet by this much or less (a row's line under the next) only touch

    // Panels never shown together, and why (each pair either way round; a name ending "*" is any holodeck's).
    static readonly string[][] Apart = {
        new[] { "Holodeck * plate", "Holodeck * show", "a sitter's console or, put away, its button: EmuHolodeck.Arrange shows one" },
        new[] { "Display keyboard", "Holodeck keyboard", "the holodeck's is on only for a sitter, at their screen: EmuHolodeck.PlaceKeyboard" },
        new[] { "Controller notice", "*", "it follows the hand that holds a controller: EmuGamepad" },
        new[] { "Clip panel", "Links panel", "one place, one at a time: EmuClip.Show closes the links, EmuMachine.PlaceLinks the field (8 November 2026)" },
    };

    static bool Never(string a, string b)
    {
        foreach (string[] pair in Apart)
            if ((Like(a, pair[0]) && Like(b, pair[1])) || (Like(b, pair[0]) && Like(a, pair[1]))) return true;
        return false;
    }

    static bool Like(string name, string pattern)
    {
        if (pattern == "*") return true;
        int star = pattern.IndexOf('*');
        if (star < 0) return name == pattern;
        return name.StartsWith(pattern.Substring(0, star)) && name.EndsWith(pattern.Substring(star + 1));
    }

    // The texts the world's behaviours hold: an empty one of those is written while the world runs; another stays empty.
    static HashSet<Object> Written()
    {
        var held = new HashSet<Object>();
        foreach (UdonSharp.UdonSharpBehaviour b in Object.FindObjectsOfType<UdonSharp.UdonSharpBehaviour>(true))
            foreach (System.Reflection.FieldInfo f in b.GetType().GetFields(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Instance))
            {
                object v = f.GetValue(b);
                if (v is TMP_Text || v is Text) held.Add((Object)v);
                else if (v is TMP_Text[]) foreach (TMP_Text t in (TMP_Text[])v) { if (t != null) held.Add(t); }
                else if (v is Text[]) foreach (Text t in (Text[])v) { if (t != null) held.Add(t); }
                else if (v is InputField && ((InputField)v).textComponent != null) held.Add(((InputField)v).textComponent);
            }
        foreach (InputField f in Object.FindObjectsOfType<InputField>(true)) if (f.textComponent != null) held.Add(f.textComponent);   // (typed into)
        return held;
    }

    [MenuItem("ShaderEmu/Check texts over each other in the open scene")]
    public static void Menu()
    {
        int pairs;
        string text = Check(out pairs);
        if (pairs == 0) Debug.Log(text); else Debug.LogError(text);
    }

    struct Placed { public Graphic graphic; public Rect box; public bool empty, shown; public string path; }

    struct World { public Graphic graphic; public Vector3[] corners; public bool text, shown, empty; public string what; public Canvas canvas; }

    public static string Check(out int pairs)
    {
        var report = new StringBuilder();
        pairs = 0;
        int texts = 0, canvases = 0;
        var all = new List<World>();
        HashSet<Object> written = Written();
        foreach (Canvas canvas in Object.FindObjectsOfType<Canvas>(true))
        {
            if (canvas.transform.parent != null && canvas.transform.parent.GetComponentInParent<Canvas>(true) != null) continue;   // a nested one is its root's
            var placed = new List<Placed>();
            var grounds = new List<Placed>();
            foreach (Graphic g in canvas.GetComponentsInChildren<Graphic>(true))
            {
                if (!(g is TMP_Text) && !(g is Text))
                {
                    if (g.color.a < 0.05f || g is TMP_SubMeshUI) continue;   // (unseen: a beam's target)
                    if (g.name.EndsWith(" plate")) continue;
                    Placed q;
                    if (Box(canvas, g, out q)) grounds.Add(q);
                    continue;
                }
                Placed p;
                if (!Measure(canvas, g, out p)) continue;
                if (p.empty && !written.Contains(g)) continue;   // (empty, and nothing writes it)
                placed.Add(p);
            }
            foreach (Placed q in placed) all.Add(Corners(canvas, q, true));
            foreach (Placed q in grounds) all.Add(Corners(canvas, q, false));
            if (placed.Count == 0) continue;
            canvases++;
            texts += placed.Count;
            var placeholders = new HashSet<Graphic>();
            foreach (InputField f in canvas.GetComponentsInChildren<InputField>(true)) if (f.placeholder != null) placeholders.Add(f.placeholder);
            foreach (TMP_InputField f in canvas.GetComponentsInChildren<TMP_InputField>(true)) if (f.placeholder != null) placeholders.Add(f.placeholder);
            for (int i = 0; i < placed.Count; i++)
                for (int j = i + 1; j < placed.Count; j++)
                {
                    Placed a = placed[i], b = placed[j];
                    if (placeholders.Contains(a.graphic) || placeholders.Contains(b.graphic)) continue;
                    if (a.graphic.transform.IsChildOf(b.graphic.transform) || b.graphic.transform.IsChildOf(a.graphic.transform)) continue;
                    float w = Mathf.Min(a.box.xMax, b.box.xMax) - Mathf.Max(a.box.xMin, b.box.xMin);
                    float h = Mathf.Min(a.box.yMax, b.box.yMax) - Mathf.Max(a.box.yMin, b.box.yMin);
                    if (w <= Edge || h <= Edge) continue;
                    pairs++;
                    report.AppendLine($"  {canvas.name}: \"{Said(a)}\" ({a.path}) over \"{Said(b)}\" ({b.path}), {w:0} x {h:0}");
                }
            foreach (Placed a in placed)
                foreach (Placed b in grounds)
                {
                    if (a.graphic.transform.IsChildOf(b.graphic.transform) || b.graphic.transform.IsChildOf(a.graphic.transform)) continue;
                    float w = Mathf.Min(a.box.xMax, b.box.xMax) - Mathf.Max(a.box.xMin, b.box.xMin);
                    float h = Mathf.Min(a.box.yMax, b.box.yMax) - Mathf.Max(a.box.yMin, b.box.yMin);
                    if (w <= Edge || h <= Edge) continue;
                    bool inside = a.box.xMin >= b.box.xMin - 0.5f && a.box.xMax <= b.box.xMax + 0.5f && a.box.yMin >= b.box.yMin - 0.5f && a.box.yMax <= b.box.yMax + 0.5f;
                    if (inside) continue;   // (its ground)
                    pairs++;
                    report.AppendLine($"  {canvas.name}: \"{Said(a)}\" ({a.path}) across {b.graphic.GetType().Name} {b.path}{(b.shown ? "" : ", off at build")}, {w:0} x {h:0}");
                }
        }
        // panels apart, in nearly one plane
        for (int i = 0; i < all.Count; i++)
        {
            if (!all[i].text) continue;
            World a = all[i];
            Vector3 n = Vector3.Cross(a.corners[1] - a.corners[0], a.corners[3] - a.corners[0]).normalized;
            for (int j = 0; j < all.Count; j++)
            {
                World b = all[j];
                if (b.canvas == a.canvas || (b.text && j < i) || Never(a.canvas.name, b.canvas.name)) continue;
                Vector3 m = Vector3.Cross(b.corners[1] - b.corners[0], b.corners[3] - b.corners[0]).normalized;
                if (Mathf.Abs(Vector3.Dot(n, m)) < Mathf.Cos(15f * Mathf.Deg2Rad)) continue;
                if (Mathf.Abs(Vector3.Dot(b.corners[0] - a.corners[0], n)) > 0.05f) continue;
                // b's corners in a's plane, as a's own axes (x along its bottom edge, y up its side)
                Vector3 o = a.corners[0], ux = (a.corners[3] - a.corners[0]), uy = (a.corners[1] - a.corners[0]);
                float lx = ux.magnitude, ly = uy.magnitude;
                ux /= lx; uy /= ly;
                float minx = float.MaxValue, maxx = float.MinValue, miny = float.MaxValue, maxy = float.MinValue;
                foreach (Vector3 c in b.corners)
                {
                    float x = Vector3.Dot(c - o, ux), y = Vector3.Dot(c - o, uy);
                    minx = Mathf.Min(minx, x); maxx = Mathf.Max(maxx, x); miny = Mathf.Min(miny, y); maxy = Mathf.Max(maxy, y);
                }
                float w = Mathf.Min(lx, maxx) - Mathf.Max(0, minx), h = Mathf.Min(ly, maxy) - Mathf.Max(0, miny);
                if (w <= 0.002f || h <= 0.002f) continue;   // (2 mm)
                if (!b.text && minx <= 0.001f && miny <= 0.001f && maxx >= lx - 0.001f && maxy >= ly - 0.001f) continue;   // (a ground behind it)
                pairs++;
                report.AppendLine($"  {a.canvas.name} / {b.canvas.name}: \"{a.what}\" over {(b.text ? "\"" + b.what + "\"" : b.what)}, {w * 1000:0} x {h * 1000:0} mm");
            }
        }
        return $"Texts over each other: {pairs} pairs among {texts} texts of {canvases} panels" + (pairs > 0 ? "\n" + report : "");
    }

    // A measured box back in the world: its four corners (bottom left, top left, top right, bottom right).
    static World Corners(Canvas canvas, Placed p, bool text)
    {
        Transform t = canvas.transform;
        var w = new World { graphic = p.graphic, text = text, shown = p.shown, empty = p.empty, canvas = canvas };
        w.corners = new[] { t.TransformPoint(new Vector3(p.box.xMin, p.box.yMin)), t.TransformPoint(new Vector3(p.box.xMin, p.box.yMax)),
                            t.TransformPoint(new Vector3(p.box.xMax, p.box.yMax)), t.TransformPoint(new Vector3(p.box.xMax, p.box.yMin)) };
        w.what = text ? Said(p) + " (" + p.path + ")" : p.graphic.GetType().Name + " " + p.path + (p.shown ? "" : ", off at build");
        return w;
    }

    static string Said(Placed p)
    {
        string t = p.graphic is TMP_Text ? ((TMP_Text)p.graphic).text : ((Text)p.graphic).text;
        t = (t ?? "").Replace("\n", " ");
        return (p.empty ? "(empty: its rectangle)" : t.Length > 40 ? t.Substring(0, 40) + "..." : t) + (p.shown ? "" : ", off at build");
    }

    // A graphic's rectangle in its canvas's own units.
    static bool Box(Canvas canvas, Graphic g, out Placed p)
    {
        p = new Placed { graphic = g, shown = g.gameObject.activeInHierarchy && g.enabled, path = Path(g.transform, canvas.transform) };
        Vector3[] c = new Vector3[4];
        g.rectTransform.GetWorldCorners(c);
        Vector2 a = canvas.transform.InverseTransformPoint(c[0]), b = canvas.transform.InverseTransformPoint(c[2]);
        p.box = Rect.MinMaxRect(Mathf.Min(a.x, b.x), Mathf.Min(a.y, b.y), Mathf.Max(a.x, b.x), Mathf.Max(a.y, b.y));
        return p.box.width >= 1 && p.box.height >= 1;
    }

    // The text's drawn box in its canvas's own units.
    static bool Measure(Canvas canvas, Graphic g, out Placed p)
    {
        p = new Placed { graphic = g, shown = g.gameObject.activeInHierarchy && g.enabled };
        RectTransform r = g.rectTransform;
        Vector3[] c = new Vector3[4];
        r.GetWorldCorners(c);
        Transform to = canvas.transform;
        Vector2 a = to.InverseTransformPoint(c[0]), b = to.InverseTransformPoint(c[2]);
        Rect rect = Rect.MinMaxRect(Mathf.Min(a.x, b.x), Mathf.Min(a.y, b.y), Mathf.Max(a.x, b.x), Mathf.Max(a.y, b.y));
        if (rect.width < 1 || rect.height < 1) return false;
        float sx = rect.width / Mathf.Max(1e-3f, r.rect.width), sy = rect.height / Mathf.Max(1e-3f, r.rect.height);
        string text;
        Vector2 size;
        TextAnchor anchor;
        if (g is TMP_Text)
        {
            TMP_Text t = (TMP_Text)g;
            text = t.text;
            if (string.IsNullOrEmpty(text)) { p.empty = true; p.box = rect; p.path = Path(g.transform, canvas.transform); return true; }
            size = t.GetPreferredValues(text, t.enableWordWrapping ? r.rect.width : 0f, 0f);
            if (t.enableWordWrapping) size.x = Mathf.Min(size.x, r.rect.width);
            if (t.overflowMode == TextOverflowModes.Truncate || t.overflowMode == TextOverflowModes.Masking || t.overflowMode == TextOverflowModes.Ellipsis)
                size.y = Mathf.Min(size.y, r.rect.height);
            anchor = Anchor(t.alignment);
        }
        else
        {
            Text t = (Text)g;
            text = t.text;
            if (string.IsNullOrEmpty(text)) { p.empty = true; p.box = rect; p.path = Path(g.transform, canvas.transform); return true; }
            TextGenerationSettings s = t.GetGenerationSettings(new Vector2(t.horizontalOverflow == HorizontalWrapMode.Wrap ? r.rect.width : 0f, 0f));
            size = new Vector2(t.cachedTextGeneratorForLayout.GetPreferredWidth(text, s) / t.pixelsPerUnit,
                               t.cachedTextGeneratorForLayout.GetPreferredHeight(text, s) / t.pixelsPerUnit);
            if (t.horizontalOverflow == HorizontalWrapMode.Wrap) size.x = Mathf.Min(size.x, r.rect.width);
            if (t.verticalOverflow == VerticalWrapMode.Truncate) size.y = Mathf.Min(size.y, r.rect.height);
            anchor = t.alignment;
        }
        float w = size.x * sx, h = size.y * sy;
        int col = (int)anchor % 3, row = (int)anchor / 3;   // (TextAnchor: upper, middle, lower rows of left, centre, right)
        float x = col == 0 ? rect.xMin : col == 1 ? rect.center.x - w / 2 : rect.xMax - w;
        float y = row == 0 ? rect.yMax - h : row == 1 ? rect.center.y - h / 2 : rect.yMin;
        p.box = new Rect(x, y, w, h);
        p.path = Path(g.transform, canvas.transform);
        return true;
    }

    static TextAnchor Anchor(TextAlignmentOptions a)
    {
        int v = (int)a;
        int col = (v & 0x1) != 0 ? 0 : (v & 0x2) != 0 ? 1 : (v & 0x4) != 0 ? 2 : 0;   // left, centre, right (justified as left)
        int row = (v & 0x100) != 0 ? 0 : (v & 0x200) != 0 ? 1 : (v & 0x400) != 0 ? 2 : 1;   // top, middle, bottom (baselines as middle)
        return (TextAnchor)(row * 3 + col);
    }

    static string Path(Transform t, Transform canvas)
    {
        string s = t.name;
        for (Transform up = t.parent; up != null && up != canvas; up = up.parent) s = up.name + "/" + s;
        return s;
    }

    // For a batch run: -executeMethod ShaderEmuTexts.Batch opens the world's scene and logs the check.
    public static void Batch()
    {
        UnityEditor.SceneManagement.EditorSceneManager.OpenScene("Assets/ShaderEmu/ShaderEmuWorld.unity");
        int pairs;
        Debug.Log("[Texts] " + Check(out pairs));
    }
}
