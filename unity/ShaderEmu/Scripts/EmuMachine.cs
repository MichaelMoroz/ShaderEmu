using System;
using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Components;
using VRC.SDK3.Rendering;
using VRC.SDK3.StringLoading;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// Runs the machine. Every Unity frame, several rounds of what the harness does per frame:
// CPUTick and Commit (Machine.shader, drawn with VRCGraphics.Blit between two state textures),
// a readback of state row 0 and the control words, the GPU's camera drawing the list the
// guest submitted, and the GPU's control pass. Around that this script does what
// rvc_harness.cpp does: boots the image and feeds the console and the input device.
// The machine is local: every player runs their own.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuMachine : UdonSharpBehaviour
{
    public RenderTexture stateA, stateB;    // 2048 x 4096, four 32-bit words a texel
    public RenderTexture tickState;         // 64 x 64: the CPU's state area after a tick
    public Material tickMaterial;           // MachineTick.shader: CPUTick
    public Material machineMaterial;        // Machine.shader: passes Commit and GPUControl
    public Material gpuMaterial;            // the GPU mesh's material
    public Material volumeMaterial;         // the volume display's: the same lists, seen from the room
    public Camera gpuCamera;                // disabled: rendered from here, once a round
    public Material displayMaterial;        // Display.shader: decodes the display from the state
    public RenderTexture displayTexture;    // the decoded picture, a pixel a texel, with mipmaps
    public Material displayShowMaterial;    // DisplayShow.shader, on the wall
    public Material heatMaterial;           // the memory view's
    public Material readbackMaterial;
    public RenderTexture readbackTexture;   // 448 x 1, one pixel per word

    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard;
    public EmuKeyboard gpuKeyboard;

    // The boot image, twelve textures: RAM r, g, b, a, then ROM, then device tree.
    // A missing texture is left black.
    public Texture2D[] imageTextures;
    public Texture2D blackTexture;

    // Pages for the guest's browser (docs/fetch.md). A world can only load the addresses it
    // was built with, and the one a visitor types into the field on the panel.
    public string[] siteAddresses;
    public VRCUrl[] siteUrls;
    public VRCUrlInputField typedUrl;
    public InputField wantedLink;      // an address the browser wants and the world may not open: to copy
    public Texture2D hostData;         // 256 x 256: an answer's bytes, four a texel
    public TextMeshProUGUI fetchLabel;

    public TextMeshProUGUI statsText;
    public TextMeshProUGUI speedLabel;
    public TextMeshProUGUI powerLabel;
    public TextMeshProUGUI pauseLabel;
    public Slider speedSlider;         // whole steps: 2,048 instructions a frame, doubled per step
    public bool powerOnAtStart;        // off until someone presses Power

    [HideInInspector] public int displayMode, displayWidth, displayHeight;
    [HideInInspector] public bool keyboardOwned;   // a guest program reads the keyboard device

    private const int Words = 448;
    private const int FetchMost = 262144;
    private const int FetchIdle = 0, FetchLoading = 1, FetchPacking = 2, FetchReady = 3, FetchDelivering = 4;
    private const int UartBurst = 4;
    private const int InitFrames = 2;
    private const int TicksPerRound = 8192;   // more per tick pass only fills the write cache
    private const int MaxRounds = 32;
    private const int PassCommit = 0, PassControl = 1;

    private bool powered, paused;
    private int budget = 32768;        // instructions a frame
    private int rounds = 4, ticks = TicksPerRound;
    private RenderTexture current, other;
    private int initLeft;
    private int requestsSent, requestsDone, ignoreUntil;
    private bool live;                 // rows read back are of the running machine
    private Color32[] row = new Color32[Words];

    private uint sentTag, consumedTag;
    private uint keySeq;
    private uint lastMs;
    private int pointerX, pointerY, pointerButtons;
    private bool pointerOn;            // a beam is on the display

    private int fetchState;
    private uint fetchSeq, fetchAnswered;
    private int fetchLength, fetchStatus, fetchPacked;
    private float fetchStarted;
    private byte[] fetchBytes;
    private Color32[] fetchPixels = new Color32[65536];

    private bool haveClock;
    private uint lastClock, lastCommits, pc, lastStall, gpuFrames;
    private float instructions;        // since the last stats line
    private int frames;
    private float statsAt;
    private double totalInstructions;
    private int rtcSecond = -1;
    private string speedLine = "";

    void Start()
    {
        current = stateA;
        other = stateB;
        if (speedSlider != null) SetBudget(2048 << (int)speedSlider.value);
        ShowLabels();
        if (powerOnAtStart) PowerOn();
        else Say("The computer is off.\r\n\r\nPress \"Power on\" on the panel to the right.\r\n");
    }

    private void Say(string text)
    {
        if (terminal == null) return;
        terminal.Clear();
        for (int i = 0; i < text.Length; i++) terminal.PutChar(text[i]);
    }

    // ---- controls (called by the panel's buttons) ----

    public void Power()
    {
        if (powered) PowerOff();
        else PowerOn();
    }

    public void ResetMachine()
    {
        PowerOn();
    }

    public void Pause()
    {
        if (!powered) return;
        paused = !paused;
        ShowLabels();
    }

    public void SpeedChanged()
    {
        SetBudget(2048 << (int)speedSlider.value);
        ShowLabels();
    }

    private void SetBudget(int instructionsPerFrame)
    {
        budget = instructionsPerFrame;
        ticks = Mathf.Min(budget, TicksPerRound);
        rounds = Mathf.Clamp(budget / TicksPerRound, 1, MaxRounds);
        tickMaterial.SetInt("_Ticks", ticks);
    }

    private void ShowLabels()
    {
        if (speedLabel != null) speedLabel.text = "up to " + (rounds * ticks).ToString("N0") + " instructions a frame";
        if (powerLabel != null) powerLabel.text = powered ? "Power off" : "Power on";
        if (pauseLabel != null) pauseLabel.text = paused ? "Resume" : "Pause";
        displayMaterial.SetFloat("_Power", powered ? 1f : 0f);
        ShowDisplay();
    }

    private void ShowDisplay()
    {
        if (current == null) return;
        displayMaterial.SetTexture("_State", current);
        displayMaterial.SetVector("_HostPointer", new Vector4(pointerX, pointerY, pointerOn ? 1 : 0, 0));
        VRCGraphics.Blit(current, displayTexture, displayMaterial);
        displayShowMaterial.SetVector("_Size", new Vector4(powered ? displayWidth : 0, powered ? displayHeight : 0, 0, 0));
    }

    private Texture Pick(int index)
    {
        Texture2D t = imageTextures[index];
        return t != null ? (Texture)t : (Texture)blackTexture;
    }

    private void ApplyImage()
    {
        machineMaterial.SetTexture("_Data_RAM_R", Pick(0));
        machineMaterial.SetTexture("_Data_RAM_G", Pick(1));
        machineMaterial.SetTexture("_Data_RAM_B", Pick(2));
        machineMaterial.SetTexture("_Data_RAM_A", Pick(3));
        machineMaterial.SetTexture("_Data_MTD_R", Pick(4));
        tickMaterial.SetTexture("_Data_MTD_R", Pick(4));
        machineMaterial.SetTexture("_Data_MTD_G", Pick(5));
        tickMaterial.SetTexture("_Data_MTD_G", Pick(5));
        machineMaterial.SetTexture("_Data_MTD_B", Pick(6));
        tickMaterial.SetTexture("_Data_MTD_B", Pick(6));
        machineMaterial.SetTexture("_Data_MTD_A", Pick(7));
        tickMaterial.SetTexture("_Data_MTD_A", Pick(7));
        tickMaterial.SetTexture("_Data_DTB_R", Pick(8));
        tickMaterial.SetTexture("_Data_DTB_G", Pick(9));
        tickMaterial.SetTexture("_Data_DTB_B", Pick(10));
        tickMaterial.SetTexture("_Data_DTB_A", Pick(11));
        gpuMaterial.SetTexture("_Data_MTD_R", Pick(4));
        gpuMaterial.SetTexture("_Data_MTD_G", Pick(5));
        gpuMaterial.SetTexture("_Data_MTD_B", Pick(6));
        gpuMaterial.SetTexture("_Data_MTD_A", Pick(7));
    }

    public void PowerOn()
    {
        ApplyImage();
        powered = true;
        paused = false;
        live = false;
        haveClock = false;
        initLeft = InitFrames;
        ignoreUntil = int.MaxValue;
        sentTag = 0;
        consumedTag = 0;
        totalInstructions = 0;
        displayMode = 0;
        displayWidth = 0;
        displayHeight = 0;
        machineMaterial.SetTexture("_TickState", tickState);
        machineMaterial.SetInt("_Init", 1);
        tickMaterial.SetInt("_Init", 1);
        machineMaterial.SetInt("_InitRaw", 0);
        tickMaterial.SetInt("_InitRaw", 0);
        tickMaterial.SetInt("_DoTick", 0);
        tickMaterial.SetInt("_Ticks", ticks);
        tickMaterial.SetInt("_UartInLo", 0);
        tickMaterial.SetInt("_UartInHi", 0);
        tickMaterial.SetInt("_UdonUARTInTag", 0);
        machineMaterial.SetInt("_InputKeyCount", 0);
        VRCPlayerApi player = Networking.LocalPlayer;
        tickMaterial.SetInt("_PlayerID", player != null ? player.playerId : 0);
        if (consoleKeyboard != null) consoleKeyboard.Flush();
        if (gpuKeyboard != null) gpuKeyboard.Flush();
        if (terminal != null) terminal.Clear();
        fetchState = FetchIdle;
        fetchAnswered = 0;
        machineMaterial.SetInt("_FetchDeliver", 0);
        ShowLabels();
    }

    public void PowerOff()
    {
        powered = false;
        paused = false;
        ShowLabels();
        Say("The computer is off.\r\n");
        if (statsText != null) statsText.text = "off";
    }

    // ---- the pointer (EmuPointer) ----

    public void SetPointer(int x, int y, int buttons)
    {
        pointerX = x;
        pointerY = y;
        pointerButtons = buttons;
        pointerOn = true;
    }

    // No beam is on the display any more.
    public void SetPointerButtons(int buttons)
    {
        pointerButtons = buttons;
        pointerOn = false;
    }

    // Notches of the wheel, up positive: events of their own in the keyboard's ring.
    public void Wheel(int notches)
    {
        if (gpuKeyboard == null) return;
        for (int i = 0; i < Mathf.Min(Mathf.Abs(notches), 8); i++) gpuKeyboard.PushEvent((notches > 0 ? 0x3fe : 0x3ff) | 65536);
    }

    // ---- pages for the guest (docs/fetch.md) ----

    // The guest asked for an address: words 16 and 17 of the control block are the request
    // and the answer, the address itself follows from texel 18.
    private void FetchAsked(uint seq)
    {
        int length = (int)Mathf.Min(Word(64 + 16, 1), 255);
        string address = "";
        for (int i = 0; i < length; i++) address += (char)((Word(64 + 18 + i / 16, (i / 4) % 4) >> (8 * (i % 4))) & 0xff);
        fetchSeq = seq;
        fetchLength = 0;
        VRCUrl url = null;
        // an address a visitor has put into the field is theirs to open: the one typed for
        // "world:typed", or the very one the browser asks for
        if (typedUrl != null && (address == "world:typed" || typedUrl.GetUrl().Get() == address)) url = typedUrl.GetUrl();
        for (int i = 0; i < siteAddresses.Length; i++)
            if (siteAddresses[i] == address) url = siteUrls[i];
        // VRChat throws on an address that is not https, and then calls neither event below
        if (address != "world:typed" && !address.StartsWith("https://"))
        {
            fetchStatus = 3;   // never, in VRChat
            fetchState = FetchReady;
            if (fetchLabel != null) fetchLabel.text = "VRChat opens only https addresses";
            return;
        }
        if (url == null || !url.Get().StartsWith("https://"))
        {
            fetchStatus = 1;   // not until a visitor hands the world this address
            fetchState = FetchReady;
            if (wantedLink != null && wantedLink.text != address) wantedLink.text = address;
            if (fetchLabel != null) fetchLabel.text = "Copy the link on the right into the field under it to open it";
            return;
        }
        fetchState = FetchLoading;
        fetchStarted = Time.time;
        if (fetchLabel != null) fetchLabel.text = "Loading " + address;
        VRCStringDownloader.LoadUrl(url, (IUdonEventReceiver)this);
    }

    public override void OnStringLoadSuccess(IVRCStringDownload result)
    {
        if (fetchState != FetchLoading) return;
        fetchBytes = result.ResultBytes;
        fetchLength = Mathf.Min(fetchBytes.Length, FetchMost);
        fetchStatus = 200;
        fetchPacked = 0;
        fetchState = FetchPacking;
        if (fetchLabel != null) fetchLabel.text = "Loaded " + fetchLength.ToString("N0") + " bytes";
    }

    public override void OnStringLoadError(IVRCStringDownload result)
    {
        if (fetchState != FetchLoading) return;
        fetchLength = 0;
        fetchStatus = result.ErrorCode > 1 && result.ErrorCode != 200 ? result.ErrorCode : 2;
        fetchState = FetchReady;
        if (fetchLabel != null) fetchLabel.text = "Not loaded: " + result.Error;
    }

    // A part of the answer a frame goes into the texture's pixels, then the control pass
    // writes them and the answer's words into the guest's memory in one frame.
    private void FetchStep()
    {
        if (fetchState == FetchDelivering)
        {
            machineMaterial.SetInt("_FetchDeliver", 0);
            fetchAnswered = fetchSeq;
            fetchState = FetchIdle;
        }
        else if (fetchState == FetchLoading && Time.time - fetchStarted > 20f)
        {
            // no answer and no error: tell the guest, and be free for its next request
            fetchLength = 0;
            fetchStatus = 2;
            fetchState = FetchReady;
            if (fetchLabel != null) fetchLabel.text = "Not loaded: VRChat gave no answer in 20 seconds";
        }
        else if (fetchState == FetchPacking)
        {
            int end = Mathf.Min(fetchPacked + 6000, fetchLength);
            for (int at = fetchPacked; at < end; at += 4)
            {
                fetchPixels[at >> 2] = new Color32(fetchBytes[at], at + 1 < fetchLength ? fetchBytes[at + 1] : (byte)0,
                                                   at + 2 < fetchLength ? fetchBytes[at + 2] : (byte)0,
                                                   at + 3 < fetchLength ? fetchBytes[at + 3] : (byte)0);
            }
            fetchPacked = end + 3 & ~3;
            if (end < fetchLength) return;
            hostData.SetPixels32(fetchPixels);
            hostData.Apply(false);
            fetchState = FetchReady;
        }
        else if (fetchState == FetchReady)
        {
            machineMaterial.SetTexture("_HostData", hostData);
            machineMaterial.SetInt("_FetchSeq", (int)(fetchSeq & 0xffffff));
            machineMaterial.SetInt("_FetchLength", fetchLength);
            machineMaterial.SetInt("_FetchStatus", fetchStatus);
            machineMaterial.SetInt("_FetchDeliver", 1);
            fetchState = FetchDelivering;
        }
    }

    // ---- one frame ----

    // One pass of the machine's shader from the current state into the other one.
    private void Pass(int pass)
    {
        machineMaterial.SetTexture("_SelfTexture2D", current);
        VRCGraphics.Blit(current, other, machineMaterial, pass);
        RenderTexture t = current;
        current = other;
        other = t;
    }

    void Update()
    {
        if (!powered || paused) return;
        frames++;

        bool running = initLeft == 0;
        if (running) Inputs();
        if (running) FetchStep();
        int n = running ? rounds : 1;
        for (int i = 0; i < n; i++)
        {
            // The tick writes only the CPU's 64 x 64 state area, into a texture of that size;
            // the commit reads it from there and writes the whole state.
            tickMaterial.SetTexture("_SelfTexture2D", current);
            VRCGraphics.Blit(current, tickState, tickMaterial, 0);
            Pass(PassCommit);
            // What that round left: the console's output is only there until the next tick.
            readbackMaterial.SetTexture("_State", current);
            VRCGraphics.Blit(current, readbackTexture, readbackMaterial);
            VRCAsyncGPUReadback.Request(readbackTexture, 0, (IUdonEventReceiver)this);
            requestsSent++;
            if (running)
            {
                // The GPU: draw the list the guest has submitted, if any; the control pass
                // then takes the submit word back and delivers the input.
                gpuMaterial.SetTexture("_State", current);
                gpuCamera.Render();
                Pass(PassControl);
            }
        }
        if (initLeft > 0)
        {
            initLeft--;
            if (initLeft == 0)
            {
                machineMaterial.SetInt("_Init", 0);
                tickMaterial.SetInt("_Init", 0);
                ignoreUntil = requestsSent;   // init frames leave junk in the UART buffer
            }
        }
        // whoever shows the state reads the texture this frame ended in
        gpuMaterial.SetTexture("_State", current);
        ShowDisplay();
        heatMaterial.SetTexture("_State", current);
        volumeMaterial.SetTexture("_State", current);

        if (Time.time - statsAt >= 0.5f) ShowStats();
    }

    private void Inputs()
    {
        // Console input: up to four characters per handshake, first one in the low byte. The
        // guest echoes the tag once it has taken them.
        if (live && sentTag == consumedTag && consoleKeyboard != null)
        {
            int lo = 0, hi = 0, count = 0;
            while (count < UartBurst && consoleKeyboard.Count() > 0)
            {
                int c = consoleKeyboard.Pop() & 0xff;
                if (c == 0) continue;   // 0 means "no character" to the guest
                if (count < 2) lo |= c << (8 * count);
                else hi |= c << (8 * (count - 2));
                count++;
            }
            if (count > 0)
            {
                sentTag += (uint)count;
                tickMaterial.SetInt("_UartInLo", lo);
                tickMaterial.SetInt("_UartInHi", hi);
                tickMaterial.SetInt("_UdonUARTInTag", (int)sentTag);
            }
        }

        // The input device: the pointer every frame and up to four key events. The control
        // pass maps the pointer from a panel to display pixels; this panel makes that 1:1.
        int keys = 0;
        if (gpuKeyboard != null)
        {
            int k0 = 0, k1 = 0, k2 = 0, k3 = 0;
            while (keys < 4 && gpuKeyboard.Count() > 0)
            {
                int e = gpuKeyboard.Pop();
                if (keys == 0) k0 = e;
                else if (keys == 1) k1 = e;
                else if (keys == 2) k2 = e;
                else k3 = e;
                keys++;
            }
            machineMaterial.SetInt("_InputKeyCode0", k0);
            machineMaterial.SetInt("_InputKeyCode1", k1);
            machineMaterial.SetInt("_InputKeyCode2", k2);
            machineMaterial.SetInt("_InputKeyCode3", k3);
        }
        machineMaterial.SetInt("_InputKeySeq", (int)(keySeq & 0xffffff));
        machineMaterial.SetInt("_InputKeyCount", keys);
        keySeq += (uint)keys;
        machineMaterial.SetVector("_InputPointer", new Vector4(pointerX + 8, pointerY + 8, displayWidth + 16, displayHeight + 16));
        machineMaterial.SetInt("_InputButtons", pointerButtons);

        uint ms = (uint)(Time.timeSinceLevelLoad * 1000f);
        machineMaterial.SetInt("_HostMsLo", (int)(ms & 0xffff));
        machineMaterial.SetInt("_HostMsHi", (int)(ms >> 16));

        // The clock chip (DS1742): local time as BCD, century in the control byte.
        DateTime now = DateTime.Now;
        if (now.Second != rtcSecond)
        {
            rtcSecond = now.Second;
            int year = now.Year;
            tickMaterial.SetInt("_Rtc0Lo", Bcd(year / 100) | Bcd(now.Second) << 8);
            tickMaterial.SetInt("_Rtc0Hi", Bcd(now.Minute) | Bcd(now.Hour) << 8);
            tickMaterial.SetInt("_Rtc1Lo", Bcd((int)now.DayOfWeek + 1) | Bcd(now.Day) << 8);
            tickMaterial.SetInt("_Rtc1Hi", Bcd(now.Month) | Bcd(year % 100) << 8);
        }
    }

    private int Bcd(int v)
    {
        return (v / 10) << 4 | (v % 10);
    }

    private uint Word(int texel, int k)
    {
        Color32 p = row[texel * 4 + k];
        return (uint)p.r | (uint)p.g << 8 | (uint)p.b << 16 | (uint)p.a << 24;
    }

    // Readbacks complete in the order they were asked for.
    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        requestsDone++;
        if (request.hasError || !request.TryGetData(row)) return;
        if (requestsDone <= ignoreUntil) return;
        live = true;

        // state row 0, as rvc_harness.cpp reads it
        uint clock = Word(28, 1), commits = Word(28, 2);
        consumedTag = Word(9, 3);
        // the control words: display (texel 64), GPU (65), input (66, 67)
        displayMode = (int)Word(64, 0);
        displayWidth = (int)Word(64, 1);
        displayHeight = (int)Word(64, 2);
        if (displayMode < 1 || displayMode > 4 || displayWidth > 2048 || displayHeight > 2048)
        {
            displayWidth = 0;
            displayHeight = 0;
        }
        gpuFrames = Word(65, 3);
        keyboardOwned = Word(67, 0) == 0x6b657973u;
        uint asked = Word(64 + 16, 0);
        if (fetchState == FetchIdle && asked != Word(64 + 17, 0) && asked != fetchAnswered) FetchAsked(asked);

        if (haveClock)
        {
            uint ran = clock - lastClock;
            instructions += ran;
            totalInstructions += ran;
        }
        haveClock = true;
        lastClock = clock;
        lastCommits = commits;
        pc = Word(36, 3);
        lastStall = Word(40, 2);

        // what the guest printed in that round: at most 64 characters
        uint ptr = Word(11, 3);
        if (ptr == 0xFFFFFFFFu || terminal == null) return;
        int n = (int)Mathf.Min(ptr + 1, 64);
        for (int i = 0; i < n; i++)
        {
            int c = (int)(Word(12 + i / 4, i % 4) & 0xff);
            if (c != 0) terminal.PutChar(c);
        }
    }

    private void ShowStats()
    {
        float dt = Time.time - statsAt;
        statsAt = Time.time;
        float ips = instructions / dt, fps = frames / dt;
        instructions = 0;
        frames = 0;
        speedLine = ips.ToString("N0") + " instructions/s   " + fps.ToString("F0") + " frames/s   "
                    + (fps > 0 ? (ips / fps).ToString("N0") : "0") + " a frame";
        if (statsText == null) return;
        string display = displayWidth > 0 ? "mode " + displayMode + ", " + displayWidth + " x " + displayHeight : "off";
        statsText.text =
            (paused ? "paused" : "running") + ", " + rounds + " x " + ticks + " instructions a frame at most\n" +
            speedLine + "\n" +
            "pc " + pc.ToString("x8") + "   stall " + lastStall + "   commits " + lastCommits + "\n" +
            "instructions " + totalInstructions.ToString("N0") + "\n" +
            "display " + display + "   GPU lists drawn " + gpuFrames + "\n" +
            "console: sent " + sentTag + ", taken " + consumedTag + ", waiting " + (consoleKeyboard != null ? consoleKeyboard.Count() : 0) + "\n" +
            "keyboard device: " + keySeq + " events" + (keyboardOwned ? ", owned by a program" : "");
    }
}
