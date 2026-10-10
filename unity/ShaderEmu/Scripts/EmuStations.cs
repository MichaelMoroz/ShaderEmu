using UdonSharp;
using UnityEngine;
using TMPro;
using VRC.SDKBase;

// The classroom's computers (docs/stations.md): eight tubes, one a visitor. A visitor's place is
// a number in their own EmuShare, so nobody can move anybody else; whoever holds a place sends
// their display (EmuShareHub does, as for a watcher), EmuStreams receives it and this puts it
// on that place's tube. A visitor's own tube shows their machine itself.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuStations : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuStreams streams;
    public EmuMachine machine;
    public EmuTerminal terminal;       // this visitor's own console
    public Material[] screens;         // CRT.shader, one a place
    public RenderTexture ownConsole;   // this visitor's console as a picture (640 x 480)
    public Texture2D blackTexture;
    public TextMeshProUGUI[] labels;   // whose it is
    public TextMeshProUGUI[] modes;    // what its tube shows, and what a press does
    public EmuPointer pointer;         // the beams: this visitor's own keyboard and tube answer them
    public EmuKeyboard[] keyboards;    // a place's own
    public Material[] speeds;          // SevenSeg.shader on each tower: its owner's machine's speed
    public Material ownSpeed;          // and on the den's own tower: this visitor's
    public GameObject[] linkLamps;     // lit on a place's monitor while its owner's browser wants addresses
    public GameObject[] powerLamps;    // a place's monitor's and tower's lamps: lit while its owner's machine is on
    public GameObject ownLamps;        // and the den's own tower's
    public Transform[] linkSpots;      // where the links panel stands before a place's tube
    public AudioSource click;          // a key of a monitor or a tower
    public AudioClip clickSound;

    private const int FirstSurface = 1;   // of EmuStreams' surfaces: the classroom's are 1 to 8

    private int count;
    private int[] owner, claimed, mode;
    private Texture[] showing;
    private float nextAt;

    void Start()
    {
        count = screens.Length;
        owner = new int[count];
        claimed = new int[count];
        mode = new int[count];
        showing = new Texture[count];
        for (int s = 0; s < count; s++) owner[s] = -1;   // not looked at yet
    }

    void Update()
    {
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.5f;
        Assign();
    }

    public void Take0() { Take(0); }
    public void Take1() { Take(1); }
    public void Take2() { Take(2); }
    public void Take3() { Take(3); }
    public void Take4() { Take(4); }
    public void Take5() { Take(5); }
    public void Take6() { Take(6); }
    public void Take7() { Take(7); }

    public void Keys0() { Keys(0); }
    public void Keys1() { Keys(1); }
    public void Keys2() { Keys(2); }
    public void Keys3() { Keys(3); }
    public void Keys4() { Keys(4); }
    public void Keys5() { Keys(5); }
    public void Keys6() { Keys(6); }
    public void Keys7() { Keys(7); }

    // The place this visitor has, or -1.
    public int Own()
    {
        EmuShare mine = hub.Mine();
        if (mine == null || owner == null) return -1;
        int s = mine.station;
        return s >= 0 && s < count && owner[s] == hub.MyId() ? s : -1;
    }

    // "Use my keyboard" at a place: its owner's real keyboard types there, as at the console.
    private void Keys(int s)
    {
        if (s >= count || owner[s] != hub.MyId() || keyboards == null || s >= keyboards.Length) return;
        keyboards[s].ToggleCapture();
    }

    // A visitor sits down at a place nobody has; at their own, the key turns the tube between
    // the display and the console.
    private void Take(int s)
    {
        EmuShare mine = hub.Mine();
        if (mine == null || s >= count || (owner[s] > 0 && owner[s] != hub.MyId())) return;
        if (owner[s] == hub.MyId()) mine.stationMode = 1 - mine.stationMode;
        mine.station = s;
        hub.StateOut();
        nextAt = 0;
    }

    // What a place's tube shows: 0 the display, 1 the console. Its owner's to set.
    public void SetMode(int s, int to)
    {
        EmuShare mine = hub.Mine();
        if (mine == null || s != Own() || mine.stationMode == to) return;
        mine.stationMode = to;
        hub.StateOut();
        nextAt = 0;
    }

    // A key of a place's monitor or tower (EmuPlace): its owner's machine's, and nobody else's.
    public void Act(int s, int what)
    {
        if (s != Own()) return;
        if (click != null && clickSound != null)
        {
            click.transform.position = linkSpots != null && s < linkSpots.Length ? linkSpots[s].position : transform.position;
            click.PlayOneShot(clickSound, 0.5f);
        }
        if (what == 0) machine.Power();
        else if (what == 1) machine.ResetMachine();
        else if (what == 2) machine.SpeedMode();
        else if (what == 3) terminal.ShowTab((terminal.Shown() + 1) % EmuTerminal.Tabs);
        else if (what == 4) terminal.ShowTab((terminal.Shown() + EmuTerminal.Tabs - 1) % EmuTerminal.Tabs);
        else if (what == 5) SetMode(s, 1);
        else if (what == 6) SetMode(s, 0);
        else if (what == 7) machine.PlaceLinks(false, null);
        else if (what == 8 && linkSpots != null && s < linkSpots.Length) machine.PlaceLinks(true, linkSpots[s]);
    }

    // Whose each place is: of those who name it, the visitor who came first.
    private void Assign()
    {
        hub.Prune();
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        int myId = hub.MyId(), others = hub.ShareCount();
        for (int s = 0; s < count; s++) claimed[s] = 0;
        if (mine.station >= 0 && mine.station < count) claimed[mine.station] = myId;
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            int s = share.station;
            if (s >= 0 && s < count && (claimed[s] == 0 || share.ownerId < claimed[s])) claimed[s] = share.ownerId;
        }
        if (mine.station < 0 || mine.station >= count || claimed[mine.station] != myId)
        {
            // none, or somebody earlier has it: the first that is free
            int free = -1;
            for (int s = 0; s < count && free < 0; s++)
                if (claimed[s] == 0) free = s;
            mine.station = free;
            if (free >= 0) claimed[free] = myId;
            hub.StateOut();
        }
        // this visitor types and points at their own place: its keys go into their own machine,
        // as characters or key events by what its tube shows
        int own = mine.station >= 0 && mine.station < count && claimed[mine.station] == myId ? mine.station : -1;
        if (pointer != null)
        {
            pointer.ownStation = own;
            pointer.ownDisplay = mine.stationMode == 0;
        }
        if (own >= 0 && keyboards != null && own < keyboards.Length) keyboards[own].Feed(machine, mine.stationMode == 0);
        if (ownSpeed != null)
        {
            ownSpeed.SetFloat("_All", machine.Powered() ? machine.mipsAll * 10f : -1f);
            ownSpeed.SetFloat("_Core", machine.Powered() ? machine.mipsCore * 10f : -1f);
        }
        if (ownLamps != null && ownLamps.activeSelf != machine.Powered()) ownLamps.SetActive(machine.Powered());
        for (int s = 0; s < count; s++)
        {
            int wants = claimed[s] == myId ? mine.stationMode : claimed[s] > 0 ? ModeOf(claimed[s]) : 0;
            bool relabel = wants != mode[s];
            if (claimed[s] != owner[s])
            {
                owner[s] = claimed[s];
                relabel = true;
            }
            mode[s] = wants;
            bool theirs = owner[s] > 0 && owner[s] != myId;
            streams.SetWant(FirstSurface + s, theirs ? owner[s] : 0);
            int slot = theirs ? streams.SlotOf(owner[s]) : -1;
            if (relabel)
            {
                // the key says whose the place is, what its tube shows, and to its owner that it switches
                if (labels[s] != null) labels[s].text = owner[s] == 0 ? "FREE" : owner[s] == myId ? Networking.LocalPlayer.displayName : NameOf(owner[s]);
                if (modes != null && modes[s] != null)
                    modes[s].text = owner[s] == 0 ? "press to sit here" : (wants == 1 ? "CONSOLE" : "DISPLAY") + (owner[s] == myId ? " - press to switch" : "");
            }
            Texture shown = blackTexture;
            int w = 0, h = 0;
            if (owner[s] == myId)
            {
                if (wants == 1)
                {
                    // this visitor's own console, as their terminal has it now
                    VRCGraphics.Blit(terminal.gridTexture, ownConsole, terminal.screenMaterial);
                    shown = ownConsole;
                    w = ownConsole.width;
                    h = ownConsole.height;
                }
                else
                {
                    shown = machine.displayTexture;
                    w = machine.OwnWidth();
                    h = machine.OwnHeight();
                }
            }
            else if (slot >= 0)
            {
                // a console is always there to show; a display only while its machine is on
                shown = wants == 1 ? streams.Console(slot) : streams.Picture(slot);
                w = wants == 1 ? shown.width : streams.Width(slot);
                h = wants == 1 ? shown.height : streams.Height(slot);
            }
            // the tower's window: its owner's machine's speed, in tenths of a million instructions a second
            if (speeds != null && s < speeds.Length && speeds[s] != null)
            {
                float all = -1f, core = -1f;
                if (owner[s] == myId && machine.Powered()) { all = machine.mipsAll * 10f; core = machine.mipsCore * 10f; }
                else if (slot >= 0 && streams.Width(slot) > 0) { all = streams.MipsAll(slot); core = streams.MipsCore(slot); }
                speeds[s].SetFloat("_All", all);
                speeds[s].SetFloat("_Core", core);
            }
            if (linkLamps != null && s < linkLamps.Length && linkLamps[s] != null) linkLamps[s].SetActive(owner[s] == myId && machine.LinksWanted());
            if (powerLamps != null && s < powerLamps.Length && powerLamps[s] != null)
            {
                bool on = owner[s] == myId ? machine.Powered() : slot >= 0 && streams.Width(slot) > 0;
                if (powerLamps[s].activeSelf != on) powerLamps[s].SetActive(on);
            }
            Material screen = screens[s];
            if (shown != showing[s])
            {
                showing[s] = shown;
                screen.SetTexture("_MainTex", shown);
                screen.SetVector("_TexSize", new Vector4(shown.width, shown.height, 0, 0));
            }
            screen.SetVector("_Size", new Vector4(w, h, 0, 0));
        }
    }

    private int ModeOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share.stationMode;
        }
        return 0;
    }

    private string NameOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share.ownerName;
        }
        return "?";
    }
}
