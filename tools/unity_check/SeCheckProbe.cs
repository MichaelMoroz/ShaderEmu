using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon;

// A play-mode check of the world (tools/unity_check, SeCheckAuto.cs says how), not part of the world: runs <project>/se_check_probe.txt in play mode, a command a line:
//   wait REGEX [S]     until the console's shown screen matches (S seconds at most; then "timeout" and on)
//   type TEXT          typed at the console (\n Enter)
//   sleep S
//   shot NAME          the Game view to OUT/NAME.png (OUT: the first line "out DIR")
//   power              EmuMachine's PowerOn
//   sit N / stand      the local player into holodeck N's seat, or out
//   look X Y Z         ClientSim's desktop view turned to a point of the world
//   lookrel DX DY DZ   to a point relative to the head
//   edge X Y           the view to the picture's share (X, Y) of the den's display's plane (outside 0..1 is beside it)
//   holoedge X Y       the same on the sitter's large screen in the holodeck
//   goto X Y Z YAW     the player teleported
//   front D            the player D metres before the den's display, facing it
//   lookpx X Y         the view turned to a place of the Game view's picture (shares across and down)
//   menu               ClientSim's own menu put away (its simulator notice accepted, as its button does)
//   key CODE           a key pressed and let go straight into the machine's key events (Linux's code: Escape 1)
//   keys SPEC          SPEC written to se_check_keys.txt for keys.ps1 (real keys and buttons); waits until it deletes it
//   state NAME         one line of what the holodeck, the machine and the pointer say
//   watch S            a state line every S seconds (0: off)
//   stop
public class SeCheckProbe : MonoBehaviour
{
    UdonBehaviour machine, holodeck, pointer, terminal;
    StreamWriter log;
    List<string> script = new List<string>();
    int step;
    float waitUntil = -1f, t0, watchEvery, watchAt;
    string outDir = ".";
    string root;
    string lastScreen = "";

    static UdonBehaviour Find(string program)
    {
        foreach (UdonBehaviour u in FindObjectsOfType<UdonBehaviour>(true))
            if (u.programSource != null && u.programSource.name == program) return u;
        return null;
    }

    void Start()
    {
        root = Path.GetDirectoryName(Application.dataPath);
        log = new StreamWriter(Path.Combine(root, "Logs", "se_check_probe.log"), true) { AutoFlush = true };
        string path = Path.Combine(root, "se_check_probe.txt");
        if (File.Exists(path)) script.AddRange(File.ReadAllLines(path));
        t0 = Time.realtimeSinceStartup;
        machine = Find("EmuMachine");
        holodeck = Find("EmuHolodeck");
        pointer = Find("EmuPointer");
        terminal = Find("EmuTerminal");
        Log("probe: machine " + (machine != null) + ", holodeck " + (holodeck != null) + ", pointer " + (pointer != null) + ", terminal " + (terminal != null)
            + ", " + script.Count + " lines, " + SystemInfo.graphicsDeviceName);
    }

    float Now() { return Time.realtimeSinceStartup - t0; }
    void Log(string text) { log.WriteLine(Now().ToString("F2", CultureInfo.InvariantCulture) + " " + text); }

    object V(UdonBehaviour u, string name) { return u == null ? null : u.GetProgramVariable(name); }

    // the shown terminal's screen, a line a row
    string Screen()
    {
        if (terminal == null) return "";
        Color32[] grid = V(terminal, "grid") as Color32[];
        int[] top = V(terminal, "top") as int[];
        if (grid == null || top == null) return "";
        int shown = (int)V(terminal, "shown"), cols = (int)V(terminal, "cols"), rows = (int)V(terminal, "rows"), kept = (int)V(terminal, "kept");
        StringBuilder sb = new StringBuilder();
        for (int r = 0; r < rows; r++)
        {
            int at = (shown * kept + (r + top[shown]) % kept) * cols;
            for (int x = 0; x < cols; x++) sb.Append((char)grid[at + x].r);
            sb.Append('\n');
        }
        return sb.ToString();
    }

    void Type(string text)
    {
        int[] ring = V(machine, "remoteChars") as int[];
        int tail = (int)V(machine, "charTail");
        foreach (char c0 in text)
        {
            int c = c0 == '\n' ? '\r' : c0;
            ring[tail] = c;
            tail = (tail + 1) & 255;
        }
        machine.SetProgramVariable("charTail", tail);
    }

    string State()
    {
        StringBuilder sb = new StringBuilder();
        Transform hud = V(holodeck, "hud") as Transform;
        Transform scene = V(holodeck, "scene") as Transform;
        sb.Append("own=" + V(holodeck, "own") + " flat=" + V(holodeck, "flat") + " played=" + V(holodeck, "played")
                  + " hud=" + (hud == null ? "none" : hud.gameObject.activeInHierarchy.ToString()) + " world=" + (scene != null && scene.gameObject.activeInHierarchy));
        sb.Append(" | gameId=" + V(machine, "gameId") + " worldFrame=" + V(machine, "worldFrame") + " flatFrame=" + V(machine, "flatFrame")
                  + " shown=" + V(machine, "shownWidth") + "x" + V(machine, "shownHeight") + " display=" + V(machine, "displayWidth") + "x" + V(machine, "displayHeight") + " guestPointer=" + V(machine, "pointerX") + "," + V(machine, "pointerY") + " on=" + V(machine, "pointerOn"));
        UdonBehaviour pad = V(holodeck, "gamepad") as UdonBehaviour;
        UdonBehaviour keys = pad != null ? V(pad, "keys") as UdonBehaviour : null;
        if (keys != null) sb.Append(" capturing=" + V(keys, "capturing") + " playAway=" + V(keys, "playAway"));
        int[] target = V(pointer, "target") as int[], px = V(pointer, "px") as int[], py = V(pointer, "py") as int[];
        bool[] edge = V(pointer, "onEdge") as bool[], landed = V(pointer, "landedOn") as bool[];
        if (target != null) sb.Append(" | pointer target=" + target[1] + " px=" + px[1] + " py=" + py[1] + " edge=" + edge[1] + " landed=" + landed[1] + " ownHolo=" + V(pointer, "ownHolo") + " holoFlat=" + V(pointer, "holoFlat"));
        if (hud != null) sb.Append(" | hud at " + hud.position.ToString("F2") + " scale " + hud.localScale.ToString("F2"));
        VRCPlayerApi me = Networking.LocalPlayer;
        if (Utilities.IsValid(me))
        {
            VRCPlayerApi.TrackingData head = me.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            sb.Append(" | head " + head.position.ToString("F2") + " " + head.rotation.eulerAngles.ToString("F1"));
        }
        return sb.ToString();
    }

    void Look(Vector3 point)
    {
        foreach (MonoBehaviour b in Resources.FindObjectsOfTypeAll<MonoBehaviour>())
            if (b.GetType().Name == "ClientSimDesktopTrackingProvider" && b.isActiveAndEnabled)
            {
                b.GetType().GetMethod("LookTowardsPoint").Invoke(b, new object[] { point });
                return;
            }
        Log("no ClientSim desktop provider");
    }

    // a place on a quad of shape quadAspect at the picture's share (x, y): Share in EmuPointer the other way
    Vector3 OnPicture(Transform quad, float quadAspect, int w, int h, float x, float y)
    {
        float shape = (float)w / h;
        Vector3 local = new Vector3((x - 0.5f) / Mathf.Max(1f, quadAspect / shape), -(y - 0.5f) / Mathf.Max(1f, shape / quadAspect), 0f);
        return quad.TransformPoint(local);
    }

    void Update()
    {
        if (machine == null) return;
        string screen = Screen();
        if (screen != lastScreen && Time.frameCount % 15 == 0)
        {
            // the screen's lines that were not on it before, into the console's log
            HashSet<string> before = new HashSet<string>(lastScreen.Split('\n'));
            StringBuilder fresh = new StringBuilder();
            foreach (string l in screen.Split('\n'))
                if (!before.Contains(l) && l.Trim().Length > 0) fresh.Append(Now().ToString("F1", CultureInfo.InvariantCulture) + " | " + l.TrimEnd() + "\n");
            File.AppendAllText(Path.Combine(root, "Logs", "se_check_console.txt"), fresh.ToString());
            lastScreen = screen;
            File.WriteAllText(Path.Combine(root, "Logs", "se_check_screen.txt"), screen);
        }
        if (watchEvery > 0 && Now() >= watchAt) { watchAt = Now() + watchEvery; Log("watch " + State()); }
        Run(screen);
    }

    void Run(string screen)
    {
        while (step < script.Count)
        {
            string cmd = script[step].Trim();
            if (cmd.Length == 0 || cmd.StartsWith("#")) { step++; continue; }
            int sp = cmd.IndexOf(' ');
            string verb = sp < 0 ? cmd : cmd.Substring(0, sp), arg = sp < 0 ? "" : cmd.Substring(sp + 1);
            string[] a = arg.Split(' ');
            if (verb == "out") { outDir = arg; Directory.CreateDirectory(outDir); }
            else if (verb == "wait")
            {
                string re = arg;
                float limit = 600f;
                Match lm = Regex.Match(arg, "^(.*) (\\d+)$");
                if (lm.Success) { re = lm.Groups[1].Value; limit = float.Parse(lm.Groups[2].Value, CultureInfo.InvariantCulture); }
                if (waitUntil < 0) waitUntil = Now() + limit;
                if (!Regex.IsMatch(screen, re))
                {
                    if (Now() < waitUntil) return;
                    Log("timeout: " + re);
                }
                else Log("matched: " + re);
                waitUntil = -1f;
            }
            else if (verb == "type") { Type(arg.Replace("\\n", "\n")); Log("typed: " + arg); }
            else if (verb == "sleep")
            {
                if (waitUntil < 0) { waitUntil = Now() + float.Parse(arg, CultureInfo.InvariantCulture); return; }
                if (Now() < waitUntil) return;
                waitUntil = -1f;
            }
            else if (verb == "shot")
            {
                ScreenCapture.CaptureScreenshot(Path.Combine(outDir, arg + ".png"));
                Log("shot " + arg + " | " + State());
            }
            else if (verb == "power") { machine.SendCustomEvent("PowerOn"); Log("power"); }
            else if (verb == "menu")
            {
                // ClientSim's own menu put away (its notice that it is a simulator, as its button does)
                bool done = false;
                foreach (MonoBehaviour b in Resources.FindObjectsOfTypeAll<MonoBehaviour>())
                    if (b.GetType().Name == "ClientSimMenu" && b.gameObject.scene.IsValid())
                    {
                        b.GetType().GetMethod("WarningAccepted").Invoke(b, null);
                        System.Reflection.MethodInfo close = b.GetType().GetMethod("CloseMenu",
                            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic);
                        if (close != null && close.GetParameters().Length == 0) close.Invoke(b, null);
                        Log("ClientSim's menu closed");
                        done = true;
                    }
                if (!done)
                {
                    if (waitUntil < 0) waitUntil = Now() + 30f;
                    if (Now() < waitUntil) return;
                    Log("no ClientSim menu");
                }
                waitUntil = -1f;
            }
            else if (verb == "sit")
            {
                VRCStation[] stations = V(holodeck, "stations") as VRCStation[];
                stations[int.Parse(arg)].UseStation(Networking.LocalPlayer);
                Log("sit " + arg);
            }
            else if (verb == "stand") { holodeck.SendCustomEvent("StandUp"); Log("stand"); }
            else if (verb == "look")
                Look(new Vector3(float.Parse(a[0], CultureInfo.InvariantCulture), float.Parse(a[1], CultureInfo.InvariantCulture), float.Parse(a[2], CultureInfo.InvariantCulture)));
            else if (verb == "lookrel")
            {
                VRCPlayerApi.TrackingData head = Networking.LocalPlayer.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
                Look(head.position + new Vector3(float.Parse(a[0], CultureInfo.InvariantCulture), float.Parse(a[1], CultureInfo.InvariantCulture), float.Parse(a[2], CultureInfo.InvariantCulture)));
            }
            else if (verb == "edge" || verb == "holoedge")
            {
                float x = float.Parse(a[0], CultureInfo.InvariantCulture), y = float.Parse(a[1], CultureInfo.InvariantCulture);
                Transform quad;
                float aspect;
                int w, h;
                if (verb == "edge")
                {
                    quad = ((Collider)V(pointer, "screen")).transform;
                    aspect = (float)V(pointer, "aspect");
                    w = (int)V(machine, "shownWidth"); h = (int)V(machine, "shownHeight");
                }
                else
                {
                    Collider[] screens = V(pointer, "holoScreens") as Collider[];
                    quad = screens[(int)V(holodeck, "own")].transform;
                    aspect = (float)V(pointer, "holoAspect");
                    w = (int)V(machine, "displayWidth"); h = (int)V(machine, "displayHeight");
                }
                Vector3 point = OnPicture(quad, aspect, w, h, x, y);
                if ((int)V(holodeck, "own") < 0)
                {
                    // (standing, the view's turn about the upright is the body's: ClientSim turns only the head's tilt)
                    VRCPlayerApi me = Networking.LocalPlayer;
                    Vector3 way = point - me.GetTrackingData(VRCPlayerApi.TrackingDataType.Head).position;
                    me.TeleportTo(me.GetPosition(), Quaternion.Euler(0f, Mathf.Atan2(way.x, way.z) * Mathf.Rad2Deg, 0f));
                }
                Look(point);
                Log(verb + " " + arg + " on " + w + "x" + h);
            }
            else if (verb == "key")
            {
                // a key pressed and let go, straight into the machine's key events (Linux's code: Escape is 1)
                int code = int.Parse(arg), tail = (int)V(machine, "keyTail");
                int[] ring = V(machine, "remoteKeys") as int[];
                ring[tail] = code | 65536;
                ring[(tail + 1) & 255] = code;
                machine.SetProgramVariable("keyTail", (tail + 2) & 255);
                Log("key " + code);
            }
            else if (verb == "lookpx")
            {
                // the view turned to a place of the Game view's picture (shares across and down), by the camera that draws it
                Camera best = null;
                foreach (Camera c in Camera.allCameras)
                    if (c.targetTexture == null && c.isActiveAndEnabled && (best == null || c.depth > best.depth)) best = c;
                float x = float.Parse(a[0], CultureInfo.InvariantCulture), y = float.Parse(a[1], CultureInfo.InvariantCulture);
                Ray r = best.ViewportPointToRay(new Vector3(x, 1f - y, 0f));
                Look(r.origin + r.direction * 3f);
                Log("lookpx " + arg + " by " + best.name);
            }
            else if (verb == "front")
            {
                // before the den's display, D metres from it, facing it
                Transform quad = ((Collider)V(pointer, "screen")).transform;
                float d = float.Parse(arg, CultureInfo.InvariantCulture);
                Vector3 at = quad.position - quad.forward * d;
                at.y = Networking.LocalPlayer.GetPosition().y;
                Networking.LocalPlayer.TeleportTo(at, Quaternion.LookRotation(Vector3.ProjectOnPlane(quad.forward, Vector3.up)));
                Log("front of the display at " + quad.position.ToString("F2") + ", forward " + quad.forward.ToString("F2") + ", scale " + quad.lossyScale.ToString("F2"));
            }
            else if (verb == "goto")
            {
                Networking.LocalPlayer.TeleportTo(new Vector3(float.Parse(a[0], CultureInfo.InvariantCulture), float.Parse(a[1], CultureInfo.InvariantCulture), float.Parse(a[2], CultureInfo.InvariantCulture)),
                    Quaternion.Euler(0f, float.Parse(a[3], CultureInfo.InvariantCulture), 0f));
                Log("goto " + arg);
            }
            else if (verb == "keys")
            {
                string req = Path.Combine(root, "se_check_keys.txt");
                if (waitUntil < 0) { File.WriteAllText(req, arg); waitUntil = Now() + 60f; Log("keys " + arg); return; }
                if (File.Exists(req) && Now() < waitUntil) return;
                if (File.Exists(req)) { Log("keys: nobody sent them"); File.Delete(req); }
                waitUntil = -1f;
            }
            else if (verb == "state") Log("state " + arg + " " + State());
            else if (verb == "watch") { watchEvery = float.Parse(arg, CultureInfo.InvariantCulture); watchAt = 0; }
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
        if (log != null) { Log("probe: end"); log.Close(); }
    }
}
