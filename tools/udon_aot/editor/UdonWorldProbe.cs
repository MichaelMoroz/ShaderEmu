// Play-mode probes of the world's Udon machine (EmuUdonCpu): its console's last lines, its status,
// and text typed at its console keyboard, through the behaviours' Udon variables.
using System.Text;
using UnityEngine;
using VRC.Udon;

namespace UdonAot
{
    public static class UdonWorldProbe
    {
        static UdonBehaviour Driver() { return GameObject.Find("Udon machine").GetComponent<UdonBehaviour>(); }

        public static string Screen(int lines)
        {
            var t = UdonSharpEditor.UdonSharpEditorUtility.GetBackingUdonBehaviour(Object.FindObjectOfType<EmuTerminal>());
            var o = new StringBuilder(GameObject.Find("Udon CPU status").GetComponent<TMPro.TextMeshProUGUI>().text + "\n");
            var view = (Color32[])t.GetProgramVariable("view");
            if (view == null) return o.ToString();
            int cols = (int)t.GetProgramVariable("cols"), rows = (int)t.GetProgramVariable("rows");
            for (int r = Mathf.Max(0, rows - lines); r < rows; r++)
            {
                var s = new char[cols];
                for (int x = 0; x < cols; x++) { int c = view[r * cols + x].r; s[x] = c < 32 ? ' ' : (char)c; }
                o.AppendLine(new string(s).TrimEnd());
            }
            return o.ToString();
        }

        public static void Type(string text)
        {
            var k = (UdonBehaviour)Driver().GetProgramVariable("consoleKeyboard");
            var q = (int[])k.GetProgramVariable("queue");
            int tail = (int)k.GetProgramVariable("tail");
            foreach (char c in text) { q[tail] = c == '\n' ? '\r' : c; tail = (tail + 1) & 1023; }
            k.SetProgramVariable("tail", tail);
        }

        public static int[] Mem() { return (int[])Driver().GetProgramVariable("mem"); }
    }
}
