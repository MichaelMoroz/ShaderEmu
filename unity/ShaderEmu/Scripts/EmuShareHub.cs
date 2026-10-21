using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// Players see each other's machines (docs/world/share.md). This is local, one a client: it fills
// this player's EmuShare with the console's changed rows and the display's changed parts
// while the machine is on and anybody is there, and puts the watched player's console,
// display and panel on the wall here (EmuStreams receives them). A watcher's keys and
// pointer go the other way, when the owner allows it.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuShareHub : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard, gpuKeyboard;
    public EmuStreams streams;              // what arrives, for every screen that shows a player
    public RenderTexture fineA, fineB;      // 1280 x 960: this capture and the last, at the display's own size
    public Material displayShowMaterial;    // the wall
    public TextMeshProUGUI watchLabel, allowLabel;
    public TextMeshProUGUI watchersLabel;   // who watches the machine shown
    public TextMeshProUGUI moreLabel;       // on the button to the next page of players
    // The panel by the display: "Automatic", "My own", then a page of the other players.
    public GameObject[] rowObjects;
    public Image[] rowPlates;
    public TextMeshProUGUI[] rowLabels;
    public GameObject moreButton;
    public Color rowColour, pickedColour;
    public bool selfTest;                   // watch this machine through the stream
    // The display goes as units the GPU picks and packs by itself (docs/world/share.md, "Display"):
    // Udon copies one row of bytes.
    public Material unitsMaterial;          // ShareUnits.shader
    public RenderTexture unitsBlocks, unitsExact, unitsNeed, unitsStats, unitsPlan, unitsTable, unitsPrefix, unitsCut, unitsPacket;
    public RenderTexture[] unitsState;      // three of 160 x 120: what the receivers have, with this capture's changes, and after the packet
    public bool netTest;                    // two clients test themselves and log (docs/world/share.md)

    // Two ticks of three carry a packet: 7.7 KB/s with the fields beside it, of the 8 to 10 a
    // client gets out in practice for everything it sends (VRChat's limit is a client's).
    private const int PacketMost = 1150, ConsoleMost = 800, HeaderBytes = 19;
    private const int StatusFixed = 130, StatusTextMost = 560;   // as EmuStreams'
    // A batch of the network's packets: a full packet and more, or many small ones. They go as a
    // network event of their own object (EmuNet.Packets), at most one a frame and 95 a second;
    // while they flow the display has one sending in six (below).
    private const int NetBatchMost = 1000;
    private float netFlowAt = -10f;   // when the machine last had packets to send
    private byte[] netScratch = new byte[NetBatchMost + 600];
    [HideInInspector] public int netBatches, netTaken;
    public bool netEvents = true;   // false: batches as EmuNet's serializations (a round trip some 70 ms longer)
    private float netSentAt = -10f;
    // (for the [Lan] lines: from the asking to VRChat's sending, and on the way, summed since the last line)
    [HideInInspector] public float netPostSum;
    [HideInInspector] public int netPostCount, netOutCount, netWaySum, netWayCount, netWayMost;
    private const float TickSeconds = 0.1f;   // input every tick, a packet on two of three
    private const float ExpressSeconds = 0.05f;   // the keys' sendings, at most this often
    // What each of the last packets carried, to send again when a receiver says one was lost.
    private const int Kept = 32, KeptMost = 96;

    private EmuShare mine;
    private EmuNet mineNet;
    private EmuShare[] shares = new EmuShare[100];   // the other players'
    private int shareCount;
    private VRCPlayerApi[] players = new VRCPlayerApi[100];
    private bool rosterDirty = true, incomplete, stateDirty = true, stateOut = true;
    private float retryAt, nextTick, sentAt = -10f;
    private int ticks, myId;

    private int picked = -1;          // -1: automatic; 0: this player's own; else a player
    private int target;               // the player shown; 0: this player's own machine
    private EmuShare targetShare;
    private bool allow, wasOn;
    private int viewers;

    // sending
    private RenderTexture fineNow, fineBefore;
    private bool headerSent;
    private int sendW, sendH;
    private byte[] scratch = new byte[2048];
    private byte[] lastHeader = new byte[HeaderBytes];
    private byte[] empty = new byte[0];
    private int[] outQueue = new int[256];
    private int outHead, outTail;
    private int pointerNow;
    private bool pointerDue;
    private float pointerSentAt;
    private int[] keptSeq = new int[Kept];
    private int[] keptCount = new int[Kept];
    private int[] keptWhat = new int[Kept * KeptMost];   // 0x10000 + a console row
    private int building;             // the place in the ring of the packet being built
    private bool refreshDue;
    private float refreshedAt = -10f, speedAt, statusAt;
    private int speedAll, speedCore;

    // sending units: a packet is made a tick ahead, its header and rows then, its units when read back
    private const int UnitsBytes = 1152, UnitsRow = 292 * 4;
    private byte[] unitsRead = new byte[UnitsRow];
    private byte[] unitsHead = new byte[2048];
    private bool unitsPending, unitsReady, unitsReset = true, unitsHeadNew, unitsDrop, unitsAsking;
    private bool unitsHeadBuilt, unitsAskedReset, unitsCarry;
    private bool unitsActive, unitsCredit, unitsAgain;   // units are what is sent now; a tick allowed one packet; it was made again
    private int unitsAskedLost;
    private float unitsAge, unitsAgeSum;   // how old the last packet's capture was when it went, and all of them (for the tests)
    private int unitsAgains;
    private int unitsTurn;            // where the last packet stopped: the next goes on from there
    private int unitsAskedMs, captureMs;   // the instance's clock when a packet's picture was captured (for the test with two clients)
    private float unitsAskedAt, unitsReadyAt;   // (when it was read back: one that then waits for the link is made again)
    private int unitsHeadLength, unitsLength, unitsNow, unitsNext, unitsLostFrom, unitsLostCount;

    // A speed in two bytes of the packet: its four digits, and from bit 14 how many of them
    // come after the point (EmuStreams.Speed is the way back).
    private int PackSpeed(float millions)
    {
        int after = millions < 9.9995f ? 3 : millions < 99.995f ? 2 : millions < 999.95f ? 1 : 0;
        float scale = after == 3 ? 1000f : after == 2 ? 100f : after == 1 ? 10f : 1f;
        return Mathf.Clamp(Mathf.RoundToInt(millions * scale), 0, 9999) | after << 14;
    }

    // the wall
    private int wallSlot = -1, wallVersion = -1;
    private Color32[] blankGrid;
    private int pointerFrom;          // the watcher whose pointer the machine follows
    private float pointerAt;

    private int page;                 // of the players' rows
    private int sentPackets, sentBytes, tookEvents, repaired, refreshed;
    private float testAt, testTypeAt, testOnAt, testFlipAt;
    private bool testStarted, testTyped;
    [HideInInspector] public int testScene = -1;   // what the first player's machine shows in the test with two clients

    void Start()
    {
        blankGrid = new Color32[terminal.cols * terminal.rows];
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < blankGrid.Length; i++) blankGrid[i] = blank;
        fineNow = fineA;
        fineBefore = fineB;
        for (int k = 0; k < Kept; k++) keptSeq[k] = -1;
        WallShowsOwn();   // the material is an asset: it keeps whatever the last session left
        ShowLabels();
    }

    private void WallShowsOwn()
    {
        displayShowMaterial.SetTexture("_MainTex", machine.displayTexture);
        displayShowMaterial.SetVector("_TexSize", new Vector4(machine.displayTexture.width, machine.displayTexture.height, 0, 0));
    }

    // ---- the panel's buttons ----

    public void PickAutomatic() { Pick(-1); }
    public void PickMine() { Pick(0); }
    public void Pick0() { PickRow(0); }
    public void Pick1() { PickRow(1); }
    public void Pick2() { PickRow(2); }
    public void Pick3() { PickRow(3); }
    public void Pick4() { PickRow(4); }
    public void Pick5() { PickRow(5); }
    public void Pick6() { PickRow(6); }
    public void Pick7() { PickRow(7); }

    private void PickRow(int row)
    {
        Prune();
        int index = page * 8 + row;
        if (index < shareCount) Pick(shares[index].ownerId);
    }

    private void Pick(int who)
    {
        picked = who;
        Choose();
        ShowLabels();
    }

    public void More()
    {
        page++;
        ShowLabels();
    }

    // Who looks at a player's machine on their wall: that player's own number when at their own.
    private int Watched(EmuShare share)
    {
        return share.watching == 0 ? share.ownerId : share.watching;
    }

    // How many others have this player's machine on their wall.
    private int Watchers(int id)
    {
        int n = 0;
        if (id != myId && target == id) n++;
        for (int i = 0; i < shareCount; i++)
            if (shares[i].ownerId != id && Watched(shares[i]) == id) n++;
        return n;
    }

    private string WatcherNames(int id)
    {
        string names = id != myId && target == id ? "you" : "";
        for (int i = 0; i < shareCount; i++)
            if (shares[i].ownerId != id && Watched(shares[i]) == id) names += (names.Length > 0 ? ", " : "") + shares[i].ownerName;
        return names;
    }

    public void ToggleAllow()
    {
        allow = !allow;
        stateOut = true;
        ShowLabels();
    }

    private void ShowLabels()
    {
        Prune();
        if (allowLabel != null) allowLabel.text = "Others may use my computer: " + (allow ? "yes" : "no");
        if (watchLabel == null) return;
        string text = "Showing your own computer";
        if (target != 0 && Utilities.IsValid(targetShare))
            text = "Showing " + (target == myId ? "your own, as others see it" : targetShare.ownerName + "'s computer")
                 + ((targetShare.flags & 1) == 0 ? ", which is off" : (targetShare.flags & 2) != 0 ? ": you may use it" : ": look only");
        int watchingMe = Watchers(myId);
        watchLabel.text = text + "\n" + (watchingMe == 0 ? "Nobody is watching yours" : watchingMe + " watching yours");
        if (watchersLabel != null)
        {
            int shown = target != 0 && Utilities.IsValid(targetShare) ? target : myId;
            string names = WatcherNames(shown);
            watchersLabel.text = (shown == myId ? "Watching yours: " : "Watching " + targetShare.ownerName + "'s: ") + (names.Length > 0 ? names : "nobody");
        }
        if (rowObjects == null || rowObjects.Length < 10) return;
        if (page * 8 >= shareCount) page = 0;
        if (moreLabel != null) moreLabel.text = "More players  (" + (page + 1) + " of " + ((shareCount + 7) / 8) + ")";
        rowPlates[0].color = picked < 0 ? pickedColour : rowColour;
        rowPlates[1].color = picked == 0 ? pickedColour : rowColour;
        for (int row = 0; row < 8; row++)
        {
            int index = page * 8 + row;
            rowObjects[row + 2].SetActive(index < shareCount);
            if (index >= shareCount) continue;
            EmuShare share = shares[index];
            int watched = Watchers(share.ownerId);
            rowLabels[row + 2].text = share.ownerName + ((share.flags & 1) == 0 ? "  (off)" : (share.flags & 2) != 0 ? "  (on, usable)" : "  (on)")
                                    + (watched > 0 ? "   " + watched + " watching" : "");
            rowPlates[row + 2].color = picked == share.ownerId ? pickedColour : rowColour;
        }
        moreButton.SetActive(shareCount > 8);
    }

    // ---- who is here ----

    public override void OnPlayerJoined(VRCPlayerApi player) { rosterDirty = true; }

    // Their object is destroyed with them: it leaves the list now, before anything reads it.
    public override void OnPlayerLeft(VRCPlayerApi player)
    {
        rosterDirty = true;
        if (Utilities.IsValid(player)) Forget(player.playerId);
        Prune();
    }

    private void Forget(int id)
    {
        for (int i = 0; i < shareCount; i++)
        {
            if (Utilities.IsValid(shares[i]) && shares[i].ownerId != id) continue;
            shares[i] = shares[--shareCount];
            shares[shareCount] = null;
            i--;
        }
        if (target == id) targetShare = null;
        if (pointerFrom == id) DropPointer();
        stateDirty = true;
    }

    // No object in the list is a destroyed one: called by whoever is about to read the list.
    public void Prune()
    {
        for (int i = 0; i < shareCount; i++)
        {
            if (Utilities.IsValid(shares[i])) continue;
            shares[i] = shares[--shareCount];
            shares[shareCount] = null;
            i--;
            stateDirty = true;
        }
        if (!Utilities.IsValid(targetShare)) targetShare = null;
        if (!Utilities.IsValid(mine)) mine = null;
        if (!Utilities.IsValid(mineNet)) mineNet = null;
    }

    // A player's object may arrive after the player: asked again while one is missing.
    private void Roster()
    {
        rosterDirty = false;
        incomplete = false;
        stateDirty = true;
        retryAt = Time.time + 1f;
        shareCount = 0;
        int n = Mathf.Min(VRCPlayerApi.GetPlayerCount(), players.Length);
        VRCPlayerApi.GetPlayers(players);
        for (int i = 0; i < n; i++)
        {
            VRCPlayerApi player = players[i];
            if (!Utilities.IsValid(player)) continue;
            EmuShare found = null;
            EmuNet foundNet = null;
            GameObject[] objects = Networking.GetPlayerObjects(player);
            if (objects != null)
            {
                for (int k = 0; k < objects.Length; k++)
                {
                    if (!Utilities.IsValid(objects[k])) continue;
                    EmuShare share = objects[k].GetComponent<EmuShare>();
                    if (Utilities.IsValid(share)) found = share;
                    EmuNet net = objects[k].GetComponent<EmuNet>();
                    if (Utilities.IsValid(net)) foundNet = net;
                }
            }
            if (found == null)
            {
                incomplete = true;
                continue;
            }
            // (an object made from a template older than a field has that field empty)
            if (found.seenAsk == null || found.seenAsk.Length != found.ask.Length) found.seenAsk = new int[found.ask.Length];
            found.ownerId = player.playerId;
            found.ownerName = player.displayName;
            if (foundNet != null) foundNet.share = found;
            else incomplete = true;
            if (player.isLocal) { mine = found; mineNet = foundNet; }
            else shares[shareCount++] = found;
        }
    }

    private EmuShare Find(int id)
    {
        if (id == myId) return mine;
        for (int i = 0; i < shareCount; i++)
            if (Utilities.IsValid(shares[i]) && shares[i].ownerId == id) return shares[i];
        return null;
    }

    // Whose machine the wall shows: the one picked; left to itself this player's own while
    // it is on, else the first other player's that is.
    private void Choose()
    {
        Prune();
        int want = 0;
        if (selfTest) want = myId;
        else if (picked > 0)
        {
            if (Find(picked) != null) want = picked;
            else picked = -1;   // they left
        }
        if (!selfTest && picked < 0 && !machine.IsOn())
        {
            for (int i = 0; i < shareCount; i++)
                if ((shares[i].flags & 1) != 0 && (want == 0 || shares[i].ownerId < want)) want = shares[i].ownerId;
        }
        if (want != target) Switch(want);
        else if (target != 0) targetShare = Find(target);
    }

    private void Switch(int to)
    {
        target = to;
        targetShare = to == 0 ? null : Find(to);
        machine.showRemote = to != 0;
        machine.inputAway = to != 0 && to != myId;
        outHead = outTail;
        pointerNow = 0;
        pointerDue = true;
        streams.SetWant(0, to);
        wallSlot = -1;
        wallVersion = -1;
        if (to != 0)
        {
            terminal.ShowRemote(blankGrid, 0, 0, 0);
            machine.shownWidth = 0;
            machine.shownHeight = 0;
            displayShowMaterial.SetVector("_Size", Vector4.zero);
        }
        else
        {
            terminal.ShowLocal();
            WallShowsOwn();
        }
        if (to == 0 || to == myId) machine.OwnStats();
        else machine.RemoteStats("", null);
        stateOut = true;
        stateDirty = true;
        ShowLabels();
    }

    // The wall and the console's screen, from the slot the watched player's stream is in.
    private void Wall()
    {
        if (target == 0) return;
        int slot = streams.SlotOf(target);
        if (slot != wallSlot)
        {
            wallSlot = slot;
            wallVersion = -1;
            if (slot >= 0)
            {
                Texture picture = streams.Picture(slot);
                displayShowMaterial.SetTexture("_MainTex", picture);
                displayShowMaterial.SetVector("_TexSize", new Vector4(picture.width, picture.height, 0, 0));
            }
        }
        if (slot < 0 || streams.Version(slot) == wallVersion) return;
        wallVersion = streams.Version(slot);
        terminal.ShowRemote(streams.Cells(slot), streams.Top(slot), streams.CursorX(slot), streams.CursorY(slot));
        displayShowMaterial.SetVector("_Size", new Vector4(streams.Width(slot), streams.Height(slot), 0, 0));
        machine.shownWidth = streams.DisplayWidth(slot);
        machine.shownHeight = streams.DisplayHeight(slot);
        if (target != myId) machine.RemoteStats(streams.Stats(slot), streams.Registers(slot));
    }

    // The machine on the wall, for whoever shows its speed and lamps: the slot of another
    // player's, or -1 for this player's own.
    public int WallSlot()
    {
        return target != 0 && target != myId ? streams.SlotOf(target) : -1;
    }

    public bool WallIsOwn() { return target == 0 || target == myId; }

    // Who shows this player on some screen, who of them is new or lost a packet, and who
    // watches on the wall (whose keys and pointer may come).
    private void Scan()
    {
        int count = selfTest ? 1 : 0, me = myId & 0xffff;
        for (int i = 0; i < shareCount; i++)
        {
            EmuShare share = shares[i];
            bool shows = false;
            int[] ask = share.ask, lost = share.lost, seen = share.seenAsk;
            for (int k = 0; k < ask.Length && k < seen.Length; k++)
            {
                int word = ask[k];
                if ((word & 0xffff) != me || word == 0)
                {
                    seen[k] = word;
                    continue;
                }
                shows = true;
                if (word == seen[k]) continue;
                seen[k] = word;
                int from = lost[k] & 0xffff, n = (lost[k] >> 16) & 255;
                if (n == 0 || !Repair(from, n)) refreshDue = true;
            }
            if (shows) count++;
            if (share.watching == myId)
            {
                if (!share.watchedMe)
                {
                    share.watchedMe = true;
                    share.seenInput = share.inputSeq;
                }
            }
            else if (share.watchedMe)
            {
                share.watchedMe = false;
                if (pointerFrom == share.ownerId) DropPointer();
            }
        }
        viewers = count;
        ShowLabels();
    }

    // What packets `from` to `from + n - 1` (their numbers' low 16 bits) carried goes out again.
    // False when one of them is no longer kept.
    private bool Repair(int from, int n)
    {
        if (n > Kept) return false;
        for (int i = 0; i < n; i++)
        {
            int k = -1;
            for (int j = 0; j < Kept; j++)
                if (keptSeq[j] >= 0 && (keptSeq[j] & 0xffff) == ((from + i) & 0xffff)) k = j;
            if (k < 0) return false;
            for (int e = 0; e < keptCount[k]; e++)
            {
                int what = keptWhat[k * KeptMost + e];
                if (what >= 0x10000) terminal.rowChanged[(what - 0x10000) % terminal.rows] = true;
            }
            if (keptCount[k] >= KeptMost) return false;   // more than is kept of one packet
        }
        // (its units: the cells that carry one of these numbers go stale in the next packet made)
        if (unitsLostCount > 0) unitsReset = true;
        unitsLostFrom = from;
        unitsLostCount = n;
        headerSent = false;
        repaired++;
        return true;
    }

    private void Keep(int what)
    {
        if (keptCount[building] < KeptMost) keptWhat[building * KeptMost + keptCount[building]] = what;
        if (keptCount[building] < KeptMost) keptCount[building]++;
    }

    private void DropPointer()
    {
        pointerFrom = 0;
        machine.SetRemotePointer(0, 0, 0, false);
    }

    // ---- one tick ----

    void Update()
    {
        SendNet();
        // Units do not wait for a tick: a tick that may carry the display allows one packet, which
        // is asked for the frame the link is free and goes the frame it has been read back. What
        // it shows is then a few frames old, not one or two tenths of a second (or a second,
        // where the link is busy that long: there it is made once more before it goes).
        if (unitsActive && mine != null && !mine.Busy() && !Networking.IsClogged)
        {
            if (!unitsPending)
            {
                if (unitsCredit) AskUnits();
            }
            else if (unitsReady && unitsCredit)
            {
                if (!unitsAgain && Time.time - unitsReadyAt >= 0.09f)
                {
                    // (what that one was asked to do is still to do)
                    unitsAgain = true;
                    unitsAgains++;
                    if (unitsAskedReset) unitsReset = true;
                    if (unitsAskedLost > 0) unitsLostCount = unitsAskedLost;
                    unitsPending = false;
                    unitsReady = false;
                    AskUnits();
                }
                else
                {
                    unitsCredit = false;
                    unitsAgain = false;
                    int taken = TakeUnits();
                    if (taken > 0 || unitsCarry) SendPacket(taken, wasOn);
                    unitsCarry = false;
                }
            }
        }
        // Typed keys do not wait for a tick either: they go the frame the link is free, half a
        // tick after the last sending at the soonest
        if (mine != null && !mine.Busy() && !Networking.IsClogged && !(unitsActive && unitsPending)
            && Time.time - sentAt >= ExpressSeconds && (outHead != outTail || pointerDue))
        {
            if (MoveInput()) SendPacket(0, wasOn);
        }
        if (Time.time < nextTick) return;
        nextTick = Time.time + TickSeconds;
        ticks++;
        VRCPlayerApi me = Networking.LocalPlayer;
        if (me == null) return;
        myId = me.playerId;
        Prune();
        if (rosterDirty || (incomplete && Time.time >= retryAt)) Roster();
        if (mine == null) return;
        if (netTest) NetTest();
        if (lanTest) LanTest();
        if (Time.time >= lanTimesAt) LanTimes();

        bool on = machine.IsOn();
        if (on != wasOn)
        {
            wasOn = on;
            stateOut = true;
            refreshDue = true;
        }
        if (stateDirty)
        {
            stateDirty = false;
            Scan();
        }
        Choose();
        Wall();
        // everything again, for a new screen or one that lost too much: not more often than
        // every five seconds, whoever asks
        if (refreshDue && Time.time - refreshedAt >= 5f)
        {
            refreshDue = false;
            refreshedAt = Time.time;
            refreshed++;
            FullRefresh();
        }
        if (pointerFrom != 0 && Time.time - pointerAt > 2f) DropPointer();
        if (machine.inputAway) Drain();
        bool wanted = shareCount > 0 || selfTest;
        unitsActive = UnitsNow(on) && wanted;
        if (unitsActive && ticks % 3 != 0 && !(Time.time - netFlowAt < 1f && ticks % 6 != 1)) unitsCredit = true;
        if (mine.Busy() || Networking.IsClogged) return;

        // while the network's packets flow (a game) the display has one sending in six: a picture
        // of the game a second and a half apart, and the game's packets nearly all of the link
        bool flowing = Time.time - netFlowAt < 1f;
        int length = 0;
        bool slot = ticks % 3 != 0 && wanted && !(flowing && ticks % 6 != 1);
        // (while a packet of units is on its way this tick's own sending, if it has any, goes with it)
        bool waits = unitsActive && unitsPending;
        if (!unitsActive && slot)
        {
            // (the machine is off, or has no display: its console alone.) A packet of units made
            // before: nothing of it goes, and its rows are sent again
            if (unitsPending && unitsReady) UnitsDropped();
            unitsDrop = unitsPending;
            if (!on || machine.displayWidth <= 0)
            {
                sendW = 0;
                sendH = 0;
            }
            length = Build(on);
        }
        bool typed = MoveInput();
        if (waits)
        {
            unitsCarry = unitsCarry || typed || stateOut;
            return;
        }
        if (length == 0 && !typed && !stateOut) return;
        SendPacket(length, on);
    }

    // What is in `scratch` (length bytes; 0: nothing of the console or the display) and the other fields.
    private void SendPacket(int length, bool on)
    {
        if (length > 0)
        {
            byte[] packet = new byte[length];
            System.Buffer.BlockCopy(scratch, 0, packet, 0, length);
            mine.packet = packet;
            mine.seq++;
            keptSeq[building] = mine.seq;
            sentPackets++;
            sentBytes += length;
            if (netTest) Debug.Log("[ShareLag] sent seq=" + mine.seq + " ms=" + Networking.GetServerTimeInMilliseconds() + " captured=" + captureMs + " units=1 bytes=" + length);
        }
        else mine.packet = empty;
        mine.flags = (on ? 1 : 0) | (allow ? 2 : 0);
        mine.watching = target == myId ? 0 : target;
        mine.Send();
        sentAt = Time.time;
        stateOut = false;
        if (selfTest && length > 0) streams.Received(mine);
    }

    // The first player in the instance turns their machine on and lets it be used; the others
    // leave theirs off, so they watch it, and type one line at it. Each logs what it sees.
    private void NetTest()
    {
        if (!testStarted)
        {
            testStarted = true;
            if (Networking.IsMaster)
            {
                machine.PowerOn();
                allow = true;
                stateOut = true;
            }
        }
        // the first player's machine shows five scenes that keep changing, 80 s each (a game,
        // windows with activity, text scrolling, a window dragged, the pointer): what the
        // other's picture of it is like, EmuShareProbe writes down (tools/share_clients.py)
        if (Networking.IsMaster && machine.IsOn())
        {
            if (testOnAt == 0f) testOnAt = Time.time;
            int step = Mathf.FloorToInt((Time.time - testOnAt - 50f) / 40f);
            if (step >= 0)
            {
                int scene = (step / 2) % 5;
                if (scene != testScene)
                {
                    string stop = testScene == 0 ? "killall doom doom.bin; " : testScene == 1 ? "killall glxgears nxmon; " : testScene == 2 ? "killall nxterm; "
                                : testScene == 3 ? "rm /tmp/drag; killall nxkey nxedit; " : "";
                    if (testScene == 4) machine.SetRemotePointer(0, 0, 0, false);
                    testScene = scene;
                    // (a short line: the console takes a few characters a frame)
                    string drags = "touch /tmp/drag; nxedit & sleep 4; (while [ -f /tmp/drag ]; do nxkey drag:290,32:400,90 200 drag:400,90:250,160 200 drag:250,160:480,60 200 drag:480,60:300,200 200 drag:300,200:290,32 200; done) &";
                    consoleKeyboard.TypeText(stop + (scene == 0 ? "doom -playdemo demo1 &" : scene == 1 ? "glxgears & nxmon &"
                        : scene == 2 ? "nxterm & sleep 4; nxkey l s space - l space / u s r / b i n enter 2000 s e q space 1 space 2 0 0 0 0 enter &"
                        : scene == 3 ? drags : "true") + "\n");
                }
                if (scene == 4) machine.SetRemotePointer((int)(400 + 160 * Mathf.Cos(Time.time * 1.5f)), (int)(300 + 120 * Mathf.Sin(Time.time * 1.5f)), 0, true);
            }
        }
        bool drive = target != 0 && Utilities.IsValid(targetShare) && (targetShare.flags & 3) == 3;
        if (!drive) testTypeAt = Time.time + 45f;
        else if (!testTyped && Time.time >= testTypeAt)
        {
            testTyped = true;
            consoleKeyboard.TypeText("echo NET-" + myId + "-OK\n");
        }
        if (Time.time < testAt) return;
        testAt = Time.time + 2f;
        string found = "";
        int cols = terminal.cols;
        for (int r = 0; r < terminal.rows; r++)
        {
            string line = "";
            if (target != 0)
            {
                if (wallSlot < 0) continue;
                Color32[] grid = streams.Cells(wallSlot);
                for (int x = 0; x < 12; x++) line += (char)grid[r * cols + x].r;
            }
            else line = terminal.LineText(r).Substring(0, 12);
            if (line.StartsWith("NET-")) found += line.Trim() + " ";
        }
        Debug.Log("[ShareTest] t=" + Time.time.ToString("F0") + " me=" + myId + " master=" + Networking.IsMaster + " players=" + (shareCount + 1)
                  + " on=" + machine.IsOn() + " target=" + target + " viewers=" + viewers + " sent=" + sentPackets + "/" + sentBytes
                  + " got=" + streams.Packets() + " gaps=" + streams.Gaps() + " repaired=" + repaired + " refreshed=" + refreshed
                  + " stream=" + (target != 0 && wallSlot >= 0 ? streams.Width(wallSlot) + "x" + streams.Height(wallSlot) : sendW + "x" + sendH)
                  + " events=" + tookEvents
                  + " clogged=" + Networking.IsClogged + " wall=" + displayShowMaterial.GetTexture("_MainTex").name + ":"
                  + displayShowMaterial.GetVector("_Size").x + " lines=[" + found + "]");
    }

    // Two real clients test the network (docs/system/lan.md): each switches its machine on, pings the
    // other's a while after both are there, then asks the Network program for it (who is there,
    // its Home, a picture fetched and compared), or with lanDoom or lanQuake starts that game
    // with it as the desktop's shortcuts do (nxgame); and logs what its machine sent, got and printed.
    public bool lanTest, lanDoom, lanQuake;
    private float lanAt, lanGameAt, lanDetailAt, lanBothAt = -1f;
    private bool lanStarted, lanPinged, lanPinged2, lanGame;

    private void LanTest()
    {
        if (!lanStarted)
        {
            lanStarted = true;
            netEvents = false;   // the first ping over serializations, the second over events
            machine.PowerOn();
        }
        int other = shareCount > 0 && Utilities.IsValid(shares[0]) ? shares[0].ownerId : 0;
        // (one ping after the other, and the fetches after both: each ping on a quiet link)
        if (other == 0 || !IsOn(other)) lanBothAt = -1f;
        else if (lanBothAt < 0f) lanBothAt = Time.time;
        if (lanBothAt >= 0f && !lanPinged && Time.time >= lanBothAt + (myId < other ? 60f : 85f))
        {
            lanPinged = true;
            consoleKeyboard.TypeText("ping -c 20 10.0." + (other >> 8) + "." + (other & 255) + " | tail -2\n");
        }
        // then the same over network events (the default)
        if (lanPinged && !lanPinged2 && Time.time >= lanBothAt + 110f && !netEvents)
        {
            netEvents = true;
            Debug.Log("[LanTest] batches go as network events from now on");
        }
        if (lanPinged && !lanPinged2 && Time.time >= lanBothAt + (myId < other ? 115f : 140f))
        {
            lanPinged2 = true;
            lanGameAt = Time.time + 30f;   // (the first ping's summary is on the screen until the clear)
            consoleKeyboard.TypeText("clear; ping -c 20 10.0." + (other >> 8) + "." + (other & 255) + " | tail -2\n");
        }
        if (Time.time < lanAt) return;
        lanAt = Time.time + 5f;
        // (a game's packets are logged too: forty of them every half minute from its start)
        if (lanGame && Time.time >= lanDetailAt)
        {
            lanDetailAt = Time.time + 30f;
            machine.LanDetailAgain();
            lanBatches = 0;
        }
        string found = "", last = "";
        bool answered = false;
        for (int r = 0; r < terminal.rows; r++)
        {
            string line = terminal.LineText(r);
            if (line.Contains("packets transmitted")) answered = true;
            if (line.Trim().Length > 0) last = line.Trim();
            if (line.Contains("packets transmitted") || line.Contains("round-trip") || line.Contains("nodes)") || line.Contains("doomstat: ") && line.Contains("tics")
                || line.Contains("NXNET") || line.Contains("computers answer") || line.Contains("nxnet:") || line.Contains(" Computer ")
                || line.Contains("nxgame:") || line.Contains("entered the game") || line.Contains("Connection accepted") || line.Contains("TAG=") || line.Contains("TIME=")) found += line.Trim() + " | ";
        }
        // the game after the ping: the lower number is player 1, and each names the other's address
        if (!answered) lanGameAt = Mathf.Max(lanGameAt, Time.time + (myId < other ? 35f : 10f));
        else if (!lanGame && lanPinged2 && Time.time >= lanGameAt)
        {
            lanGame = true;
            string its = "10.0." + (other >> 8) + "." + (other & 255);
            if (lanDoom) consoleKeyboard.TypeText("clear; nxgame doom now\n");
            // (the host says every twenty seconds whether it is said to host; the other what it is answered)
            else if (lanQuake) consoleKeyboard.TypeText(myId < other
                ? "clear; (while true; do sleep 20; echo TAG=$(cat /tmp/nxhosting 2>&1) $(date +%s); done) & nxgame quake host\n"
                : "clear; until nxnet --find 12 | grep quake; do date +TIME=%s; done; nxgame quake join\n");
            else consoleKeyboard.TypeText("clear; nxnet --find; echo NXNET-HOME $(nxnet --ls " + its + " / | wc -l) entries; nxnet --get " + its
                + " /Pictures/Earthrise.jpg /tmp/p; cmp /tmp/p /root/Pictures/Earthrise.jpg && echo NXNET-SAME-PICTURE\n");
        }
        Debug.Log("[LanTest] t=" + Time.time.ToString("F0") + " me=" + myId + " other=" + other + " on=" + machine.IsOn() + " pinged=" + lanPinged + " game=" + lanGame
                  + " sent=" + machine.netSent + " received=" + machine.netReceived + " batches=" + netBatches + " taken=" + netTaken
                  + " lines=[" + found + "] last=[" + last + "]");
    }

    public void SendFailed()
    {
        refreshDue = true;
    }

    // Everything goes out again.
    private void FullRefresh()
    {
        terminal.MarkAll();
        unitsReset = true;
        headerSent = false;
    }

    // ---- sending: the display ----

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        if (unitsPending) UnitsRead(request);
    }

    // The next packet: its header and console rows now, its units by the GPU (ShareUnits.shader,
    // pass by pass as tools/share_units.py), to be read back.
    private void AskUnits()
    {
        int w = machine.displayWidth, h = machine.displayHeight, scale = 1;
        if (w <= 0 || h <= 0) return;
        while (w > 1280 * scale || h > 960 * scale) scale++;
        if (w / scale != sendW || h / scale != sendH)
        {
            sendW = w / scale;
            sendH = h / scale;
            unitsReset = true;
        }
        // its header and rows once, however often its units are asked for again (into a buffer
        // of its own: `scratch` may hold the packet this tick sends)
        if (!unitsHeadBuilt)
        {
            unitsAsking = true;
            unitsHeadLength = Build(true);
            unitsAsking = false;
            unitsHeadBuilt = true;
        }
        unitsHead[4] = (byte)(sendW & 255);
        unitsHead[5] = (byte)(sendW >> 8);
        unitsHead[6] = (byte)(sendH & 255);
        unitsHead[7] = (byte)(sendH >> 8);
        int number = mine.seq + 1;
        Material m = unitsMaterial;
        m.SetVector("_TexSize", new Vector4(machine.displayTexture.width, machine.displayTexture.height, 0, 0));
        m.SetVector("_Src", new Vector4(w, h, scale, 0));
        VRCGraphics.Blit(machine.displayTexture, fineNow, m, 0);
        m.SetTexture("_Now", fineNow);
        m.SetTexture("_Before", fineBefore);
        m.SetVector("_Size", new Vector4(sendW, sendH, 0, 0));
        m.SetVector("_Lost", new Vector4(unitsLostFrom & 255, unitsLostCount, 0, 0));
        m.SetFloat("_Reset", unitsReset ? 1 : 0);
        m.SetFloat("_Room", Mathf.Max(0, PacketMost - unitsHeadLength));
        m.SetFloat("_Seq", number & 255);
        m.SetFloat("_Turn", unitsTurn);
        RenderTexture marked = unitsState[(unitsNow + 1) % 3];
        unitsNext = (unitsNow + 2) % 3;
        VRCGraphics.Blit(fineNow, unitsBlocks, m, 1);
        m.SetTexture("_Blocks", unitsBlocks);
        VRCGraphics.Blit(fineNow, unitsExact, m, 2);
        m.SetTexture("_Exact", unitsExact);
        VRCGraphics.Blit(fineNow, unitsNeed, m, 3);
        m.SetTexture("_Need", unitsNeed);
        m.SetTexture("_State", unitsState[unitsNow]);
        VRCGraphics.Blit(fineNow, marked, m, 4);
        m.SetTexture("_State", marked);
        VRCGraphics.Blit(fineNow, unitsStats, m, 5);
        m.SetTexture("_Stats", unitsStats);
        VRCGraphics.Blit(fineNow, unitsPlan, m, 6);
        m.SetTexture("_Plan", unitsPlan);
        VRCGraphics.Blit(fineNow, unitsTable, m, 7);
        m.SetTexture("_Units", unitsTable);
        VRCGraphics.Blit(fineNow, unitsPrefix, m, 8);
        m.SetTexture("_Prefix", unitsPrefix);
        VRCGraphics.Blit(fineNow, unitsCut, m, 9);
        m.SetTexture("_Cut", unitsCut);
        VRCGraphics.Blit(fineNow, unitsPacket, m, 10);
        VRCGraphics.Blit(fineNow, unitsState[unitsNext], m, 11);
        if (netTest) unitsAskedMs = Networking.GetServerTimeInMilliseconds();
        VRCAsyncGPUReadback.Request(unitsPacket, 0, (IUdonEventReceiver)this);
        unitsPending = true;
        unitsReady = false;
        unitsDrop = false;
        unitsAskedAt = Time.time;
        unitsAskedReset = unitsReset;
        unitsAskedLost = unitsLostCount;
        unitsReset = false;
        unitsLostCount = 0;
    }

    // Whether the display goes as units now: the machine is on and has a display.
    private bool UnitsNow(bool on)
    {
        return unitsMaterial != null && on && machine.displayWidth > 0 && machine.displayHeight > 0;
    }

    private void UnitsRead(VRCAsyncGPUReadbackRequest request)
    {
        if (unitsDrop || request.hasError || !request.TryGetData(unitsRead))
        {
            UnitsDropped();
            return;
        }
        unitsLength = Mathf.Min(unitsRead[UnitsBytes] | unitsRead[UnitsBytes + 1] << 8, PacketMost - unitsHeadLength);
        unitsReady = true;
        unitsReadyAt = Time.time;
    }

    // Nothing of the packet asked for goes: its rows are sent again, and the display from nothing.
    private void UnitsDropped()
    {
        unitsPending = false;
        unitsReady = false;
        unitsDrop = false;
        unitsReset = true;
        unitsHeadBuilt = false;
        headerSent = false;
        terminal.MarkAll();
    }

    // The packet asked for, whole in `scratch`; the state is the one after it. 0: it has nothing new.
    private int TakeUnits()
    {
        unitsReady = false;
        unitsPending = false;
        unitsHeadBuilt = false;
        unitsNow = unitsNext;
        unitsAge = Time.time - unitsAskedAt;
        unitsAgeSum += unitsAge;
        unitsTurn = unitsRead[UnitsBytes + 4] | unitsRead[UnitsBytes + 5] << 8;
        captureMs = unitsAskedMs;
        // (the capture it was made of is the one the next is compared with)
        RenderTexture t = fineNow;
        fineNow = fineBefore;
        fineBefore = t;
        if (unitsLength == 0 && !unitsHeadNew) return 0;
        System.Buffer.BlockCopy(unitsHead, 0, scratch, 0, unitsHeadLength);
        System.Buffer.BlockCopy(unitsRead, 0, scratch, unitsHeadLength, unitsLength);
        System.Buffer.BlockCopy(unitsHead, 0, lastHeader, 0, HeaderBytes);
        headerSent = true;
        building = (mine.seq + 1) & (Kept - 1);
        return unitsHeadLength + unitsLength;
    }

    // ---- sending: a packet ----

    // Header and the console's changed rows; the display's units come after (TakeUnits).
    // Returns its length; 0: nothing is new.
    private int Build(bool on)
    {
        byte[] b = unitsAsking ? unitsHead : scratch;
        int w = on ? sendW : 0, h = on ? sendH : 0;
        // the machine's speed, for whoever reads it off its tower: not more often than every two seconds
        if (Time.time - speedAt >= 2f)
        {
            speedAt = Time.time;
            speedAll = on ? PackSpeed(machine.mipsAll) : 0;
            speedCore = on ? PackSpeed(machine.mipsCore) : 0;
        }
        bool asUnits = UnitsNow(on);
        b[0] = (byte)(on ? 1 : 0);
        b[1] = (byte)terminal.Top();
        b[2] = (byte)terminal.CursorColumn();
        b[3] = (byte)terminal.CursorRow();
        b[4] = (byte)(w & 255);
        b[5] = (byte)(w >> 8);
        b[6] = (byte)(h & 255);
        b[7] = (byte)(h >> 8);
        b[8] = (byte)(machine.displayWidth & 255);
        b[9] = (byte)(machine.displayWidth >> 8);
        b[10] = (byte)(machine.displayHeight & 255);
        b[11] = (byte)(machine.displayHeight >> 8);
        b[13] = (byte)(speedAll & 255);
        b[14] = (byte)(speedAll >> 8);
        b[15] = (byte)(speedCore & 255);
        b[16] = (byte)(speedCore >> 8);
        building = (mine.seq + 1) & (Kept - 1);
        keptSeq[building] = -1;
        keptCount[building] = 0;
        // what the panel says of the machine, its registers and its lamp: every two seconds
        int status = 0;
        if (on && Time.time - statusAt >= 2f)
        {
            statusAt = Time.time;
            status = PutStatus(b, HeaderBytes);
        }
        b[17] = (byte)(status & 255);
        b[18] = (byte)(status >> 8);
        int at = HeaderBytes + status, count = 0, rowsEnd = Mathf.Min(ConsoleMost + status, PacketMost);
        bool[] changed = terminal.rowChanged;
        for (int r = 0; r < terminal.rows && at + 3 + 2 * terminal.cols <= rowsEnd; r++)
        {
            if (!changed[r]) continue;
            changed[r] = false;
            at = terminal.PackRow(r, b, at);
            Keep(0x10000 + r);
            count++;
        }
        b[12] = (byte)count;
        bool header = !headerSent;
        for (int i = 0; i < 17; i++)
            if (i != 12 && b[i] != lastHeader[i]) header = true;
        if (asUnits)
        {
            // the units come after, when the GPU has made them (TakeUnits)
            unitsHeadNew = count > 0 || header || status > 0;
            return at;
        }
        if (count == 0 && !header && status == 0) return 0;
        System.Buffer.BlockCopy(b, 0, lastHeader, 0, HeaderBytes);
        headerSent = true;
        return at;
    }

    // The machine's activity bits, pc and x1 to x31, then its panel's text.
    private int PutStatus(byte[] b, int at)
    {
        int lamp = machine.activity;
        b[at] = (byte)(lamp & 255);
        b[at + 1] = (byte)((lamp >> 8) & 255);
        uint[] r = machine.registers;
        for (int i = 0; i < 32; i++)
        {
            int k = at + 2 + i * 4;
            uint v = r[i];
            b[k] = (byte)(v & 255);
            b[k + 1] = (byte)((v >> 8) & 255);
            b[k + 2] = (byte)((v >> 16) & 255);
            b[k + 3] = (byte)(v >> 24);
        }
        string text = machine.statsLine;
        int n = Mathf.Min(text.Length, StatusTextMost);
        for (int i = 0; i < n; i++) b[at + StatusFixed + i] = (byte)(text[i] & 0x7f);
        return StatusFixed + n;
    }

    // ---- sending: this player's hands on the watched machine ----

    private void Queue(int e)
    {
        int next = (outTail + 1) & 255;
        if (next == outHead) return;
        outQueue[outTail] = e;
        outTail = next;
    }

    // The keyboards and the pointer are not the local machine's while another is shown: to
    // its owner if they allow it, else nowhere. (A classroom place's are its owner's machine's
    // whatever the wall shows, and never come this way.)
    private void Drain()
    {
        bool drive = Utilities.IsValid(targetShare) && (targetShare.flags & 3) == 3;
        while (consoleKeyboard.Count() > 0)
        {
            int c = consoleKeyboard.Pop() & 0xff;
            if (drive) Queue(1 << 24 | c);
        }
        while (!machine.keysOwn && gpuKeyboard.Count() > 0)
        {
            int e = gpuKeyboard.Pop() & 0xffffff;
            if (drive) Queue(2 << 24 | e);
        }
        int p = 0;
        if (drive && machine.pointerOn && !machine.pointerOwn)
            p = (machine.pointerX & 0xfff) | (machine.pointerY & 0xfff) << 12 | (machine.pointerButtons & 7) << 24 | 1 << 28;
        // said again twice a second while it is there: the owner drops a pointer gone quiet
        if (p == pointerNow && (p == 0 || Time.time - pointerSentAt < 0.5f)) return;
        pointerNow = p;
        pointerDue = true;
        pointerSentAt = Time.time;
    }

    // Up to twelve events a packet into the ring of sixteen, so none is overwritten unread.
    private bool MoveInput()
    {
        bool any = pointerDue;
        if (pointerDue) mine.pointer = pointerNow;
        pointerDue = false;
        for (int i = 0; i < 12 && outHead != outTail; i++)
        {
            mine.input[mine.inputSeq & 15] = outQueue[outHead];
            mine.inputSeq++;
            outHead = (outHead + 1) & 255;
            any = true;
        }
        return any;
    }

    // The machine's packets for the others (docs/system/lan.md): as many as fit a batch, the
    // frame its object is free. Late in the frame too: for a readback that came after Update.
    void LateUpdate() { SendNet(); }

    private void SendNet()
    {
        if (mineNet == null || machine.NetWaiting() == 0) return;
        if (netEvents ? Networking.IsClogged || Time.time - netSentAt < 0.0105f : mineNet.Busy()) return;
        if (mineNet.lastPostSeconds > 0f)
        {
            netPostSum += mineNet.lastPostSeconds;
            netPostCount++;
            mineNet.lastPostSeconds = 0f;
        }
        int at = 0;
        while (machine.NetWaiting() > 0 && at + 2 + machine.NetNextLength() <= NetBatchMost)
        {
            int length = machine.NetPop(netScratch, at + 2);
            netScratch[at] = (byte)(length & 255);
            netScratch[at + 1] = (byte)(length >> 8);
            at += 2 + length;
        }
        byte[] batch = new byte[at];
        System.Buffer.BlockCopy(netScratch, 0, batch, 0, at);
        mineNet.packets = batch;
        mineNet.seq++;
        mineNet.sentMs = Networking.GetServerTimeInMilliseconds();
        if (netEvents) mineNet.SendCustomNetworkEvent(NetworkEventTarget.Others, nameof(EmuNet.Packets), batch, mineNet.seq, mineNet.sentMs);
        else mineNet.Send();
        netSentAt = Time.time;
        netOutCount++;
        if (mine != null) mine.netSeq = mineNet.seq;
        netBatches++;
        netFlowAt = Time.time;
        if (lanBatches < 40) { lanBatches++; Debug.Log("[Lan] batch " + mineNet.seq + " out to the others: " + at + " bytes"); }
    }

    private int lanBatches, lanMissed;   // [Lan] lines written: the first of them, not all for ever
    private float lanTimesAt;

    // Every five seconds in which batches moved: how long they took (docs/system/lan.md, "What the world writes").
    private void LanTimes()
    {
        lanTimesAt = Time.time + 5f;
        if (netOutCount == 0 && netWayCount == 0) return;
        Debug.Log("[Lan] times: " + netOutCount + " batches out" + (netEvents ? " as events" : ", " + (netPostCount > 0 ? Mathf.RoundToInt(netPostSum * 1000f / netPostCount) : 0)
                  + " ms from asking to VRChat's sending") + "; " + netWayCount + " in, " + (netWayCount > 0 ? netWaySum / netWayCount : 0)
                  + " ms on the way (most " + netWayMost + ")");
        netPostSum = 0f;
        netPostCount = netOutCount = netWaySum = netWayCount = netWayMost = 0;
    }

    // Another machine's packets: each is the local machine's if its address says so.
    public void ReceivedNet(EmuNet from)
    {
        if (!Utilities.IsValid(from) || from == mineNet || from.seq == from.seen) return;
        EmuShare share = from.share;
        int owner = share != null ? share.ownerId : 0;
        // (a batch is whatever its sender put last: one that came and went between two looks is lost)
        int missed = from.seq - from.seen - 1;
        if (missed > 0 && from.seen != 0 && lanMissed < 60)
        {
            lanMissed++;
            Debug.Log("[Lan] player " + owner + ": " + missed + " batch" + (missed > 1 ? "es" : "") + " never seen before batch " + from.seq);
        }
        from.seen = from.seq;
        if (share != null) share.netSeq = from.seq;
        byte[] batch = from.packets;
        if (batch == null) return;
        int way = Networking.GetServerTimeInMilliseconds() - from.sentMs;
        if (way >= 0 && way < 10000)
        {
            netWaySum += way;
            netWayCount++;
            if (way > netWayMost) netWayMost = way;
        }
        if (batch.Length > 0 && lanBatches < 40) { lanBatches++; Debug.Log("[Lan] batch " + from.seq + " in from player " + owner + ": " + batch.Length + " bytes, " + way + " ms on the way"); }
        int at = 0;
        while (at + 2 <= batch.Length)
        {
            int length = batch[at] | batch[at + 1] << 8;
            if (length < 20 || at + 2 + length > batch.Length) break;
            machine.NetIn(batch, at + 2, length);
            netTaken++;
            at += 2 + length;
        }
    }

    // ---- receiving ----

    // For EmuStreams and the screens' own behaviours: this player's object, the others'
    // (after Prune, in the same frame), and "send my fields".
    public EmuShare Mine() { return mine; }
    public int MyId() { return myId; }
    public int ShareCount() { return shareCount; }
    public EmuShare ShareAt(int i) { return shares[i]; }
    public void StateOut() { stateOut = true; }
    public bool Known(int id) { return Find(id) != null; }

    public bool IsOn(int id)
    {
        EmuShare share = Find(id);
        return share != null && (share.flags & 1) != 0;
    }

    public void Received(EmuShare share)
    {
        if (!Utilities.IsValid(share) || share == mine) return;
        stateDirty = true;
        streams.Received(share);
        if (share.watchedMe && share.watching == myId) TakeInput(share);
    }

    // A watcher's keys and pointer, for the local machine.
    private void TakeInput(EmuShare share)
    {
        int n = share.inputSeq - share.seenInput;
        share.seenInput = share.inputSeq;
        if (!allow || !machine.IsOn())
        {
            if (pointerFrom == share.ownerId) DropPointer();
            return;
        }
        for (int k = Mathf.Min(n, 16); k >= 1; k--)
        {
            int e = share.input[(share.inputSeq - k) & 15];
            tookEvents++;
            if ((e >> 24) == 1) machine.RemoteChar(e & 0xff);
            else if ((e >> 24) == 2) machine.RemoteKey(e & 0xffffff);
        }
        int p = share.pointer;
        if (((p >> 28) & 1) != 0)
        {
            pointerFrom = share.ownerId;
            pointerAt = Time.time;
            machine.SetRemotePointer(p & 0xfff, (p >> 12) & 0xfff, (p >> 24) & 7, true);
        }
        else if (pointerFrom == share.ownerId) DropPointer();
    }
}
