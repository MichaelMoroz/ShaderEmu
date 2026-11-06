using UdonSharp;
using UnityEngine;
using VRC.SDKBase;

// Other players' consoles and displays as they arrive (docs/world/share.md), for every screen that
// shows one: the wall, a classroom tube, a holodeck's screen. Every player whose machine is on
// has a slot (a picture, a console) and is drawn as packets come, shown or not, so a screen
// that turns to them has their picture at once. A surface says whose it
// wants. This client's asks for what it lost go out in its own EmuShare.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuStreams : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuTerminal terminal;       // for the console's columns and rows
    public RenderTexture[] pictures;   // a slot's display, as its packets' units have drawn it
    public Texture2D[] grids;          // its console, a texel a cell
    public RenderTexture[] consoles;   // and drawn (640 x 480)
    public Material consoleMaterial;   // Terminal.shader, this behaviour's own
    public Texture2D blackTexture;
    // A packet's units (docs/world/share.md, "Display"): its bytes as a texture, where each unit's
    // record is in it, and the shader that draws them into a slot's picture.
    public Material unitsMaterial;     // ShareUnitsDecode.shader
    public Texture2D unitsPacket;      // 288 x 1
    public RenderTexture unitsIndex;   // 80 x 113
    public bool dropTest;              // every tenth packet is treated as lost
    public bool log;                   // a line a minute in the output log

    public const int Surfaces = 28;    // 0: the wall; 1 to 12: the classroom; 13 to 20: the holodecks; 21 to 23: the side rooms
    public const int HeaderBytes = 19;
    public const int StatusFixed = 130;   // a status's activity bits, pc and x1 to x31; its text follows

    private int count;
    private int[] wants = new int[Surfaces];   // the sender each surface shows; 0: none
    private int[] sender;              // a slot's; 0: free
    private Color32[][] cells;
    private int[] lastSeq, gotW, gotH, shownW, shownH, top, cursorX, cursorY, version, mipsAll, mipsCore, askCount, activity;
    private uint[][] registers;
    private string[] stats;
    private bool[] haveSeq, on, fresh, drawn;
    private byte[] unitsBytes = new byte[1152];
    private float[] since, askedAt, heardAt;
    private int cols, rows;
    private float nextAt, logAt;
    private bool started;
    private int packets, gaps, repairs, refreshes;

    private void Init()
    {
        if (started) return;
        started = true;
        count = pictures.Length;
        sender = new int[count];
        cells = new Color32[count][];
        lastSeq = new int[count];
        gotW = new int[count];
        gotH = new int[count];
        shownW = new int[count];
        shownH = new int[count];
        top = new int[count];
        cursorX = new int[count];
        cursorY = new int[count];
        version = new int[count];
        mipsAll = new int[count];
        mipsCore = new int[count];
        askCount = new int[count];
        activity = new int[count];
        registers = new uint[count][];
        stats = new string[count];
        haveSeq = new bool[count];
        fresh = new bool[count];
        drawn = new bool[count];
        on = new bool[count];
        since = new float[count];
        askedAt = new float[count];
        heardAt = new float[count];
        cols = terminal.cols;
        rows = terminal.rows;
        for (int s = 0; s < count; s++)
        {
            cells[s] = new Color32[cols * rows];
            registers[s] = new uint[32];
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

    // A sender some surface wants and no slot can take: every slot holds another that is shown
    // (16 slots, 24 surfaces). Its screen stays black, and the place says why.
    public bool NoRoom(int id)
    {
        Init();
        if (id <= 0 || SlotOf(id) >= 0 || !hub.Known(id)) return false;
        for (int s = 0; s < count; s++)
            if (sender[s] == 0 || !Shown(sender[s])) return false;
        return true;
    }

    public Texture Picture(int slot) { return pictures[slot]; }
    public Texture Console(int slot) { return consoles[slot]; }
    public Color32[] Cells(int slot) { return cells[slot]; }
    public int Width(int slot) { return on[slot] ? gotW[slot] : 0; }      // of the picture drawn
    public int Height(int slot) { return on[slot] ? gotH[slot] : 0; }
    public int DisplayWidth(int slot) { return on[slot] && gotW[slot] > 0 ? shownW[slot] : 0; }   // of the sender's display
    public int DisplayHeight(int slot) { return on[slot] && gotW[slot] > 0 ? shownH[slot] : 0; }
    public int Top(int slot) { return top[slot]; }
    public int CursorX(int slot) { return cursorX[slot]; }
    public int CursorY(int slot) { return cursorY[slot]; }
    public int Version(int slot) { return version[slot]; }   // counted up with every packet taken
    public float MipsAll(int slot) { return Speed(mipsAll[slot]); }   // tenths of a million instructions a second
    public float MipsCore(int slot) { return Speed(mipsCore[slot]); }
    // (as EmuShareHub.PackSpeed sent it: four digits, and from bit 14 how many come after the point)
    private float Speed(int packed)
    {
        int after = packed >> 14;
        return (packed & 0x3fff) * (after == 3 ? 0.01f : after == 2 ? 0.1f : after == 1 ? 1f : 10f);
    }
    public int Activity(int slot) { return on[slot] ? activity[slot] : 0; }   // EmuMachine.activity, as last sent
    public uint[] Registers(int slot) { return registers[slot]; }
    public string Stats(int slot) { return on[slot] ? stats[slot] : "off"; }
    public int Gaps() { return gaps; }
    public int Packets() { return packets; }

    // ---- slots ----

    // Nothing of a sender is held or shown.
    private void Blank(int s)
    {
        Color32 blank = new Color32(32, 7, 0, 255);
        Color32[] grid = cells[s];
        for (int i = 0; i < grid.Length; i++) grid[i] = blank;
        gotW[s] = 0;
        gotH[s] = 0;
        shownW[s] = 0;
        shownH[s] = 0;
        mipsAll[s] = 0;
        mipsCore[s] = 0;
        activity[s] = 0;
        stats[s] = "";
        on[s] = false;
        haveSeq[s] = false;
        drawn[s] = false;
        version[s]++;
        if (blackTexture != null)
        {
            VRCGraphics.Blit(blackTexture, pictures[s]);
            VRCGraphics.Blit(blackTexture, consoles[s]);
        }
    }

    private bool Shown(int id)
    {
        for (int i = 0; i < Surfaces; i++)
            if (wants[i] == id) return true;
        return false;
    }

    // A free slot becomes this sender's; false when there is none.
    private bool Seat(int id)
    {
        for (int s = 0; s < count; s++)
        {
            if (sender[s] != 0) continue;
            sender[s] = id;
            since[s] = Time.time;
            askedAt[s] = Time.time;
            heardAt[s] = Time.time;
            askCount[s] = (askCount[s] + 1) & 255;   // a new entry: its sender sends everything
            fresh[s] = true;
            return true;
        }
        return false;
    }

    // Every sender some surface wants has a slot, and so has every other player whose machine
    // is on while slots last; a slot whose sender left, or is off and not shown, is emptied.
    private void Reconcile()
    {
        bool changed = false;
        for (int s = 0; s < count; s++)
        {
            if (sender[s] == 0) continue;
            if (hub.Known(sender[s]) && (Shown(sender[s]) || hub.IsOn(sender[s]))) continue;
            sender[s] = 0;
            Blank(s);
            changed = true;
        }
        for (int i = 0; i < Surfaces; i++)
        {
            int id = wants[i];
            if (id <= 0 || SlotOf(id) >= 0 || !hub.Known(id)) continue;
            if (!Seat(id))
            {
                // full of players nobody looks at: one of them makes room
                for (int s = 0; s < count; s++)
                {
                    if (Shown(sender[s])) continue;
                    sender[s] = 0;
                    Blank(s);
                    break;
                }
                if (!Seat(id)) continue;
            }
            changed = true;
        }
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if ((share.flags & 1) == 0 || SlotOf(share.ownerId) >= 0) continue;
            if (!Seat(share.ownerId)) break;
            changed = true;
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
        if (hub.netTest) Debug.Log("[ShareLag] got seq=" + share.seq + " ms=" + Networking.GetServerTimeInMilliseconds());

        int w = p[4] | p[5] << 8, h = p[6] | p[7] << 8;
        if (w != gotW[s] || h != gotH[s])
        {
            // another size: its units come from nothing
            gotW[s] = w;
            gotH[s] = h;
            if (blackTexture != null) VRCGraphics.Blit(blackTexture, pictures[s]);
        }
        bool moved = top[s] != p[1] || cursorX[s] != p[2] || cursorY[s] != p[3] || !drawn[s];
        drawn[s] = true;
        on[s] = (p[0] & 1) != 0;
        top[s] = p[1];
        cursorX[s] = p[2];
        cursorY[s] = p[3];
        shownW[s] = p[8] | p[9] << 8;
        shownH[s] = p[10] | p[11] << 8;
        mipsAll[s] = p[13] | p[14] << 8;
        mipsCore[s] = p[15] | p[16] << 8;
        version[s]++;

        int at = HeaderBytes, sent = p[12], status = p[17] | p[18] << 8;
        if (at + status > p.Length) return;
        if (status >= StatusFixed)
        {
            // what its owner's panel says of the machine (EmuShareHub.Build)
            activity[s] = p[at] | p[at + 1] << 8;
            uint[] r = registers[s];
            for (int i = 0; i < 32; i++)
            {
                int k = at + 2 + i * 4;
                r[i] = (uint)p[k] | (uint)p[k + 1] << 8 | (uint)p[k + 2] << 16 | (uint)p[k + 3] << 24;
            }
            string text = "";
            for (int i = StatusFixed; i < status; i++) text += (char)p[at + i];
            stats[s] = text;
        }
        at += status;
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
        if (sent > 0 || moved)
        {
            grids[s].SetPixels32(grid);
            grids[s].Apply(false);
            consoleMaterial.SetTexture("_Grid", grids[s]);
            consoleMaterial.SetInt("_RowOffset", top[s]);
            consoleMaterial.SetVector("_Cursor", new Vector4(cursorX[s], cursorY[s], 1, 0));
            VRCGraphics.Blit(grids[s], consoles[s], consoleMaterial);
        }

        // the rest is units: the GPU finds each in the bytes and draws the pixels it covers
        int bytes = Mathf.Min(p.Length - at, unitsBytes.Length);
        if (bytes <= 0 || unitsMaterial == null || w <= 0) return;
        System.Buffer.BlockCopy(p, at, unitsBytes, 0, bytes);
        unitsPacket.LoadRawTextureData(unitsBytes);
        unitsPacket.Apply(false);
        unitsMaterial.SetTexture("_Packet", unitsPacket);
        unitsMaterial.SetFloat("_Length", bytes);
        unitsMaterial.SetVector("_Stream", new Vector4(w, h, 0, 0));
        unitsMaterial.SetVector("_TargetSize", new Vector4(pictures[s].width, pictures[s].height, 0, 0));
        VRCGraphics.Blit(unitsPacket, unitsIndex, unitsMaterial, 0);
        unitsMaterial.SetTexture("_Index", unitsIndex);
        VRCGraphics.Blit(unitsPacket, pictures[s], unitsMaterial, 1);
    }
}
