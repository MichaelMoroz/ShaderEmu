using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// The text field in front of the screen (docs/system/clipboard.md): what the guest shows a visitor to copy
// (nxexport's base64 text, the machine's clipboard) and where a visitor pastes text for the machine. EmuMachine
// answers the guest's "clip:" addresses from here:
//   clip:show  the guest's bytes (a line of 256, then the text) read out of the fetch's memory and shown, in pages
//   clip:set   the guest's clipboard kept, shown when the Clipboard key is pressed
//   clip:ask   the field shown empty, for a paste; "To the machine" keeps what is in it
//   clip:take  what was kept, once (404 until there is something)
//   clip:hide  the field put away
// and its "photo:" addresses (docs/apps/export.md, "A photo"):
//   photo:show=TEXEL,ENTRIES,TAIL,NAME  a zip in RAM drawn into VRChat's camera's pictures while the camera is in the
//                                       cube before the panel (Photo.shader), the panel saying how; photo:hide
// The Clipboard key of the wall's computer and of a classroom place shows the machine's clipboard; its "To the
// machine" puts the field's text on the machine's clipboard (the bar runs nxexport --paste for "clip:paste").
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuClip : UdonSharpBehaviour
{
    public EmuMachine machine;
    public EmuStations stations;          // a visitor at their own classroom place has the field before its tube
    public Material readMaterial;         // ShaderEmu/PersistRead
    public RenderTexture readTexture;     // 256 x 256: a pixel a word of the fetch's 256 KB
    public GameObject panel;
    public InputField field;
    public TextMeshProUGUI titleLabel, pageLabel, giveLabel;
    public GameObject giveButton, backButton, onButton;
    public MeshRenderer photo;            // the cube before the panel: PhotoCube.shader, then Photo.shader (its second submesh)
    public Material photoMaterial;        // ShaderEmu/Photo

    public const int Page = 8000;          // characters a page: a legacy Text draws at most 16,383 (four vertices each)

    [HideInInspector] public bool reading;      // a readback of the guest's bytes is on its way
    [HideInInspector] public int readStatus;    // and what came of it, as a fetch's status
    private byte[] taken = new byte[256 * 256 * 4];
    private int wanted;
    private bool wantedShown;                    // show (the field comes up) or set (only kept)
    private string text = "", board = "", title = "";
    private int page, pages;
    private int mode;                            // 0 shown to copy, 1 asked for a paste, 2 the machine's clipboard, 3 a photo
    private bool given;
    private byte[] gift;                         // what "To the machine" kept for clip:take
    private bool home;
    private Vector3 homeAt, homeSize;
    private Quaternion homeTurn;

    public bool clipTest;                  // a client tests itself and logs [ClipTest] lines (docs/system/clipboard.md, "Checked")

    void Start()
    {
        if (panel != null) panel.SetActive(false);
        if (photo != null) photo.enabled = false;
        if (clipTest) SendCustomEventDelayedSeconds("TestStep", 2f);
    }

    // The test: the machine started and its desktop up; text shown in the field by nxexport, the clipboard handed over by the
    // bar and shown by the Clipboard key, a paste given to an import, and the clipboard's field given to the machine. Each
    // step a line of this client's log; the console's last lines say what the machine made of it.
    private int testStep;
    private float testAt;
    private const string TestPaste = "data:text/plain;name=hello.txt;base64,aGVsbG8gZnJvbSB0aGUgZmllbGQK";   // "hello from the field\n"

    private void TestConsole(string what, int lines)
    {
        EmuTerminal terminal = machine.terminal;
        int row = terminal.CursorRow();
        for (int r = Mathf.Max(0, row - lines); r <= row; r++) Debug.Log("[ClipTest] " + what + ": " + terminal.LineText(r).TrimEnd());
    }

    private string Shown()
    {
        string f = field.text;
        return "panel " + (panel.activeSelf ? "shown" : "hidden") + ", mode " + mode + ", " + text.Length + " characters, title '" +
               (titleLabel != null ? titleLabel.text : "") + "', field begins '" + (f.Length > 60 ? f.Substring(0, 60) : f) + "'";
    }

    public void TestStep()
    {
        if (testStep == 0)
        {
            Debug.Log("[ClipTest] powering on");
            machine.PowerOn();
            testStep = 1;
        }
        else if (testStep == 1 && machine.displayWidth != 0)
        {
            testStep = 2;
            testAt = Time.time + 20f;   // (the desktop's bar starts after the display)
        }
        else if (testStep == 2 && Time.time >= testAt)
        {
            Debug.Log("[ClipTest] desktop " + machine.displayWidth + " x " + machine.displayHeight);
            machine.consoleKeyboard.TypeText("echo clip-from-the-machine > /tmp/clipboard; sleep 5; nxexport --text /tmp/t.txt /etc/hostname; cp /tmp/t.txt /tmp/clipboard; nxexport --clipboard\n");
            testStep = 3;
            testAt = Time.time + 40f;
        }
        else if (testStep == 3 && Time.time >= testAt)
        {
            Debug.Log("[ClipTest] after nxexport --clipboard: " + Shown());
            Debug.Log("[ClipTest] the machine's clipboard as the bar handed it: '" + board + "'");
            TestConsole("console", 4);
            Close();
            ShowBoard();
            Debug.Log("[ClipTest] the Clipboard key: " + Shown());
            Close();
            machine.consoleKeyboard.TypeText("mkdir -p /root/imp; nxexport --import /root/imp &\n");
            testStep = 4;
            testAt = Time.time + 15f;
        }
        else if (testStep == 4 && Time.time >= testAt)
        {
            Debug.Log("[ClipTest] the import asked: " + Shown());
            field.text = TestPaste;
            Give();
            Debug.Log("[ClipTest] given: " + (giveLabel != null ? giveLabel.text : ""));
            testStep = 5;
            testAt = Time.time + 20f;
        }
        else if (testStep == 5 && Time.time >= testAt)
        {
            Debug.Log("[ClipTest] after the import: " + Shown());
            machine.consoleKeyboard.TypeText("cat /root/imp/hello.txt\n");
            testStep = 6;
            testAt = Time.time + 8f;
        }
        else if (testStep == 6 && Time.time >= testAt)
        {
            TestConsole("import", 2);
            ShowBoard();
            field.text = "from-the-world-field";
            Give();
            Debug.Log("[ClipTest] the clipboard's To the machine: " + (giveLabel != null ? giveLabel.text : ""));
            testStep = 7;
            testAt = Time.time + 20f;
        }
        else if (testStep == 7 && Time.time >= testAt)
        {
            Close();
            machine.consoleKeyboard.TypeText("echo; cat /tmp/clipboard; echo\n");
            testStep = 8;
            testAt = Time.time + 8f;
        }
        else if (testStep == 8 && Time.time >= testAt)
        {
            TestConsole("clipboard", 3);
            // a classroom place: the first free one taken (its tower's POWER), then its strip's Clipboard key
            if (stations != null)
            {
                Debug.Log("[ClipTest] the classroom: " + (stations.linkSpots != null ? stations.linkSpots.Length : 0) + " places' spots");
                stations.Take0();
                testStep = 9;
                testAt = Time.time + 15f;   // (a place is this visitor's once the places are assigned again)
            }
            else testStep = 10;
        }
        else if (testStep == 9 && Time.time >= testAt)
        {
            int own = stations.Own();
            EmuShare mine = stations.hub != null ? stations.hub.Mine() : null;
            Debug.Log("[ClipTest] the place taken: Own() " + own + ", this player's share " + (mine != null ? "station " + mine.station : "none"));
            stations.Clipboard();
            Transform spot = own >= 0 && own < stations.linkSpots.Length ? stations.linkSpots[own] : null;
            Debug.Log("[ClipTest] the classroom place " + own + "'s Clipboard key: " + Shown() + "; the field " +
                      (spot != null ? (Vector3.Distance(panel.transform.position, spot.position) * 1000f).ToString("0") + " mm from the place's spot" : "with no place's spot"));
            Close();
            testStep = 10;
        }
        else if (testStep == 10)
        {
            Debug.Log("[ClipTest] done");
            testStep = 11;
        }
        if (testStep < 11) SendCustomEventDelayedSeconds("TestStep", 1f);
    }

    // The guest left `bytes` in the fetch's memory: read from the state ("show" also shows them).
    public void Read(RenderTexture state, int bytes, bool show)
    {
        wanted = bytes;
        wantedShown = show;
        readStatus = bytes > taken.Length ? 413 : 500;
        if (bytes > taken.Length || readMaterial == null || (show && bytes < 256)) return;
        reading = true;
        readMaterial.SetTexture("_State", state);
        VRCGraphics.Blit(state, readTexture, readMaterial);
        VRCAsyncGPUReadback.Request(readTexture, 0, (IUdonEventReceiver)this);
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        reading = false;
        if (request.hasError || !request.TryGetData(taken)) { readStatus = 500; return; }
        if (wantedShown)
        {
            int line = 0;
            while (line < 256 && taken[line] != 0) line++;
            title = System.Text.Encoding.UTF8.GetString(taken, 0, line);
            text = System.Text.Encoding.UTF8.GetString(taken, 256, wanted - 256);
            Show(0);
        }
        else
        {
            board = System.Text.Encoding.UTF8.GetString(taken, 0, wanted);
            if (panel != null && panel.activeSelf && mode == 2) ShowBoard();
        }
        readStatus = 200;
    }

    // The field where the visitor works: before their classroom tube, else before the wall's display.
    private void Place()
    {
        Transform p = panel.transform;
        if (!home)
        {
            home = true;
            homeAt = p.position;
            homeTurn = p.rotation;
            homeSize = p.localScale;
        }
        int own = stations != null ? stations.Own() : -1;
        if (own >= 0 && stations.linkSpots != null && own < stations.linkSpots.Length && stations.linkSpots[own] != null)
        {
            p.SetPositionAndRotation(stations.linkSpots[own].position, stations.linkSpots[own].rotation);
            p.localScale = homeSize * 0.32f;
        }
        else
        {
            p.SetPositionAndRotation(homeAt, homeTurn);
            p.localScale = homeSize;
        }
    }

    private void Show(int how)
    {
        if (panel == null) return;
        mode = how;
        pages = text.Length == 0 ? 1 : (text.Length + Page - 1) / Page;
        page = 0;
        Place();
        panel.SetActive(true);
        ShowPage();
    }

    private void ShowPage()
    {
        int from = page * Page;
        field.text = from < text.Length ? text.Substring(from, Mathf.Min(Page, text.Length - from)) : "";
        if (titleLabel != null)
            titleLabel.text = mode == 3 ? title :
                              mode == 1 ? "Paste the text for the machine here (Ctrl+V), then press To the machine." :
                              mode == 2 ? "The machine's clipboard. Copy it (Ctrl+A, Ctrl+C), or write here and press To the machine." :
                              title + ". Copy it: click the field, Ctrl+A, Ctrl+C" + (pages > 1 ? ", page by page." : ".");
        if (pageLabel != null) pageLabel.text = pages > 1 ? "Page " + (page + 1) + " of " + pages + "  (" + text.Length + " characters)" :
                                                text.Length + " characters";
        if (giveButton != null) giveButton.SetActive(mode == 1 || mode == 2);
        if (backButton != null) backButton.SetActive(pages > 1);
        if (onButton != null) onButton.SetActive(pages > 1);
    }

    // ---- the guest's asks ----

    public void Ask()
    {
        text = "";
        given = false;
        gift = null;
        Show(1);
    }

    public byte[] Take()
    {
        if (!given) return null;
        given = false;
        byte[] g = gift;
        gift = null;
        if (mode == 1 && panel != null) panel.SetActive(false);
        return g;
    }

    public void Hide()
    {
        if (panel != null && mode != 2) panel.SetActive(false);
    }

    // A photo of a file (docs/apps/export.md, "A photo"): the zip at RAM texel `texel` (from 0x80000000), `entries` bytes of
    // its entries and `tail` of its directory, drawn by Photo.shader into the camera's pictures while the camera is in the cube.
    public bool ShowPhoto(Texture state, int texel, int entries, int tail, string name)
    {
        if (photo == null || photoMaterial == null) return false;
        int at = 64 * 2048 + texel;   // (RAM's texels begin at the state texture's row 64)
        photoMaterial.SetTexture("_State", state);
        photoMaterial.SetVector("_At", new Vector4(at % 2048, at / 2048, 0, 0));
        photoMaterial.SetVector("_Lengths", new Vector4(entries / 65536, entries % 65536, tail / 65536, tail % 65536));
        photo.enabled = true;
        title = "A photo of " + name + " (" + (entries + tail) + " bytes)";
        text = "1. Open VRChat's camera and hold it inside the glowing cube in front of this panel: its picture becomes the file " +
               "(coloured noise). Take the picture. Any of the camera's sizes; a smaller one than the file needs shows red.\n\n" +
               "2. On your PC the picture is in Pictures\\VRChat as a PNG. Use that file itself, not a copy shared or uploaded " +
               "(those may be recompressed).\n\n" +
               "3. Open it in Paint, then File, Save as, BMP picture (a 24-bit bitmap). Never JPEG: it changes the bytes.\n\n" +
               "4. Rename the .bmp to .zip (Windows must show file name extensions: File Explorer, View, Show, File name extensions) " +
               "and open it: it is the zip of " + name + ". A damaged picture fails the zip's check instead of giving wrong files.";
        Show(3);
        return true;
    }

    public void HidePhoto()
    {
        if (photo != null) photo.enabled = false;
        if (panel != null && mode == 3) panel.SetActive(false);
    }

    // ---- the keys ----

    // The Clipboard key of the wall's computer or of a place: the machine's clipboard as the bar last handed it over.
    public void ShowBoard()
    {
        text = board;
        Show(2);
    }

    public void NextPage()
    {
        if (page + 1 < pages) { page++; ShowPage(); }
    }

    public void LastPage()
    {
        if (page > 0) { page--; ShowPage(); }
    }

    public void Close()
    {
        if (panel != null) panel.SetActive(false);
        if (photo != null) photo.enabled = false;
    }

    // To the machine: what the field holds, kept for the guest's clip:take. For the clipboard the machine is told
    // as it is told to open an address ("clip:paste": the desktop's bar takes it); an import asks by itself.
    public void Give()
    {
        string t = field.text;
        if (pages > 1 && mode == 2) t = text;   // (the whole clipboard, not a page of it)
        gift = System.Text.Encoding.UTF8.GetBytes(t);
        given = true;
        if (mode == 2)
        {
            board = t;
            bool told = machine != null && machine.AskOpenFor("clip:paste");
            if (giveLabel != null) giveLabel.text = told ? "Given to the machine's clipboard." : "Start the desktop first.";
        }
        else if (giveLabel != null) giveLabel.text = "Given: " + t.Length + " characters.";
    }
}
