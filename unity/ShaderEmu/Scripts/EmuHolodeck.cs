using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDKBase;
using VRC.Udon.Common;

// The holodecks (docs/world/holodeck.md): eight rooms, a seat each. Whoever sits in a seat has the
// room. This is local, one a client: it draws this visitor's own program's world round the seat
// they sit in, puts each occupant's display on their room's screen (their own machine's, or
// what EmuStreams receives of another's), and says on each stand what its sitter's controls are.
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
    // The HUD layer (GpuHolodeckHud.shader; docs/world/holodeck.md, "The HUD layer"): what the program draws flat over its
    // picture, which the world cannot place, on a quad before the sitter: it follows the head's turn slowly, below the eyes.
    public Transform hud;
    public float hudDistance = 1.1f, hudWidth = 1.0f, hudDrop = 0.12f, hudFollow = 2.5f;   // metres, metres, metres, a second
    private float hudYaw, hudPitch;
    private bool hudPlaced;
    public Material sceneMaterial;
    public Transform[] screens;        // a room's screen: a unit quad, scaled
    public Renderer[] screenRenderers;
    public Material[] screenMaterials; // DisplayShow.shader, one a room
    public Vector3 screenAt, screenSize;   // in a room's frame: the small screen, in the console's face
    public Vector3 screenTurn;             // its tip back and its turn about the upright, degrees
    public Vector3 largeAt, largeSize;     // the large screen: upright before the seat
    public Transform[] screenGuards;       // each screen's mark (HoloGuard.shader), which goes where its screen goes
    public TextMeshProUGUI[] doorLabels;   // whose a room is, by its door
    public TextMeshProUGUI[] plateLabels;  // and on its stand: what is held, what a key does
    public TextMeshProUGUI[] scaleLabels;
    // A console's two sliders, a room: the world's size and the large screen's (WorldSlid, ScreenSlid)
    public Slider[] worldSliders, screenSliders;
    // and two more: the machine's sound (the sound panel's slider, EmuSound) and the other players' voices, for this visitor
    public Slider[] volumeSliders, voiceSliders;
    public TextMeshProUGUI[] volumeLabels, voiceLabels;
    public EmuSound sound;
    public TextMeshProUGUI[] screenLabels;
    public TextMeshProUGUI[] worldLabels;   // each console's 3D key: games only, or every program
    public Collider[] plateColliders;      // a console's own, which VRChat's laser presses: off for a sitter in a headset
    // The console can be put away (HideConsole): its model and the mark of it, one for all the
    // rooms, and a room's own plate, keys and screen; a small button in its place brings it back.
    public GameObject consolesModel, consolesGuard;
    public GameObject seatsModel, seatsGuard;   // the chairs go with it: one mesh for all the rooms too
    public EmuKeyboard keyboard;               // a keyboard before the seat, the sitter's own machine's: one, moved to their room
    public Vector3 keyboardAt, keyboardTurn;   // (before 6 November 2026 its place in a room's frame: now it hangs under the screen, PlaceKeyboard)
    public GameObject[] plates, plateGuards, keyPlates, showButtons;
    public Texture2D blackTexture;
    public float plane = 0.35f;        // the world's scale: the plane of the program's view that is 1.28 m wide
                                       // (for a program that does not say its player's eye: Natural)
    public float mouseCounts = 20f;    // a desktop sitter's mouse: counts told the game for one of Unity's "Mouse X"
    private float wheelGone;           // and the notches their wheel has turned up
    public bool beam = true;           // a sitter in a headset sees where their hand points (the console's Beam key)
    // A test of a join while someone sits (a crash on join): the master sits in room 0 by
    // itself, and every client logs the seats and the players as [SitTest] lines.
    public bool sitTest;
    private float sitTestAt;
    private bool sitTestSat;

    private const int FirstSurface = 13;  // of EmuStreams' surfaces: the holodecks' are 13 to 20

    private int count;
    private int[] occupant;            // a room's sitter, by player number; 0: nobody
    private int[] holds;               // and the controller in their hands
    private Texture[] showing;
    private int own = -1;              // the room this visitor sits in
    private int screenMode;            // this visitor's own screen: 0 the console's small one, 1 the large one before the seat
    private int[] placed;              // a room's screen as it is put now: 0 small, 1 large
    private float nextAt;
    private bool[] trigger = new bool[2], grip = new bool[2];   // left, right: for the machine's host words
    private int[] told = new int[16];
    private int roomMinId, roomMaxId;  // the sitter's room for HoloGuard.shader, which marks only there
    private bool leaving;              // this visitor asked to stand up (a jump): the seat lets them
    private bool respawned;
    private int putOut = -1;           // the room whose seat let go of them unasked
    private bool consoleHidden;        // this sitter put the console away
    private bool worldsForAll;         // this visitor's 3D key: any program's world round the seat, not the games' alone
    private float voices = 1f;         // the other players' voices and avatars while seated, of VRChat's own gains
    private bool flat;                 // their program shows a flat screen now, or draws no world: the large screen, no world
    private float mouseGone;           // a desktop sitter's mouse: how far right it has gone, in counts
    private bool firing;               // and its left button, which is the games' Ctrl
    private int played;                // the program whose world is round them: kept while another window has the keyboard
    private float inUse = -1f;         // the plane in use: Natural's, moved by the World slider
    private float shift;               // what the World slider moved it by, for the game played now
    private float screenScale = 1f;    // the large screen's size, by the Screen slider: half to twice
    private float[] placedScale;
    private int shiftFor;
    private const int Ctrl = 29, Pressed = 65536;
    // The programs that are played with both hands, by their numbers in linux/apps/gameid.h:
    // each hand has a beam there. Any other program's aim is the right hand's.
    private int[] twoHanded = { 8, 3 };   // nxphys; ClassiCube (the right hand breaks, the left places)

    void Start()
    {
        count = stations.Length;
        occupant = new int[count];
        holds = new int[count];
        showing = new Texture[count];
        placed = new int[count];
        placedScale = new float[count];
        content.gameObject.SetActive(false);
        scene.gameObject.SetActive(false);
        sceneMaterial.SetFloat("_Plane", plane);
        roomMinId = VRCShader.PropertyToID("_UdonHoloMin");
        roomMaxId = VRCShader.PropertyToID("_UdonHoloMax");
        VRCShader.SetGlobalVector(roomMinId, Vector4.zero);
        VRCShader.SetGlobalVector(roomMaxId, Vector4.zero);
    }

    // ---- the seats (EmuSeat) ----

    public void Entered(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (sitTest) Debug.Log("[SitTest] entered room " + room + ": player " + player.playerId + (player.isLocal ? " (me)" : ""));
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
        VRCShader.SetGlobalVector(roomMinId, Vector3.Min(a, b));
        VRCShader.SetGlobalVector(roomMaxId, Vector3.Max(a, b));
        flat = false;
        if (keyboard != null)
        {
            keyboard.Feed(machine, true);
            placed[room] = -1;   // (the screen placed again, and the keyboard under it: PlaceKeyboard)
        }
        Arrange();
        if (pointer != null) pointer.ownHolo = room;
        machine.keysOwn = true;   // (a sitter plays their own machine, whatever their wall shows)
        Hear(voices);
        // without a headset the visitor's own keyboard is the game's (Insert gives it back);
        // with one their controllers are, and their beams press the console's keys
        if (!player.IsUserInVR())
        {
            if (gamepad != null && gamepad.keys != null) gamepad.keys.SetCapture(true);
            return;
        }
        if (gamepad != null) gamepad.Take(stations[room]);
        BeamKeys(room, true);
    }

    // A console's keys are the sitter's beams' (VRChat's laser did not reach them from the seat).
    private void BeamKeys(int room, bool on)
    {
        if (pointer != null) pointer.holoKeys = on;
        if (plateColliders != null && room < plateColliders.Length && plateColliders[room] != null) plateColliders[room].enabled = !on;
    }

    public void Exited(int room, VRCPlayerApi player)
    {
        nextAt = 0f;
        if (sitTest) Debug.Log("[SitTest] exited room " + room + ": player " + player.playerId + (player.isLocal ? " (me)" : ""));
        if (!player.isLocal) return;
        // A stick stood the sitter up in a headset whatever the station was told: unless they
        // asked (a jump) or were sent back to the start, they are sat down again.
        if (!leaving && own == room)
        {
            putOut = room;
            SendCustomEventDelayedFrames(nameof(SitAgain), 2);
            return;
        }
        Leave();
    }

    public void SitAgain()
    {
        int room = putOut;
        putOut = -1;
        if (room < 0 || own != room) return;
        if (respawned) Leave();
        else stations[room].UseStation(Networking.LocalPlayer);
    }

    public override void OnPlayerRespawn(VRCPlayerApi player)
    {
        if (player == null || !player.isLocal || own < 0) return;
        if (putOut >= 0) respawned = true;   // (SitAgain is on its way: it lets go instead)
        else Leave();
    }

    private void Leave()
    {
        leaving = false;
        respawned = false;
        if (own < 0) return;
        if (firing && gamepad != null && gamepad.keys != null) gamepad.keys.PushEvent(Ctrl);
        firing = false;
        if (gamepad != null)
        {
            gamepad.PutBack();
            if (gamepad.keys != null) gamepad.keys.SetCapture(false);
        }
        machine.keysOwn = false;
        Hear(1f);
        BeamKeys(own, false);
        own = -1;
        played = 0;
        EmuShare mine = hub.Mine();
        if (mine != null)
        {
            mine.holo = -1;
            hub.StateOut();
        }
        consoleHidden = false;
        flat = false;
        Arrange();
        VRCShader.SetGlobalVector(roomMinId, Vector4.zero);
        VRCShader.SetGlobalVector(roomMaxId, Vector4.zero);
        if (pointer != null) pointer.ownHolo = -1;
    }

    // The seat keeps its sitter whatever the sticks do (they are the game's): jumping stands
    // up a sitter without a headset whose keyboard is their own again (a controller's jump and
    // the space bar are the game's; a headset's way out is both grips, or the console's Exit).
    public override void InputJump(bool value, UdonInputEventArgs args)
    {
        if (!value || own < 0 || gamepad == null || gamepad.Holding() != 0) return;
        if (gamepad.keys != null && gamepad.keys.Capturing()) return;
        leaving = true;
        respawned = false;
        stations[own].ExitStation(Networking.LocalPlayer);
    }

    // End stands the sitter up whatever they hold and whoever has their keyboard: the one way
    // out that needs nothing else to work first (a desktop's, mostly: its keys are the game's).
    public void StandUp()
    {
        if (own < 0) return;
        if (gamepad != null)
        {
            gamepad.PutBack();
            if (gamepad.keys != null) gamepad.keys.SetCapture(false);
        }
        leaving = true;
        respawned = false;
        stations[own].ExitStation(Networking.LocalPlayer);
    }

    public int Own() { return own; }
    public bool Flat() { return own >= 0 && flat; }

    // ---- the console put away and brought back (its Hide key, the small button, the Home key) ----

    public void HideConsole()
    {
        if (own < 0) return;
        consoleHidden = true;
        Arrange();
    }

    public void ShowConsole()
    {
        consoleHidden = false;
        Arrange();
    }

    // ---- where a hand points: a beam in the program's world (EmuPointer draws it, as the machine is told)

    public void ToggleBeam()
    {
        beam = !beam;
        Aims();
    }

    private void Aims()
    {
        if (pointer == null) return;
        VRCPlayerApi player = Networking.LocalPlayer;
        int hands = 0;
        if (beam && own >= 0 && !flat && Utilities.IsValid(player) && (player.IsUserInVR() || pointer.debugBeam))
        {
            hands = 2;
            for (int i = 0; i < twoHanded.Length; i++) if (twoHanded[i] == machine.gameId) hands = 3;
        }
        if (pointer.aimHands != hands) pointer.aimHands = hands;
    }

    private void Put(GameObject[] things, int k, bool on)
    {
        if (things != null && k < things.Length && things[k] != null && things[k].activeSelf != on) things[k].SetActive(on);
    }

    // What of the room this visitor sees: the world or the flat screen, the console or its button.
    private void Arrange()
    {
        bool seated = own >= 0, hidden = seated && consoleHidden, world = seated && !flat;
        if (consolesModel != null) consolesModel.SetActive(!hidden);
        if (consolesGuard != null) consolesGuard.SetActive(!hidden);
        if (seatsModel != null) seatsModel.SetActive(!hidden);
        if (seatsGuard != null) seatsGuard.SetActive(!hidden);
        // (the keyboard is seen wherever the sitter's screen is: under it)
        bool keys = seated && (!hidden || flat);
        if (keyboard != null && keyboard.gameObject.activeSelf != keys)
        {
            if (!keys) { keyboard.Release(0); keyboard.Release(1); }   // (a key held as it goes comes up)
            keyboard.gameObject.SetActive(keys);
        }
        for (int k = 0; k < count; k++)
        {
            bool mine = k == own, away = mine && hidden, screen = !away || flat;
            Put(plates, k, !away);
            Put(plateGuards, k, !away);
            Put(keyPlates, k, !away);
            Put(showButtons, k, away);
            if (screens[k].gameObject.activeSelf != screen) screens[k].gameObject.SetActive(screen);
            if (screenGuards != null && k < screenGuards.Length && screenGuards[k] != null) screenGuards[k].gameObject.SetActive(screen);
        }
        content.gameObject.SetActive(world);
        scene.gameObject.SetActive(world);
        if (hud != null && hud.gameObject.activeSelf != world) { hud.gameObject.SetActive(world); hudPlaced = false; }
        if (pointer != null) { pointer.holoFlat = seated && flat; pointer.holoHidden = hidden; }
        Aims();
        nextAt = 0f;
    }

    // ---- a stand's keys: they are the sitter's ----

    // The games made for a world round the seat (linux/apps/gameid.h): Doom, Quake, ClassiCube, Quake III, nxphys, Half-Life, Jedi Outcast.
    private bool Registered(int id) { return id == 1 || id == 2 || id == 3 || id == 7 || id == 8 || id == 9 || id == 10; }

    // The console's 3D key: every program's world round the seat, or the registered games' alone.
    public void ToggleWorld()
    {
        worldsForAll = !worldsForAll;
        if (worldLabels == null) return;
        for (int k = 0; k < worldLabels.Length; k++)
            if (worldLabels[k] != null) worldLabels[k].text = worldsForAll ? "3D: every program" : "3D: games only";
    }

    public void NextScreen()
    {
        if (own < 0) return;
        screenMode = 1 - screenMode;
        nextAt = 0f;
    }

    // The sliders, by VRChat's laser or a desktop's click (WorldSlid, ScreenSlid), or by a
    // sitter's beam (WorldSlide, ScreenSlide: EmuPointer.holoSlide, 0 to 1 across the slot).
    public void WorldSlid() { if (own >= 0 && Has(worldSliders)) World(worldSliders[own].value); }
    public void ScreenSlid() { if (own >= 0 && Has(screenSliders)) ScreenSize(screenSliders[own].value); }
    public void WorldSlide() { if (own >= 0 && pointer != null) World(pointer.holoSlide); }
    public void ScreenSlide() { if (own >= 0 && pointer != null) ScreenSize(pointer.holoSlide); }
    public void VolumeSlid() { if (own >= 0 && Has(volumeSliders)) Volume(volumeSliders[own].value); }
    public void VoiceSlid() { if (own >= 0 && Has(voiceSliders)) Voices(voiceSliders[own].value); }
    public void VolumeSlide() { if (own >= 0 && pointer != null) Volume(pointer.holoSlide); }
    public void VoiceSlide() { if (own >= 0 && pointer != null) Voices(pointer.holoSlide); }

    private void Volume(float level)
    {
        if (sound != null && sound.volumeSlider != null) sound.volumeSlider.value = Mathf.Round(Mathf.Clamp01(level) * 100f);
    }

    private void Voices(float level)
    {
        voices = Mathf.Clamp01(level);
        Hear(voices);
    }

    // VRChat's defaults are 15 dB a voice and 10 an avatar's sounds; a seat's slider scales both
    private void Hear(float level)
    {
        VRCPlayerApi[] players = new VRCPlayerApi[VRCPlayerApi.GetPlayerCount()];
        VRCPlayerApi.GetPlayers(players);
        for (int i = 0; i < players.Length; i++)
        {
            if (!Utilities.IsValid(players[i]) || players[i].isLocal) continue;
            players[i].SetVoiceGain(15f * level);
            players[i].SetAvatarAudioGain(10f * level);
        }
    }

    public override void OnPlayerJoined(VRCPlayerApi player)
    {
        if (own < 0 || !Utilities.IsValid(player) || player.isLocal) return;
        player.SetVoiceGain(15f * voices);
        player.SetAvatarAudioGain(10f * voices);
    }

    private bool Has(Slider[] sliders) { return sliders != null && own < sliders.Length && sliders[own] != null; }

    // The world's size, 0 to 1 (1 - the plane in use), as the slider shows it
    private void World(float size)
    {
        if (inUse >= 0f && Mathf.Abs(size - (1f - inUse)) < 0.002f) return;
        shift = Mathf.Clamp(1f - size - Natural(), -1f, 1f);
        nextAt = 0f;
    }

    private void ScreenSize(float at)
    {
        screenScale = 0.5f * Mathf.Pow(4f, Mathf.Clamp01(at));
        nextAt = 0f;
    }

    // The plane at which the program's player's eye is the sitter's avatar's eye height, where the
    // game played says it (docs/world/holodeck.md, "The world's size"); else `plane`.
    private float Natural()
    {
        uint w = machine.worldEye;
        VRCPlayerApi player = Networking.LocalPlayer;
        if (played == 0 || (int)(w >> 28) != played || !Utilities.IsValid(player)) return plane;
        int a = (int)(w & 0xffff);
        if (a >= 32768) a -= 65536;
        float b = (int)((w >> 16) & 0xfff) / 4096f, eye = Mathf.Clamp(player.GetAvatarEyeHeightAsMeters(), 0.2f, 5f);
        return b <= 0f ? plane : a / 4096f - b * Mathf.Log(eye);
    }

    private void Plane()
    {
        if (shiftFor != played) { shiftFor = played; shift = 0f; }
        float want = Mathf.Clamp01(Natural() + shift);
        if (Mathf.Abs(want - inUse) < 0.001f) return;
        inUse = want;
        sceneMaterial.SetFloat("_Plane", inUse);
    }

    // ---- who sits where, and what each room shows ----

    private void SitTest()
    {
        if (Time.time < sitTestAt) return;
        sitTestAt = Time.time + 5f;
        VRCPlayerApi me = Networking.LocalPlayer;
        if (!Utilities.IsValid(me)) return;
        string seats = "";
        for (int k = 0; k < count; k++) seats += occupant[k] + " ";
        Debug.Log("[SitTest] t=" + Time.time.ToString("F0") + " me=" + me.playerId + " master=" + Networking.IsMaster
                  + " vr=" + me.IsUserInVR() + " players=" + VRCPlayerApi.GetPlayerCount() + " own=" + own + " seats=" + seats);
        if (Networking.IsMaster && !sitTestSat && own < 0 && Time.time > 15f)
        {
            sitTestSat = true;
            Debug.Log("[SitTest] the master sits in room 0");
            stations[0].UseStation(me);
        }
    }

    void Update()
    {
        if (sitTest) SitTest();
        if (own >= 0 && Input.GetKeyDown(KeyCode.End)) StandUp();
        // (Home is the guest's too: in a game's world only, not where it moves a cursor)
        if (own >= 0 && !flat && Input.GetKeyDown(KeyCode.Home)) { consoleHidden = !consoleHidden; Arrange(); }
        // a menu, a level loading, a program that draws no world, or one that is no registered game
        // (an RTS's list is no world to stand in): its picture on the large screen
        bool flatNow = own >= 0 && (machine.flatFrame || !machine.worldFrame || (!worldsForAll && !Registered(machine.gameId)));
        if (flatNow != flat) { flat = flatNow; Arrange(); }
        if (own >= 0 && machine.gameId != 0) played = machine.gameId;
        bool byWords = ByWords();
        if (gamepad != null && gamepad.keys != null && gamepad.keys.playAway != byWords) gamepad.keys.playAway = byWords;
        Mouse();
        Plane();
        Tell();
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.25f;
        Aims();   // (the program in front may have changed)
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
            // this visitor's own screen is the console's small one or a large one before the seat,
            // as its key says; anybody else's is the console's. Its mark goes with it.
            int large = me && (screenMode == 1 || flat) ? 1 : 0;
            float scale = large == 1 ? screenScale : 1f;
            if (placed[k] != large || placedScale[k] != scale)
            {
                placed[k] = large;
                placedScale[k] = scale;
                Transform screen = screens[k];
                screen.localPosition = large == 1 ? largeAt : screenAt;
                screen.localEulerAngles = large == 1 ? Vector3.zero : screenTurn;
                screen.localScale = large == 1 ? new Vector3(largeSize.x * scale, largeSize.y * scale, largeSize.z) : screenSize;
                if (screenGuards != null && k < screenGuards.Length && screenGuards[k] != null)
                {
                    screenGuards[k].SetPositionAndRotation(screen.position, screen.rotation);
                    screenGuards[k].localScale = screen.lossyScale;
                }
                if (me) PlaceKeyboard(screen, frames[k]);
            }

            string who = occupant[k] == 0 ? "FREE" : me ? Networking.LocalPlayer.displayName : NameOf(occupant[k]);
            if (doorLabels[k] != null && doorLabels[k].text != who) doorLabels[k].text = who;
            if (plateLabels[k] != null)
            {
                string says = occupant[k] == 0 ? "Sit down to play" : !me ? who + " plays here"
                            : holds[k] != 0 ? gamepad.SchemeName() + ". Both grips: stand up"
                            : gamepad != null && gamepad.keys != null && gamepad.keys.Capturing() ? "Your keys play. End: stand up"
                            : "Space or End: stand up. Sit again for the keys";
                if (plateLabels[k].text != says) plateLabels[k].text = says;
            }
            if (scaleLabels[k] != null && me) scaleLabels[k].text = Mathf.RoundToInt((1f - inUse) * 100f).ToString();
            if (screenLabels != null && k < screenLabels.Length && screenLabels[k] != null && me)
                screenLabels[k].text = Mathf.RoundToInt(screenScale * 100f) + "%";
            // (the sliders show what is in use: a game's own size, or another's slider moved)
            if (worldSliders != null && k < worldSliders.Length && worldSliders[k] != null && me && Mathf.Abs(worldSliders[k].value - (1f - inUse)) > 0.002f)
                worldSliders[k].value = 1f - inUse;
            float level = sound != null && sound.volumeSlider != null ? sound.volumeSlider.value / 100f : 0f;
            if (volumeLabels != null && k < volumeLabels.Length && volumeLabels[k] != null && me) volumeLabels[k].text = Mathf.RoundToInt(level * 100f) + "%";
            if (volumeSliders != null && k < volumeSliders.Length && volumeSliders[k] != null && me && Mathf.Abs(volumeSliders[k].value - level) > 0.002f)
                volumeSliders[k].value = level;
            if (voiceLabels != null && k < voiceLabels.Length && voiceLabels[k] != null && me) voiceLabels[k].text = Mathf.RoundToInt(voices * 100f) + "%";
            float screenPos = Mathf.Log(screenScale / 0.5f) / Mathf.Log(4f);
            if (screenSliders != null && k < screenSliders.Length && screenSliders[k] != null && me && Mathf.Abs(screenSliders[k].value - screenPos) > 0.002f)
                screenSliders[k].value = screenPos;
        }
    }

    // A desktop's sitter in the world of a game that reads the host's words (Doom, Quake,
    // ClassiCube, Quake III, Half-Life in linux/apps/gameid.h): the keys they walk, fire and jump by are told
    // it as the left stick and the triggers (Tell), whichever window has the guest's keyboard.
    private bool ByWords()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (own < 0 || flat || !Utilities.IsValid(player) || player.IsUserInVR()) return false;
        if (gamepad == null || gamepad.keys == null || !gamepad.keys.Capturing()) return false;
        return played == 1 || played == 2 || played == 3 || played == 7 || played == 9 || played == 10;
    }

    private int Key(KeyCode key) { return Input.GetKey(key) ? 127 : 0; }

    // A sitter without a headset plays as at a desk (docs/world/holodeck.md, "Played from the seat"):
    // while their keys are the game's, their mouse turns it and its left button is Ctrl.
    private void Mouse()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        bool playing = own >= 0 && Utilities.IsValid(player) && !player.IsUserInVR()
                    && gamepad != null && gamepad.keys != null && gamepad.keys.Capturing();
        if (playing) mouseGone = Mathf.Repeat(mouseGone + Input.GetAxis("Mouse X") * mouseCounts, 65536f);
        // (by words it is the right trigger; on a flat screen, or with the view on the screen, the pointer's button)
        bool fire = playing && !flat && !ByWords() && Input.GetMouseButton(0) && (pointer == null || !pointer.Landed(1));
        if (fire == firing) return;
        firing = fire;
        if (gamepad == null || gamepad.keys == null) return;
        gamepad.keys.PushEvent(fire ? Ctrl | Pressed : Ctrl);   // (this machine's: keysOwn)
    }

    // The program's world goes round with a desktop sitter's head: what turns the game is their
    // mouse, which turns their head too, and the game would turn twice as far as they looked.
    public override void PostLateUpdate()
    {
        VRCPlayerApi player = Networking.LocalPlayer;
        if (own < 0 || !Utilities.IsValid(player)) return;
        VRCPlayerApi.TrackingData head = player.GetTrackingData(VRCPlayerApi.TrackingDataType.Head);
        PlaceHud(player, head);
        if (player.IsUserInVR()) return;
        scene.SetPositionAndRotation(head.position, Quaternion.Euler(0f, head.rotation.eulerAngles.y, 0f));
    }

    // The HUD layer before the sitter's eyes: on a desktop it turns and tilts with the view at once; in a headset it follows
    // the head's turn about the upright and its tilt up and down a little late (a layer fixed to the face is tiring), its roll never.
    // The keyboard hangs under the sitter's screen, the console's small one or the large one, in its plane tipped back
    // KeyboardTilt degrees, its top KeyboardGap below the screen's lower edge: seen wherever the screen is, never behind the
    // sitter's own body (before, it lay before the seat, under their hands). Where the floor is too near it tips further
    // back, to 85 degrees, and then rises over the screen's edge, its lowest edge KeyboardFloor over the floor. It is as wide
    // as the screen, KeyboardWidth at least and KeyboardMost at most: under the large screen, 1.7 m off, one 0.54 m wide had
    // keys too small to hit; under the largest (3.2 m, its edge 5 cm over the floor) it lies at 85 degrees, 12 cm up.
    private const float KeyboardTilt = 25f, KeyboardGap = 0.04f, KeyboardFloor = 0.12f, KeyboardWidth = 0.54f, KeyboardMost = 1.6f;

    private void PlaceKeyboard(Transform screen, Transform frame)
    {
        if (keyboard == null) return;
        Transform kb = keyboard.transform;
        float wide = Mathf.Clamp(screen.lossyScale.x, KeyboardWidth, KeyboardMost), parent = kb.parent != null ? kb.parent.lossyScale.x : 1f;
        kb.localScale = Vector3.one * (wide / keyboard.panelWidth / parent);
        Vector3 edge = screen.TransformPoint(new Vector3(0f, -0.5f, 0f)) - screen.up * KeyboardGap;
        float floor = frame.position.y + KeyboardFloor;
        float tilt = KeyboardTilt, low = 0f;
        Vector3 centre = edge;
        while (true)
        {
            kb.rotation = screen.rotation * Quaternion.Euler(tilt, 0f, 0f);
            Vector3 up = kb.TransformVector(new Vector3(0f, keyboard.panelHeight, 0f));
            centre = edge - up * 0.5f;
            low = centre.y - Mathf.Abs(up.y) * 0.5f;
            if (low >= floor || tilt >= 85f) break;
            tilt += 5f;
        }
        if (low < floor) centre.y += floor - low;
        kb.position = centre;
    }

    private void PlaceHud(VRCPlayerApi player, VRCPlayerApi.TrackingData head)
    {
        if (hud == null || !hud.gameObject.activeSelf) return;
        Vector3 euler = head.rotation.eulerAngles;
        float yaw = euler.y, pitch = Mathf.Clamp(Mathf.DeltaAngle(0f, euler.x), -89f, 89f);
        if (!hudPlaced || !player.IsUserInVR()) { hudYaw = yaw; hudPitch = pitch; }
        else
        {
            float follow = Mathf.Clamp01(Time.deltaTime * hudFollow);
            hudYaw = Mathf.LerpAngle(hudYaw, yaw, follow);
            hudPitch = Mathf.Lerp(hudPitch, pitch, follow);
        }
        hudPlaced = true;
        Quaternion turn = Quaternion.Euler(hudPitch, hudYaw, 0f);
        hud.SetPositionAndRotation(head.position + turn * new Vector3(0f, -hudDrop, hudDistance), turn);
        int w = machine.OwnWidth(), h = machine.OwnHeight();
        float aspect = w > 0 && h > 0 ? (float)w / h : 4f / 3f;
        hud.localScale = new Vector3(hudWidth, hudWidth / aspect, 1f);
    }

    // ---- what the machine is told of where it is shown (docs/world/holodeck.md, "What the machine is told")

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
        // (a test's invented hands are told as a headset's)
        bool headset = player.IsUserInVR() || (pointer != null && pointer.debugBeam);
        // (bit 5: the hands are tracked, and a program may aim by one)
        now[0] = (headset ? 3 | 32 : 2) | (own >= 0 ? 16 : 0) | (Mathf.Min(VRCPlayerApi.GetPlayerCount(), 255) << 8);
        now[1] = (player.playerId & 255) | (Mathf.RoundToInt(Mathf.Clamp01(inUse) * 255f) << 8);   // (and the room's scale)
        now[2] = Turn(turn.y);    // to the right
        now[3] = Turn(-turn.x);   // upwards
        now[4] = Half(at.x);
        now[5] = Half(at.y);
        now[6] = Half(at.z);
        now[7] = Turn(turn.z);
        for (int h = 0; h < 2; h++)
        {
            VRCPlayerApi.TrackingData hand = player.GetTrackingData(h == 0 ? VRCPlayerApi.TrackingDataType.LeftHand : VRCPlayerApi.TrackingDataType.RightHand);
            Vector3 from = hand.position, along = hand.rotation * (h == 0 ? Vector3.left : Vector3.right);
            // (a game aims and holds its weapon where the visitor's own beam is: EmuPointer's)
            if (pointer != null && pointer.handsKnown) { from = pointer.handAt[h]; along = pointer.handWay[h]; }
            Vector3 place = back * (from - origin) * 250f, way = back * along;
            // its place in 4 mm, ten bits each way, with its trigger and grip; then where it points:
            // its turn to the right and its rise, 65,536 to a turn as the head's
            int x = Mathf.Clamp(Mathf.RoundToInt(place.x), -512, 511) & 1023, y = Mathf.Clamp(Mathf.RoundToInt(place.y), -512, 511) & 1023;
            int z = Mathf.Clamp(Mathf.RoundToInt(place.z), -512, 511) & 1023;
            int word = x | (y << 10) | (z << 20);
            // (a hand whose beam is on the console or a screen presses that: no program's hand takes by it)
            bool free = pointer == null || !pointer.Landed(h);
            now[8 + h * 4] = word & 0xffff;
            now[9 + h * 4] = ((word >> 16) & 0x3fff) | (trigger[h] && free ? 1 << 14 : 0) | (grip[h] && free ? 1 << 15 : 0);
            now[10 + h * 4] = Turn(Mathf.Atan2(way.x, way.z) * Mathf.Rad2Deg);
            now[11 + h * 4] = Turn(Mathf.Asin(Mathf.Clamp(way.y, -1f, 1f)) * Mathf.Rad2Deg);
        }
        // (no hands: the first of their words is how far a sitter's mouse has gone to the right;
        // with hands, the left one's second word is the sticks)
        if (!headset)
        {
            // (and the keys played by: the left stick, Space the jump button, the right trigger fires)
            bool play = ByWords();
            int x = Mathf.Clamp(Key(KeyCode.D) + Key(KeyCode.RightArrow) - Key(KeyCode.A) - Key(KeyCode.LeftArrow), -127, 127);
            int y = Mathf.Clamp(Key(KeyCode.W) + Key(KeyCode.UpArrow) - Key(KeyCode.S) - Key(KeyCode.DownArrow), -127, 127);
            // (a click with the view on the console or the room's screen is theirs, not the game's)
            bool fires = (Input.GetMouseButton(0) && (pointer == null || !pointer.Landed(1))) || Input.GetKey(KeyCode.LeftControl)
                      || Input.GetKey(KeyCode.RightControl);
            // (the right button is the left trigger: ClassiCube places by it, Doom uses)
            bool other = Input.GetMouseButton(1) && (pointer == null || !pointer.Landed(1));
            now[8] = Mathf.RoundToInt(mouseGone) & 0xffff;
            now[9] = (now[9] & 0x3fff) | (play && other ? 1 << 14 : 0);
            if (play && Input.GetKey(KeyCode.Space)) now[0] |= 64;
            now[10] = play ? (x & 255) | ((y & 255) << 8) : 0;
            // (and the wheel's notches up, a counter: SEGL_HOST_WHEEL)
            // (with the view on the console or the screen the wheel is the pointer's: EmuPointer)
            if (play && (pointer == null || !pointer.Landed(1))) wheelGone = Mathf.Repeat(wheelGone + Input.GetAxis("Mouse ScrollWheel") * 10f, 256f);
            now[11] = Mathf.FloorToInt(wheelGone) & 255;
            // (E is the right grip: Half-Life's use)
            now[13] = (now[13] & 0x3fff) | (play && fires ? 1 << 14 : 0) | (play && Input.GetKey(KeyCode.E) ? 1 << 15 : 0);
        }
        else if (gamepad != null)
        {
            // (the left hand's way, its word being the sticks: in place of the head's roll, a byte each)
            int yaw = now[10] >= 32768 ? now[10] - 65536 : now[10], rise = now[11] >= 32768 ? now[11] - 65536 : now[11];
            now[7] = (Mathf.RoundToInt(yaw / 256f) & 255) | ((Mathf.Clamp(Mathf.RoundToInt(rise / 128f), -127, 127) & 255) << 8);
            now[10] = gamepad.StickLow();
            now[11] = gamepad.StickHigh();
            if (gamepad.Jumping()) now[0] |= 64;   // (bit 6: the jump button; no trigger jumps)
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
}
