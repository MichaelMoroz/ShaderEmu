using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// Players see each other's machines (docs/share.md). This is local, one a client: it fills
// this player's EmuShare with the console's changed rows and the display's changed tiles
// while somebody shows them, and puts the watched player's console and display on the wall
// here (EmuStreams receives them). A watcher's keys and pointer go the other way, when the
// owner allows it.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuShareHub : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard, gpuKeyboard;
    public EmuStreams streams;              // what arrives, for every screen that shows a player
    public Material encodeMaterial;         // ShareEncode.shader
    public RenderTexture captureA, captureB;   // 640 x 480 with mipmaps: this capture and the last
    public RenderTexture atlas;             // 128 x 1500: a row a tile, then a row a quarter of a tile at the fine level
    public RenderTexture fineA, fineB;      // 1280 x 960: this capture and the last at the display's own size
    public RenderTexture changed;           // 20 x 15: which tiles changed
    public RenderTexture fineBlocks;        // 640 x 240: the fine level's blocks
    public Material displayShowMaterial;    // the wall
    public TextMeshProUGUI watchLabel, allowLabel;
    // The panel by the display: "Automatic", "My own", then a page of the other players.
    public GameObject[] rowObjects;
    public Image[] rowPlates;
    public TextMeshProUGUI[] rowLabels;
    public GameObject moreButton;
    public Color rowColour, pickedColour;
    public bool selfTest;                   // watch this machine through the stream
    public bool netTest;                    // two clients test themselves and log (docs/share.md)

    private const int TileCols = 20, TileCount = 300, RowBytes = 512;
    private const int ChangedAt = 504, FlatAt = 505;   // bytes of a tile's row
    // The fine level: the display's own pixels, where the stream is half its size. A row a
    // quarter of a tile from row 300: its length, (in the store) present and stamp, its bytes.
    private const int FineAt = 300, FineMost = 400;
    // Two ticks of three carry a packet: 7.7 KB/s with the fields beside it, of the 8 to 10 a
    // client gets out in practice for everything it sends (VRChat's limit is a client's).
    private const int PacketMost = 1150, ConsoleMost = 800, HeaderBytes = 17;
    private const int NetBatchMost = 600;   // a sending's share for the network: one full packet, or several small
    private byte[] netScratch = new byte[NetBatchMost + 600];
    [HideInInspector] public int netBatches, netTaken;
    private const float TickSeconds = 0.1f;   // input every tick, a packet on two of three
    // What each of the last packets carried, to send again when a receiver says one was lost.
    private const int Kept = 32, KeptMost = 96;

    private EmuShare mine;
    private EmuShare[] shares = new EmuShare[100];   // the other players'
    private int shareCount;
    private VRCPlayerApi[] players = new VRCPlayerApi[100];
    private bool rosterDirty = true, incomplete, stateDirty = true, stateOut = true;
    private float retryAt, nextTick;
    private int ticks, myId;

    private int picked = -1;          // -1: automatic; 0: this player's own; else a player
    private int target;               // the player shown; 0: this player's own machine
    private EmuShare targetShare;
    private bool allow, wasOn;
    private int viewers;

    // sending
    private RenderTexture now, before, fineNow, fineBefore;
    private byte[] atlasBytes = new byte[(TileCount + 1200) * RowBytes];
    private int[] have = new int[TileCount];   // the level each tile was last sent at; 3: stale
    private int[] fineHave = new int[TileCount];   // a bit a quarter of the tile sent at the fine level
    private bool fineOn, codeAll = true;
    private bool reading, haveAtlas, work, headerSent;
    private int asked, done, validFrom;
    private int sendW, sendH, cursor;
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
    private int[] keptWhat = new int[Kept * KeptMost];   // a tile and its level << 12; 0x10000 + a console row
    private int building;             // the place in the ring of the packet being built
    private bool refreshDue;
    private float refreshedAt = -10f, speedAt;
    private int speedAll, speedCore;

    // the wall
    private int wallSlot = -1, wallVersion = -1;
    private Color32[] blankGrid;
    private int pointerFrom;          // the watcher whose pointer the machine follows
    private float pointerAt;

    private int page;                 // of the players' rows
    private int sentPackets, sentBytes, tookEvents, repaired, refreshed;
    private float testAt, testTypeAt;
    private bool testStarted, testTyped;

    void Start()
    {
        blankGrid = new Color32[terminal.cols * terminal.rows];
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < blankGrid.Length; i++) blankGrid[i] = blank;
        now = captureA;
        before = captureB;
        fineNow = fineA;
        fineBefore = fineB;
        for (int t = 0; t < TileCount; t++) have[t] = 3;
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
        watchLabel.text = text + "\n" + (viewers == 0 ? "Nobody is watching yours" : viewers + " watching yours");
        if (rowObjects == null || rowObjects.Length < 10) return;
        if (page * 8 >= shareCount) page = 0;
        rowPlates[0].color = picked < 0 ? pickedColour : rowColour;
        rowPlates[1].color = picked == 0 ? pickedColour : rowColour;
        for (int row = 0; row < 8; row++)
        {
            int index = page * 8 + row;
            rowObjects[row + 2].SetActive(index < shareCount);
            if (index >= shareCount) continue;
            EmuShare share = shares[index];
            rowLabels[row + 2].text = share.ownerName + ((share.flags & 1) == 0 ? "  (off)" : (share.flags & 2) != 0 ? "  (on, usable)" : "  (on)");
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
            GameObject[] objects = Networking.GetPlayerObjects(player);
            if (objects != null)
            {
                for (int k = 0; k < objects.Length; k++)
                {
                    if (!Utilities.IsValid(objects[k])) continue;
                    EmuShare share = objects[k].GetComponent<EmuShare>();
                    if (Utilities.IsValid(share)) found = share;
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
            if (player.isLocal) mine = found;
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
    }

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
                else
                {
                    int t = what & 0xfff, level = what >> 12;
                    if (level >= 4) fineHave[t] &= ~(1 << (level - 4));
                    else
                    {
                        have[t] = 3;
                        fineHave[t] = 0;
                    }
                }
            }
            if (keptCount[k] >= KeptMost) return false;   // more than is kept of one packet
        }
        work = true;
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
        if (mine.Busy() || Networking.IsClogged) return;

        // the network's packets go ahead of the display, which while they flow is sent half as often
        bool net = MoveNet();
        int length = 0;
        if (ticks % 3 != 0 && viewers > 0 && !(net && (ticks & 1) == 0))
        {
            if (on && !reading) Capture();
            length = Build(on);
        }
        bool typed = MoveInput();
        if (length == 0 && !typed && !stateOut && !net) return;
        if (length > 0)
        {
            byte[] packet = new byte[length];
            System.Buffer.BlockCopy(scratch, 0, packet, 0, length);
            mine.packet = packet;
            mine.seq++;
            keptSeq[building] = mine.seq;
            sentPackets++;
            sentBytes += length;
        }
        else mine.packet = empty;
        mine.flags = (on ? 1 : 0) | (allow ? 2 : 0);
        mine.watching = target == myId ? 0 : target;
        mine.Send();
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
        bool drive = target != 0 && Utilities.IsValid(targetShare) && (targetShare.flags & 3) == 3;
        if (!drive) testTypeAt = Time.time + 45f;
        else if (!testTyped && Time.time >= testTypeAt)
        {
            testTyped = true;
            consoleKeyboard.TypeText("echo NET-" + myId + "-OK\n");
        }
        if (Time.time < testAt) return;
        testAt = Time.time + 2f;
        int[] levels = new int[4];
        if (target != 0 && wallSlot >= 0) levels = streams.Levels(wallSlot);
        else for (int t = 0; t < TileCount; t++) levels[have[t]]++;
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
                  + " tiles=" + levels[0] + "/" + levels[1] + "/" + levels[2] + "/" + levels[3] + " events=" + tookEvents
                  + " clogged=" + Networking.IsClogged + " wall=" + displayShowMaterial.GetTexture("_MainTex").name + ":"
                  + displayShowMaterial.GetVector("_Size").x + " lines=[" + found + "]");
    }

    // Two real clients test the network (docs/lan.md): each switches its machine on, pings the
    // other's a while after both are there, then starts a two-player game of Doom with it,
    // and logs what its machine sent, got and printed.
    public bool lanTest;
    private float lanAt, lanPingAt, lanGameAt;
    private bool lanStarted, lanPinged, lanGame;

    private void LanTest()
    {
        if (!lanStarted)
        {
            lanStarted = true;
            machine.PowerOn();
        }
        int other = shareCount > 0 && Utilities.IsValid(shares[0]) ? shares[0].ownerId : 0;
        if (other == 0 || !IsOn(other)) lanPingAt = Time.time + 40f;
        else if (!lanPinged && Time.time >= lanPingAt)
        {
            lanPinged = true;
            consoleKeyboard.TypeText("ping -c 5 10.0." + (other >> 8) + "." + (other & 255) + " | tail -2\n");
        }
        if (Time.time < lanAt) return;
        lanAt = Time.time + 5f;
        string found = "", last = "";
        bool answered = false;
        for (int r = 0; r < terminal.rows; r++)
        {
            string line = terminal.LineText(r);
            if (line.Contains("packets transmitted")) answered = true;
            if (line.Trim().Length > 0) last = line.Trim();
            if (line.Contains("packets transmitted") || line.Contains("nodes)") || line.Contains("doomstat: ") && line.Contains("tics")) found += line.Trim() + " | ";
        }
        // the game after the ping: the lower number is player 1, and each names the other's address
        if (!answered) lanGameAt = Time.time + 5f;
        else if (!lanGame && Time.time >= lanGameAt)
        {
            lanGame = true;
            consoleKeyboard.TypeText("doom -net " + (myId < other ? 1 : 2) + " .10.0." + (other >> 8) + "." + (other & 255) + "\n");
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
        for (int t = 0; t < TileCount; t++)
        {
            have[t] = 3;
            fineHave[t] = 0;
        }
        work = true;
        headerSent = false;
    }

    // ---- sending: the display ----

    // The GPU packs the whole picture; what comes back says which tiles changed.
    private void Capture()
    {
        int w = machine.displayWidth, h = machine.displayHeight;
        if (w <= 0 || h <= 0)
        {
            sendW = 0;
            sendH = 0;
            return;
        }
        int scale = 1;
        while (w > 640 * scale || h > 480 * scale) scale++;
        if (w / scale != sendW || h / scale != sendH || (scale == 2) != fineOn)
        {
            sendW = w / scale;
            sendH = h / scale;
            fineOn = scale == 2;
            for (int t = 0; t < TileCount; t++)
            {
                have[t] = 3;
                fineHave[t] = 0;
            }
            haveAtlas = false;
            validFrom = asked + 1;
            codeAll = true;
        }
        Material m = encodeMaterial;
        m.SetVector("_TexSize", new Vector4(machine.displayTexture.width, machine.displayTexture.height, 0, 0));
        m.SetVector("_Src", new Vector4(w, h, scale, 0));
        VRCGraphics.Blit(machine.displayTexture, now, m, 0);
        if (fineOn)
        {
            // the display's own pixels, for the level a still tile ends at
            m.SetVector("_Src", new Vector4(w, h, 1, 0));
            VRCGraphics.Blit(machine.displayTexture, fineNow, m, 0);
        }
        m.SetTexture("_Now", now);
        m.SetTexture("_Before", before);
        m.SetTexture("_FineNow", fineNow);
        m.SetTexture("_FineBefore", fineBefore);
        m.SetVector("_Fine", new Vector4(fineOn ? 1 : 0, codeAll ? 1 : 0, 0, 0));
        codeAll = false;
        VRCGraphics.Blit(now, changed, m, 2);
        m.SetTexture("_Changed", changed);
        VRCGraphics.Blit(now, atlas, m, 1);
        if (fineOn)
        {
            // only of the tiles that changed: the rest of the target keeps its bytes
            VRCGraphics.Blit(now, fineBlocks, m, 3);
            m.SetTexture("_FineBlocks", fineBlocks);
            VRCGraphics.Blit(now, atlas, m, 4);
        }
        VRCAsyncGPUReadback.Request(atlas, 0, (IUdonEventReceiver)this);
        asked++;
        reading = true;
        RenderTexture t2 = now;
        now = before;
        before = t2;
        t2 = fineNow;
        fineNow = fineBefore;
        fineBefore = t2;
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        reading = false;
        done++;
        if (request.hasError || !request.TryGetData(atlasBytes))
        {
            haveAtlas = false;
            for (int t = 0; t < TileCount; t++) have[t] = 3;
            return;
        }
        if (done < validFrom) return;   // packed before the picture changed size
        haveAtlas = true;
        int across = (sendW + 31) / 32, used = across * ((sendH + 31) / 32);
        for (int i = 0; i < used; i++)
        {
            int t = (i / across) * TileCols + i % across;
            if (atlasBytes[t * RowBytes + ChangedAt] == 0) continue;
            have[t] = 3;
            fineHave[t] = 0;
            work = true;
        }
    }

    // A tile at a level into the packet, if it fits: its number and level, then its blocks.
    private int Put(byte[] b, int at, int t, int level)
    {
        int size = level == 0 ? 384 : level == 1 ? 96 : 24;
        if (at + 2 + size > PacketMost) return at;
        int v = t | level << 12;
        b[at] = (byte)(v & 255);
        b[at + 1] = (byte)(v >> 8);
        System.Buffer.BlockCopy(atlasBytes, t * RowBytes + (level == 0 ? 0 : level == 1 ? 384 : 480), b, at + 2, size);
        have[t] = level;
        Keep(v);
        return at + 2 + size;
    }

    // A quarter of a tile at the fine level, if it fits: the tile's number and 4 + the
    // quarter, its length, its bytes.
    private int PutFine(byte[] b, int at, int t, int part)
    {
        int row = (FineAt + t * 4 + part) * RowBytes, size = atlasBytes[row] | atlasBytes[row + 1] << 8;
        if (size < 16 || size > FineMost)
        {
            fineHave[t] |= 1 << part;   // not made: nothing to wait for
            return at;
        }
        if (at + 4 + size > PacketMost) return at;
        int v = t | (4 + part) << 12;
        b[at] = (byte)(v & 255);
        b[at + 1] = (byte)(v >> 8);
        b[at + 2] = (byte)(size & 255);
        b[at + 3] = (byte)(size >> 8);
        System.Buffer.BlockCopy(atlasBytes, row + 4, b, at + 4, size);
        fineHave[t] |= 1 << part;
        Keep(v);
        return at + 4 + size;
    }

    // Stale tiles first, as sharp as two packets can carry them all; in what room they leave,
    // the coarsest of the rest a level sharper, and then tiles that are as sharp as the
    // stream gets, at the fine level. A flat tile is never sent sharper than it needs.
    private int PutTiles(byte[] b, int at)
    {
        int across = (sendW + 31) / 32, used = across * ((sendH + 31) / 32);
        if (used == 0) return at;
        int stale = 0;
        for (int i = 0; i < used; i++)
            if (have[(i / across) * TileCols + i % across] == 3) stale++;
        int room = PacketMost - at, start = at;
        if (stale > 0)
        {
            int level = stale * 386 <= 2 * room ? 0 : stale * 98 <= 2 * room ? 1 : 2;
            for (int i = 0; i < used; i++)
            {
                int index = (cursor + i) % used, t = (index / across) * TileCols + index % across;
                if (have[t] != 3) continue;
                int flat = atlasBytes[t * RowBytes + FlatAt];
                int next = Put(b, at, t, flat > level ? flat : level);
                if (next == at)
                {
                    cursor = index;
                    break;
                }
                at = next;
            }
            if (at + 26 > PacketMost) return at;
        }
        int mark = at;
        bool full = false;
        for (int from = 2; from >= 1 && at == mark; from--)
        {
            for (int i = 0; i < used; i++)
            {
                int index = (cursor + i) % used, t = (index / across) * TileCols + index % across;
                if (have[t] != from) continue;
                int flat = atlasBytes[t * RowBytes + FlatAt];
                if (flat >= from) continue;
                int next = Put(b, at, t, flat > from - 1 ? flat : from - 1);
                if (next == at)
                {
                    cursor = index;
                    full = true;
                    break;
                }
                at = next;
            }
        }
        for (int i = 0; fineOn && !full && i < used; i++)
        {
            int index = (cursor + i) % used, t = (index / across) * TileCols + index % across;
            if (have[t] != 0 || fineHave[t] == 15 || atlasBytes[t * RowBytes + FlatAt] != 0) continue;
            for (int part = 0; part < 4 && !full; part++)
            {
                if ((fineHave[t] >> part & 1) != 0) continue;
                int next = PutFine(b, at, t, part);
                if (next == at && (fineHave[t] >> part & 1) == 0)
                {
                    cursor = index;
                    full = true;
                }
                at = next;
            }
        }
        if (at == start) work = false;
        return at;
    }

    // ---- sending: a packet ----

    // Header, the console's changed rows, then tiles. Returns its length; 0: nothing is new.
    private int Build(bool on)
    {
        byte[] b = scratch;
        int w = on ? sendW : 0, h = on ? sendH : 0;
        // the machine's speed, for whoever reads it off its tower: not more often than every two seconds
        if (Time.time - speedAt >= 2f)
        {
            speedAt = Time.time;
            speedAll = on ? Mathf.Clamp(Mathf.RoundToInt(machine.mipsAll * 10f), 0, 9999) : 0;
            speedCore = on ? Mathf.Clamp(Mathf.RoundToInt(machine.mipsCore * 10f), 0, 9999) : 0;
        }
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
        int at = HeaderBytes, count = 0;
        bool[] changed = terminal.rowChanged;
        for (int r = 0; r < terminal.rows && at + 3 + 2 * terminal.cols <= ConsoleMost; r++)
        {
            if (!changed[r]) continue;
            changed[r] = false;
            at = terminal.PackRow(r, b, at);
            Keep(0x10000 + r);
            count++;
        }
        b[12] = (byte)count;
        int tilesAt = at;
        if (w > 0 && haveAtlas && work) at = PutTiles(b, at);
        bool header = !headerSent;
        for (int i = 0; i < HeaderBytes; i++)
            if (i != 12 && b[i] != lastHeader[i]) header = true;
        if (count == 0 && at == tilesAt && !header) return 0;
        System.Buffer.BlockCopy(b, 0, lastHeader, 0, HeaderBytes);
        headerSent = true;
        return at;
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
        while (gpuKeyboard.Count() > 0)
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

    // The machine's packets for the others (docs/lan.md): as many as fit a sending's share.
    private bool MoveNet()
    {
        int at = 0;
        while (machine.NetWaiting() > 0 && at + 2 + machine.NetNextLength() <= NetBatchMost)
        {
            int length = machine.NetPop(netScratch, at + 2);
            netScratch[at] = (byte)(length & 255);
            netScratch[at + 1] = (byte)(length >> 8);
            at += 2 + length;
        }
        if (at == 0)
        {
            if (mine.net == null || mine.net.Length != 0) mine.net = empty;
            return false;
        }
        byte[] batch = new byte[at];
        System.Buffer.BlockCopy(netScratch, 0, batch, 0, at);
        mine.net = batch;
        mine.netSeq++;
        netBatches++;
        return true;
    }

    // Another machine's packets: each is the local machine's if its address says so.
    private void TakeNet(EmuShare share)
    {
        byte[] batch = share.net;
        if (share.netSeq == share.seenNet) return;
        share.seenNet = share.netSeq;
        if (batch == null) return;
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
        TakeNet(share);
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
