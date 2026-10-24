using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using Unity.Profiling;
using UnityEngine;
using UnityEngine.UI;
using VRC.Udon;

// Measures the machine in the editor's play mode (unity/gpuvm/README.md, "Measuring"). Not a part of a VRChat world: a
// plain MonoBehaviour beside EmuMachine that reads its Udon program's variables. It runs the script in the project's
// gvm_probe.txt, a command a line:
//   wait REGEX        until the console has printed text that matches, after what the last wait matched
//   type TEXT         typed at the console (\n is Enter)
//   sleep S           seconds of the wall
//   mark NAME         a line in the log with the time
//   speed N / tick N  the panel's sliders: 2,048 << N instructions a frame, 1,024 << N a pass
//   sample S NAME     S seconds of Unity's frames and the machine's instructions, averaged
//   shot NAME         the Game view to Logs/NAME.png
//   stop              play mode ends (gvm_auto.txt then goes on: GvmAuto.cs)
// and logs to Logs/gvm_probe.log (Logs/NAME.log when the first line is "log NAME"): the console's lines with the wall's seconds since play began, and once a second
// the frame rate, the frame's milliseconds and core 0's instructions.
public class GvmProbe : MonoBehaviour
{
    public string scriptFile = "gvm_probe.txt";

    UdonBehaviour machine;
    StreamWriter log;
    string console = "";         // everything printed since play began
    int printed;
    int searchFrom;
    StringBuilder line = new StringBuilder();
    List<string> script = new List<string>();
    int step;
    float waitUntil = -1f;
    float t0;
    // the once-a-second line and the samples
    int secFrames;
    float secStart;
    double secInstr;
    bool sampling;
    string sampleName;
    float sampleEnd, sampleStart;
    int sampleFrames;
    double sampleInstr, sampleWorkers;
    List<float> sampleMs = new List<float>();
    // where a frame goes, summed over a sample: Unity's frame timing counters and a few markers of the main and the
    // render thread (ProfilerRecorder: no profiler needed); "Frame Timing Stats" is on (GvmAuto)
    static readonly string[] Counters = { "CPU Main Thread Frame Time", "CPU Render Thread Frame Time", "GPU Frame Time",
                                          "Update.ScriptRunBehaviourUpdate", "Gfx.WaitForPresentOnGfxThread",
                                          "Gfx.WaitForGfxCommandsFromMainThread", "Gfx.PresentFrame", "Camera.Render",
                                          "Gfx.ReadbackImage", "AsyncGPUReadback.Update" };
    ProfilerRecorder[] recorders;
    double[] sums;
    int sumFrames;
    FrameTiming[] timing = new FrameTiming[1];
    double ftCpu, ftGpu;
    int ftFrames;

    // The card's time between cameras (minimal/GpuTimer: a native plugin of D3D11 timestamps, a measure only): event
    // 2 + 2k is the time from the event before to camera k's start, 3 + 2k its own; k is the tick camera (0), the
    // points camera's renders after a tick (1-4: the commit's, the operations' writes), the GPU's camera (5), any other (6)
    [DllImport("GpuTimer")] static extern IntPtr GpuTimer_Event();
    [DllImport("GpuTimer")] static extern int GpuTimer_Read(double[] ms, long[] n, int events, out long lost);
    [DllImport("GpuTimer")] static extern void GpuTimer_Reset();
    [DllImport("GpuTimer")] static extern int GpuTimer_Device(IntPtr resource);
    static readonly string[] CameraNames = { "tick", "commit", "write 1", "write 2", "points 4", "gpu", "other" };
    IntPtr timerEvent;
    int pointsSinceTick;

    int CameraKind(Camera c)
    {
        if (c.name.StartsWith("Tick")) { pointsSinceTick = 0; return 0; }
        if (c.name.StartsWith("Points")) return 1 + Mathf.Min(pointsSinceTick, 3);
        if (c.name.StartsWith("GPU")) return 5;
        return 6;
    }

    void Pre(Camera c)
    {
        if (timerEvent != IntPtr.Zero) GL.IssuePluginEvent(timerEvent, 2 + 2 * CameraKind(c));
    }

    void Post(Camera c)
    {
        if (timerEvent == IntPtr.Zero) return;
        int k = CameraKind(c);
        if (k >= 1 && k <= 4) pointsSinceTick++;
        GL.IssuePluginEvent(timerEvent, 3 + 2 * k);
    }

    IEnumerator FrameEnds()
    {
        WaitForEndOfFrame end = new WaitForEndOfFrame();
        while (true)
        {
            yield return end;
            GL.IssuePluginEvent(timerEvent, 1);
            GL.IssuePluginEvent(timerEvent, 0);
        }
    }

    void Start()
    {
        try
        {
            timerEvent = GpuTimer_Event();
            Texture2D any = new Texture2D(4, 4);
            any.Apply();
            if (GpuTimer_Device(any.GetNativeTexturePtr()) == 0) throw new Exception("no D3D11 device");
            GpuTimer_Reset();
            Camera.onPreRender += Pre;
            Camera.onPostRender += Post;

        }
        catch (Exception e) { timerEvent = IntPtr.Zero; Debug.Log("[GvmProbe] no card timer: " + e.Message); }
        foreach (UdonBehaviour u in GetComponents<UdonBehaviour>())
            if (u.GetProgramVariable("consoleOut") != null) machine = u;
        string root = Path.GetDirectoryName(Application.dataPath);
        Directory.CreateDirectory(Path.Combine(root, "Logs"));
        string path = Path.Combine(root, scriptFile);
        if (File.Exists(path)) script.AddRange(File.ReadAllLines(path));
        string logName = "gvm_probe";
        if (script.Count > 0 && script[0].StartsWith("log "))
        {
            logName = script[0].Substring(4).Trim();
            script.RemoveAt(0);
        }
        log = new StreamWriter(Path.Combine(root, "Logs", logName + ".log"), false) { AutoFlush = true };
        recorders = new ProfilerRecorder[Counters.Length];
        sums = new double[Counters.Length];
        for (int i = 0; i < Counters.Length; i++)
            recorders[i] = ProfilerRecorder.StartNew(i < 3 ? ProfilerCategory.Internal : ProfilerCategory.Render, Counters[i]);
        t0 = Time.realtimeSinceStartup;
        secStart = t0;
        Log("probe: " + (machine != null ? "machine found" : "NO MACHINE") + ", " + script.Count + " lines of " + path
            + ", " + SystemInfo.graphicsDeviceName + ", " + SystemInfo.graphicsDeviceType);
    }

    float Now() { return Time.realtimeSinceStartup - t0; }

    void Log(string text)
    {
        log.WriteLine(Now().ToString("F3", CultureInfo.InvariantCulture) + " " + text);
    }

    double Instructions()
    {
        // the count itself (a private field is a variable of the program too), else ShowStats's line, every half
        // second: "instructions N"
        object total = machine.GetProgramVariable("totalInstructions");
        if (total is double) return (double)total;
        string stats = machine.GetProgramVariable("statsLine") as string;
        Match m = stats != null ? Regex.Match(stats, "\ninstructions ([\\d,]+)") : Match.Empty;
        return m.Success ? double.Parse(m.Groups[1].Value.Replace(",", ""), CultureInfo.InvariantCulture) : 0;
    }

    void Update()
    {
        if (timerEvent != IntPtr.Zero)
        {
            GL.IssuePluginEvent(timerEvent, 1);   // (a frame on the card: from one of these to the next)
            GL.IssuePluginEvent(timerEvent, 0);
        }
        if (machine == null) return;
        TakeConsole();
        float dt = Time.unscaledDeltaTime;
        secFrames++;
        if (Now() - secStart >= 1f)
        {
            double instr = Instructions();
            float mipsAll = (float)machine.GetProgramVariable("mipsAll"), mipsCore = (float)machine.GetProgramVariable("mipsCore");
            Log(string.Format(CultureInfo.InvariantCulture, "sec: {0:F1} fps, {1:F2} ms a frame, core 0 {2:F3} MIPS, all {3:F3} MIPS, total {4:N0}",
                secFrames / (Now() - secStart), 1000f * (Now() - secStart) / secFrames, mipsCore, mipsAll, instr));
            secFrames = 0;
            secStart = Now();
        }
        if (sampling)
        {
            sampleFrames++;
            sampleMs.Add(dt * 1000f);
            for (int i = 0; i < Counters.Length; i++) if (recorders[i].Valid) sums[i] += recorders[i].LastValue;
            sumFrames++;
            FrameTimingManager.CaptureFrameTimings();
            if (FrameTimingManager.GetLatestTimings(1, timing) > 0)
            {
                ftCpu += timing[0].cpuFrameTime;
                ftGpu += timing[0].gpuFrameTime;
                ftFrames++;
            }
            if (Now() >= sampleEnd)
            {
                sampling = false;
                float span = Now() - sampleStart;
                sampleMs.Sort();
                double instr = Instructions() - sampleInstr;
                Log(string.Format(CultureInfo.InvariantCulture,
                    "sample {0}: {1:F2} s, {2} frames, {3:F1} fps, frame {4:F2} ms mean, {5:F2} median, {6:F2} p95; core 0 {7:N0} instructions, {8:F3} M a second",
                    sampleName, span, sampleFrames, sampleFrames / span, 1000f * span / sampleFrames, sampleMs[sampleMs.Count / 2],
                    sampleMs[(int)(sampleMs.Count * 0.95f)], instr, instr / span / 1e6));
                StringBuilder where = new StringBuilder("  where " + sampleName + ":");
                for (int i = 0; i < Counters.Length; i++)
                    if (recorders[i].Valid && sums[i] > 0)
                        where.Append(string.Format(CultureInfo.InvariantCulture, " {0} {1:F2} ms;", Counters[i], sums[i] / sumFrames / 1e6));
                if (timerEvent != IntPtr.Zero)
                {
                    double[] ms = new double[16];
                    long[] n = new long[16];
                    long lost;
                    int fr = GpuTimer_Read(ms, n, 16, out lost);
                    Log(string.Format("  card {0}: {1} frames read, {2} lost, {3} events, {4} begun", sampleName, fr, lost & 0xfffff, (lost >> 20) & 0x3fffff, lost >> 42));
                    lost &= 0xfffff;
                    if (fr > 0)
                    {
                        StringBuilder g = new StringBuilder(string.Format(CultureInfo.InvariantCulture, "  card {0}: {1} frames ({2} lost), a frame:", sampleName, fr, lost));
                        double all = 0;
                        for (int i = 1; i < 16; i++) all += ms[i];
                        g.Append(string.Format(CultureInfo.InvariantCulture, " all {0:F3} ms; frame's end {1:F3};", all / fr, ms[1] / fr));
                        for (int k = 0; k < CameraNames.Length; k++)
                            if (n[3 + 2 * k] > 0)
                                g.Append(string.Format(CultureInfo.InvariantCulture, " {0} x{1:F1}: before {2:F3} own {3:F3};", CameraNames[k],
                                    (double)n[3 + 2 * k] / fr, ms[2 + 2 * k] / fr, ms[3 + 2 * k] / fr));
                        Log(g.ToString());
                    }
                }
                if (ftFrames > 0)
                    where.Append(string.Format(CultureInfo.InvariantCulture, " FrameTiming cpu {0:F2} gpu {1:F2} ms", ftCpu / ftFrames, ftGpu / ftFrames));
                Log(where.ToString());
            }
            else return;
        }
        Run();
    }

    void TakeConsole()
    {
        int now = (int)machine.GetProgramVariable("consolePrinted");
        if (now < printed) printed = 0;   // powered on again
        if (now == printed) return;
        string tail = machine.GetProgramVariable("consoleOut") as string;
        int n = Mathf.Min(now - printed, tail.Length);
        string fresh = tail.Substring(tail.Length - n);
        printed = now;
        console += fresh;
        foreach (char c in fresh)
        {
            if (c == '\n')
            {
                Log("con| " + line.ToString().TrimEnd('\r'));
                line.Length = 0;
            }
            else line.Append(c);
        }
    }

    void Run()
    {
        while (step < script.Count)
        {
            string cmd = script[step].Trim();
            if (cmd.Length == 0 || cmd.StartsWith("#")) { step++; continue; }
            int sp = cmd.IndexOf(' ');
            string verb = sp < 0 ? cmd : cmd.Substring(0, sp), arg = sp < 0 ? "" : cmd.Substring(sp + 1);
            if (verb == "wait")
            {
                Match m = new Regex(arg).Match(console, Mathf.Min(searchFrom, console.Length));
                if (!m.Success) return;
                searchFrom = m.Index + m.Length;
                Log("matched: " + arg);
            }
            else if (verb == "type")
            {
                machine.SetProgramVariable("hostInput", (machine.GetProgramVariable("hostInput") as string) + arg.Replace("\\n", "\n"));
                Log("typed: " + arg);
            }
            else if (verb == "sleep")
            {
                if (waitUntil < 0) { waitUntil = Now() + float.Parse(arg, CultureInfo.InvariantCulture); return; }
                if (Now() < waitUntil) return;
                waitUntil = -1f;
            }
            else if (verb == "mark") Log("mark: " + arg);
            else if (verb == "speed" || verb == "tick")
            {
                Slider s = (Slider)machine.GetProgramVariable(verb == "speed" ? "speedSlider" : "tickSlider");
                s.value = int.Parse(arg);
                machine.SendCustomEvent(verb == "speed" ? "SpeedChanged" : "TickChanged");
                Log(verb + " " + arg);
            }
            else if (verb == "sample")
            {
                string[] p = arg.Split(new[] { ' ' }, 2);
                sampling = true;
                sampleName = p.Length > 1 ? p[1] : "";
                sampleStart = Now();
                sampleEnd = sampleStart + float.Parse(p[0], CultureInfo.InvariantCulture);
                sampleFrames = 0;
                sampleMs.Clear();
                for (int i = 0; i < sums.Length; i++) sums[i] = 0;
                if (timerEvent != IntPtr.Zero) GpuTimer_Reset();
                sumFrames = ftFrames = 0;
                ftCpu = ftGpu = 0;
                sampleInstr = Instructions();
                step++;
                return;
            }
            else if (verb == "var")
            {
                // var NAME N: an int of the machine's program
                string[] p = arg.Split(' ');
                machine.SetProgramVariable(p[0], int.Parse(p[1]));
                Log("var " + arg);
            }
            else if (verb == "frozen")
            {
                // which workers GvmFreeze.shader has frozen: how many, and how many have been quiet long enough
                RenderTexture[] f = machine.GetProgramVariable("gvmFreeze") as RenderTexture[];
                Shader view = Shader.Find("Hidden/GvmFreezeView");
                if (f != null && view != null)
                {
                    Material m = new Material(view);
                    m.SetTexture("_F", f[0]);
                    RenderTexture rt = RenderTexture.GetTemporary(256, 1, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
                    Graphics.Blit(null, rt, m);
                    RenderTexture was = RenderTexture.active;
                    RenderTexture.active = rt;
                    Texture2D t = new Texture2D(256, 1, TextureFormat.RGBA32, false, true);
                    t.ReadPixels(new Rect(0, 0, 256, 1), 0, 0);
                    RenderTexture.active = was;
                    RenderTexture.ReleaseTemporary(rt);
                    Color32[] px = t.GetPixels32();
                    int frozen = 0, quiet = 0, jobs = 0, runs = 0, asleep = 0, there = 0;
                    for (int k = 1; k < 256; k++)
                    {
                        if (px[k].r > 127) frozen++;
                        if (px[k].g > 250) quiet++;
                        if (px[k].b > 127) jobs++;
                        int w = (px[k].a * 7 + 127) / 255;
                        if ((w & 1) != 0) runs++;
                        if ((w & 2) != 0) asleep++;
                        if ((w & 4) != 0) there++;
                    }
                    Log("frozen: " + frozen + " of 255, " + quiet + " quiet, " + jobs + " with a job word, " + runs + " run, " + asleep + " asleep, " + there + " there");
                }
                else Log("frozen: nothing to read");
            }
            else if (verb == "on" || verb == "off")
            {
                // on OBJECT / off OBJECT: its renderer drawn or not (a measure: a draw's share of its camera's time)
                GameObject o = GameObject.Find(arg);
                if (o == null) Log(verb + ": no " + arg);
                else o.GetComponent<Renderer>().enabled = verb == "on";
                Log(verb + " " + arg);
            }
            else if (verb == "queue" || verb == "float")
            {
                // queue N OBJECT: its renderer's material's render queue; float PROP V OBJECT: a number of that material
                string[] p = arg.Split(new[] { ' ' }, verb == "queue" ? 2 : 3);
                string name = p[p.Length - 1];
                GameObject o = GameObject.Find(name);
                Material m = o != null ? o.GetComponent<Renderer>().sharedMaterial : null;
                if (m == null) Log(verb + ": no " + name);
                else if (verb == "queue") m.renderQueue = int.Parse(p[0]);
                else m.SetFloat(p[0], float.Parse(p[1], CultureInfo.InvariantCulture));
                Log(verb + " " + arg);
            }
            else if (verb == "shot")
            {
                ScreenCapture.CaptureScreenshot(Path.Combine(Path.GetDirectoryName(Application.dataPath), "Logs", arg + ".png"));
                Log("shot " + arg);
            }
            else if (verb == "stop")
            {
                Log("stop");
#if UNITY_EDITOR
                UnityEditor.EditorApplication.isPlaying = false;
#endif
            }
            else Log("unknown: " + cmd);
            step++;
        }
    }

    void OnDestroy()
    {
        Camera.onPreRender -= Pre;
        Camera.onPostRender -= Post;
        if (log != null)
        {
            if (line.Length > 0) Log("con| " + line);
            Log("probe: end");
            log.Close();
        }
    }
}
