using System.Collections.Generic;
using System.IO;
using System.Text;
using TMPro;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;

// The system sheet (docs/world/world.md): fanfold paper on the den's back wall over the television, what
// the machine has installed and what opens what. Its words are ShaderEmuSheet.txt, laid out here.
public static partial class ShaderEmuBuilder
{
    public const string SheetName = "System sheet";
    const string SheetWords = Root + "/Editor/ShaderEmuSheet.txt";
    const float SheetWide = 2300, SheetHigh = 1540, SheetSide = 70, SheetGutter = 60, SheetTop = 125, SheetFoot = 50;

    class SheetPart { public string head; public List<string> lines = new List<string>(); }

    static void SystemSheet(Transform world)
    {
        Gone(world, SheetName);
        // from the corner to the posters (x -1.9), its frame over the television (1.22 m) and under the rail
        Vector3 centre = new Vector3(-3.22f, 2.085f, -5.47f);
        RectTransform sheet = Panel(world, SheetName, centre, new Vector3(0, 180f, 0), SheetWide, SheetHigh, Color.white);
        Object.DestroyImmediate(sheet.GetComponent<VRC.SDK3.Components.VRCUiShape>());
        Object.DestroyImmediate(sheet.GetComponent<BoxCollider>());
        Object.DestroyImmediate(sheet.GetComponent<GraphicRaycaster>());
        SheetLabels(sheet);
        SheetFrame(world, centre);
    }

    // The board's own frame (world/computer.py's "Board", round 2.3 x 1.7 m of paper on the left
    // wall), turned onto the back wall and stretched to this paper. Not static: the probes light it
    // (a lightmap index borrowed from the board is dropped when the scene is opened again).
    static void SheetFrame(Transform world, Vector3 centre)
    {
        Gone(world, SheetName + " frame");
        MeshRenderer board = null;
        foreach (MeshRenderer r in world.GetComponentsInChildren<MeshRenderer>(true))
            if (r.name == "Board" && r.GetComponent<MeshFilter>() != null) { board = r; break; }
        GameObject about = GameObject.Find("About");
        if (board == null || about == null) { Debug.LogWarning("[ShaderEmu] no board model or board: the system sheet has no frame"); return; }
        Vector3 paper = board.transform.InverseTransformPoint(about.transform.position);
        Vector3 scale = new Vector3(1f, SheetHigh / 1700f, SheetWide / 2300f);
        Quaternion turn = Quaternion.Euler(0, -90f, 0);   // the board's room side (+x) towards +z
        GameObject frame = new GameObject(SheetName + " frame", typeof(MeshFilter), typeof(MeshRenderer));
        frame.transform.SetParent(world, false);
        frame.transform.localRotation = turn;
        frame.transform.localScale = scale;
        frame.transform.localPosition = centre - turn * Vector3.Scale(scale, paper);
        frame.GetComponent<MeshFilter>().sharedMesh = board.GetComponent<MeshFilter>().sharedMesh;
        MeshRenderer renderer = frame.GetComponent<MeshRenderer>();
        // the board's materials without the light volume, which reads black this close to the back wall
        Material[] own = board.sharedMaterials;
        for (int i = 0; i < own.Length; i++)
        {
            string path = Generated + "/" + own[i].name + " by probes.mat";
            Material copy = new Material(own[i]);
            copy.SetFloat("_LightVolumesToggle", 0f);
            copy.SetFloat("_AdditiveLightVolumesToggle", 0f);
            AssetDatabase.DeleteAsset(path);
            AssetDatabase.CreateAsset(copy, path);
            own[i] = copy;
        }
        renderer.sharedMaterials = own;
        renderer.receiveGI = ReceiveGI.LightProbes;
        renderer.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.BlendProbes;
    }

    // A column's names are padded to its longest that leaves the text two thirds of a line.
    static int SheetPad(List<SheetPart> parts, int chars)
    {
        int pad = 0;
        foreach (SheetPart part in parts)
            foreach (string line in part.lines)
            {
                int bar = line.IndexOf(" | ");
                if (bar >= 0 && bar + 2 <= chars / 3) pad = Mathf.Max(pad, bar + 2);
            }
        return pad;
    }

    // NAME | TEXT is NAME padded to `pad`, TEXT wrapped under itself; any other line wraps at the left.
    static List<string> SheetWrap(List<string> lines, int chars, int pad)
    {
        int longest = 0;   // a numbered list keeps its numbers close
        foreach (string line in lines) longest = Mathf.Max(longest, line.IndexOf(" | "));
        if (longest > 0 && longest <= 3) pad = longest + 2;
        List<string> output = new List<string>();
        foreach (string line in lines)
        {
            int bar = line.IndexOf(" | ");
            string name = bar >= 0 ? line.Substring(0, bar) : "", text = bar >= 0 ? line.Substring(bar + 3) : line;
            int indent = bar >= 0 ? pad : 0;
            if (bar >= 0 && name.Length + 2 > pad) { output.Add(name); name = ""; }   // too long a name: a line of its own
            string start = bar >= 0 ? name.PadRight(indent) : "";
            StringBuilder now = new StringBuilder(start);
            bool empty = true;
            foreach (string word in text.Split(' '))
            {
                if (word.Length == 0) continue;
                if (!empty && now.Length + 1 + word.Length > chars) { output.Add(now.ToString()); now = new StringBuilder(new string(' ', indent)); empty = true; }
                if (!empty) now.Append(' ');
                now.Append(word);
                empty = false;
            }
            output.Add(now.ToString());
        }
        return output;
    }

    // The words laid out in their columns at the largest size at which every column fits.
    static void SheetLabels(RectTransform sheet)
    {
        foreach (TextMeshProUGUI old in sheet.GetComponentsInChildren<TextMeshProUGUI>(true)) Object.DestroyImmediate(old.gameObject);
        string path = Path.Combine(Path.GetDirectoryName(Application.dataPath), SheetWords);
        string title = "";
        List<List<SheetPart>> columns = new List<List<SheetPart>>();
        foreach (string raw in File.ReadAllLines(path))
        {
            string text = raw.TrimEnd();
            if (text.StartsWith("#")) continue;
            for (int i = 0; i < text.Length; i++)
                if (text[i] < ' ' || text[i] > '~') throw new System.Exception(SheetWords + ": not ASCII: " + text);
            if (text.StartsWith("title:")) { title = text.Substring(6).Trim(); continue; }
            if (text == "column") { columns.Add(new List<SheetPart>()); continue; }
            if (columns.Count == 0) { if (text.Length > 0) throw new System.Exception(SheetWords + ": words before the first column"); continue; }
            List<SheetPart> parts = columns[columns.Count - 1];
            if (text.StartsWith("head:")) { parts.Add(new SheetPart { head = text.Substring(5).Trim() }); continue; }
            if (parts.Count == 0) parts.Add(new SheetPart { head = "" });
            parts[parts.Count - 1].lines.Add(text);
        }
        foreach (List<SheetPart> parts in columns)
            foreach (SheetPart part in parts)
            {
                while (part.lines.Count > 0 && part.lines[0].Length == 0) part.lines.RemoveAt(0);
                while (part.lines.Count > 0 && part.lines[part.lines.Count - 1].Length == 0) part.lines.RemoveAt(part.lines.Count - 1);
            }

        TMP_FontAsset mono = RetroFont("IBMPlexMono-Regular"), bold = RetroFont("IBMPlexMono-Bold");
        float wide = (SheetWide - 2 * SheetSide - (columns.Count - 1) * SheetGutter) / columns.Count;
        float room = SheetHigh - SheetTop - SheetFoot;
        TextMeshProUGUI probe = Label(sheet, "Probe", "", 0, 0, wide, 100, 20, TextAnchor.UpperLeft, Color.black);
        probe.font = mono;
        probe.enableWordWrapping = false;
        float size = 26f, advance = 0, line = 0, headLine = 0;
        List<List<string>[]> wrapped = null;
        for (; size >= 12f; size -= 0.5f)
        {
            probe.fontSize = size;
            advance = probe.GetPreferredValues("0000000000", 1e5f, 1e5f).x / 10f;
            line = probe.GetPreferredValues("0\n0", 1e5f, 1e5f).y - probe.GetPreferredValues("0", 1e5f, 1e5f).y;
            headLine = line * 1.2f;
            int chars = Mathf.FloorToInt(wide / advance);
            wrapped = new List<List<string>[]>();
            float tallest = 0;
            foreach (List<SheetPart> parts in columns)
            {
                List<string>[] bodies = new List<string>[parts.Count];
                int pad = SheetPad(parts, chars);
                float high = 0;
                for (int p = 0; p < parts.Count; p++)
                {
                    bodies[p] = SheetWrap(parts[p].lines, chars, pad);
                    high += (p > 0 ? line * 0.7f : 0) + (parts[p].head.Length > 0 ? headLine * 1.15f : 0) + bodies[p].Count * line;
                }
                wrapped.Add(bodies);
                tallest = Mathf.Max(tallest, high);
            }
            if (tallest <= room) break;
        }
        Object.DestroyImmediate(probe.gameObject);

        Label(sheet, "Title", title, SheetSide, 38, SheetWide - 2 * SheetSide, 64, 46, TextAnchor.MiddleLeft, Color.black).font = bold;
        int n = 0;
        for (int c = 0; c < columns.Count; c++)
        {
            float x = SheetSide + c * (wide + SheetGutter), y = SheetTop;
            for (int p = 0; p < columns[c].Count; p++, n++)
            {
                if (p > 0) y += line * 0.7f;
                if (columns[c][p].head.Length > 0)
                {
                    TextMeshProUGUI head = Label(sheet, "Part " + n + " head", columns[c][p].head, x, y, wide, headLine, Mathf.RoundToInt(size * 1.2f), TextAnchor.MiddleLeft, Color.black);
                    head.font = bold;
                    if (head.GetPreferredValues(head.text, 1e5f, 1e5f).x > wide) Debug.LogWarning("[ShaderEmu] the system sheet: the heading " + head.text + " is wider than its column");
                    y += headLine * 1.15f;
                }
                List<string> body = wrapped[c][p];
                if (body.Count == 0) continue;
                TextMeshProUGUI words = Label(sheet, "Part " + n, string.Join("\n", body), x, y, wide, body.Count * line + 4, 12, TextAnchor.UpperLeft, Color.black);
                words.font = mono;
                words.fontSize = size;
                words.enableWordWrapping = false;   // wrapped above, under each name
                words.overflowMode = TextOverflowModes.Overflow;
                y += body.Count * line;
            }
        }
        Debug.Log("[ShaderEmu] the system sheet: " + columns.Count + " columns at size " + size + " (" + Mathf.FloorToInt(wide / advance) + " characters a line)");
    }

}
