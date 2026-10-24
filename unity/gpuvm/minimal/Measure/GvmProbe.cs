using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
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

    void Start()
    {
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
                sampleInstr = Instructions();
                step++;
                return;
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
        if (log != null)
        {
            if (line.Length > 0) Log("con| " + line);
            Log("probe: end");
            log.Close();
        }
    }
}
