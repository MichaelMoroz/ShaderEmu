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

    public const int Page = 8000;          // characters a page: a legacy Text draws at most 16,383 (four vertices each)

    [HideInInspector] public bool reading;      // a readback of the guest's bytes is on its way
    [HideInInspector] public int readStatus;    // and what came of it, as a fetch's status
    private byte[] taken = new byte[256 * 256 * 4];
    private int wanted;
    private bool wantedShown;                    // show (the field comes up) or set (only kept)
    private string text = "", board = "", title = "";
    private int page, pages;
    private int mode;                            // 0 shown to copy, 1 asked for a paste, 2 the machine's clipboard
    private bool given;
    private byte[] gift;                         // what "To the machine" kept for clip:take
    private bool home;
    private Vector3 homeAt, homeSize;
    private Quaternion homeTurn;

    void Start()
    {
        if (panel != null) panel.SetActive(false);
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
            titleLabel.text = mode == 1 ? "Paste the text for the machine here (Ctrl+V), then press To the machine." :
                              mode == 2 ? "The machine's clipboard. Copy it (Ctrl+A, Ctrl+C), or write here and press To the machine." :
                              title + ". Copy it: click the field, Ctrl+A, Ctrl+C" + (pages > 1 ? ", page by page." : ".");
        if (pageLabel != null) pageLabel.text = pages > 1 ? "Page " + (page + 1) + " of " + pages + "  (" + text.Length + " characters)" :
                                                text.Length + " characters";
        if (giveButton != null) giveButton.SetActive(mode != 0);
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
