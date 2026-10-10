using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// Other players' consoles and displays as they arrive (docs/share.md), for every screen that
// shows one: the wall, a classroom tube, a holodeck's screen. A surface says whose it wants;
// each sender wanted has a slot (a store of tiles, a picture, a console) and is decoded once.
// This client's asks for what it lost go out in its own EmuShare.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuStreams : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuTerminal terminal;       // for the console's columns and rows
    public Texture2D[] stores;         // a slot's tiles, laid out as the sender's atlas
    public RenderTexture[] pictures;   // and decoded
    public Texture2D[] grids;          // its console, a texel a cell
    public RenderTexture[] consoles;   // and drawn (640 x 480)
    public Material consoleMaterial;   // Terminal.shader, this behaviour's own
    public Material decodeMaterial;    // ShareDecode.shader, this behaviour's own
    public Texture2D blackTexture;
    public bool dropTest;              // every tenth packet is treated as lost
    public bool log;                   // a line a minute in the output log

    public const int Surfaces = 24;    // 0: the wall; 1 to 8: the classroom; 9 to 16: the holodecks
    public const int HeaderBytes = 17;
    // as EmuShareHub's
    private const int TileCount = 300, RowBytes = 512, LevelAt = 508, StampAt = 509;
    private const int FineAt = 300, FineRows = 1200, FineMost = 400, PresentAt = 2, FineStampAt = 3;

    private int count;
    private int[] wants = new int[Surfaces];   // the sender each surface shows; 0: none
    private int[] sender;              // a slot's; 0: free
    private byte[][] bytes;
    private Color32[][] cells;
    private int[] lastSeq, gotW, gotH, gotScale, stamp, shownW, shownH, top, cursorX, cursorY, version, mipsAll, mipsCore, askCount;
    private bool[] haveSeq, on, fresh;
    private float[] since, askedAt, heardAt;
    private int cols, rows;
    private float nextAt, logAt;
    private bool started;
    private int packets, gaps, repairs, refreshes;

    private void Init()
    {
        if (started) return;
        started = true;
        count = stores.Length;
        sender = new int[count];
        bytes = new byte[count][];
        cells = new Color32[count][];
        lastSeq = new int[count];
        gotW = new int[count];
        gotH = new int[count];
        gotScale = new int[count];
        stamp = new int[count];
        shownW = new int[count];
        shownH = new int[count];
        top = new int[count];
        cursorX = new int[count];
        cursorY = new int[count];
        version = new int[count];
        mipsAll = new int[count];
        mipsCore = new int[count];
        askCount = new int[count];
        haveSeq = new bool[count];
        fresh = new bool[count];
        on = new bool[count];
        since = new float[count];
        askedAt = new float[count];
        heardAt = new float[count];
        cols = terminal.cols;
        rows = terminal.rows;
        for (int s = 0; s < count; s++)
        {
            bytes[s] = new byte[(TileCount + FineRows) * RowBytes];
            cells[s] = new Color32[cols * rows];
            Blank(s);
        }
    }

    void Start()
    {
        Init();
    }

    // ---- for the surfaces ----

    // A surface shows this sender from now on (0: nobody).
    public void SetWant(int surface, int id)
    {
        Init();
        if (wants[surface] == id) return;
        wants[surface] = id;
        nextAt = 0f;
    }

    public int SlotOf(int id)
    {
        Init();
        if (id <= 0) return -1;
        for (int s = 0; s < count; s++)
            if (sender[s] == id) return s;
        return -1;
    }

    public Texture Picture(int slot) { return pictures[slot]; }
    public Texture Console(int slot) { return consoles[slot]; }
    public Color32[] Cells(int slot) { return cells[slot]; }
    public int Width(int slot) { return on[slot] ? gotW[slot] * gotScale[slot] : 0; }      // of the picture drawn
    public int Height(int slot) { return on[slot] ? gotH[slot] * gotScale[slot] : 0; }
    public int DisplayWidth(int slot) { return on[slot] && gotW[slot] > 0 ? shownW[slot] : 0; }   // of the sender's display
    public int DisplayHeight(int slot) { return on[slot] && gotW[slot] > 0 ? shownH[slot] : 0; }
    public int Top(int slot) { return top[slot]; }
    public int CursorX(int slot) { return cursorX[slot]; }
    public int CursorY(int slot) { return cursorY[slot]; }
    public int Version(int slot) { return version[slot]; }   // counted up with every packet taken
    public int MipsAll(int slot) { return mipsAll[slot]; }   // tenths of a million instructions a second
    public int MipsCore(int slot) { return mipsCore[slot]; }
    public int Gaps() { return gaps; }
    public int Packets() { return packets; }

    // ---- slots ----

    // Nothing of a sender is held or shown.
    private void Blank(int s)
    {
        byte[] b = bytes[s];
        for (int t = 0; t < TileCount; t++) b[t * RowBytes + LevelAt] = 3;
        for (int r = 0; r < FineRows; r++) b[(FineAt + r) * RowBytes + PresentAt] = 0;
        Color32 blank = new Color32(32, 7, 0, 255);
        Color32[] grid = cells[s];
        for (int i = 0; i < grid.Length; i++) grid[i] = blank;
        gotW[s] = 0;
        gotH[s] = 0;
        gotScale[s] = 1;
        shownW[s] = 0;
        shownH[s] = 0;
        mipsAll[s] = 0;
        mipsCore[s] = 0;
        on[s] = false;
        haveSeq[s] = false;
        version[s]++;
        if (blackTexture != null)
        {
            VRCGraphics.Blit(blackTexture, pictures[s]);
            VRCGraphics.Blit(blackTexture, consoles[s]);
        }
    }

    // Every sender some surface wants has a slot; a slot nobody wants is emptied.
    private void Reconcile()
    {
        bool changed = false;
        for (int s = 0; s < count; s++)
        {
            if (sender[s] == 0) continue;
            bool wanted = false;
            for (int i = 0; i < Surfaces; i++)
                if (wants[i] == sender[s]) wanted = true;
            if (wanted && hub.Known(sender[s])) continue;
            sender[s] = 0;
            Blank(s);
            changed = true;
        }
        for (int i = 0; i < Surfaces; i++)
        {
            int id = wants[i];
            if (id <= 0 || SlotOf(id) >= 0 || !hub.Known(id)) continue;
            for (int s = 0; s < count; s++)
            {
                if (sender[s] != 0) continue;
                sender[s] = id;
                since[s] = Time.time;
                askedAt[s] = Time.time;
                heardAt[s] = Time.time;
                askCount[s] = (askCount[s] + 1) & 255;   // a new entry: its sender sends everything
                fresh[s] = true;
                changed = true;
                break;
            }
        }
        if (changed) WriteAsks(-1, 0, 0);
    }

    // This client's asks, in its own share: a slot's sender and a count, and what was lost
    // (a packet number's low 16 bits and how many from it; none: everything).
    private void WriteAsks(int slot, int from, int lost)
    {
        EmuShare mine = hub.Mine();
        if (mine == null) return;
        // (an object made from a template older than these fields has them empty)
        if (mine.ask == null || mine.ask.Length != count) mine.ask = new int[count];
        if (mine.lost == null || mine.lost.Length != count) mine.lost = new int[count];
        // (a slot's new sender is not asked for what was lost of the one before)
        for (int s = 0; s < count; s++)
        {
            if (fresh[s]) mine.lost[s] = 0;
            fresh[s] = false;
        }
        if (slot >= 0)
        {
            askCount[slot] = (askCount[slot] + 1) & 255;
            mine.lost[slot] = (from & 0xffff) | (Mathf.Min(lost, 255) << 16);
            askedAt[slot] = Time.time;
        }
        for (int s = 0; s < count && s < mine.ask.Length; s++)
            mine.ask[s] = sender[s] == 0 ? 0 : (sender[s] & 0xffff) | askCount[s] << 16;
        hub.StateOut();
    }

    void Update()
    {
        if (Time.time < nextAt) return;
        nextAt = Time.time + 0.5f;
        Init();
        if (hub.Mine() == null) return;
        hub.Prune();
        Reconcile();
        // a sender that is on and has sent nothing since the slot was made, or for a while after
        // an ask, is asked for everything: a packet, or the ask itself, was lost
        for (int s = 0; s < count; s++)
        {
            if (sender[s] == 0 || haveSeq[s]) continue;
            if (Time.time - since[s] < 2f || Time.time - askedAt[s] < 5f || !hub.IsOn(sender[s])) continue;
            refreshes++;
            WriteAsks(s, 0, 0);
        }
        if (log && Time.time >= logAt)
        {
            logAt = Time.time + 60f;
            string line = "";
            for (int s = 0; s < count; s++)
                if (sender[s] != 0)
                    line += " [" + s + ": " + sender[s] + (haveSeq[s] ? " seq " + lastSeq[s] : " nothing yet") + " " + gotW[s] + "x" + gotH[s]
                          + " heard " + (Time.time - heardAt[s]).ToString("F0") + "s ago]";
            Debug.Log("[Streams] t=" + Time.time.ToString("F0") + " packets=" + packets + " gaps=" + gaps + " repairs=" + repairs
                      + " refreshes=" + refreshes + line);
        }
    }

    // ---- a packet ----

    // A packet of somebody's: if a slot is theirs, rows into its console, tiles into its store.
    public void Received(EmuShare share)
    {
        Init();
        if (!Utilities.IsValid(share)) return;
        int s = SlotOf(share.ownerId);
        if (s < 0) return;
        byte[] p = share.packet;
        if (p == null || p.Length < HeaderBytes) return;
        if (haveSeq[s] && share.seq == lastSeq[s]) return;
        if (dropTest && share.seq % 10 == 0) return;
        if (haveSeq[s] && share.seq != lastSeq[s] + 1 && share.seq > lastSeq[s])
        {
            // some were lost: their sender sends what they carried again
            gaps++;
            repairs++;
            WriteAsks(s, lastSeq[s] + 1, share.seq - lastSeq[s] - 1);
        }
        haveSeq[s] = true;
        lastSeq[s] = share.seq;
        heardAt[s] = Time.time;
        packets++;

        byte[] b = bytes[s];
        bool redraw = false, all = false;
        int w = p[4] | p[5] << 8, h = p[6] | p[7] << 8;
        // where the stream is half the display's size, tiles come at the fine level too
        int scale = w > 0 && (p[8] | p[9] << 8) / w == 2 ? 2 : 1;
        if (w != gotW[s] || h != gotH[s] || scale != gotScale[s])
        {
            for (int t = 0; t < TileCount; t++) b[t * RowBytes + LevelAt] = 3;
            for (int r = 0; r < FineRows; r++) b[(FineAt + r) * RowBytes + PresentAt] = 0;
            gotW[s] = w;
            gotH[s] = h;
            gotScale[s] = scale;
            all = true;
        }
        on[s] = p[0] != 0;
        top[s] = p[1];
        cursorX[s] = p[2];
        cursorY[s] = p[3];
        shownW[s] = p[8] | p[9] << 8;
        shownH[s] = p[10] | p[11] << 8;
        mipsAll[s] = p[13] | p[14] << 8;
        mipsCore[s] = p[15] | p[16] << 8;
        stamp[s] = stamp[s] % 255 + 1;
        version[s]++;

        int at = HeaderBytes, sent = p[12];
        Color32[] grid = cells[s];
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < sent; i++)
        {
            if (at + 3 > p.Length) return;
            int r = p[at], length = p[at + 1], from = at + 2;
            if (r >= rows || length > cols || from + length + 1 > p.Length) return;
            bool coloured = p[from + length] != 0;
            at = from + length + 1;
            if (coloured && at + length > p.Length) return;
            int g = r * cols;
            for (int x = 0; x < length; x++)
            {
                int a = coloured ? p[at + x] : 7;
                grid[g + x] = new Color32(p[from + x], (byte)(a & 15), (byte)(a >> 4), 255);
            }
            for (int x = length; x < cols; x++) grid[g + x] = blank;
            if (coloured) at += length;
        }
        // (the cursor moves with no row sent)
        grids[s].SetPixels32(grid);
        grids[s].Apply(false);
        consoleMaterial.SetTexture("_Grid", grids[s]);
        consoleMaterial.SetInt("_RowOffset", top[s]);
        consoleMaterial.SetVector("_Cursor", new Vector4(cursorX[s], cursorY[s], 1, 0));
        VRCGraphics.Blit(grids[s], consoles[s], consoleMaterial);

        while (at + 2 <= p.Length)
        {
            int v = p[at] | p[at + 1] << 8, t = v & 0xfff, level = v >> 12;
            if (t >= TileCount) break;
            if (level >= 4)
            {
                // a quarter of the tile at the fine level: its length, its bytes
                if (level > 7 || at + 4 > p.Length) break;
                int length = p[at + 2] | p[at + 3] << 8, row = (FineAt + t * 4 + level - 4) * RowBytes;
                if (length < 16 || length > FineMost || at + 4 + length > p.Length) break;
                System.Buffer.BlockCopy(p, at + 4, b, row + 4, length);
                b[row + PresentAt] = 1;
                b[row + FineStampAt] = (byte)stamp[s];
                at += 4 + length;
                redraw = true;
                continue;
            }
            int size = level == 0 ? 384 : level == 1 ? 96 : 24;
            if (level > 2 || at + 2 + size > p.Length) break;
            System.Buffer.BlockCopy(p, at + 2, b, t * RowBytes + (level == 0 ? 0 : level == 1 ? 384 : 480), size);
            b[t * RowBytes + LevelAt] = (byte)level;
            b[t * RowBytes + StampAt] = (byte)stamp[s];
            // the tile changed: what came of it at the fine level is of the old picture
            for (int k = 0; k < 4; k++) b[(FineAt + t * 4 + k) * RowBytes + PresentAt] = 0;
            at += 2 + size;
            redraw = true;
        }
        if (redraw || all)
        {
            Texture2D store = stores[s];
            store.LoadRawTextureData(b);
            store.Apply(false);
            decodeMaterial.SetTexture("_Store", store);
            decodeMaterial.SetFloat("_Scale", scale);
            decodeMaterial.SetFloat("_Stamp", stamp[s]);
            decodeMaterial.SetFloat("_All", all ? 1 : 0);
            decodeMaterial.SetVector("_Stream", new Vector4(w, h, 0, 0));
            decodeMaterial.SetVector("_TargetSize", new Vector4(pictures[s].width, pictures[s].height, 0, 0));
            VRCGraphics.Blit(store, pictures[s], decodeMaterial);
        }
    }

    // How many of a slot's tiles are held at each level (3: not at all), for the tests.
    public int[] Levels(int slot)
    {
        int[] levels = new int[4];
        byte[] b = bytes[slot];
        for (int t = 0; t < TileCount; t++) levels[b[t * RowBytes + LevelAt] & 3]++;
        return levels;
    }
}
