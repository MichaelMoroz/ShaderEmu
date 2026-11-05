using UdonSharp;
using UnityEngine;
using TMPro;
using UnityEngine.UI;
using VRC.SDKBase;

// The classroom's computers (docs/world/stations.md): twelve tubes, one a visitor whose machine is on,
// and three more in the side rooms off the corridor (places 12 to 14: N13 to N15 on their doors).
// A visitor's place is a number in their own EmuShare, so nobody can move anybody else; whoever
// holds a place sends their display (EmuShareHub does, as it does anyway), EmuStreams receives
// it and this puts it on that place's tube. A visitor's own tube shows their machine itself.
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
    public Material ownSpeed;          // and on the den's own tower, and the wall: the machine the wall shows
    public GameObject[] linkLamps;     // lit on a place's monitor while its owner's browser wants addresses
    public GameObject[] powerLamps;    // a place's monitor's and tower's lamps: lit while its owner's machine is on
    public GameObject ownLamps;        // and the den's own tower's
    public Transform[] linkSpots;      // where the links panel stands before a place's tube
    public AudioSource click;          // a key of a monitor or a tower
    public AudioClip clickSound;
    // Blink.shader: a tower's disk lamp flickers as its machine's registers change, its network
    // socket's (and its port's on a hub) as its packets go; a free place's power key beckons.
    public Material[] diskLamps, netLamps, beacons;
    public Material ownDisk;           // the den's own tower's
    public Material[] hubLamps;        // a port of a hub each
    public int[] hubPlaces;            // and the place it serves; -1: a lead to another hub
    public GameObject fields;          // an address to open and text to type: before this visitor's own keyboard
    public Transform[] fieldSpots;
    public EmuPaste paste;             // the text field's, which types at whichever keyboard is this visitor's
    // The speed's slider on each tower: its owner's machine's speed step, which only the owner moves
    // (their EmuShare's speed carries it to everybody); the steps of EmuMachine.speedSlider.
    public Slider[] speedSliders;
    // Each monitor's volume (EmuPlace.volume): the owner's own machine's sound, as the control panel's.
    public Slider[] volumeSliders;

    private const int FirstSurface = 1;   // of EmuStreams' surfaces: the classroom's are 1 to 12
    private const int SideSurface = 21;   // and the side rooms' 21 to 23 (13 to 20 are the holodecks')
    private const int SidePlace = 12;
    private const int Steady = 0xffff, Beckon = 0x00ff;

    private int count;
    private int[] owner, claimed, mode, netSeen;
    private int[] localMode;           // what this visitor chose to see on somebody else's tube; -1: what its owner shows
    private Texture[] showing;
    private float nextAt;
    private bool left;                 // this visitor gave their place up: none is found for them until they ask
    private bool wasOn;
    private int fieldsAt = -1;
    private int tubeFor = -1;          // the place whose tube was last turned to what its machine has
    private bool tubeHad;              // and whether the machine had a display then
    private int[] speedShown;

    void Start()
    {
        count = screens.Length;
        owner = new int[count];
        claimed = new int[count];
        mode = new int[count];
        netSeen = new int[count];
        localMode = new int[count];
        for (int s = 0; s < count; s++) localMode[s] = -1;
        speedShown = new int[count];
        for (int s = 0; s < count; s++) speedShown[s] = -1;
        showing = new Texture[count];
        for (int s = 0; s < count; s++) owner[s] = -1;   // not looked at yet
        if (fields != null) fields.SetActive(false);
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
    public void Take8() { Take(8); }
    public void Take9() { Take(9); }
    public void Take10() { Take(10); }
    public void Take11() { Take(11); }
    public void Take12() { Take(12); }
    public void Take13() { Take(13); }
    public void Take14() { Take(14); }

    // The place this visitor has, or -1.
    public int Own()
    {
        EmuShare mine = hub.Mine();
        if (mine == null || owner == null) return -1;
        int s = mine.station;
        return s >= 0 && s < count && owner[s] == hub.MyId() ? s : -1;
    }

    // A visitor sits down at a place nobody has, which starts their machine if it is off; at
    // their own, the key turns the tube between the display and the console; at somebody
    // else's, it turns that tube for this visitor alone.
    private void Take(int s)
    {
        EmuShare mine = hub.Mine();
        if (mine == null || s >= count) return;
        if (owner[s] > 0 && owner[s] != hub.MyId())
        {
            Look(s, 1 - mode[s]);
            return;
        }
        if (owner[s] == hub.MyId()) mine.stationMode = 1 - mine.stationMode;
        mine.station = s;
        left = false;
        if (!machine.Powered()) machine.PowerOn();
        hub.StateOut();
        nextAt = 0;
    }

    // The place is given up: free for anybody, and none is found for this visitor until they
    // take one or switch their machine on again.
    private void Leave(int s)
    {
        EmuShare mine = hub.Mine();
        if (mine == null || s != Own()) return;
        if (keyboards != null && s < keyboards.Length) keyboards[s].SetCapture(false);
        mine.station = -1;
        left = true;
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

    // Somebody else's tube shown as this visitor wants (0 the display, 1 the console), for them alone.
    private void Look(int s, int to)
    {
        localMode[s] = to;
        nextAt = 0;
    }

    // A key of a place's monitor or tower (EmuPlace): its owner's machine's, and nobody else's.
    // The power key of a free place is anybody's: it starts their machine there. CONSOLE and
    // DISPLAY on another's turn this visitor's view; the pointer's switches are anybody's.
    public void Act(int s, int what)
    {
        if (what == 0 && s < count && owner[s] <= 0)
        {
            Take(s);
            return;
        }
        bool others = s < count && owner[s] > 0 && owner[s] != hub.MyId();
        if (s != Own() && !(others && (what == 5 || what == 6)) && what != 10 && what != 11) return;
        if (click != null && clickSound != null)
        {
            click.transform.position = linkSpots != null && s < linkSpots.Length ? linkSpots[s].position : transform.position;
            click.PlayOneShot(clickSound, 0.5f);
        }
        if (what == 10 || what == 11)
        {
            if (pointer == null) return;
            if (what == 10) pointer.PhysicalButtons();
            else pointer.SteadyPointer();
            return;
        }
        if (s != Own())
        {
            Look(s, what == 5 ? 1 : 0);
            return;
        }
        if (what == 0) machine.Power();
        else if (what == 1) machine.ResetMachine();
        else if (what == 3) terminal.ShowTab((terminal.Shown() + 1) % EmuTerminal.Tabs);
        else if (what == 4) terminal.ShowTab((terminal.Shown() + EmuTerminal.Tabs - 1) % EmuTerminal.Tabs);
        else if (what == 5) SetMode(s, 1);
        else if (what == 6) SetMode(s, 0);
        else if (what == 7) machine.PlaceLinks(false, null);
        else if (what == 8 && linkSpots != null && s < linkSpots.Length) machine.PlaceLinks(true, linkSpots[s]);
        else if (what == 9) Leave(s);
    }

    // A tower's slider was moved: its owner's machine takes the step; anybody else's move is undone.
    public void SpeedMoved(int s, Slider slider)
    {
        if (slider == null || speedShown == null || s >= count) return;
        int step = Mathf.RoundToInt(slider.value);
        if (s != Own())
        {
            if (speedShown[s] >= 0) slider.SetValueWithoutNotify(speedShown[s]);
            return;
        }
        machine.SetSpeedStep(step);
        speedShown[s] = step;
        EmuShare mine = hub.Mine();
        if (mine != null && mine.speed != step)
        {
            mine.speed = step;
            hub.StateOut();
        }
    }

    // A monitor's volume was moved: its owner's sound takes it (the panel's slider applies it).
    public void VolumeMoved(int s, Slider slider)
    {
        if (slider == null || s != Own() || machine.sound == null || machine.sound.volumeSlider == null) return;
        machine.sound.volumeSlider.value = slider.value;
    }

    // Whose each place is: of those who name it and have their machine on, the visitor who came first.
    private void Assign()
    {
        hub.Prune();
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        int myId = hub.MyId(), others = hub.ShareCount();
        bool on = machine.Powered();
        if (on && !wasOn) left = false;
        wasOn = on;
        for (int s = 0; s < count; s++) claimed[s] = 0;
        if (on && mine.station >= 0 && mine.station < count) claimed[mine.station] = myId;
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            int s = share.station;
            if ((share.flags & 1) == 0 || s < 0 || s >= count) continue;
            if (claimed[s] == 0 || share.ownerId < claimed[s]) claimed[s] = share.ownerId;
        }
        if (mine.station < 0 || mine.station >= count || claimed[mine.station] != myId)
        {
            // none, or somebody earlier has it: the first that is free, for a machine that is on
            int free = -1;
            for (int s = 0; s < count && free < 0 && on && !left; s++)
                if (claimed[s] == 0) free = s;
            if (mine.station != free)
            {
                mine.station = free;
                hub.StateOut();
            }
            if (free >= 0) claimed[free] = myId;
        }
        // this visitor types and points at their own place: its keys go into their own machine,
        // as characters or key events by what its tube shows
        int own = mine.station >= 0 && mine.station < count && claimed[mine.station] == myId ? mine.station : -1;
        // the tube follows the machine: the console until it has a display, then the display.
        // Between the two its owner's keys choose.
        bool has = machine.OwnWidth() > 0;
        if (own >= 0 && (own != tubeFor || has != tubeHad) && mine.stationMode != (has ? 0 : 1))
        {
            mine.stationMode = has ? 0 : 1;
            hub.StateOut();
        }
        tubeFor = own;
        tubeHad = has;
        // this visitor's machine's speed step, for their tower's slider on everybody's client
        int step = machine.SpeedStep();
        if (mine.speed != step)
        {
            mine.speed = step;
            hub.StateOut();
        }
        if (pointer != null)
        {
            pointer.ownStation = own;
            pointer.ownDisplay = mine.stationMode == 0;
        }
        if (own >= 0 && keyboards != null && own < keyboards.Length) keyboards[own].Feed(machine, mine.stationMode == 0);
        Fields(own);
        // the den's own tower and the wall's window: the machine the wall shows
        int wall = hub.WallSlot();
        bool wallOn = hub.WallIsOwn() ? on : wall >= 0 && streams.Width(wall) > 0;
        if (ownSpeed != null)
        {
            ownSpeed.SetFloat("_All", !wallOn ? -1f : hub.WallIsOwn() ? machine.mipsAll * 10f : streams.MipsAll(wall));
            ownSpeed.SetFloat("_Core", !wallOn ? -1f : hub.WallIsOwn() ? machine.mipsCore * 10f : streams.MipsCore(wall));
        }
        if (ownLamps != null && ownLamps.activeSelf != wallOn) ownLamps.SetActive(wallOn);
        if (ownDisk != null) ownDisk.SetFloat("_Pattern", hub.WallIsOwn() ? machine.activity : wall >= 0 ? streams.Activity(wall) : 0);
        for (int s = 0; s < count; s++)
        {
            if (claimed[s] != owner[s]) localMode[s] = -1;   // (a choice about the one before)
            int wants = claimed[s] == myId ? mine.stationMode : claimed[s] > 0 ? (localMode[s] >= 0 ? localMode[s] : ModeOf(claimed[s])) : 0;
            bool relabel = wants != mode[s];
            if (claimed[s] != owner[s])
            {
                owner[s] = claimed[s];
                netSeen[s] = -1;
                relabel = true;
            }
            mode[s] = wants;
            bool theirs = owner[s] > 0 && owner[s] != myId;
            streams.SetWant(s < SidePlace ? FirstSurface + s : SideSurface + s - SidePlace, theirs ? owner[s] : 0);
            int slot = theirs ? streams.SlotOf(owner[s]) : -1;
            if (relabel)
            {
                // the key says whose the place is, what its tube shows, and to its owner that it switches
                if (labels[s] != null) labels[s].text = owner[s] == 0 ? "FREE" : owner[s] == myId ? Networking.LocalPlayer.displayName : NameOf(owner[s]);
                if (modes != null && modes[s] != null)
                    modes[s].text = owner[s] == 0 ? "press POWER: yours starts here" : (wants == 1 ? "CONSOLE" : "DISPLAY") + (owner[s] == myId ? " - press to switch" : " - press to switch your view");
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
                // a console is always there to show; a display only while its machine has one
                shown = wants == 1 ? streams.Console(slot) : streams.Picture(slot);
                w = wants == 1 ? shown.width : streams.Width(slot);
                h = wants == 1 ? shown.height : streams.Height(slot);
            }
            // the tower's window: its owner's machine's speed, in tenths of a million instructions a second
            if (speeds != null && s < speeds.Length && speeds[s] != null)
            {
                float all = -1f, core = -1f;
                if (owner[s] == myId) { all = machine.mipsAll * 10f; core = machine.mipsCore * 10f; }
                else if (slot >= 0) { all = streams.MipsAll(slot); core = streams.MipsCore(slot); }
                speeds[s].SetFloat("_All", all);
                speeds[s].SetFloat("_Core", core);
            }
            if (speedSliders != null && s < speedSliders.Length && speedSliders[s] != null)
            {
                int shownStep = owner[s] == myId ? step : owner[s] > 0 ? SpeedOf(owner[s]) : 3;
                Slider slider = speedSliders[s];
                if (shownStep != speedShown[s] || Mathf.RoundToInt(slider.value) != shownStep)
                {
                    speedShown[s] = shownStep;
                    slider.SetValueWithoutNotify(shownStep);
                }
                bool mineToMove = owner[s] == myId;
                if (slider.interactable != mineToMove) slider.interactable = mineToMove;
            }
            if (volumeSliders != null && s < volumeSliders.Length && volumeSliders[s] != null)
            {
                Slider volume = volumeSliders[s];
                bool ours = owner[s] == myId;
                if (ours && machine.sound != null && machine.sound.volumeSlider != null && volume.value != machine.sound.volumeSlider.value)
                    volume.SetValueWithoutNotify(machine.sound.volumeSlider.value);
                if (volume.interactable != ours) volume.interactable = ours;
            }
            if (linkLamps != null && s < linkLamps.Length && linkLamps[s] != null) linkLamps[s].SetActive(owner[s] == myId && machine.LinksWanted());
            if (powerLamps != null && s < powerLamps.Length && powerLamps[s] != null && powerLamps[s].activeSelf != (owner[s] > 0))
                powerLamps[s].SetActive(owner[s] > 0);
            Lamps(s, owner[s] == myId ? machine.activity : slot >= 0 ? streams.Activity(slot) : 0);
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

    // A place's small lamps: the disk's by its machine's registers, the network's lit while the
    // machine is on and flickering in a half second in which it sent packets; the power key's
    // beckons while the place is free.
    private void Lamps(int s, int activity)
    {
        if (diskLamps != null && s < diskLamps.Length && diskLamps[s] != null) diskLamps[s].SetFloat("_Pattern", activity);
        if (beacons != null && s < beacons.Length && beacons[s] != null) beacons[s].SetFloat("_Pattern", owner[s] == 0 ? Beckon : 0);
        int sent = owner[s] == 0 ? -1 : owner[s] == hub.MyId() ? machine.netSent + machine.netReceived : NetOf(owner[s]);
        int lit = owner[s] == 0 ? 0 : sent != netSeen[s] && netSeen[s] >= 0 ? Random.Range(0x1111, 0xffff) | 0x8001 : Steady;
        netSeen[s] = sent;
        if (netLamps != null && s < netLamps.Length && netLamps[s] != null) netLamps[s].SetFloat("_Pattern", lit);
        if (hubLamps == null) return;
        for (int p = 0; p < hubLamps.Length; p++)
            if (hubPlaces[p] == s && hubLamps[p] != null) hubLamps[p].SetFloat("_Pattern", lit);
    }

    // The fields stand before this visitor's own keyboard, and type at it.
    private void Fields(int own)
    {
        if (fields == null || own == fieldsAt) return;
        fieldsAt = own;
        fields.SetActive(own >= 0);
        if (own < 0 || fieldSpots == null || own >= fieldSpots.Length) return;
        fields.transform.SetPositionAndRotation(fieldSpots[own].position, fieldSpots[own].rotation);
        if (paste != null) paste.keyboard = keyboards[own];
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

    private int SpeedOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share.speed;
        }
        return 3;   // (EmuShare.speed's default)
    }

    private int NetOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share.netSeq;
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
