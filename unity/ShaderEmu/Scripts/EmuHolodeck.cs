using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon.Common;

// The holodecks (docs/holodeck.md): eight rooms, a seat each. Whoever sits in a seat has the
// room. This is local, one a client: it draws this visitor's own program's world round the seat
// they sit in, puts each occupant's display on their room's screen (their own machine's, or
// what EmuStreams receives of another's), and shows who holds which controller.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuHolodeck : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuStreams streams;
    public EmuMachine machine;
    public EmuPointer pointer;
    public EmuGamepad gamepad;
    public VRCStation[] stations;      // a room's seat
    public Transform[] frames;         // a room's own frame: its door behind, the room towards +z
    public Transform[] eyes;           // where a sitter's eyes are: the program's camera stands there
    public Vector3 roomSize;           // wide, high, deep
    public Transform content;          // the mask and the seal: one room's faces, moved to the room in use
    public Transform scene;            // the program's world (GpuHolodeck.shader)
    public Material sceneMaterial;
    public Transform[] screens;        // a room's screen: a unit quad, scaled
    public Renderer[] screenRenderers;
    public Material[] screenMaterials; // DisplayShow.shader, one a room
    public Vector3 screenAt, screenSize;   // in a room's frame, at its largest
    public TextMeshProUGUI[] doorLabels;   // whose a room is, by its door
    public TextMeshProUGUI[] plateLabels;  // and on its stand: what is held, what a key does
    public TextMeshProUGUI[] scaleLabels;
    public GameObject[] shooters, strategies;   // the controllers lying on a room's stand
    public Texture2D blackTexture;
    public float plane = 0.35f;        // the world's scale: the plane of the program's view that is 1.28 m wide

    private const int FirstSurface = 9;   // of EmuStreams' surfaces: the holodecks' are 9 to 16

    private int count;
    private int[] occupant;            // a room's sitter, by player number; 0: nobody
    private int[] holds;               // and the controller in their hands
    private Texture[] showing;
    private int own = -1;              // the room this visitor sits in
    private int screenMode;            // this visitor's own screen: 0 on, 1 off
    private float nextAt;
    private bool[] trigger = new bool[2], grip = new bool[2];   // left, right: for the machine's host words
    private int[] told = new int[16];

    void Start()
    {
        count = stations.Length;
        occupant = new int[count];
        holds = new int[count];
        showing = new Texture[count];
        content.gameObject.SetActive(false);
        scene.gameObject.SetActive(false);
        sceneMaterial.SetFloat("_Plane", plane);
    }

    // ---- the seats (EmuSeat) ----

    public void Entered(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (!player.isLocal) return;
        own = room;
        EmuShare mine = hub.Mine();
        if (mine != null)
        {
            mine.holo = room;
            hub.StateOut();
        }
        // the world round this seat: the room's faces are where it shows
        Transform frame = frames[room];
        content.SetPositionAndRotation(frame.position, frame.rotation);
        scene.SetPositionAndRotation(eyes[room].position, eyes[room].rotation);
        Vector3 a = frame.TransformPoint(new Vector3(-roomSize.x / 2, 0, 0)), b = frame.TransformPoint(new Vector3(roomSize.x / 2, roomSize.y, roomSize.z));
        sceneMaterial.SetVector("_RoomMin", Vector3.Min(a, b));
        sceneMaterial.SetVector("_RoomMax", Vector3.Max(a, b));
        content.gameObject.SetActive(true);
        scene.gameObject.SetActive(true);
        if (pointer != null) pointer.ownHolo = room;
    }

    public void Exited(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (!player.isLocal) return;
        if (gamepad != null) gamepad.PutBack();
        own = -1;
        EmuShare mine = hub.Mine();
        if (mine != null)
        {
            mine.holo = -1;
            hub.StateOut();
        }
        content.gameObject.SetActive(false);
        scene.gameObject.SetActive(false);
        if (pointer != null) pointer.ownHolo = -1;
    }

    public int Own() { return own; }

    // ---- a stand's keys: they are the sitter's ----

    public void TakeShooter() { if (own >= 0) gamepad.Take(1, stations[own]); nextAt = 0f; }
    public void TakeStrategy() { if (own >= 0) gamepad.Take(2, stations[own]); nextAt = 0f; }
    public void PutBack() { gamepad.PutBack(); nextAt = 0f; }

    public void NextScreen()
    {
        if (own < 0) return;
        screenMode = 1 - screenMode;
        nextAt = 0f;
    }

    public void Larger() { Scale(-0.05f); }
    public void Smaller() { Scale(0.05f); }

    private void Scale(float by)
    {
        if (own < 0) return;
        plane = Mathf.Clamp01(plane + by);
        sceneMaterial.SetFloat("_Plane", plane);
        nextAt = 0f;
    }

    // ---- who sits where, and what each room shows ----

    void Update()
    {
        Pads();
        Tell();
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.25f;
        hub.Prune();
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        int myId = hub.MyId(), others = hub.ShareCount();
        // a room is whoever names it's: this visitor while they sit there, else the first of the others
        for (int k = 0; k < count; k++) { occupant[k] = 0; holds[k] = 0; }
        if (own >= 0) { occupant[own] = myId; holds[own] = mine.holding; }
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            int k = share.holo;
            if (k < 0 || k >= count || k == own) continue;
            if (occupant[k] == 0 || share.ownerId < occupant[k]) { occupant[k] = share.ownerId; holds[k] = share.holding; }
        }
        for (int k = 0; k < count; k++)
        {
            bool me = occupant[k] == myId, theirs = occupant[k] > 0 && !me;
            streams.SetWant(FirstSurface + k, theirs ? occupant[k] : 0);
            int slot = theirs ? streams.SlotOf(occupant[k]) : -1;
            Texture shown = blackTexture;
            int w = 0, h = 0;
            if (me) { shown = machine.displayTexture; w = machine.OwnWidth(); h = machine.OwnHeight(); }
            else if (slot >= 0) { shown = streams.Picture(slot); w = streams.Width(slot); h = streams.Height(slot); }
            Material material = screenMaterials[k];
            if (shown != showing[k])
            {
                showing[k] = shown;
                material.SetTexture("_MainTex", shown);
                material.SetVector("_TexSize", new Vector4(shown.width, shown.height, 0, 0));
            }
            material.SetVector("_Size", new Vector4(w, h, 0, 0));
            // this visitor may switch their own screen off; anybody else's is on
            screenRenderers[k].enabled = !me || screenMode == 0;

            string who = occupant[k] == 0 ? "FREE" : me ? Networking.LocalPlayer.displayName : NameOf(occupant[k]);
            if (doorLabels[k] != null && doorLabels[k].text != who) doorLabels[k].text = who;
            if (plateLabels[k] != null)
            {
                string says = occupant[k] == 0 ? "Sit down to play" : !me ? who + " plays here"
                            : holds[k] == 1 ? "SHOOTER in hand" : holds[k] == 2 ? "STRATEGY in hand" : "Take a controller";
                if (plateLabels[k].text != says) plateLabels[k].text = says;
            }
            if (scaleLabels[k] != null && me) scaleLabels[k].text = "World " + Mathf.RoundToInt((1f - plane) * 100f);
            // a controller lies on the stand unless the sitter has it
            if (shooters[k] != null) shooters[k].SetActive(holds[k] != 1);
            if (strategies[k] != null) strategies[k].SetActive(holds[k] != 2);
        }
    }

    // ---- what the machine is told of where it is shown (docs/holodeck.md, "What the machine is told")

    public override void InputUse(bool value, UdonInputEventArgs args) { trigger[args.handType == HandType.LEFT ? 0 : 1] = value; }
    public override void InputGrab(bool value, UdonInputEventArgs args) { grip[args.handType == HandType.LEFT ? 0 : 1] = value; }

    // A sixteenth of a word: a length in millimetres, or a turn in 65,536ths, two's complement.
    private int Half(float v) { return Mathf.Clamp(Mathf.RoundToInt(v), -32768, 32767) & 0xffff; }
    private int Turn(float degrees) { return Mathf.RoundToInt(Mathf.DeltaAngle(0f, degrees) * (65536f / 360f)) & 0xffff; }

    // Eight words in halves: against the seat's eye point while seated, else the visitor's own feet.
    private void Tell()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (!Utilities.IsValid(player) || machine.machineMaterial == null) return;
        Vector3 origin = own >= 0 ? eyes[own].position : player.GetPosition();
        Quaternion back = Quaternion.Inverse(own >= 0 ? eyes[own].rotation : player.GetRotation());
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        Vector3 at = back * (head.position - origin) * 1000f, turn = (back * head.rotation).eulerAngles;
        int[] now = new int[16];
        // (bit 5: the hands are tracked, and a program may aim by one)
        now[0] = (player.IsUserInVR() ? 3 | 32 : 2) | (own >= 0 ? 16 : 0) | (Mathf.Min(VRCPlayerApi.GetPlayerCount(), 255) << 8);
        now[1] = player.playerId & 255;
        now[2] = Turn(turn.y);    // to the right
        now[3] = Turn(-turn.x);   // upwards
        now[4] = Half(at.x);
        now[5] = Half(at.y);
        now[6] = Half(at.z);
        now[7] = Turn(turn.z);
        for (int h = 0; h < 2; h++)
        {
            VRCPlayerApi.TrackingData hand = player.GetTrackingData(h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
            Vector3 place = back * (hand.position - origin) * 250f, way = back * (hand.rotation * (h == 0 ? Vector3.left : Vector3.right));
            // its place in 4 mm, ten bits each way, with its trigger and grip; then where it points:
            // its turn to the right and its rise, 65,536 to a turn as the head's
            int x = Mathf.Clamp(Mathf.RoundToInt(place.x), -512, 511) & 1023, y = Mathf.Clamp(Mathf.RoundToInt(place.y), -512, 511) & 1023;
            int z = Mathf.Clamp(Mathf.RoundToInt(place.z), -512, 511) & 1023;
            int word = x | (y << 10) | (z << 20);
            now[8 + h * 4] = word & 0xffff;
            now[9 + h * 4] = ((word >> 16) & 0x3fff) | (trigger[h] ? 1 << 14 : 0) | (grip[h] ? 1 << 15 : 0);
            now[10 + h * 4] = Turn(Mathf.Atan2(way.x, way.z) * Mathf.Rad2Deg);
            now[11 + h * 4] = Turn(Mathf.Asin(Mathf.Clamp(way.y, -1f, 1f)) * Mathf.Rad2Deg);
        }
        for (int i = 0; i < 16; i++)
        {
            if (now[i] == told[i]) continue;
            told[i] = now[i];
            machine.machineMaterial.SetInt("_HostS" + i, now[i]);
        }
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

    // A controller somebody holds is in their right hand, on every client: each player's own
    // object carries the two models.
    private void Pads()
    {
        hub.Prune();
        Pad(hub.Mine(), Networking.LocalPlayer);
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            Pad(share, Networking.GetOwner(share.gameObject));
        }
    }

    private void Pad(EmuShare share, VRCPlayerApi player)
    {
        if (share == null || share.pads == null) return;
        bool there = Utilities.IsValid(player) && share.holding > 0;
        for (int i = 0; i < share.pads.Length; i++)
            if (share.pads[i] != null && share.pads[i].gameObject.activeSelf != (there && share.holding == i + 1)) share.pads[i].gameObject.SetActive(there && share.holding == i + 1);
        if (!there || share.holding > share.pads.Length || share.pads[share.holding - 1] == null) return;
        Transform pad = share.pads[share.holding - 1];
        if (player.isLocal && !player.IsUserInVR())
        {
            // no hands on a desktop: before the chest
            VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
            pad.SetPositionAndRotation(head.position + head.rotation * new Vector3(0f, -0.28f, 0.45f), head.rotation * Quaternion.Euler(-60f, 0f, 0f));
            return;
        }
        Vector3 at = player.GetBonePosition(HumanBodyBones.RightHand);
        Quaternion turn = player.GetBoneRotation(HumanBodyBones.RightHand);
        if (player.isLocal || at == Vector3.zero)
        {
            // this visitor's own, or an avatar with no hand bone: the tracked hand
            VRCPlayerApi.TrackingData hand = player.GetTrackingData(VRCPlayerApi.TrackingDataType.RightHand);
            at = hand.position;
            turn = hand.rotation;
        }
        pad.SetPositionAndRotation(at, turn * Quaternion.Euler(0f, 90f, 0f));
    }
}
