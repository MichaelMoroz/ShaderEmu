// Loads linux2udon.py's output into the editor's Udon VM and boots it slice by slice, with the
// host side (console, clock) of RvRef.cs; the console is compared with the reference's.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;

namespace UdonAot
{
    public static class UdonLinux
    {
        static IUdonProgram Load(string dir, out Dictionary<string, uint> syms, out long loadMs)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var externs = File.ReadAllLines(dir + "/prog.externs");
            var hb = File.ReadAllBytes(dir + "/prog.heap");
            int n = BitConverter.ToInt32(hb, 0);
            var heap = new UdonHeap((uint)n + 16);
            for (int i = 0; i < n; i++)
            {
                int at = 4 + i * 9; byte typ = hb[at]; long v = BitConverter.ToInt64(hb, at + 1);
                uint slot = (uint)i;
                switch (typ)
                {
                    case 0: heap.InitializeHeapVariable(slot, typeof(int)); heap.SetHeapVariable(slot, (int)v, typeof(int)); break;
                    case 1: heap.InitializeHeapVariable(slot, typeof(uint)); heap.SetHeapVariable(slot, (uint)v, typeof(uint)); break;
                    case 2: heap.InitializeHeapVariable(slot, typeof(bool)); heap.SetHeapVariable(slot, v != 0, typeof(bool)); break;
                    case 3: heap.InitializeHeapVariable(slot, typeof(long)); heap.SetHeapVariable(slot, v, typeof(long)); break;
                    case 4: heap.InitializeHeapVariable(slot, typeof(string)); heap.SetHeapVariable(slot, externs[v], typeof(string)); break;
                    case 5: heap.InitializeHeapVariable(slot, typeof(int[])); break;
                    case 6: heap.InitializeHeapVariable(slot, typeof(uint[])); break;
                    case 7: heap.InitializeHeapVariable(slot, typeof(byte)); heap.SetHeapVariable(slot, (byte)v, typeof(byte)); break;
                    case 8: heap.InitializeHeapVariable(slot, typeof(float)); heap.SetHeapVariable(slot, BitConverter.Int32BitsToSingle((int)v), typeof(float)); break;
                }
            }
            syms = new Dictionary<string, uint>();
            foreach (var line in File.ReadAllLines(dir + "/prog.syms"))
            {
                var p = line.Split(' ');
                if (p.Length == 2) syms[p[0]] = uint.Parse(p[1]);
            }
            var none = new UdonSymbolTable(new List<IUdonSymbol>(), new string[0]);
            var prog = new UdonProgram("UDON", 1, File.ReadAllBytes(dir + "/prog.bc"), heap, none,
                new UdonSymbolTable(new List<IUdonSymbol>(), new string[0]), new UdonSyncMetadataTable(), 0);
            loadMs = sw.ElapsedMilliseconds;
            return prog;
        }

        public static readonly string[] Commands = { "/ # ", "uname -a\n", "/ # ", "ls /\n", "/ # ", "echo $((6*7)) hello\n",
            "/ # ", "cat /proc/cpuinfo\n", "/ # ", "head -c 300 /etc/passwd; echo; md5sum /bin/busybox\n" };

        static string Console(string run)
        {
            var c = run.Substring(run.IndexOf("---- console") + 13);
            return System.Text.RegularExpressions.Regex.Replace(c, @"\[ *\d+\.\d+\]", "[T]").Replace("\r", "");
        }

        /// The reference and Udon on one script; the consoles compared with the boot's timestamps aside.
        public static string Check(string dir, string img, double maxSeconds, params string[] script)
        {
            var refr = Runs.Reference(img, "/ # ", 200000, 600000000L, script);
            var ud = Run(dir, img, "/ # ", 40000, maxSeconds, script);
            string a = Console(refr), b = Console(ud);
            int i = 0; while (i < a.Length && i < b.Length && a[i] == b[i]) i++;
            var o = new StringBuilder(ud.Substring(0, ud.IndexOf("---- console")));
            o.AppendLine(a == b ? "console: the reference's" : string.Format("console DIFFERS at {0}: reference \"{1}\" udon \"{2}\"", i,
                a.Substring(i, Math.Min(120, a.Length - i)), b.Substring(i, Math.Min(120, b.Length - i))));
            File.WriteAllText(dir + "/udon_console.txt", b);
            return o.ToString();
        }

        static int[] I32(string path) { var b = File.ReadAllBytes(path); var a = new int[b.Length / 4]; Buffer.BlockCopy(b, 0, a, 0, b.Length); return a; }
        static uint[] U32(string path) { var b = File.ReadAllBytes(path); var a = new uint[b.Length / 4]; Buffer.BlockCopy(b, 0, a, 0, b.Length); return a; }

        /// Boots the image in Udon and runs the script; stops when `until` is printed after the script.
        public static readonly List<double> SliceMs = new List<double>();
        public static readonly List<long> SliceCount = new List<long>();
        public static readonly List<int> SliceStatus = new List<int>();
        public static int PromptSlice;

        public static string Run(string dir, string img, string until, int budget, double maxSeconds, params string[] script)
        {
            Dictionary<string, uint> S; long loadMs;
            var prog = Load(dir, out S, out loadMs);
            var mgr = VRC.Udon.Editor.UdonEditorManager.Instance;
            var vm = mgr.ConstructUdonVM();
            vm.LoadProgram(prog);
            var h = vm.InspectHeap();
            Mem.LoadRom(img + "/rootfs.bin");
            var mem = Mem.LoadRam(img + "/linux_payload.bin");
            h.SetHeapVariable(S["mem"], mem, typeof(int[]));
            h.SetHeapVariable(S["rom"], Mem.Rom, typeof(int[]));
            h.SetHeapVariable(S["romlen"], Mem.Rom.Length, typeof(int));
            h.SetHeapVariable(S["xr"], new int[32], typeof(int[]));
            h.SetHeapVariable(S["fr"], new int[32], typeof(int[]));
            h.SetHeapVariable(S["csr"], new int[16], typeof(int[]));
            foreach (var f in Directory.GetFiles(dir, "*.i32"))
                if (S.ContainsKey(Path.GetFileNameWithoutExtension(f))) h.SetHeapVariable(S[Path.GetFileNameWithoutExtension(f)], I32(f), typeof(int[]));
            foreach (var f in Directory.GetFiles(dir, "*.u32"))
                if (S.ContainsKey(Path.GetFileNameWithoutExtension(f))) h.SetHeapVariable(S[Path.GetFileNameWithoutExtension(f)], U32(f), typeof(uint[]));
            h.SetHeapVariable(S["x11"], unchecked((int)0x83e00000u), typeof(int));
            h.SetHeapVariable(S["priv"], 1, typeof(int));
            h.SetHeapVariable(S["mode"], 1, typeof(int));
            h.SetHeapVariable(S["res"], -1, typeof(int));
            h.SetHeapVariable(S["rpc"], unchecked((int)0x82000000u), typeof(int));
            h.SetHeapVariable(S["resume"], S["@resume0"], typeof(uint));
            h.SetHeapVariable(S["k_u32"], 0xFFFFFFFFL, typeof(long));
            h.SetHeapVariable(S["k_l0"], 0L, typeof(long));

            var clock = new Clock();
            var con = new ConsoleHost(Runs.Script(script));
            uint entry = S["@entry"];
            var su = new System.Diagnostics.Stopwatch();
            long slices = 0, wfis = 0, pauses = 0; double firstPrompt = -1;
            string stop = "time limit";
            SliceMs.Clear(); SliceCount.Clear(); SliceStatus.Clear(); PromptSlice = -1;
            bool counting = S.ContainsKey("gcount");
            long lastCount = 0;
            while (su.Elapsed.TotalSeconds < maxSeconds)
            {
                h.SetHeapVariable(S["now"], (int)clock.Time, typeof(int));
                h.SetHeapVariable(S["budget"], budget, typeof(int));
                h.SetHeapVariable(S["status"], 0, typeof(int));
                vm.SetProgramCounter(entry);
                double before = su.Elapsed.TotalMilliseconds;
                su.Start();
                try { vm.Interpret(); }
                catch (Exception e)
                {
                    su.Stop();
                    stop = string.Format("Udon threw in slice {0} at {1:x8}: {2}", slices, vm.GetProgramCounter(), (e.InnerException ?? e).Message);
                    break;
                }
                su.Stop(); slices++;
                SliceMs.Add(su.Elapsed.TotalMilliseconds - before);
                long cnow = (long)h.GetHeapVariable<int>(S["icount"]) + (counting ? h.GetHeapVariable<int>(S["gcount"]) : 0);
                SliceCount.Add(cnow - lastCount); lastCount = cnow;
                int status = h.GetHeapVariable<int>(S["status"]);
                SliceStatus.Add(status);
                if (status == 2) wfis++; else if (status == 3) pauses++;
                ulong tcmp = (uint)h.GetHeapVariable<int>(S["tl"]) | ((ulong)(uint)h.GetHeapVariable<int>(S["th"]) << 32);
                clock.Advance(status == 2, tcmp);
                con.Service(mem);
                if (firstPrompt < 0 && con.Out.ToString().Contains("/ # ")) { firstPrompt = su.Elapsed.TotalMilliseconds; PromptSlice = (int)slices; }
                if (con.ScriptDone && con.Out.ToString().IndexOf(until, con.DoneAt, StringComparison.Ordinal) >= 0) { stop = "done"; break; }
            }
            Func<string, int> G = name => h.GetHeapVariable<int>(S[name]);
            var o = new StringBuilder();
            o.AppendLine(string.Format("udon: loaded in {0} ms; ran {1:F0} ms in {2:N0} slices (wfi {3:N0}, pause {4:N0}), first prompt at {5:F0} ms; stopped: {6}",
                loadMs, su.Elapsed.TotalMilliseconds, slices, wfis, pauses, firstPrompt, stop));
            o.AppendLine(string.Format("interpreted {0:N0}; to the interpreter {1:N0}; interrupts {2:N0}, traps {3:N0}, system {4:N0}, csr {5:N0}, memop {6:N0} ({7:N0} word by word); guest time {8:N0} ticks; console lost {9}",
                G("icount"), G("nint"), G("nirq"), G("ntrap"), G("nsys"), G("ncsr"), G("nmemop"), G("nmoslow"), clock.Time, con.Lost));
            o.AppendLine(string.Format("priv {0}, mode {1}, rpc {2:x8}, ipc {3:x8}", G("priv"), G("mode"), G("rpc"), G("ipc")));
            o.AppendLine("---- console");
            o.Append(con.Out.ToString());
            return o.ToString();
        }
    }
}
