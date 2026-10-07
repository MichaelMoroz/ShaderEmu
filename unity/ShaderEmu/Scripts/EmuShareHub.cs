using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// Players see each other's machines (docs/share.md). This is local, one a client: it fills
// this player's EmuShare with the console's changed rows and the display's changed tiles
// while somebody watches, and puts the watched player's console and display on the screens
// here. A watcher's keys and pointer go the other way, when the owner allows it.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuShareHub : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard, gpuKeyboard;
    public Material encodeMaterial;         // ShareEncode.shader
    public RenderTexture captureA, captureB;   // 640 x 480 with mipmaps: this capture and the last
    public RenderTexture atlas;             // 128 x 300: a row a tile
    public Material decodeMaterial;         // ShareDecode.shader
    public Texture2D store;                 // 128 x 300: the tiles received
    public RenderTexture remotePicture;     // the watched display, decoded
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
    private const int ChangedAt = 504, FlatAt = 505, LevelAt = 508;   // bytes of a tile's row
    private const int PacketMost = 1400, ConsoleMost = 800, HeaderBytes = 13;
    private const float TickSeconds = 0.1f;   // input every tick, a packet every second one

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
    private RenderTexture now, before;
    private byte[] atlasBytes = new byte[TileCount * RowBytes];
    private int[] have = new int[TileCount];   // the level each tile was last sent at; 3: stale
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

    // receiving
    private byte[] storeBytes = new byte[TileCount * RowBytes];
    private Color32[] remoteGrid;
    private int cols, rows;
    private bool haveSeq;
    private int lastSeq, gotW, gotH;
    private float askedAt;
    private int pointerFrom;          // the watcher whose pointer the machine follows
    private float pointerAt;

    private int page;                 // of the players' rows
    private int sentPackets, sentBytes, gotPackets, gotBytes, gaps, tookEvents;
    private float testAt, testTypeAt;
    private bool testStarted, testTyped;

    void Start()
    {
        cols = terminal.cols;
        rows = terminal.rows;
        remoteGrid = new Color32[cols * rows];
        now = captureA;
        before = captureB;
        for (int t = 0; t < TileCount; t++) have[t] = 3;
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
        if (allowLabel != null) allowLabel.text = "Others may use my computer: " + (allow ? "yes" : "no");
        if (watchLabel == null) return;
        string text = "Showing your own computer";
        if (target != 0 && targetShare != null)
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
    public override void OnPlayerLeft(VRCPlayerApi player) { rosterDirty = true; }

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
                    if (share != null) found = share;
                }
            }
            if (found == null)
            {
                incomplete = true;
                continue;
            }
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
            if (shares[i].ownerId == id) return shares[i];
        return null;
    }

    // Whose machine the screens show: the one picked; left to itself this player's own while
    // it is on, else the first other player's that is.
    private void Choose()
    {
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
        haveSeq = false;
        gotW = 0;
        gotH = 0;
        outHead = outTail;
        pointerNow = 0;
        pointerDue = true;
        if (to != 0)
        {
            Color32 blank = new Color32(32, 7, 0, 255);
            for (int i = 0; i < remoteGrid.Length; i++) remoteGrid[i] = blank;
            for (int t = 0; t < TileCount; t++) storeBytes[t * RowBytes + LevelAt] = 3;
            terminal.ShowRemote(remoteGrid, 0, 0, 0);
            machine.shownWidth = 0;
            machine.shownHeight = 0;
            ShowStore();
            displayShowMaterial.SetTexture("_MainTex", remotePicture);
            displayShowMaterial.SetVector("_TexSize", new Vector4(remotePicture.width, remotePicture.height, 0, 0));
            displayShowMaterial.SetVector("_Size", Vector4.zero);
            if (to == myId) FullRefresh();
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

    // Who watches this player, who of them is new, and who has asked for everything again.
    private void Scan()
    {
        int count = selfTest ? 1 : 0;
        for (int i = 0; i < shareCount; i++)
        {
            EmuShare share = shares[i];
            if (share.watching == myId)
            {
                count++;
                if (!share.watchedMe)
                {
                    share.watchedMe = true;
                    share.seenInput = share.inputSeq;
                    share.seenResync = share.resync;
                    FullRefresh();
                }
                else if (share.resync != share.seenResync)
                {
                    share.seenResync = share.resync;
                    FullRefresh();
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
        if (rosterDirty || (incomplete && Time.time >= retryAt)) Roster();
        if (mine == null) return;
        if (netTest) NetTest();

        bool on = machine.IsOn();
        if (on != wasOn)
        {
            wasOn = on;
            stateOut = true;
            FullRefresh();
        }
        if (stateDirty)
        {
            stateDirty = false;
            Scan();
        }
        Choose();
        if (pointerFrom != 0 && Time.time - pointerAt > 2f) DropPointer();
        if (machine.inputAway) Drain();
        if (mine.Busy() || Networking.IsClogged) return;

        int length = 0;
        if ((ticks & 1) == 0 && viewers > 0)
        {
            if (on && !reading) Capture();
            length = Build(on);
        }
        bool typed = MoveInput();
        if (length == 0 && !typed && !stateOut) return;
        if (length > 0)
        {
            byte[] packet = new byte[length];
            System.Buffer.BlockCopy(scratch, 0, packet, 0, length);
            mine.packet = packet;
            mine.seq++;
            sentPackets++;
            sentBytes += length;
        }
        else mine.packet = empty;
        mine.flags = (on ? 1 : 0) | (allow ? 2 : 0);
        mine.watching = target == myId ? 0 : target;
        mine.Send();
        stateOut = false;
        if (selfTest && length > 0) Consume(mine);
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
        bool drive = target != 0 && targetShare != null && (targetShare.flags & 3) == 3;
        if (!drive) testTypeAt = Time.time + 45f;
        else if (!testTyped && Time.time >= testTypeAt)
        {
            testTyped = true;
            consoleKeyboard.TypeText("echo NET-" + myId + "-OK\n");
        }
        if (Time.time < testAt) return;
        testAt = Time.time + 2f;
        int[] levels = new int[4];
        for (int t = 0; t < TileCount; t++) levels[target != 0 ? storeBytes[t * RowBytes + LevelAt] : have[t]]++;
        string found = "";
        for (int r = 0; r < rows; r++)
        {
            string line = "";
            if (target != 0)
                for (int x = 0; x < 12; x++) line += (char)remoteGrid[r * cols + x].r;
            else line = terminal.LineText(r).Substring(0, 12);
            if (line.StartsWith("NET-")) found += line.Trim() + " ";
        }
        Debug.Log("[ShareTest] t=" + Time.time.ToString("F0") + " me=" + myId + " master=" + Networking.IsMaster + " players=" + (shareCount + 1)
                  + " on=" + machine.IsOn() + " target=" + target + " viewers=" + viewers + " sent=" + sentPackets + "/" + sentBytes
                  + " got=" + gotPackets + "/" + gotBytes + " gaps=" + gaps + " stream=" + (target != 0 ? gotW + "x" + gotH : sendW + "x" + sendH)
                  + " tiles=" + levels[0] + "/" + levels[1] + "/" + levels[2] + "/" + levels[3] + " events=" + tookEvents
                  + " clogged=" + Networking.IsClogged + " wall=" + displayShowMaterial.GetTexture("_MainTex").name + ":"
                  + displayShowMaterial.GetVector("_Size").x + " lines=[" + found + "]");
    }

    public void SendFailed()
    {
        FullRefresh();
    }

    // Everything goes out again: someone started watching, or lost a packet.
    private void FullRefresh()
    {
        terminal.MarkAll();
        for (int t = 0; t < TileCount; t++) have[t] = 3;
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
        if (w / scale != sendW || h / scale != sendH)
        {
            sendW = w / scale;
            sendH = h / scale;
            for (int t = 0; t < TileCount; t++) have[t] = 3;
            haveAtlas = false;
            validFrom = asked + 1;
        }
        encodeMaterial.SetVector("_Src", new Vector4(w, h, scale, 0));
        encodeMaterial.SetVector("_TexSize", new Vector4(machine.displayTexture.width, machine.displayTexture.height, 0, 0));
        VRCGraphics.Blit(machine.displayTexture, now, encodeMaterial, 0);
        encodeMaterial.SetTexture("_Now", now);
        encodeMaterial.SetTexture("_Before", before);
        VRCGraphics.Blit(now, atlas, encodeMaterial, 1);
        VRCAsyncGPUReadback.Request(atlas, 0, (IUdonEventReceiver)this);
        asked++;
        reading = true;
        RenderTexture t2 = now;
        now = before;
        before = t2;
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
        return at + 2 + size;
    }

    // Stale tiles first, as sharp as two packets can carry them all; with none left, the
    // coarsest of the rest a level sharper. A flat tile is never sent sharper than it needs.
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
            return at;
        }
        for (int from = 2; from >= 1 && at == start; from--)
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
                    break;
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
        int at = HeaderBytes, count = 0;
        bool[] changed = terminal.rowChanged;
        for (int r = 0; r < rows && at + 3 + 2 * cols <= ConsoleMost; r++)
        {
            if (!changed[r]) continue;
            changed[r] = false;
            at = terminal.PackRow(r, b, at);
            count++;
        }
        b[12] = (byte)count;
        int tilesAt = at;
        if (w > 0 && haveAtlas && work) at = PutTiles(b, at);
        bool header = !headerSent;
        for (int i = 0; i < HeaderBytes - 1; i++)
            if (b[i] != lastHeader[i]) header = true;
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
    // its owner if they allow it, else nowhere.
    private void Drain()
    {
        bool drive = targetShare != null && (targetShare.flags & 3) == 3;
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
        if (drive && machine.pointerOn)
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

    // ---- receiving ----

    public void Received(EmuShare share)
    {
        if (share == mine) return;
        stateDirty = true;
        if (target != 0 && share == targetShare) Consume(share);
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

    private void ShowStore()
    {
        store.LoadRawTextureData(storeBytes);
        store.Apply(false);
        decodeMaterial.SetTexture("_Store", store);
        decodeMaterial.SetVector("_Stream", new Vector4(gotW, gotH, 0, 0));
        decodeMaterial.SetVector("_TargetSize", new Vector4(remotePicture.width, remotePicture.height, 0, 0));
        VRCGraphics.Blit(store, remotePicture, decodeMaterial);
    }

    // A packet of the watched player's: rows into the console's grid, tiles into the store.
    private void Consume(EmuShare share)
    {
        byte[] p = share.packet;
        if (p == null || p.Length < HeaderBytes) return;
        if (haveSeq && share.seq == lastSeq) return;
        if (haveSeq && share.seq != lastSeq + 1 && Time.time - askedAt > 2f)
        {
            // one was lost: ask for everything again
            gaps++;
            askedAt = Time.time;
            mine.resync++;
            stateOut = true;
        }
        haveSeq = true;
        lastSeq = share.seq;
        gotPackets++;
        gotBytes += p.Length;

        bool on = p[0] != 0, redraw = false;
        int w = p[4] | p[5] << 8, h = p[6] | p[7] << 8;
        if (w != gotW || h != gotH)
        {
            gotW = w;
            gotH = h;
            for (int t = 0; t < TileCount; t++) storeBytes[t * RowBytes + LevelAt] = 3;
            redraw = true;
        }
        int at = HeaderBytes, count = p[12];
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < count; i++)
        {
            if (at + 3 > p.Length) return;
            int r = p[at], length = p[at + 1], cells = at + 2;
            if (r >= rows || length > cols || cells + length + 1 > p.Length) return;
            bool coloured = p[cells + length] != 0;
            at = cells + length + 1;
            if (coloured && at + length > p.Length) return;
            int g = r * cols;
            for (int x = 0; x < length; x++)
            {
                int a = coloured ? p[at + x] : 7;
                remoteGrid[g + x] = new Color32(p[cells + x], (byte)(a & 15), (byte)(a >> 4), 255);
            }
            for (int x = length; x < cols; x++) remoteGrid[g + x] = blank;
            if (coloured) at += length;
        }
        terminal.ShowRemote(remoteGrid, p[1], p[2], p[3]);

        while (at + 2 <= p.Length)
        {
            int v = p[at] | p[at + 1] << 8, t = v & 0xfff, level = v >> 12;
            int size = level == 0 ? 384 : level == 1 ? 96 : 24;
            if (t >= TileCount || level > 2 || at + 2 + size > p.Length) break;
            System.Buffer.BlockCopy(p, at + 2, storeBytes, t * RowBytes + (level == 0 ? 0 : level == 1 ? 384 : 480), size);
            storeBytes[t * RowBytes + LevelAt] = (byte)level;
            at += 2 + size;
            redraw = true;
        }
        if (redraw) ShowStore();
        displayShowMaterial.SetVector("_Size", new Vector4(on ? w : 0, on ? h : 0, 0, 0));
        machine.shownWidth = on && w > 0 ? p[8] | p[9] << 8 : 0;
        machine.shownHeight = on && w > 0 ? p[10] | p[11] << 8 : 0;
    }
}
