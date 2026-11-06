using System.Collections.Generic;
using System.Reflection;
using System.Text;
using TMPro;
using UnityEditor;
using UnityEngine;
using UnityEngine.UI;

// Lines of sight (docs/world/holodeck.md, "What the sitter must see"): from where a visitor's eyes are
// expected, every element of the panels meant for them must be seen whole, not covered by another
// panel or any other thing. A view is an eye and the panels it looks at; each target, a graphic's
// rectangle or a screen's quad, is tried at its centre and four points inside its corners, by a
// segment from the eye, against every other mesh (its triangles) and every graphic of another canvas.
// The holodeck's views are each room's seat with its screen small (in the console's face) and large
// (before the seat), the keyboard placed by EmuHolodeck.PlaceKeyboard itself, called by reflection;
// what a view moves is put back as it was.
public static class ShaderEmuSight
{
    const float Near = 0.005f;   // what lies within this of a target point, before it, is its own face

    [MenuItem("ShaderEmu/Check lines of sight in the open scene")]
    public static void Menu()
    {
        int blocked;
        string text = Check(out blocked);
        if (blocked == 0) Debug.Log(text); else Debug.LogError(text);
    }

    struct Target { public string name; public Vector3[] points; public Object own; public Canvas canvas; }

    class Blocker
    {
        public string name;
        public Renderer renderer; public Matrix4x4 toLocal; public Vector3[] v; public int[] tri;
        public Vector3[] quad;   // a graphic's corners, in the world
        public Canvas canvas;
    }

    // Every target of every view, and what covers it; blocked counts the targets not seen whole.
    public static string Check(out int blocked)
    {
        blocked = 0;
        var report = new StringBuilder();
        EmuHolodeck holo = Object.FindObjectOfType<EmuHolodeck>();
        if (holo == null) { blocked = -1; return "[Sight] no EmuHolodeck in the open scene"; }
        MethodInfo place = typeof(EmuHolodeck).GetMethod("PlaceKeyboard", BindingFlags.NonPublic | BindingFlags.Instance);
        if (place == null) { blocked = -1; return "[Sight] EmuHolodeck has no PlaceKeyboard"; }
        UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(holo);
        if (holo.keyboard != null) UdonSharpEditor.UdonSharpEditorUtility.CopyUdonToProxy(holo.keyboard);

        Transform kb = holo.keyboard != null ? holo.keyboard.transform : null;
        var kept = new List<System.Action>();
        if (kb != null) Keep(kb, kept);
        int rooms = holo.screens.Length, views = 0, targets = 0;
        try
        {
            for (int k = 0; k < rooms; k++)
            {
                Transform screen = holo.screens[k];
                Keep(screen, kept);
                // a seated visitor's room as Arrange leaves it: the console, its keys, the screen and the keyboard
                Show(Item(holo.plates, k), kept);
                Show(Item(holo.keyPlates, k), kept);
                Show(screen.gameObject, kept);
                if (kb != null) Show(kb.gameObject, kept);
                for (int large = 0; large < 2; large++)
                {
                    screen.localPosition = large == 1 ? holo.largeAt : holo.screenAt;
                    screen.localEulerAngles = large == 1 ? Vector3.zero : holo.screenTurn;
                    screen.localScale = large == 1 ? holo.largeSize : holo.screenSize;
                    place.Invoke(holo, new object[] { screen, holo.frames[k], large == 1 ? null : Item(holo.plates, k) });
                    Canvas.ForceUpdateCanvases();
                    Vector3 eye = holo.eyes[k].position;
                    var list = new List<Target>();
                    Graphics(Item(holo.plates, k), "plate", list);
                    Graphics(Item(holo.keyPlates, k), "keys", list);
                    if (kb != null) Graphics(kb.gameObject, "keyboard", list);
                    Renderer screenRenderer = screen.GetComponent<Renderer>();
                    list.Add(new Target
                    {
                        name = "screen", own = screenRenderer, canvas = null,
                        points = Points(screen.TransformPoint(new Vector3(-0.5f, -0.5f, 0f)), screen.TransformPoint(new Vector3(-0.5f, 0.5f, 0f)),
                                        screen.TransformPoint(new Vector3(0.5f, -0.5f, 0f)))
                    });
                    List<Blocker> blockers = Blockers(holo, eye);
                    string view = "room " + k + ", " + (large == 1 ? "large screen" : "small screen");
                    int here = 0;
                    var lines = new StringBuilder();
                    foreach (Target t in list)
                    {
                        targets++;
                        int hits = 0;
                        string by = null;
                        foreach (Vector3 p in t.points)
                        {
                            string b = Covered(eye, p, t, blockers);
                            if (b != null) { hits++; by = by ?? b; }
                        }
                        if (hits == 0) continue;
                        here++;
                        if (here <= 12) lines.Append("\n    " + t.name + ": " + hits + " of " + t.points.Length + " points behind " + by);
                    }
                    views++;
                    blocked += here;
                    report.Append("\n  " + view + ": " + (here == 0 ? "all " + list.Count + " seen" : here + " of " + list.Count + " covered") + lines);
                    if (here > 12) report.Append("\n    (and " + (here - 12) + " more)");
                }
            }
        }
        finally
        {
            for (int i = kept.Count - 1; i >= 0; i--) kept[i]();
            Canvas.ForceUpdateCanvases();
        }
        return "[Sight] " + views + " views, " + targets + " targets, " + blocked + " covered" + report;
    }

    static GameObject Item(GameObject[] a, int k) { return a != null && k < a.Length ? a[k] : null; }

    static void Keep(Transform t, List<System.Action> kept)
    {
        Vector3 p = t.localPosition, s = t.localScale;
        Quaternion r = t.localRotation;
        kept.Add(() => { t.localPosition = p; t.localRotation = r; t.localScale = s; });
    }

    static void Show(GameObject g, List<System.Action> kept)
    {
        if (g == null || g.activeSelf) return;
        g.SetActive(true);
        kept.Add(() => g.SetActive(false));
    }

    static Vector3[] Points(Vector3 o, Vector3 up, Vector3 right)
    {
        Vector3 u = right - o, v = up - o;
        var points = new Vector3[5];
        points[0] = o + 0.5f * u + 0.5f * v;
        points[1] = o + 0.15f * u + 0.15f * v;
        points[2] = o + 0.85f * u + 0.15f * v;
        points[3] = o + 0.15f * u + 0.85f * v;
        points[4] = o + 0.85f * u + 0.85f * v;
        return points;
    }

    static bool Visible(Graphic g)
    {
        if (!g.enabled || !g.gameObject.activeInHierarchy || g.color.a < 0.1f) return false;
        var tmp = g as TextMeshProUGUI;
        if (tmp != null && string.IsNullOrWhiteSpace(tmp.text)) return false;
        var text = g as Text;
        if (text != null && string.IsNullOrWhiteSpace(text.text)) return false;
        Rect r = g.rectTransform.rect;
        return r.width > 1e-4f && r.height > 1e-4f;
    }

    static void Graphics(GameObject root, string what, List<Target> list)
    {
        if (root == null) return;
        foreach (Graphic g in root.GetComponentsInChildren<Graphic>(false))
        {
            if (!Visible(g)) continue;
            var c = new Vector3[4];
            g.rectTransform.GetWorldCorners(c);   // bottom left, top left, top right, bottom right
            var tmp = g as TextMeshProUGUI;
            string name = what + " " + Path(g.transform, root.transform) + (tmp != null ? " \"" + Short(tmp.text) + "\"" : "");
            list.Add(new Target { name = name, own = g, canvas = g.canvas != null ? g.canvas.rootCanvas : null, points = Points(c[0], c[1], c[3]) });
        }
    }

    static string Short(string s) { s = s.Replace("\n", " "); return s.Length > 24 ? s.Substring(0, 24) + "..." : s; }

    static string Path(Transform t, Transform root)
    {
        string p = t.name;
        while (t.parent != null && t != root && t.parent != root) { t = t.parent; p = t.name + "/" + p; }
        return p;
    }

    // What can cover a target: every mesh drawn but the program's world and the marks, and every graphic.
    static List<Blocker> Blockers(EmuHolodeck holo, Vector3 eye)
    {
        var skip = new HashSet<Transform>();
        foreach (Transform t in new[] { holo.content, holo.scene, holo.hud }) if (t != null) skip.Add(t);
        var marks = new List<Transform>();
        if (holo.screenGuards != null) marks.AddRange(holo.screenGuards);
        foreach (GameObject[] a in new[] { holo.plateGuards })
            if (a != null) foreach (GameObject g in a) if (g != null) marks.Add(g.transform);
        foreach (GameObject g in new[] { holo.consolesGuard, holo.seatsGuard }) if (g != null) marks.Add(g.transform);
        foreach (Transform t in marks) if (t != null) skip.Add(t);

        var list = new List<Blocker>();
        foreach (MeshRenderer r in Object.FindObjectsOfType<MeshRenderer>())
        {
            if (!r.enabled || !r.gameObject.activeInHierarchy || Under(r.transform, skip)) continue;
            if (r.bounds.Contains(eye)) continue;   // what holds the eye (a room's shell) covers nothing before it
            MeshFilter f = r.GetComponent<MeshFilter>();
            if (f == null || f.sharedMesh == null) continue;
            Material m = r.sharedMaterial;
            if (m != null && m.shader != null && m.shader.name.Contains("Guard")) continue;
            Mesh mesh = f.sharedMesh;
            // (a mesh of lines or points covers nothing: only its submeshes of triangles)
            var tri = new List<int>();
            for (int sub = 0; sub < mesh.subMeshCount; sub++)
                if (mesh.GetTopology(sub) == MeshTopology.Triangles) tri.AddRange(mesh.GetTriangles(sub));
            if (tri.Count == 0) continue;
            list.Add(new Blocker { name = Named(r.transform), renderer = r, toLocal = r.transform.worldToLocalMatrix, v = mesh.vertices, tri = tri.ToArray() });
        }
        foreach (Graphic g in Object.FindObjectsOfType<Graphic>())
        {
            if (!Visible(g) || Under(g.transform, skip)) continue;
            var c = new Vector3[4];
            g.rectTransform.GetWorldCorners(c);
            list.Add(new Blocker { name = Named(g.transform), quad = c, canvas = g.canvas != null ? g.canvas.rootCanvas : null });
        }
        return list;
    }

    static bool Under(Transform t, HashSet<Transform> set)
    {
        for (; t != null; t = t.parent) if (set.Contains(t)) return true;
        return false;
    }

    static string Named(Transform t)
    {
        string p = t.name;
        for (int i = 0; i < 2 && t.parent != null; i++) { t = t.parent; p = t.name + "/" + p; }
        return p;
    }

    // The first thing between the eye and p that is not the target's own, or null.
    static string Covered(Vector3 eye, Vector3 p, Target t, List<Blocker> blockers)
    {
        Vector3 d = p - eye;
        float len = d.magnitude;
        if (len < 1e-4f) return null;
        float limit = 1f - Near / len;
        foreach (Blocker b in blockers)
        {
            if (b.quad != null)
            {
                if (b.canvas == t.canvas && t.canvas != null) continue;   // a panel's own layers are its design
                float s;
                if ((Segment(eye, d, b.quad[0], b.quad[1], b.quad[2], out s) || Segment(eye, d, b.quad[0], b.quad[2], b.quad[3], out s)) && s < limit)
                    return b.name;
                continue;
            }
            if (b.renderer == t.own) continue;
            if (!b.renderer.bounds.IntersectRay(new Ray(eye, d), out float enter) || enter > len) continue;
            Vector3 o = b.toLocal.MultiplyPoint3x4(eye), dl = b.toLocal.MultiplyVector(d);
            for (int i = 0; i + 2 < b.tri.Length; i += 3)
            {
                float s;
                if (Segment(o, dl, b.v[b.tri[i]], b.v[b.tri[i + 1]], b.v[b.tri[i + 2]], out s) && s < limit) return b.name;
            }
        }
        return null;
    }

    // Moller and Trumbore: where o + s d meets the triangle, for s in (0, 1), either face.
    static bool Segment(Vector3 o, Vector3 d, Vector3 a, Vector3 b, Vector3 c, out float s)
    {
        s = 0f;
        Vector3 e1 = b - a, e2 = c - a, h = Vector3.Cross(d, e2);
        float det = Vector3.Dot(e1, h);
        if (Mathf.Abs(det) < 1e-12f) return false;
        float inv = 1f / det;
        Vector3 q = o - a;
        float u = Vector3.Dot(q, h) * inv;
        if (u < 0f || u > 1f) return false;
        Vector3 r = Vector3.Cross(q, e1);
        float v = Vector3.Dot(d, r) * inv;
        if (v < 0f || u + v > 1f) return false;
        s = Vector3.Dot(e2, r) * inv;
        return s > 0f && s < 1f;
    }
}
