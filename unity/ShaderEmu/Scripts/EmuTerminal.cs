using UdonSharp;
using UnityEngine;

// The console's screen: a character grid kept here and shown by Terminal.shader.
// Understands the control characters and the few ANSI sequences a Linux console uses.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuTerminal : UdonSharpBehaviour
{
    public Texture2D gridTexture;     // cols x rows, readable, one texel per cell
    public Material screenMaterial;
    public int cols = 80;
    public int rows = 30;
    public float flushInterval = 0.05f;

    private Color32[] grid;
    private int top;                  // grid row shown as the first line (rows are a ring)
    private int cx, cy;
    private int fg = 7, bg = 0;
    private bool bold;
    private bool dirty = true;
    private float nextFlush;

    private int escState;             // 0 text, 1 after ESC, 2 inside ESC [
    private int[] escArgs = new int[8];
    private int escCount;
    private bool escHasDigits;

    [HideInInspector] public bool[] rowChanged;   // grid rows written since EmuShareHub last sent them
    private bool remote;              // another player's screen is shown, not this one

    void Start()
    {
        Init();
    }

    private void Init()
    {
        if (grid != null) return;
        grid = new Color32[cols * rows];
        rowChanged = new bool[rows];
        Clear();
    }

    public void Clear()
    {
        Init();
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < grid.Length; i++) grid[i] = blank;
        MarkAll();
        top = 0;
        cx = 0;
        cy = 0;
        fg = 7;
        bg = 0;
        bold = false;
        escState = 0;
        dirty = true;
    }

    private void ClearCells(int row, int from, int to)
    {
        Color32 blank = new Color32(32, (byte)fg, (byte)bg, 255);
        int at = ((row + top) % rows) * cols;
        for (int x = from; x < to; x++) grid[at + x] = blank;
        rowChanged[(row + top) % rows] = true;
    }

    private void LineFeed()
    {
        cy++;
        if (cy < rows) return;
        cy = rows - 1;
        top = (top + 1) % rows;   // the old first line becomes the new last one
        ClearCells(cy, 0, cols);
    }

    public void PutChar(int c)
    {
        Init();
        dirty = true;
        if (escState == 1)
        {
            if (c == '[')
            {
                escState = 2;
                escCount = 0;
                escHasDigits = false;
                for (int i = 0; i < escArgs.Length; i++) escArgs[i] = 0;
            }
            else escState = 0;
            return;
        }
        if (escState == 2)
        {
            if (c >= '0' && c <= '9')
            {
                if (escCount < escArgs.Length) escArgs[escCount] = escArgs[escCount] * 10 + (c - '0');
                escHasDigits = true;
            }
            else if (c == ';')
            {
                if (escCount < escArgs.Length) escCount++;
                escHasDigits = false;
            }
            else if (c >= 0x40 && c <= 0x7e)
            {
                if (escHasDigits && escCount < escArgs.Length) escCount++;
                Control(c);
                escState = 0;
            }
            // anything else (?, spaces) belongs to the sequence and is ignored
            return;
        }
        if (c == 27) { escState = 1; return; }
        if (c == '\r') { cx = 0; return; }
        if (c == '\n') { cx = 0; LineFeed(); return; }
        if (c == 8) { if (cx > 0) cx--; return; }
        if (c == 9) { cx = (cx + 8) & ~7; if (cx >= cols) cx = cols - 1; return; }
        if (c < 32 || c > 126) return;
        if (cx >= cols) { cx = 0; LineFeed(); }
        rowChanged[(cy + top) % rows] = true;
        grid[((cy + top) % rows) * cols + cx] = new Color32((byte)c, (byte)(bold && fg < 8 ? fg + 8 : fg), (byte)bg, 255);
        cx++;
    }

    private int Arg(int i, int fallback)
    {
        return i < escCount && escArgs[i] != 0 ? escArgs[i] : fallback;
    }

    private void Control(int final)
    {
        if (final == 'm')
        {
            if (escCount == 0) { fg = 7; bg = 0; bold = false; }
            for (int i = 0; i < escCount; i++)
            {
                int a = escArgs[i];
                if (a == 0) { fg = 7; bg = 0; bold = false; }
                else if (a == 1) bold = true;
                else if (a == 22) bold = false;
                else if (a == 7) { int t = fg; fg = bg; bg = t; }
                else if (a >= 30 && a <= 37) fg = a - 30;
                else if (a == 39) fg = 7;
                else if (a >= 40 && a <= 47) bg = a - 40;
                else if (a == 49) bg = 0;
                else if (a >= 90 && a <= 97) fg = a - 90 + 8;
                else if (a >= 100 && a <= 107) bg = a - 100 + 8;
            }
        }
        else if (final == 'H' || final == 'f')
        {
            cy = Mathf.Clamp(Arg(0, 1) - 1, 0, rows - 1);
            cx = Mathf.Clamp(Arg(1, 1) - 1, 0, cols - 1);
        }
        else if (final == 'A') cy = Mathf.Max(cy - Arg(0, 1), 0);
        else if (final == 'B') cy = Mathf.Min(cy + Arg(0, 1), rows - 1);
        else if (final == 'C') cx = Mathf.Min(cx + Arg(0, 1), cols - 1);
        else if (final == 'D') cx = Mathf.Max(cx - Arg(0, 1), 0);
        else if (final == 'G') cx = Mathf.Clamp(Arg(0, 1) - 1, 0, cols - 1);
        else if (final == 'd') cy = Mathf.Clamp(Arg(0, 1) - 1, 0, rows - 1);
        else if (final == 'K')
        {
            int how = escCount > 0 ? escArgs[0] : 0;
            int x = Mathf.Min(cx, cols - 1);
            if (how == 0) ClearCells(cy, x, cols);
            else if (how == 1) ClearCells(cy, 0, x + 1);
            else ClearCells(cy, 0, cols);
        }
        else if (final == 'J')
        {
            int how = escCount > 0 ? escArgs[0] : 0;
            int x = Mathf.Min(cx, cols - 1);
            if (how == 0)
            {
                ClearCells(cy, x, cols);
                for (int r = cy + 1; r < rows; r++) ClearCells(r, 0, cols);
            }
            else if (how == 1)
            {
                for (int r = 0; r < cy; r++) ClearCells(r, 0, cols);
                ClearCells(cy, 0, x + 1);
            }
            else
            {
                for (int r = 0; r < rows; r++) ClearCells(r, 0, cols);
            }
        }
    }

    // The text of screen line `row`, for scripts and tests.
    public string LineText(int row)
    {
        Init();
        char[] line = new char[cols];
        int at = ((row + top) % rows) * cols;
        for (int x = 0; x < cols; x++) line[x] = (char)grid[at + x].r;
        return new string(line);
    }

    public int CursorRow() { return cy; }
    public int CursorColumn() { return Mathf.Min(cx, cols - 1); }
    public int Top() { return top; }

    // ---- for EmuShareHub ----

    public void MarkAll()
    {
        Init();
        for (int r = 0; r < rows; r++) rowChanged[r] = true;
    }

    // Grid row r as bytes: row, length without the blanks at its end, characters, then 0, or
    // 1 and a byte of colours a cell. Returns where the next row goes; at most 3 + 2 * cols.
    public int PackRow(int r, byte[] into, int at)
    {
        int from = r * cols, length = cols;
        while (length > 0)
        {
            Color32 c = grid[from + length - 1];
            if (c.r != 32 || c.g != 7 || c.b != 0) break;
            length--;
        }
        into[at++] = (byte)r;
        into[at++] = (byte)length;
        bool plain = true;
        for (int x = 0; x < length; x++)
        {
            Color32 c = grid[from + x];
            into[at + x] = c.r;
            if (c.g != 7 || c.b != 0) plain = false;
        }
        at += length;
        into[at++] = (byte)(plain ? 0 : 1);
        if (plain) return at;
        for (int x = 0; x < length; x++)
        {
            Color32 c = grid[from + x];
            into[at + x] = (byte)(c.g | c.b << 4);
        }
        return at + length;
    }

    // Another player's screen, in this one's place until ShowLocal.
    public void ShowRemote(Color32[] cells, int first, int x, int y)
    {
        remote = true;
        gridTexture.SetPixels32(cells);
        gridTexture.Apply(false);
        screenMaterial.SetInt("_RowOffset", first);
        screenMaterial.SetVector("_Cursor", new Vector4(x, y, 1, 0));
    }

    public void ShowLocal()
    {
        remote = false;
        dirty = true;
        nextFlush = 0f;
    }

    void Update()
    {
        if (remote || !dirty || Time.time < nextFlush) return;
        dirty = false;
        nextFlush = Time.time + flushInterval;
        gridTexture.SetPixels32(grid);
        gridTexture.Apply(false);
        screenMaterial.SetInt("_RowOffset", top);
        screenMaterial.SetVector("_Cursor", new Vector4(Mathf.Min(cx, cols - 1), cy, 1, 0));
    }
}
