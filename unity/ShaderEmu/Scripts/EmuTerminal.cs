using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;

// The console's screen: character grids kept here and shown by Terminal.shader. Four
// terminals (docs/console.md), one of them shown, each with the lines that scrolled off it.
// Understands the control characters and the few ANSI sequences a Linux console uses.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuTerminal : UdonSharpBehaviour
{
    public Texture2D gridTexture;     // cols x rows, readable, one texel per cell
    public Material screenMaterial;
    public int cols = 80;
    public int rows = 30;
    public float flushInterval = 0.05f;

    public EmuKeyboard keyboard;      // the console's: choosing a terminal is typed on it
    public Image[] tabPlates;         // the buttons above the screen, one a terminal
    public Color tabColour, tabShownColour, tabNewsColour;
    public TextMeshProUGUI scrollLabel;

    public const int Tabs = 4;
    private const int Switch = 0x1e;  // in the console's stream: the next byte says whose bytes follow
    private const int Pages = 10;     // a terminal keeps this many screens of lines

    private int kept;                 // rows a terminal keeps: Pages screens, a ring
    private Color32[] grid;           // Tabs x kept x cols
    private Color32[] view;           // what is on the screen now: rows x cols
    private int[] top;                // per terminal: the kept row that is its screen's first line
    private int[] cx, cy, fg, bg;
    private bool[] bold;
    private int[] escState;           // 0 text, 1 after ESC, 2 inside ESC [
    private int[] escArgs;            // 8 a terminal
    private int[] escCount;
    private bool[] escHasDigits;
    private int[] past;               // lines that have scrolled off its top and are still kept
    private int[] back;               // how many of those the view is scrolled back by
    private bool[] news;              // it printed something while another was shown

    private int writing;              // the terminal the stream's bytes belong to
    private int shown;
    private bool switching;           // the stream's last byte was Switch
    private bool dirty = true;
    private float nextFlush;

    [HideInInspector] public bool[] rowChanged;   // screen rows written since EmuShareHub last sent them (of the shown terminal)
    private bool remote;              // another player's screen is shown, not this one

    void Start()
    {
        Init();
    }

    private void Init()
    {
        if (grid != null) return;
        kept = rows * Pages;
        grid = new Color32[Tabs * kept * cols];
        view = new Color32[rows * cols];
        top = new int[Tabs];
        cx = new int[Tabs];
        cy = new int[Tabs];
        fg = new int[Tabs];
        bg = new int[Tabs];
        bold = new bool[Tabs];
        escState = new int[Tabs];
        escArgs = new int[Tabs * 8];
        escCount = new int[Tabs];
        escHasDigits = new bool[Tabs];
        past = new int[Tabs];
        back = new int[Tabs];
        news = new bool[Tabs];
        rowChanged = new bool[rows];
        Clear();
    }

    public void Clear()
    {
        Init();
        Color32 blank = new Color32(32, 7, 0, 255);
        for (int i = 0; i < grid.Length; i++) grid[i] = blank;
        for (int t = 0; t < Tabs; t++)
        {
            top[t] = 0;
            cx[t] = 0;
            cy[t] = 0;
            bg[t] = 0;
            escState[t] = 0;
            past[t] = 0;
            back[t] = 0;
            fg[t] = 7;
            bold[t] = false;
            news[t] = false;
        }
        writing = 0;
        shown = 0;
        switching = false;
        MarkAll();
        dirty = true;
        ShowTabs();
    }

    // Where screen line `row` of terminal t is in the grid.
    private int At(int t, int row)
    {
        return (t * kept + (row + top[t]) % kept) * cols;
    }

    private void Touched(int t, int row)
    {
        if (t == shown) rowChanged[(row + top[t]) % rows] = true;   // (kept is whole screens: these are distinct)
    }

    private void ClearCells(int t, int row, int from, int to)
    {
        Color32 blank = new Color32(32, (byte)fg[t], (byte)bg[t], 255);
        int at = At(t, row);
        for (int x = from; x < to; x++) grid[at + x] = blank;
        Touched(t, row);
    }

    private void LineFeed(int t)
    {
        cy[t]++;
        if (cy[t] < rows) return;
        cy[t] = rows - 1;
        top[t] = (top[t] + 1) % kept;   // the first line goes into the past; the oldest kept line is the new last one
        if (past[t] < kept - rows) past[t]++;
        if (back[t] > 0 && back[t] < past[t]) back[t]++;   // a view scrolled back stays on its lines
        ClearCells(t, cy[t], 0, cols);
    }

    public void PutChar(int c)
    {
        Init();
        if (switching)
        {
            switching = false;
            if (c >= '0' && c < '0' + Tabs)
            {
                writing = c - '0';
                return;
            }
            if (c != Switch) return;
        }
        else if (c == Switch)
        {
            switching = true;
            return;
        }
        int t = writing;
        if (t == shown) dirty = true;
        else if (!news[t])
        {
            news[t] = true;
            ShowTabs();
        }
        if (escState[t] == 1)
        {
            if (c == '[')
            {
                escState[t] = 2;
                escCount[t] = 0;
                escHasDigits[t] = false;
                for (int i = 0; i < 8; i++) escArgs[t * 8 + i] = 0;
            }
            else escState[t] = 0;
            return;
        }
        if (escState[t] == 2)
        {
            if (c >= '0' && c <= '9')
            {
                if (escCount[t] < 8) escArgs[t * 8 + escCount[t]] = escArgs[t * 8 + escCount[t]] * 10 + (c - '0');
                escHasDigits[t] = true;
            }
            else if (c == ';')
            {
                if (escCount[t] < 8) escCount[t]++;
                escHasDigits[t] = false;
            }
            else if (c >= 0x40 && c <= 0x7e)
            {
                if (escHasDigits[t] && escCount[t] < 8) escCount[t]++;
                Control(t, c);
                escState[t] = 0;
            }
            // anything else (?, spaces) belongs to the sequence and is ignored
            return;
        }
        if (c == 27) { escState[t] = 1; return; }
        if (c == '\r') { cx[t] = 0; return; }
        if (c == '\n') { cx[t] = 0; LineFeed(t); return; }
        if (c == 8) { if (cx[t] > 0) cx[t]--; return; }
        if (c == 9) { cx[t] = (cx[t] + 8) & ~7; if (cx[t] >= cols) cx[t] = cols - 1; return; }
        if (c < 32 || c > 126) return;
        if (cx[t] >= cols) { cx[t] = 0; LineFeed(t); }
        Touched(t, cy[t]);
        grid[At(t, cy[t]) + cx[t]] = new Color32((byte)c, (byte)(bold[t] && fg[t] < 8 ? fg[t] + 8 : fg[t]), (byte)bg[t], 255);
        cx[t]++;
    }

    private int Arg(int t, int i, int fallback)
    {
        return i < escCount[t] && escArgs[t * 8 + i] != 0 ? escArgs[t * 8 + i] : fallback;
    }

    private void Control(int t, int final)
    {
        if (final == 'm')
        {
            if (escCount[t] == 0) { fg[t] = 7; bg[t] = 0; bold[t] = false; }
            for (int i = 0; i < escCount[t]; i++)
            {
                int a = escArgs[t * 8 + i];
                if (a == 0) { fg[t] = 7; bg[t] = 0; bold[t] = false; }
                else if (a == 1) bold[t] = true;
                else if (a == 22) bold[t] = false;
                else if (a == 7) { int swap = fg[t]; fg[t] = bg[t]; bg[t] = swap; }
                else if (a >= 30 && a <= 37) fg[t] = a - 30;
                else if (a == 39) fg[t] = 7;
                else if (a >= 40 && a <= 47) bg[t] = a - 40;
                else if (a == 49) bg[t] = 0;
                else if (a >= 90 && a <= 97) fg[t] = a - 90 + 8;
                else if (a >= 100 && a <= 107) bg[t] = a - 100 + 8;
            }
        }
        else if (final == 'H' || final == 'f')
        {
            cy[t] = Mathf.Clamp(Arg(t, 0, 1) - 1, 0, rows - 1);
            cx[t] = Mathf.Clamp(Arg(t, 1, 1) - 1, 0, cols - 1);
        }
        else if (final == 'A') cy[t] = Mathf.Max(cy[t] - Arg(t, 0, 1), 0);
        else if (final == 'B') cy[t] = Mathf.Min(cy[t] + Arg(t, 0, 1), rows - 1);
        else if (final == 'C') cx[t] = Mathf.Min(cx[t] + Arg(t, 0, 1), cols - 1);
        else if (final == 'D') cx[t] = Mathf.Max(cx[t] - Arg(t, 0, 1), 0);
        else if (final == 'G') cx[t] = Mathf.Clamp(Arg(t, 0, 1) - 1, 0, cols - 1);
        else if (final == 'd') cy[t] = Mathf.Clamp(Arg(t, 0, 1) - 1, 0, rows - 1);
        else if (final == 'K')
        {
            int how = escCount[t] > 0 ? escArgs[t * 8] : 0;
            int x = Mathf.Min(cx[t], cols - 1);
            if (how == 0) ClearCells(t, cy[t], x, cols);
            else if (how == 1) ClearCells(t, cy[t], 0, x + 1);
            else ClearCells(t, cy[t], 0, cols);
        }
        else if (final == 'J')
        {
            int how = escCount[t] > 0 ? escArgs[t * 8] : 0;
            int x = Mathf.Min(cx[t], cols - 1);
            if (how == 0)
            {
                ClearCells(t, cy[t], x, cols);
                for (int r = cy[t] + 1; r < rows; r++) ClearCells(t, r, 0, cols);
            }
            else if (how == 1)
            {
                for (int r = 0; r < cy[t]; r++) ClearCells(t, r, 0, cols);
                ClearCells(t, cy[t], 0, x + 1);
            }
            else
            {
                for (int r = 0; r < rows; r++) ClearCells(t, r, 0, cols);
            }
        }
    }

    // ---- which terminal, and how far back (the buttons above the screen) ----

    public void ShowTab(int t)
    {
        Init();
        if (t < 0 || t >= Tabs) return;
        shown = t;
        news[t] = false;
        MarkAll();
        dirty = true;
        nextFlush = 0f;
        ShowTabs();
        // what is typed from now on is this terminal's: said to the guest on the console itself
        if (keyboard != null)
        {
            keyboard.PushEvent(Switch);
            keyboard.PushEvent('0' + t);
        }
    }

    public void Tab0() { ShowTab(0); }
    public void Tab1() { ShowTab(1); }
    public void Tab2() { ShowTab(2); }
    public void Tab3() { ShowTab(3); }

    private void Scroll(int lines)
    {
        Init();
        back[shown] = Mathf.Clamp(back[shown] + lines, 0, past[shown]);
        dirty = true;
        nextFlush = 0f;
        ShowTabs();
    }

    public void ScrollUp() { Scroll(rows / 2); }
    public void ScrollDown() { Scroll(-rows / 2); }
    public void ScrollEnd() { Scroll(-kept); }

    public int Shown() { return shown; }
    public int ScrolledBack() { Init(); return back[shown]; }

    private void ShowTabs()
    {
        if (tabPlates != null)
            for (int t = 0; t < tabPlates.Length && t < Tabs; t++)
                if (tabPlates[t] != null) tabPlates[t].color = t == shown ? tabShownColour : news[t] ? tabNewsColour : tabColour;
        if (scrollLabel != null) scrollLabel.text = back[shown] > 0 ? back[shown] + " lines back" : "";
    }

    // The text of screen line `row` of the terminal shown, for scripts and tests.
    public string LineText(int row)
    {
        Init();
        char[] line = new char[cols];
        int at = At(shown, row);
        for (int x = 0; x < cols; x++) line[x] = (char)grid[at + x].r;
        return new string(line);
    }

    public int CursorRow() { Init(); return cy[shown]; }
    public int CursorColumn() { Init(); return Mathf.Min(cx[shown], cols - 1); }
    public int Top() { Init(); return top[shown] % rows; }

    // ---- for EmuShareHub: the shown terminal's screen as a ring of `rows` rows, first row Top() ----

    public void MarkAll()
    {
        Init();
        for (int r = 0; r < rows; r++) rowChanged[r] = true;
    }

    // Ring row r as bytes: row, length without the blanks at its end, characters, then 0, or
    // 1 and a byte of colours a cell. Returns where the next row goes; at most 3 + 2 * cols.
    public int PackRow(int r, byte[] into, int at)
    {
        int from = At(shown, (r - top[shown] % rows + rows) % rows), length = cols;
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
        // the screen's lines in order, from as far back as the view is scrolled
        int first = top[shown] - back[shown] + kept;
        for (int r = 0; r < rows; r++)
            System.Array.Copy(grid, (shown * kept + (first + r) % kept) * cols, view, r * cols, cols);
        gridTexture.SetPixels32(view);
        gridTexture.Apply(false);
        screenMaterial.SetInt("_RowOffset", 0);
        screenMaterial.SetVector("_Cursor", new Vector4(Mathf.Min(cx[shown], cols - 1), cy[shown], back[shown] > 0 ? 0 : 1, 0));
    }
}
