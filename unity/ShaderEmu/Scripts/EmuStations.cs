using UdonSharp;
using UnityEngine;
using TMPro;
using VRC.SDKBase;

// The classroom's computers (docs/stations.md): eight tubes, one a visitor. A visitor's place is
// a number in their own EmuShare, so nobody can move anybody else; whoever holds a place sends
// their display (EmuShareHub does, as for a watcher) and every client draws it on that tube.
// A visitor's own tube shows their machine itself.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuStations : UdonSharpBehaviour
{
    public EmuShareHub hub;
    public EmuMachine machine;
    public EmuTerminal terminal;       // this visitor's own console
    public Material[] screens;         // CRT.shader, one a place
    public Texture2D[] stores;         // the tiles received for each, laid out as the hub's
    public RenderTexture[] pictures;   // and decoded
    public Texture2D[] grids;          // a place's console, a texel a cell
    public RenderTexture[] consoles;   // and drawn (640 x 480)
    public Material consoleMaterial;   // Terminal.shader, this behaviour's own
    public Material decodeMaterial;    // ShareDecode.shader, this behaviour's own
    public TextMeshProUGUI[] labels;   // whose it is

    // as EmuShareHub's
    private const int TileCount = 300, RowBytes = 512, LevelAt = 508, StampAt = 509;
    private const int FineAt = 300, FineRows = 1200, FineMost = 400, PresentAt = 2, FineStampAt = 3, HeaderBytes = 13;

    private int count;
    private byte[][] bytes;
    private int[] owner, lastSeq, gotW, gotH, gotScale, stamp, claimed, mode, sizeW, sizeH;
    private Color32[][] cells;
    private int cols, rows;
    private bool[] haveSeq;
    private float[] askedAt;
    private float nextAt;

    void Start()
    {
        count = screens.Length;
        bytes = new byte[count][];
        owner = new int[count];
        claimed = new int[count];
        lastSeq = new int[count];
        gotW = new int[count];
        gotH = new int[count];
        gotScale = new int[count];
        stamp = new int[count];
        haveSeq = new bool[count];
        askedAt = new float[count];
        mode = new int[count];
        sizeW = new int[count];
        sizeH = new int[count];
        cells = new Color32[count][];
        cols = terminal.cols;
        rows = terminal.rows;
        for (int s = 0; s < count; s++)
        {
            bytes[s] = new byte[(TileCount + FineRows) * RowBytes];
            cells[s] = new Color32[cols * rows];
            owner[s] = -1;   // not looked at yet
        }
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

    // Whose each place is: of those who name it, the visitor who came first.
    private void Assign()
    {
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
        for (int s = 0; s < count; s++)
        {
            int wants = claimed[s] == myId ? mine.stationMode : claimed[s] > 0 ? ShareOf(claimed[s]).stationMode : 0;
            if (claimed[s] != owner[s])
            {
                owner[s] = claimed[s];
                haveSeq[s] = false;
                Forget(s);
                mode[s] = -1;
                if (labels[s] != null) labels[s].text = owner[s] == 0 ? "FREE" : owner[s] == myId ? Networking.LocalPlayer.displayName : NameOf(owner[s]);
            }
            if (owner[s] == myId)
            {
                sizeW[s] = machine.OwnWidth();
                sizeH[s] = machine.OwnHeight();
                // this visitor's own console, as their terminal has it now
                if (wants == 1) VRCGraphics.Blit(terminal.gridTexture, consoles[s], terminal.screenMaterial);
            }
            Material screen = screens[s];
            if (wants != mode[s])
            {
                mode[s] = wants;
                Texture shown = wants == 1 ? (Texture)consoles[s] : owner[s] == myId ? (Texture)machine.displayTexture : (Texture)pictures[s];
                screen.SetTexture("_MainTex", shown);
                screen.SetVector("_TexSize", new Vector4(shown.width, shown.height, 0, 0));
            }
            // a console is always there to show; a display only while its machine is on
            bool console = wants == 1 && owner[s] > 0;
            screen.SetVector("_Size", new Vector4(console ? consoles[s].width : sizeW[s], console ? consoles[s].height : sizeH[s], 0, 0));
        }
    }

    private EmuShare ShareOf(int id)
    {
        int others = hub.ShareCount();
        for (int i = 0; i < others; i++)
        {
            EmuShare share = hub.ShareAt(i);
            if (share.ownerId == id) return share;
        }
        return hub.Mine();
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

    private void Forget(int s)
    {
        byte[] b = bytes[s];
        for (int t = 0; t < TileCount; t++) b[t * RowBytes + LevelAt] = 3;
        for (int r = 0; r < FineRows; r++) b[(FineAt + r) * RowBytes + PresentAt] = 0;
        gotW[s] = 0;
        sizeW[s] = 0;
        sizeH[s] = 0;
    }

    // Asks the place's owner for everything again: this client is new to it, or lost a packet.
    private void Ask(int s, EmuShare share)
    {
        EmuShare mine = hub.Mine();
        if (mine == null || Time.time - askedAt[s] < 3f) return;
        askedAt[s] = Time.time;
        mine.askFrom = share.ownerId;
        mine.askCount++;
        hub.StateOut();
    }

    // A packet of somebody's: if a place is theirs, its tiles go into that place's store.
    public void Received(EmuShare share)
    {
        int s = share.station;
        if (bytes == null || s < 0 || s >= count || owner[s] != share.ownerId) return;
        byte[] p = share.packet;
        if (p == null || p.Length < HeaderBytes) return;
        if (haveSeq[s] && share.seq == lastSeq[s]) return;
        if (!haveSeq[s] || share.seq != lastSeq[s] + 1) Ask(s, share);
        haveSeq[s] = true;
        lastSeq[s] = share.seq;

        byte[] b = bytes[s];
        bool on = p[0] != 0, redraw = false, all = false;
        int w = p[4] | p[5] << 8, h = p[6] | p[7] << 8;
        int scale = w > 0 && (p[8] | p[9] << 8) / w == 2 ? 2 : 1;
        if (w != gotW[s] || h != gotH[s] || scale != gotScale[s])
        {
            Forget(s);
            gotW[s] = w;
            gotH[s] = h;
            gotScale[s] = scale;
            all = true;
        }
        stamp[s] = stamp[s] % 255 + 1;
        // the console's rows, into this place's own grid
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
        if (sent > 0 || all)
        {
            grids[s].SetPixels32(grid);
            grids[s].Apply(false);
            consoleMaterial.SetTexture("_Grid", grids[s]);
            consoleMaterial.SetInt("_RowOffset", p[1]);
            consoleMaterial.SetVector("_Cursor", new Vector4(p[2], p[3], 1, 0));
            VRCGraphics.Blit(grids[s], consoles[s], consoleMaterial);
        }
        while (at + 2 <= p.Length)
        {
            int v = p[at] | p[at + 1] << 8, t = v & 0xfff, level = v >> 12;
            if (t >= TileCount) break;
            if (level >= 4)
            {
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
        sizeW[s] = on ? w * scale : 0;
        sizeH[s] = on ? h * scale : 0;
    }
}
