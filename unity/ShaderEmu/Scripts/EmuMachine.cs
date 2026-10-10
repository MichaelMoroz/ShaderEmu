using System;
using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Components;
using VRC.SDK3.Image;
using VRC.SDK3.Rendering;
using VRC.SDK3.StringLoading;
using VRC.SDKBase;
using VRC.Udon.Common.Interfaces;

// Runs the machine. Every Unity frame, several rounds of what the harness does per frame:
// CPUTick and Commit (Machine.shader, drawn with VRCGraphics.Blit between two state textures),
// a readback of state row 0 and the control words, the GPU's camera drawing the list the
// guest submitted, and the GPU's control pass. Around that this script does what
// rvc_harness.cpp does: boots the image and feeds the console and the input device.
// The machine is local: every player runs their own, and EmuShareHub shows it to the others.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuMachine : UdonSharpBehaviour
{
    public RenderTexture stateA, stateB;    // 2048 x 4096, four 32-bit words a texel
    public RenderTexture tickState;         // 2048 x 16: core 0's state rectangle and the workers' band, after a tick
    public Material tickMaterial;           // MachineTick.shader: CPUTick, on the tick camera's mesh
    // The tick is a camera's pass into eight targets, eight state texels a pixel, and what the
    // cores stored is committed as points by another camera (docs/multicore.md, "In Unity").
    public Camera tickCamera;               // disabled: rendered from here, once a round
    public RenderTexture[] tickTargets;     // eight of 1024 x 16
    public RenderTexture tickDepth;         // the depth buffer a camera with several targets must be given
    public Camera pointsCamera;             // disabled: rendered from here, twice a round
    public Material pointsMaterial;         // MachineCommitPoints.shader, on the points camera's mesh
    public Material machineMaterial;        // Machine.shader: passes Commit and GPUControl
    public Material gpuMaterial;            // the GPU mesh's material
    public Material volumeMaterial;         // the holodeck's: the same lists, seen from the room
    public Material volumeMaskMaterial;     // and what marks where they show, which looks whether there is a frame
    public Camera gpuCamera;                // disabled: rendered from here, once a round
    public Material displayMaterial;        // Display.shader: decodes the display from the state
    public RenderTexture displayTexture;    // the decoded picture, a pixel a texel, with mipmaps
    public Material displayShowMaterial;    // DisplayShow.shader, on the wall
    public Material heatMaterial;           // the memory view's
    public Material stateViewMaterial;      // Scope.shader showing the CPU's state texels
    public Material romViewMaterial;        // and the ROM
    public Material readbackMaterial;
    public RenderTexture readbackTexture;   // 768 x 32: one pixel per word, a row for each round of a frame

    public EmuSound sound;                  // the sound card's host side, if the world has one
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
    public InputField[] wantedLinks;       // four addresses the browser wants and the world may not open: to copy
    public VRCUrlInputField[] typedUrls;   // and four fields to paste them into
    public GameObject linksButton;         // by the display, while something is wanted
    public GameObject linksPanel;          // the eight fields, in front of the display
    public VRCUrlInputField openField;     // an address for the machine to open (docs/open.md)
    public TextMeshProUGUI openLabel;
    public Texture2D hostData;         // 256 x 256: an answer's bytes, four a texel
    public Texture2D netData;          // 160 x 4: the network's packets for the guest, a row each (docs/lan.md)
    public TextMeshProUGUI fetchLabel;

    public TextMeshProUGUI statsText;
    public TextMeshProUGUI speedLabel;
    public TextMeshProUGUI powerLabel;
    public TextMeshProUGUI powerSign;  // on the button on the computer itself, and the line under it
    public Image powerPlate;           // that button: red while off, green while running
    public TextMeshProUGUI powerSays;
    public TextMeshProUGUI pauseLabel;
    public Slider speedSlider;         // whole steps: 2,048 instructions a frame, doubled per step
    public TextMeshProUGUI modeLabel;  // on the button that picks how the slider is read
    public float minFrameRate = 45f;   // "steady" gives up instructions before frames come slower than this
    public bool powerOnAtStart;        // off until someone presses Power

    [HideInInspector] public int displayMode, displayWidth, displayHeight;
    [HideInInspector] public bool keyboardOwned;   // a guest program reads the keyboard device

    // Another player's machine is on the screens (EmuShareHub): the wall is not this one's to
    // size, and the keyboards and pointer are not this one's to read.
    [HideInInspector] public bool showRemote, inputAway;
    [HideInInspector] public int shownWidth, shownHeight;   // the display the pointer is mapped to
    [HideInInspector] public int pointerX, pointerY, pointerButtons;
    [HideInInspector] public bool pointerOn;            // a beam is on the display
    [HideInInspector] public bool pointerOwn;           // and it is on this player's own classroom tube: this machine's, whatever the wall shows
    [HideInInspector] public float mipsAll, mipsCore;   // millions of instructions a second: every core, and core 0

    private const int Words = 768;      // state row 0's 64 texels, the first 64 control texels, 64 of the network's row
    private const int CoresMore = 168;  // what cores 16 and up ran, in texels: thirteen after the network's window
    // The network (docs/lan.md): the guest's packets out of its window, and packets for it into its ring.
    private const int NetSlot = 640, NetMost = 576, NetQueue = 32;
    private byte[] netOut = new byte[NetQueue * NetMost], netIn = new byte[NetQueue * NetMost];
    private int[] netOutLength = new int[NetQueue], netInLength = new int[NetQueue];
    private int netOutHead, netOutTail, netInHead, netInTail;
    private uint netAcked, netTaken, netRxSeq;
    private int netRxCount, netBad, netId;
    private bool netSeen;
    private Color32[] netPixels = new Color32[160 * 4];
    [HideInInspector] public int netSent, netReceived;   // packets, for whoever watches
    private const int FetchMost = 262144;
    private const int FetchIdle = 0, FetchLoading = 1, FetchPacking = 2, FetchReady = 3, FetchDelivering = 4, FetchOpening = 5;
    private const int KindFile = 3;     // a request for whatever an address holds, as a file
    private const int UartBurst = 4;
    private const int InitFrames = 2;
    private const int TicksPerRound = 8192;   // instructions a pass, as the harness's
    private const int MaxRounds = 32;
    private const int PassCommit = 0, PassControl = 1, PassUnpack = 2;
    private bool targetsSet;

    private bool powered, paused;
    private int budget = 32768;        // instructions a frame
    private int rounds = 4, ticks = TicksPerRound;
    // Steady: so many instructions a second, however long a frame is, as far as the frame rate allows.
    private bool steady;
    private float targetIps = 2000000f;
    private int roundCap = MaxRounds, guardWait, steadyRounds = 1;
    private float smoothDt = 0.011f, carry;
    private float perRound = TicksPerRound;   // instructions a round really ran, lately: many end early
    private RenderTexture current, other;
    private int initLeft;
    private int requestsSent, requestsDone, ignoreUntil;
    private bool live;                 // rows read back are of the running machine
    private Color32[] row = new Color32[Words * MaxRounds];
    private int rowAt;                 // where in it the round being taken starts
    private int[] roundsOf = new int[64];   // how many rounds each readback on its way has

    private uint sentTag, consumedTag;
    private uint keySeq;
    private uint lastMs;
    private int remoteX, remoteY, remoteButtons;   // a watching player's pointer
    private bool remoteOn;
    private float ownMovedAt, remoteMovedAt;
    private int[] remoteChars = new int[256];      // and what they typed: console, key events
    private int[] remoteKeys = new int[256];
    private int charHead, charTail, keyHead, keyTail;

    private int fetchState;
    private uint fetchSeq, fetchAnswered;
    private int fetchLength, fetchStatus, fetchPacked;
    private float fetchStarted;
    private int fetchKind, fetchW, fetchH;
    private int fetchOffset;           // the part of the answer asked for begins here
    private string heldAddress = "";   // what the answer that is kept is of
    private bool heldImage;            // and whether it is a picture's texture or bytes
    private Vector3 linksAt, linksSize;   // the links panel's own place, before the wall's display
    private Quaternion linksTurn;
    private bool linksHome;
    private string openDue = "";       // an address the visitor gave, not handed to the guest yet
    private int openCount;
    private VRCImageDownloader imageLoader;
    private Texture2D hostImage;
    private string[] wanted = new string[4];
    private float[] wantedAt = new float[4];
    private byte[] fetchBytes;
    private Color32[] fetchPixels = new Color32[65536];

    private bool soundEnabled;         // the guest has the sound card on, and where its clock is
    private uint soundClock;
    private bool haveClock;
    private uint lastClock, lastCommits, pc, lastStall, gpuFrames;
    private float instructions;        // since the last stats line
    // the worker cores (docs/multicore.md): how many cores, which run, and what the workers ran
    private int cores = 1;
    private uint coresRunning, coresRunningMore;   // a bit a core that is at work: the first 32, the rest
    private uint[] workerClock = new uint[64];
    private bool haveWorkerClock;
    private float workerInstructions;  // since the last stats line
    private double totalWorkerInstructions;
    private int frames;
    private float statsAt;
    private double totalInstructions;
    private int rtcSecond = -1;
    private string speedLine = "";

    void Start()
    {
        current = stateA;
        other = stateB;
        for (int i = 0; i < 4; i++) wanted[i] = "";
        linksPanel.SetActive(false);
        linksButton.SetActive(false);
        if (speedSlider != null) SetBudget(2048 << (int)speedSlider.value);
        ShowLabels();
        if (powerOnAtStart) PowerOn();
        else Say("The computer is off.\r\n\r\nPress the red POWER button on the computer\r\n(the tower by the desk's right end), or \"Power on\" on the panel.\r\n");
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
        if (paused && sound != null) sound.Quiet();
        ShowLabels();
    }

    public void SpeedChanged()
    {
        // the same eight steps: instructions a frame from 2,048, or a second from 125,000
        if (steady) targetIps = 125000f * (1 << (int)speedSlider.value);
        else SetBudget(2048 << (int)speedSlider.value);
        ShowLabels();
    }

    public void SpeedMode()
    {
        steady = !steady;
        roundCap = MaxRounds;
        carry = 0f;
        SpeedChanged();
    }

    // How many rounds this frame gets in steady mode. The target times the frame's length is
    // what is owed, and a round counts for what rounds have really run lately (but no less than
    // a quarter of what it may: an idle guest is not worth more rounds). When frames come slower
    // than minFrameRate the most rounds a frame may have comes down, and creeps back up after.
    private int SteadyRounds()
    {
        float dt = Mathf.Min(Time.unscaledDeltaTime, 0.1f);
        smoothDt += (dt - smoothDt) * 0.1f;
        float limit = 1f / minFrameRate;
        if (--guardWait <= 0)
        {
            guardWait = 6;
            if (smoothDt > limit) roundCap = Mathf.Max(1, roundCap - 1 - roundCap / 8);
            else if (smoothDt < limit * 0.8f && roundCap < MaxRounds) roundCap++;
        }
        float owed = targetIps * dt + carry;
        float worth = Mathf.Clamp(perRound, TicksPerRound / 4, TicksPerRound);
        int n = Mathf.Clamp((int)(owed / worth), 1, roundCap);
        int each = owed < TicksPerRound / 2 ? Mathf.Clamp((int)owed, 512, TicksPerRound) : TicksPerRound;
        carry = Mathf.Clamp(owed - n * Mathf.Min(worth, each), -TicksPerRound, TicksPerRound);
        if (each != ticks)
        {
            ticks = each;
            tickMaterial.SetInt("_Ticks", ticks);
        }
        steadyRounds = n;
        return n;
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
        if (speedLabel != null)
            speedLabel.text = steady ? targetIps.ToString("N0") + " a second, while frames keep to " + minFrameRate.ToString("F0") + " a second"
                                     : "up to " + (rounds * ticks).ToString("N0") + " instructions a frame";
        if (modeLabel != null) modeLabel.text = steady ? "Steady: per second" : "Fixed: per frame";
        if (powerLabel != null) powerLabel.text = powered ? "Power off" : "Power on";
        if (powerSign != null) powerSign.text = "POWER";
        if (powerPlate != null) powerPlate.color = powered ? new Color(0.10f, 0.62f, 0.20f) : new Color(0.80f, 0.07f, 0.05f);
        if (powerSays != null) powerSays.text = powered ? "Running. Press to switch off." : "Off. Press to start.";
        if (pauseLabel != null) pauseLabel.text = paused ? "Resume" : "Pause";
        displayMaterial.SetFloat("_Power", powered ? 1f : 0f);
        ShowDisplay();
    }

    // This machine's own display: its size, 0 while it is off.
    public bool Powered() { return powered; }
    public int OwnWidth() { return powered ? displayWidth : 0; }
    public int OwnHeight() { return powered ? displayHeight : 0; }

    private void ShowDisplay()
    {
        if (current == null) return;
        displayMaterial.SetTexture("_State", current);
        bool theirs = UseRemote();
        displayMaterial.SetVector("_HostPointer", new Vector4(theirs ? remoteX : pointerX, theirs ? remoteY : pointerY,
                                                              theirs || (pointerOn && (!inputAway || pointerOwn)) ? 1 : 0, 0));
        // off: black, as the picture is also the light the display gives the room (LTCGI)
        if (powered || theirs) VRCGraphics.Blit(current, displayTexture, displayMaterial);
        else VRCGraphics.Blit(blackTexture, displayTexture);
        if (showRemote) return;
        shownWidth = powered ? displayWidth : 0;
        shownHeight = powered ? displayHeight : 0;
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
        // (a copy out of the ROM is committed as points)
        pointsMaterial.SetTexture("_Data_MTD_R", Pick(4));
        pointsMaterial.SetTexture("_Data_MTD_G", Pick(5));
        pointsMaterial.SetTexture("_Data_MTD_B", Pick(6));
        pointsMaterial.SetTexture("_Data_MTD_A", Pick(7));
        gpuMaterial.SetTexture("_Data_MTD_R", Pick(4));
        gpuMaterial.SetTexture("_Data_MTD_G", Pick(5));
        gpuMaterial.SetTexture("_Data_MTD_B", Pick(6));
        gpuMaterial.SetTexture("_Data_MTD_A", Pick(7));
        if (romViewMaterial != null) romViewMaterial.SetTexture("_Data_MTD_R", Pick(4));
        if (sound != null)
        {
            // samples are played from where they lie in the ROM
            sound.mixMaterial.SetTexture("_Data_MTD_R", Pick(4));
            sound.mixMaterial.SetTexture("_Data_MTD_G", Pick(5));
            sound.mixMaterial.SetTexture("_Data_MTD_B", Pick(6));
            sound.mixMaterial.SetTexture("_Data_MTD_A", Pick(7));
        }
    }

    public void PowerOn()
    {
        ApplyImage();
        powered = true;
        paused = false;
        live = false;
        haveClock = false;
        soundEnabled = false;
        if (sound != null) sound.Quiet();
        initLeft = InitFrames;
        ignoreUntil = int.MaxValue;
        sentTag = 0;
        consumedTag = 0;
        charHead = charTail = keyHead = keyTail = 0;
        totalInstructions = 0;
        totalWorkerInstructions = 0;
        haveWorkerClock = false;
        displayMode = 0;
        displayWidth = 0;
        displayHeight = 0;
        machineMaterial.SetTexture("_TickState", tickState);
        pointsMaterial.SetTexture("_TickState", tickState);
        if (!targetsSet)
        {
            // the tick camera's eight targets, which the unpack pass then reads
            targetsSet = true;
            RenderBuffer[] colours = new RenderBuffer[tickTargets.Length];
            for (int i = 0; i < tickTargets.Length; i++)
            {
                colours[i] = tickTargets[i].colorBuffer;
                machineMaterial.SetTexture("_TickOut" + i, tickTargets[i]);
            }
            tickCamera.SetTargetBuffers(colours, tickDepth.depthBuffer);
        }
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
        // the host flags: the desktop at boot, and the largest screen the GPU's target can show
        // (in 16s of pixels: the word must stay within what a material's number holds exactly)
        RenderTexture target = gpuCamera.targetTexture;
        machineMaterial.SetInt("_HostFlags", 3 | (target.width / 16) << 8 | (target.height / 16) << 16);
        VRCPlayerApi player = Networking.LocalPlayer;
        tickMaterial.SetInt("_PlayerID", player != null ? player.playerId : 0);
        if (consoleKeyboard != null) consoleKeyboard.Flush();
        if (gpuKeyboard != null) gpuKeyboard.Flush();
        if (terminal != null) terminal.Clear();
        fetchState = FetchIdle;
        fetchAnswered = 0;
        heldAddress = "";
        openDue = "";
        openCount = 0;
        machineMaterial.SetInt("_FetchDeliver", 0);
        // the network: a new machine counts from nothing, and so does its host
        netOutHead = netOutTail = netInHead = netInTail = 0;
        netAcked = netTaken = netRxSeq = 0;
        netRxCount = netBad = 0;
        netSeen = false;
        netId = player != null ? player.playerId & 0xffff : 0;
        NetUniforms();
        ShowLabels();
    }

    public void PowerOff()
    {
        powered = false;
        paused = false;
        soundEnabled = false;
        if (sound != null) sound.Quiet();
        ShowLabels();
        Say("The computer is off.\r\n");
        if (statsText != null) statsText.text = "off";
        mipsAll = 0f;
        mipsCore = 0f;
    }

    public bool IsOn()
    {
        return powered;
    }

    // ---- a watching player's hands (EmuShareHub), and this player's own at their classroom place ----

    public void RemoteChar(int c)
    {
        int next = (charTail + 1) & 255;
        if (next == charHead) return;
        remoteChars[charTail] = c;
        charTail = next;
    }

    public void RemoteKey(int e)
    {
        int next = (keyTail + 1) & 255;
        if (next == keyHead) return;
        remoteKeys[keyTail] = e;
        keyTail = next;
    }

    public void SetRemotePointer(int x, int y, int buttons, bool on)
    {
        if (x != remoteX || y != remoteY || (on ? buttons : 0) != remoteButtons || on != remoteOn) remoteMovedAt = Time.time;
        remoteX = x;
        remoteY = y;
        remoteButtons = on ? buttons : 0;
        remoteOn = on;
    }

    // ---- the pointer (EmuPointer) ----

    // Two pointers may be on the display, this player's and a watcher's: the one holding a
    // button has the machine, else the one that moved last. A desktop player's is always there.
    private bool UseRemote()
    {
        if (!remoteOn) return false;
        if (pointerOn && pointerOwn) return false;
        if (!pointerOn || inputAway) return true;
        if (pointerButtons != 0) return false;
        return remoteButtons != 0 || remoteMovedAt > ownMovedAt;
    }

    public void SetPointer(int x, int y, int buttons)
    {
        if (x != pointerX || y != pointerY || buttons != pointerButtons || !pointerOn) ownMovedAt = Time.time;
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
        pointerOwn = false;
    }

    // Notches of the wheel, up positive: events of their own in the keyboard's ring.
    public void Wheel(int notches)
    {
        if (gpuKeyboard == null) return;
        for (int i = 0; i < Mathf.Min(Mathf.Abs(notches), 8); i++) gpuKeyboard.PushEvent((notches > 0 ? 0x3fe : 0x3ff) | 65536);
    }

    // ---- pages for the guest (docs/fetch.md) ----

    // The guest asked for an address: words 16 and 17 of the control block are the request
    // and the answer, the address itself follows from texel 18. Kind 1 is a picture.
    private void FetchAsked(uint seq)
    {
        int length = (int)Mathf.Min(Word(64 + 16, 1), 255);
        string address = "";
        for (int i = 0; i < length; i++) address += (char)((Word(64 + 18 + i / 16, (i / 4) % 4) >> (8 * (i % 4))) & 0xff);
        fetchSeq = seq;
        fetchKind = (int)Word(64 + 16, 2);
        fetchOffset = (int)Mathf.Min(Word(64 + 16, 3), 0xffffff);
        if (fetchOffset != 0 && address == heldAddress)
        {
            // a further part of the answer that is kept: no asking again
            fetchStatus = 200;
            fetchPacked = fetchOffset;
            fetchState = heldImage ? FetchReady : FetchPacking;
            return;
        }
        fetchLength = 0;
        heldAddress = "";
        if (imageLoader != null)
        {
            // the picture kept for its parts is done with: VRChat keeps every one until told
            imageLoader.Dispose();
            imageLoader = null;
        }
        VRCUrl url = null;
        // an address a visitor has put into one of the fields is theirs to open: the very one
        // the guest asks for, or the first field's for "world:typed"
        for (int i = 0; i < typedUrls.Length; i++)
            if (typedUrls[i].GetUrl().Get() == address || (i == 0 && address == "world:typed")) url = typedUrls[i].GetUrl();
        if (openField != null && openField.GetUrl().Get() == address) url = openField.GetUrl();
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
            Want(address, fetchKind);
            return;
        }
        Unwant(address);
        fetchState = FetchLoading;
        fetchStarted = Time.time;
        if (fetchLabel != null) fetchLabel.text = "Loading " + address;
        heldAddress = address;
        heldImage = fetchKind == 1 || (fetchKind == KindFile && IsPicture(address));
        if (heldImage)
        {
            // a picture: VRChat fetches and decodes it into a texture, which the control pass
            // scales into the guest's memory as pixels. Asked for as a file it may carry one in
            // its pixels (docs/fetch.md), which have to arrive as they are: no smaller copies.
            if (imageLoader == null) imageLoader = new VRCImageDownloader();
            TextureInfo info = new TextureInfo();
            info.GenerateMipMaps = fetchKind == 1;
            imageLoader.DownloadImage(url, null, (IUdonEventReceiver)this, info);
        }
        else
        {
            VRCStringDownloader.LoadUrl(url, (IUdonEventReceiver)this);
        }
    }

    // VRChat gives a world a picture only through its picture loader: by the address's ending.
    private bool IsPicture(string address)
    {
        int cut = address.IndexOf('?');
        string path = (cut < 0 ? address : address.Substring(0, cut)).ToLower();
        return path.EndsWith(".png") || path.EndsWith(".jpg") || path.EndsWith(".jpeg");
    }

    // The button by the link field: the machine is told to open the address in it.
    public void OpenLink()
    {
        if (openField == null) return;
        string address = openField.GetUrl().Get();
        if (address == null || address.Length == 0 || !powered)
        {
            if (openLabel != null) openLabel.text = powered ? "paste an address" : "computer is off";
            return;
        }
        if (!address.StartsWith("https://"))
        {
            if (openLabel != null) openLabel.text = "https only";
            return;
        }
        openDue = address.Length > 255 ? address.Substring(0, 255) : address;
        if (openLabel != null) openLabel.text = address.Length > 255 ? "too long (255)" : "opening...";
        if (address.Length > 255) openDue = "";
    }

    // ---- addresses the visitor has to hand over ----

    // Slot 0 is the page, 1 to 3 what it wants next (pictures, in its order of asking).
    private void Want(string address, int kind)
    {
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (wanted[i] == address) slot = i;
        if (slot < 0)
        {
            if (kind == 0)
            {
                // another page: what the last one wanted is no longer wanted
                for (int i = 1; i < 4; i++) wanted[i] = "";
                slot = 0;
            }
            else
            {
                for (int i = 3; i >= 1; i--)
                    if (wanted[i] == "") slot = i;
            }
            if (slot < 0) return;
            wanted[slot] = address;
        }
        wantedAt[slot] = Time.time;
        ShowLinks();
    }

    private void Unwant(string address)
    {
        for (int i = 0; i < 4; i++)
            if (wanted[i] == address) wanted[i] = "";
        ShowLinks();
    }

    private void ShowLinks()
    {
        bool any = false;
        for (int i = 0; i < 4; i++)
        {
            if (wanted[i] != "") any = true;
            if (wantedLinks[i].text != wanted[i]) wantedLinks[i].text = wanted[i];
        }
        linksButton.SetActive(any && !linksPanel.activeSelf);
        if (fetchLabel != null && any) fetchLabel.text = "The browser needs addresses pasted: the button by the display";
    }

    // The button by the display, and the panel's Close.
    public void ToggleLinks()
    {
        PlaceLinks(!linksPanel.activeSelf, null);
    }

    public bool LinksWanted()
    {
        for (int i = 0; i < 4; i++)
            if (wanted[i] != "") return true;
        return false;
    }

    // The links panel, open or shut: before the wall's display, or (at) before a classroom
    // place's tube, small enough for it.
    public void PlaceLinks(bool open, Transform at)
    {
        Transform panel = linksPanel.transform;
        if (!linksHome)
        {
            linksHome = true;
            linksAt = panel.position;
            linksTurn = panel.rotation;
            linksSize = panel.localScale;
        }
        if (at != null)
        {
            panel.SetPositionAndRotation(at.position, at.rotation);
            panel.localScale = linksSize * 0.32f;
        }
        else
        {
            panel.SetPositionAndRotation(linksAt, linksTurn);
            panel.localScale = linksSize;
        }
        linksPanel.SetActive(open);
        ShowLinks();
    }

    public override void OnImageLoadSuccess(IVRCImageDownload result)
    {
        if (fetchState != FetchLoading) return;
        hostImage = result.Result;
        // as large as fits what the guest can be handed: 65,536 pixels, 256 either way
        int w = hostImage.width, h = hostImage.height;
        fetchW = w;
        fetchH = h;
        while (fetchW > 256 || fetchH > 256 || fetchW * fetchH > 65536)
        {
            fetchW = Mathf.Max(1, fetchW * 7 / 8);
            fetchH = Mathf.Max(1, h * fetchW / w);
        }
        fetchLength = fetchW * fetchH * 4;
        fetchStatus = 200;
        fetchState = FetchReady;
        if (fetchLabel != null) fetchLabel.text = "Loaded a picture, " + w + " x " + h;
    }

    public override void OnImageLoadError(IVRCImageDownload result)
    {
        if (fetchState != FetchLoading) return;
        fetchLength = 0;
        fetchStatus = 415;
        fetchState = FetchReady;
        if (fetchLabel != null) fetchLabel.text = "Picture not loaded: " + result.ErrorMessage;
    }

    public override void OnStringLoadSuccess(IVRCStringDownload result)
    {
        if (fetchState != FetchLoading) return;
        // (a page is cut to what the browser takes at once; a file is handed over in parts)
        fetchBytes = result.ResultBytes;
        fetchLength = fetchKind == KindFile ? fetchBytes.Length : Mathf.Min(fetchBytes.Length, FetchMost);
        fetchStatus = 200;
        fetchPacked = 0;
        fetchOffset = 0;
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
        // an address nobody has asked for in a while is no longer wanted
        for (int i = 0; i < 4; i++)
            if (wanted[i] != "" && Time.time - wantedAt[i] > 8f) Unwant(wanted[i]);
        if (fetchState == FetchOpening)
        {
            machineMaterial.SetInt("_FetchDeliver", 0);
            fetchState = FetchIdle;
        }
        else if (fetchState == FetchIdle && openDue.Length > 0)
        {
            // "open this": the address into the answer's texture, and from there into the guest's words
            for (int i = 0; i < 64; i++)
            {
                int at = i * 4;
                fetchPixels[i] = new Color32(at < openDue.Length ? (byte)openDue[at] : (byte)0, at + 1 < openDue.Length ? (byte)openDue[at + 1] : (byte)0,
                                             at + 2 < openDue.Length ? (byte)openDue[at + 2] : (byte)0, at + 3 < openDue.Length ? (byte)openDue[at + 3] : (byte)0);
            }
            hostData.SetPixels32(fetchPixels);
            hostData.Apply(false);
            openCount++;
            machineMaterial.SetTexture("_HostData", hostData);
            machineMaterial.SetInt("_FetchSeq", openCount);
            machineMaterial.SetInt("_FetchLength", openDue.Length);
            machineMaterial.SetInt("_FetchDeliver", 4);
            if (openLabel != null) openLabel.text = "sent";
            openDue = "";
            fetchState = FetchOpening;
        }
        else if (fetchState == FetchDelivering)
        {
            machineMaterial.SetInt("_FetchDeliver", 0);
            fetchAnswered = fetchSeq;
            fetchState = FetchIdle;
            // a picture's texture has been copied: VRChat keeps every one until told. (One asked
            // for as a file is kept, for its further parts, until something else is asked for.)
            if (imageLoader != null && fetchKind != KindFile)
            {
                imageLoader.Dispose();
                imageLoader = null;
                heldAddress = "";
            }
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
            // the part from the offset asked for: 256 KB of it at most
            int partEnd = Mathf.Min(fetchOffset + FetchMost, fetchLength);
            int end = Mathf.Min(fetchPacked + 6000, partEnd);
            for (int at = fetchPacked; at < end; at += 4)
            {
                fetchPixels[(at - fetchOffset) >> 2] = new Color32(fetchBytes[at], at + 1 < fetchLength ? fetchBytes[at + 1] : (byte)0,
                                                                   at + 2 < fetchLength ? fetchBytes[at + 2] : (byte)0,
                                                                   at + 3 < fetchLength ? fetchBytes[at + 3] : (byte)0);
            }
            fetchPacked = end + 3 & ~3;
            if (end < partEnd) return;
            hostData.SetPixels32(fetchPixels);
            hostData.Apply(false);
            fetchState = FetchReady;
        }
        else if (fetchState == FetchReady)
        {
            // a picture's pixels (2), or what a picture asked for as a file holds (3: the control
            // pass looks for the mark, and hands over pixels when there is none)
            bool picture = heldImage && fetchStatus == 200;
            machineMaterial.SetTexture("_HostData", hostData);
            if (picture) machineMaterial.SetTexture("_HostImage", hostImage);
            machineMaterial.SetInt("_FetchOffset", fetchOffset);
            machineMaterial.SetInt("_FetchSeq", (int)(fetchSeq & 0xffffff));
            machineMaterial.SetInt("_FetchLength", fetchLength);
            machineMaterial.SetInt("_FetchStatus", fetchStatus);
            machineMaterial.SetInt("_FetchW", picture ? fetchW : 0);
            machineMaterial.SetInt("_FetchH", picture ? fetchH : 0);
            machineMaterial.SetInt("_FetchDeliver", !picture ? 1 : fetchKind == KindFile ? 3 : 2);
            fetchState = FetchDelivering;
        }
    }

    // ---- one frame ----

    // One pass of the machine's shader from the current state into the other one. With
    // `points` (the commit, once the machine runs) what the cores stored is then drawn there
    // as points, each texel by the commit's own shader.
    private void Pass(int pass, bool points)
    {
        machineMaterial.SetTexture("_SelfTexture2D", current);
        VRCGraphics.Blit(current, other, machineMaterial, pass);
        if (points) Points(false);
        RenderTexture t = current;
        current = other;
        other = t;
    }

    // The cores' stores of this round as points into the other state: the commit's (from the
    // current state and the tick's), or, as copies, the texels the commit made of them, which
    // the buffer the commit did not draw into still lacks.
    private void Points(bool copies)
    {
        pointsMaterial.SetTexture("_SelfTexture2D", current);
        pointsMaterial.SetInt("_Copy", copies ? 1 : 0);
        pointsCamera.targetTexture = other;
        pointsCamera.Render();
    }

    void Update()
    {
        if (!powered || paused) return;
        frames++;

        bool running = initLeft == 0;
        if (running) Inputs();
        if (running) FetchStep();
        if (running) NetFrame();
        int n = !running ? 1 : steady ? SteadyRounds() : rounds;
        machineMaterial.SetInt("_SoundMixed", 0);
        for (int i = 0; i < n; i++)
        {
            // The tick camera draws what the cores keep of their state (core 0's pixels, and a
            // block for each worker with something to run) into its eight targets; the unpack
            // pass puts that where the state has its rows, with the state as it was for the rest;
            // the commit reads it from there and writes the whole state.
            tickMaterial.SetTexture("_SelfTexture2D", current);
            tickCamera.Render();
            machineMaterial.SetTexture("_SelfTexture2D", current);
            VRCGraphics.Blit(current, tickState, machineMaterial, PassUnpack);
            Pass(PassCommit, running);
            // What that round left, into its own row: the console's output is only there
            // until the next tick. The rows are read back together when the frame's rounds are done.
            readbackMaterial.SetTexture("_State", current);
            readbackMaterial.SetInt("_Row", i);
            VRCGraphics.Blit(current, readbackTexture, readbackMaterial);
            if (running)
            {
                // The GPU: draw the list the guest has submitted, if any; the control pass
                // then takes the submit word back and delivers the input.
                gpuMaterial.SetTexture("_State", current);
                gpuCamera.Render();
                // The sound card mixes once a frame, before the last control pass, which then
                // moves its voices up to where the mix began.
                if (i == n - 1 && sound != null) sound.Mix(current, machineMaterial, soundEnabled, soundClock);
                // (the points before the control pass, whose bands are newer than they are)
                Points(true);
                Pass(PassControl, false);
            }
        }
        roundsOf[requestsSent & 63] = n;
        VRCAsyncGPUReadback.Request(readbackTexture, 0, (IUdonEventReceiver)this);
        requestsSent++;
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
        if (stateViewMaterial != null) stateViewMaterial.SetTexture("_State", current);
        volumeMaterial.SetTexture("_State", current);
        if (volumeMaskMaterial != null) volumeMaskMaterial.SetTexture("_State", current);

        if (Time.time - statsAt >= 0.5f) ShowStats();
    }

    private void Inputs()
    {
        // Console input: up to four characters per handshake, first one in the low byte. The
        // guest echoes the tag once it has taken them.
        if (live && sentTag == consumedTag && consoleKeyboard != null)
        {
            int lo = 0, hi = 0, count = 0;
            while (count < UartBurst)
            {
                int c;
                if (charHead != charTail)
                {
                    c = remoteChars[charHead] & 0xff;
                    charHead = (charHead + 1) & 255;
                }
                else if (!inputAway && consoleKeyboard.Count() > 0) c = consoleKeyboard.Pop() & 0xff;
                else break;
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
            while (keys < 4)
            {
                int e;
                if (keyHead != keyTail)
                {
                    e = remoteKeys[keyHead];
                    keyHead = (keyHead + 1) & 255;
                }
                else if (!inputAway && gpuKeyboard.Count() > 0) e = gpuKeyboard.Pop();
                else break;
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
        bool theirs = UseRemote();
        machineMaterial.SetVector("_InputPointer", new Vector4((theirs ? remoteX : pointerX) + 8, (theirs ? remoteY : pointerY) + 8,
                                                               displayWidth + 16, displayHeight + 16));
        machineMaterial.SetInt("_InputButtons", theirs ? remoteButtons : inputAway && !pointerOwn ? 0 : pointerButtons);

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
        Color32 p = row[rowAt + texel * 4 + k];
        return (uint)p.r | (uint)p.g << 8 | (uint)p.b << 16 | (uint)p.a << 24;
    }

    // ---- the network (docs/lan.md) ----

    private void NetUniforms()
    {
        machineMaterial.SetInt("_NetId", netId);
        machineMaterial.SetInt("_NetTxAckLo", (int)(netAcked & 0xffff));
        machineMaterial.SetInt("_NetTxAckHi", (int)(netAcked >> 16));
        machineMaterial.SetInt("_NetRxSeqLo", (int)(netRxSeq & 0xffff));
        machineMaterial.SetInt("_NetRxSeqHi", (int)(netRxSeq >> 16));
        machineMaterial.SetInt("_NetRxCount", netRxCount);
    }

    // Before a frame's rounds: what the last frame delivered is delivered; up to four more
    // packets go into the ring, never more than the guest has room for.
    private void NetFrame()
    {
        netRxSeq += (uint)netRxCount;
        netRxCount = 0;
        uint waiting = netRxSeq - netTaken;
        int queued = (netInTail - netInHead) & (NetQueue - 1);
        if (queued > 0 && netSeen && waiting <= 8)
        {
            int n = Mathf.Min(Mathf.Min(queued, 4), 8 - (int)waiting);
            for (int e = 0; e < n; e++)
            {
                int from = netInHead * NetMost, length = netInLength[netInHead], at = e * 160;
                uint number = netRxSeq + (uint)e + 1;
                netPixels[at] = new Color32((byte)(length & 255), (byte)(length >> 8), 0, 0);
                netPixels[at + 1] = new Color32((byte)(number & 255), (byte)((number >> 8) & 255), (byte)((number >> 16) & 255), (byte)(number >> 24));
                netPixels[at + 2] = new Color32(0, 0, 0, 0);
                netPixels[at + 3] = new Color32(0, 0, 0, 0);
                for (int i = 0; i < length; i += 4)
                    netPixels[at + 4 + i / 4] = new Color32(netIn[from + i], netIn[from + i + 1], netIn[from + i + 2], netIn[from + i + 3]);
                netInHead = (netInHead + 1) & (NetQueue - 1);
            }
            if (n > 0)
            {
                netData.SetPixels32(netPixels);
                netData.Apply(false);
                machineMaterial.SetTexture("_NetData", netData);
                netRxCount = n;
                netReceived += n;
            }
        }
        NetUniforms();
    }

    // A byte of the guest's window, as this round's row has it.
    private int NetByte(int i)
    {
        Color32 p = row[rowAt + 512 + (i >> 2)];
        int k = i & 3;
        return k == 0 ? p.r : k == 1 ? p.g : k == 2 ? p.b : p.a;
    }

    // What a round's readback says of the device: the guest's counts, and the packets in its
    // window that have not been taken, which are then acked.
    private void NetWindow()
    {
        uint sent = Word(64 + 0x3e, 0);
        netTaken = Word(64 + 0x3e, 1);
        if (!netSeen)
        {
            // (a machine that was running already: nothing from before is sent on)
            netSeen = true;
            netAcked = sent;
            return;
        }
        if (sent == netAcked) return;
        int at = 0, found = 0;
        bool whole = false;
        int[] starts = new int[40], lengths = new int[40];
        while (at + 16 <= NetSlot && found < 40)
        {
            int length = NetByte(at) | NetByte(at + 1) << 8 | NetByte(at + 2) << 16 | NetByte(at + 3) << 24;
            uint number = (uint)(NetByte(at + 4) | NetByte(at + 5) << 8 | NetByte(at + 6) << 16 | NetByte(at + 7) << 24);
            if (length < 20 || length > NetMost || at + 16 + length > NetSlot) break;
            uint ahead = number - netAcked;
            if (ahead >= 1 && ahead <= 64) { starts[found] = at + 16; lengths[found] = length; found++; }
            if (number == sent) { whole = true; break; }
            at += 16 + ((length + 15) & ~15);
        }
        if (!whole)
        {
            // half written, or not this run's: wait, but never for ever
            if (++netBad < 200) return;
            found = 0;
        }
        netBad = 0;
        for (int k = 0; k < found; k++)
        {
            int next = (netOutTail + 1) & (NetQueue - 1);
            if (next == netOutHead) break;   // nobody takes them: dropped, as a full wire drops
            int to = netOutTail * NetMost;
            for (int i = 0; i < lengths[k]; i++) netOut[to + i] = (byte)NetByte(starts[k] + i);
            netOutLength[netOutTail] = lengths[k];
            netOutTail = next;
            netSent++;
        }
        netAcked = sent;
    }

    // The machine's number on the network, and its packets for the others (EmuShareHub takes them).
    public int NetId() { return powered ? netId : 0; }
    public int NetWaiting() { return (netOutTail - netOutHead) & (NetQueue - 1); }
    public int NetNextLength() { return netOutHead == netOutTail ? 0 : netOutLength[netOutHead]; }

    public int NetPop(byte[] into, int at)
    {
        if (netOutHead == netOutTail) return 0;
        int length = netOutLength[netOutHead];
        System.Buffer.BlockCopy(netOut, netOutHead * NetMost, into, at, length);
        netOutHead = (netOutHead + 1) & (NetQueue - 1);
        return length;
    }

    // A packet from another machine: this one's if it is addressed to it or to everybody.
    public void NetIn(byte[] from, int at, int length)
    {
        if (!powered || netId == 0 || length < 20 || length > NetMost) return;
        int a = from[at + 16], b = from[at + 17], c = from[at + 18], d = from[at + 19];
        bool mine = a == 10 && b == 0 && c == (netId >> 8) && d == (netId & 255);
        bool all = (a == 10 && b == 0 && c == 255 && d == 255) || (a == 255 && b == 255 && c == 255 && d == 255);
        if (!mine && !all) return;
        int next = (netInTail + 1) & (NetQueue - 1);
        if (next == netInHead) netInHead = (netInHead + 1) & (NetQueue - 1);   // full: the oldest goes
        System.Buffer.BlockCopy(from, at, netIn, netInTail * NetMost, length);
        for (int i = length; i < ((length + 3) & ~3); i++) netIn[netInTail * NetMost + i] = 0;
        netInLength[netInTail] = length;
        netInTail = next;
    }

    // Readbacks complete in the order they were asked for: a frame's rounds, a row each.
    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        int n = roundsOf[requestsDone & 63];
        requestsDone++;
        if (request.hasError || !request.TryGetData(row)) return;
        if (requestsDone <= ignoreUntil) return;
        live = true;
        for (int i = 0; i < n; i++)
        {
            rowAt = i * Words;
            TakeRound();
        }
    }

    // What one round left in the row at rowAt.
    private void TakeRound()
    {

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
        soundEnabled = Word(64 + 0x22, 0) != 0;
        soundClock = Word(64 + 0x23, 0);
        NetWindow();
        uint asked = Word(64 + 16, 0);
        if (fetchState == FetchIdle && asked != Word(64 + 17, 0) && asked != fetchAnswered) FetchAsked(asked);

        if (haveClock)
        {
            uint ran = clock - lastClock;
            if (steady && ticks == TicksPerRound) perRound += (Mathf.Min(ran, TicksPerRound) - perRound) * 0.05f;
            instructions += ran;
            totalInstructions += ran;
        }
        haveClock = true;
        lastClock = clock;
        // the cores' counts of instructions, which the control pass publishes at 0x87000380
        cores = Mathf.Clamp((int)Word(64 + 0x3c, 0), 1, 64);
        coresRunning = Word(64 + 0x3c, 1) & ~Word(64 + 0x3c, 2);   // started, and not asleep on its job word
        coresRunningMore = Word(CoresMore + 12, 0) & ~Word(CoresMore + 12, 1);
        for (int c = 1; c < cores; c++)
        {
            uint now = c < 16 ? Word(64 + 0x38 + c / 4, c % 4) : Word(CoresMore + (c - 16) / 4, c % 4);
            if (haveWorkerClock)
            {
                // (a geometry laid out anew starts the counts again: one that went down is all new)
                uint more = now < workerClock[c] ? now : now - workerClock[c];
                workerInstructions += more;
                totalWorkerInstructions += more;
            }
            workerClock[c] = now;
        }
        haveWorkerClock = cores > 1;
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
        float ips = instructions / dt, fps = frames / dt, workerIps = workerInstructions / dt;
        instructions = 0;
        workerInstructions = 0;
        frames = 0;
        mipsCore = ips / 1000000f;
        mipsAll = (ips + workerIps) / 1000000f;
        speedLine = ips.ToString("N0") + " instructions/s   " + fps.ToString("F0") + " frames/s   "
                    + (fps > 0 ? (ips / fps).ToString("N0") : "0") + " a frame";
        int busy = 0;
        for (int c = 1; c < cores; c++) if (((c < 32 ? coresRunning >> c : coresRunningMore >> (c - 32)) & 1) != 0) busy++;
        string workerLine = cores < 2 ? "one core" :
            (cores - 1) + " workers (" + busy + " running): " + workerIps.ToString("N0") + " instructions/s, all cores " + (ips + workerIps).ToString("N0");
        if (statsText == null) return;
        string display = displayWidth > 0 ? "mode " + displayMode + ", " + displayWidth + " x " + displayHeight : "off";
        statsText.text =
            (paused ? "paused" : "running") + ", " + (steady ? steadyRounds + " x " + ticks + " this frame (steady, at most " + roundCap + " rounds)"
                                                             : rounds + " x " + ticks + " instructions a frame at most") + "\n" +
            speedLine + "\n" +
            workerLine + "\n" +
            "pc " + pc.ToString("x8") + "   stall " + lastStall + "   commits " + lastCommits + "\n" +
            "instructions " + totalInstructions.ToString("N0") + "\n" +
            "display " + display + "   GPU lists drawn " + gpuFrames + "\n" +
            "console: sent " + sentTag + ", taken " + consumedTag + ", waiting " + (consoleKeyboard != null ? consoleKeyboard.Count() : 0) + "\n" +
            "keyboard device: " + keySeq + " events" + (keyboardOwned ? ", owned by a program" : "") + "\n" +
            "sound: " + (sound == null ? "none" : !sound.on ? "off" : !soundEnabled ? "idle" : sound.mixes + " mixes, " + sound.late + " late");
    }
}
