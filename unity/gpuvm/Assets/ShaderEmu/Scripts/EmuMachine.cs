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
// CPUTick and Commit (Machine.shader and its cameras, RAM in one texture written in place),
// a readback of state row 0 and the control words, the GPU's camera drawing the list the
// guest submitted, and the GPU's control pass. Around that this script does what
// rvc_harness.cpp does: boots the image and feeds the console and the input device.
// The machine is local: every player runs their own, and EmuShareHub shows it to the others.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuMachine : UdonSharpBehaviour
{
    public RenderTexture state;             // 2048 x 9280, four 32-bit words a texel: no pass reads what it draws
    public RenderTexture tickState;         // 2048 x 64: the state rows after a tick
    public RenderTexture stage;             // 2048 x 21: what the commit needs of RAM as it was (MachineGather.shader)
    public RenderTexture window;            // 2048 x 11: the rows the control pass draws, then copies into the state
    public Material tickMaterial;           // MachineTickLean.shader: CPUTick, on the tick camera's mesh
    // The tick is a camera's pass into eight targets, eight state texels a pixel, and what the
    // cores stored is committed as points by another camera (docs/machine/multicore.md, "In Unity").
    public Camera tickCamera;               // disabled: rendered from here, once a round
    public RenderTexture[] tickTargets;     // eight of 1040 x 32
    public RenderTexture tickDepth;         // the depth buffer a camera with several targets must be given
    public Camera pointsCamera;             // disabled: rendered from here, twice a round (Commit)
    public Material pointsMaterial;         // MachineCommitPoints.shader, on the points camera's mesh
    public MeshRenderer pointsRenderer;     // that mesh
    public Material gatherMaterial;         // MachineGather.shader, on the gather's mesh on the same layer
    public MeshRenderer gatherRenderer;     // that mesh: the camera draws one of the two at a time
    public Material machineMaterial;        // Machine.shader: Commit (the state rows), GPUControl, Unpack, WindowBack
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
    public RenderTexture readbackTexture;   // 1032 x 32: one pixel per word, a row for each round of a frame

    public EmuSound sound;                  // the sound card's host side, if the world has one
    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard;
    public EmuKeyboard gpuKeyboard;

    // The boot image, twelve textures: RAM r, g, b, a, then ROM, then device tree.
    // A missing texture is left black.
    public Texture2D[] imageTextures;
    public Texture2D blackTexture;

    // ---- gpu_vm (gpu_vm/platform.md): this world's machine is GPU-VM, run as rvc_harness --gvm runs it
    // (harness/apps/gvm_backend11.cpp). state is its RAM; tickMaterial its tick (GvmTick.shader) on the tick
    // camera; machineMaterial GvmMachine.shader; imageTextures its boot RAM's four textures, then the ROM's.
    public RenderTexture gvmRom;              // 8192 wide: the ROM from 0x40000000, four words a texel
    public RenderTexture[] gvmSetA, gvmSetB;  // the tick's state, eight textures a set: a pass reads one, draws the other
    public RenderTexture[] gvmOps;            // the operations' progress, two of 256 x 1
    public RenderTexture gvmPending;          // 1 x 1: how many cores have operations queued after a pass
    public bool gvmTwoRounds;                 // two rounds of the operations every pass, as the harness (a measure)
    public int gvmBusyRounds = 4;             // rounds of the operations a pass while some are queued
    public RenderTexture gvmStage, gvmZone, gvmCtl, gvmTickDepth;
    public Camera gvmPassCamera;              // disabled: the commit's and the operations' draws, one mesh at a time
    public MeshRenderer gvmCommitRenderer, gvmStageRenderer, gvmWriteRenderer, gvmProgRenderer;
    public Material gvmCommitMaterial, gvmStageMaterial, gvmWriteMaterial, gvmProgMaterial, gvmWritebackMaterial;
    public Material gvmTick0Material;         // core 0's tick when the step has CORE0 (GvmTick0), else none
    public Material gvmOpsBlitMaterial;       // a round's plan, stage and progress as blits (GvmOpsBlit)
    public RenderTexture gvmPlan;             // each core's and slot's piece of the round, two texels a slot
    public int gvmEntry = 0x02000000, gvmRamMB = 192;   // the kernel's entry and RAM (unity_boot.txt)
    public int gvmBlockH = 4, gvmStateTexels = 203;    // a core's block rows and state texels (tick.cginc's GEO_BH, NTEX)

    // Pages for the guest's browser (docs/system/fetch.md). A world can only load the addresses it
    // was built with, and the one a visitor types into the field on the panel.
    public string[] siteAddresses;
    public VRCUrl[] siteUrls;
    public InputField[] wantedLinks;       // four addresses the browser wants and the world may not open: to copy
    public VRCUrlInputField[] typedUrls;   // and four fields to paste them into
    public GameObject linksButton;         // by the display, while something is wanted
    public GameObject linksPanel;          // the eight fields, in front of the display
    public VRCUrlInputField openField;     // an address for the machine to open (docs/system/open.md)
    public TextMeshProUGUI openLabel;
    public VRCUrlInputField placeField;    // the same at this visitor's classroom place (EmuStations)
    public TextMeshProUGUI placeLabel;
    public VRCUrlInputField consoleField;  // and the console's strip, whose text is typed at the console or opened
    public TextMeshProUGUI consoleLabel;     // that strip's own line: the link's and the fetch's news
    public Texture2D hostData;         // 256 x 256: an answer's bytes, four a texel
    public Texture2D netData;          // 160 x 4: the network's packets for the guest, a row each (docs/system/lan.md)
    public TextMeshProUGUI fetchLabel;
    public EmuPersist persist;         // the guest's kept folder, as this player's PlayerData (docs/system/persist.md)

    public TextMeshProUGUI statsText;
    public TextMeshProUGUI speedLabel;
    public TextMeshProUGUI powerLabel;
    public TextMeshProUGUI powerSign;  // on the button on the computer itself, and the line under it
    public Image powerPlate;           // that button: red while off, green while running
    public TextMeshProUGUI powerSays;
    public TextMeshProUGUI pauseLabel;
    public Slider speedSlider;         // whole steps: 2,048 instructions a frame, doubled per step
    public TextMeshProUGUI modeLabel;  // on the button that picks how the slider is read
    public Slider tickSlider;          // instructions a tick (a round's pass): 1,024 doubled per step
    public TextMeshProUGUI tickLabel;  // and how much of it core 0 really runs, lately
    // The workers' layout geotune makes at boot (docs/machine/multicore.md, "Packing the small workers"):
    // its own pick, or 2, 4 or 8 columns a small worker; the keys, and what it picked and runs.
    public Image[] workerPlates;
    public Color workerPlain, workerPicked;
    public TextMeshProUGUI workersLabel;
    public float minFrameRate = 45f;   // "steady" gives up instructions before frames come slower than this
    public bool powerOnAtStart;        // off until someone presses Power
    public int hostWants = 3;          // the host flags' bits 0-1 (bit 0: the desktop at boot, rcS); a measuring scene's 0 is the console alone
    // A real client's test (docs/system/fetch.md, "In VRChat itself"): with fetchTest on, the
    // address is loaded as text and as a picture eight seconds after start, and what came is logged.
    public bool fetchTest;
    public VRCUrl fetchTestUrl;
    // For a script of the editor's that measures the machine (unity/gpuvm/minimal: GvmProbe): text set here is typed
    // at the console ('\n' as Enter), and what the console printed is kept, its last 16 K characters.
    [HideInInspector] public string hostInput = "";
    [HideInInspector] public string consoleOut = "";
    [HideInInspector] public int consolePrinted;   // characters printed since power-on
    private float fetchTestAt;
    private VRCImageDownloader fetchTestImages;

    [HideInInspector] public int displayMode, displayWidth, displayHeight;
    [HideInInspector] public bool keyboardOwned;   // a guest program reads the keyboard device

    // Another player's machine is on the screens (EmuShareHub): the wall is not this one's to
    // size, and the keyboards and pointer are not this one's to read.
    [HideInInspector] public bool showRemote, inputAway;
    [HideInInspector] public bool keysOwn;   // the display keyboard is this machine's whatever the wall shows (a sitter's)
    [HideInInspector] public int shownWidth, shownHeight;   // the display the pointer is mapped to
    [HideInInspector] public int pointerX, pointerY, pointerButtons;
    [HideInInspector] public bool pointerOn;            // a beam is on the display
    [HideInInspector] public bool pointerOwn;           // and it is on this player's own classroom tube: this machine's, whatever the wall shows
    [HideInInspector] public float mipsAll, mipsCore;   // millions of instructions a second: every core, and core 0
    // Core 0 as the last readback had it: pc, then x1 to x31. And sixteen bits, one an eighth of
    // a second, newest lowest: set where the registers had changed (the disk lamp's flicker).
    [HideInInspector] public uint[] registers = new uint[32];
    [HideInInspector] public int activity;
    [HideInInspector] public string statsLine = "off";   // the panel's text of this machine, without the registers
    // The frame a 3D program drew last (docs/world/holodeck.md): whether there is one, and whether the
    // program says it is a flat screen (a menu, a level loading) and no world.
    [HideInInspector] public bool worldFrame, flatFrame;
    [HideInInspector] public int gameId;                 // the program whose window has the keyboard (linux/apps/gameid.h)
    [HideInInspector] public uint worldEye;              // its player's eye for the holodeck's scale (seglWorldEye in GLES/segl.h)

    private const int Words = 1032;     // state row 0's 64 texels, the first 64 control texels, 130 of the network's row
    private const int CoresMore = 168;  // what every core ran, in texels: 64 after the network's window, then eight of bits
    private const int MostCores = 256;
    // The network (docs/system/lan.md): the guest's packets out of its window, and packets for it into its ring.
    private const int NetSlot = 640, NetMost = 576, NetQueue = 64;
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
    private const int TicksPerRound = 4096;   // instructions a pass, as the harness's
    private const int MaxRounds = 32;
    private const int PassCommit = 0, PassControl = 1, PassUnpack = 2, PassWindowBack = 3;
    private const int FieldPoints = 2 * 3968;   // the lanes' field's buckets (FIELD_MAX in lean/tick.hlsl)
    private const int CommitPoints = 1 + 258 + 255 * 130 + FieldPoints + 1;   // MachineCommitPoints.shader's mesh
    // To compare (docs/machine/multicore.md, "In Unity"): bit 0 the three cameras as game cameras, which
    // prepare every canvas of the room at each Render; bit 1 the GPU's camera every round;
    // bit 4, a test: that camera in a frame's last round only, whatever comes.
    [HideInInspector] public int oldPaths;
    private int pathsSet = -1;
    private bool gpuDrew = true;       // what the control pass was last told of this round's camera
    private uint gpuFramesSeen;
    private float gpuBusyUntil;        // lists have come lately: the GPU's camera draws every round

    private bool powered, paused;
    private int budget = 32768;        // instructions a frame
    private int rounds = 4, ticks = TicksPerRound;
    private int tickSize = TicksPerRound;   // the tick slider's: a round's pass at most
    private float reached = 1f;             // of a pass's instructions, what core 0 ran, lately
    // Steady: so many instructions a second, however long a frame is, as far as the frame rate allows.
    private bool steady;
    public bool gvmDevicesEachPass;   // the GPU's list and the control words after every pass while lists come (a measure)
    private int workerPreset, workerPresetBooted, geoBits;   // 0 geotune's pick, 1-3 blocks of 2, 4, 8 columns
    private uint geoFirst, geoHybrid;
    private float targetIps = 2000000f;
    private int roundCap = MaxRounds, guardWait, steadyRounds = 1;
    private float smoothDt = 0.011f, carry;
    private float perRound = TicksPerRound;   // instructions a round really ran, lately: many end early
    private int initLeft;
    private int requestsSent, requestsDone, ignoreUntil;
    private bool live;                 // rows read back are of the running machine
    private Color32[] row = new Color32[Words * MaxRounds];
    private int rowAt;                 // where in it the round being taken starts
    private int[] roundsOf = new int[64];   // how many rounds each readback on its way has

    private uint sentTag, consumedTag;
    // The console is words of the control page (docs/machine/console.md): its ring of output bytes and what was taken of it.
    private const int ConTexel = 241;   // the console's words (docs/machine/console.md), then its ring
    private const uint ConRing = 256;
    private uint conWritten;
    private bool conSeen;
    private uint keySeq;
    private uint lastMs;
    private float frameMs, frameLength = 11f;   // this frame's start on the host's clock, and how long frames are lately
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
    private string persistWhat = "";   // what the guest asks of its kept folder ("persist:" and this)
    private bool persistReading;
    private byte[] persistPart = new byte[FetchMost];
    private Color32[] fetchPixels = new Color32[65536];

    private bool soundEnabled;         // the guest has the sound card on, and where its clock is
    private uint soundClock;
    private bool haveClock;
    private uint lastClock, lastCommits, pc, lastStall, gpuFrames;
    private float instructions;        // since the last stats line
    // the worker cores (docs/machine/multicore.md): how many cores, which run, and what the workers ran
    private int cores = 1;
    private uint[] coresRunning = new uint[MostCores / 32];   // a bit a core that is at work
    private uint[] workerClock = new uint[MostCores];
    private bool haveWorkerClock;
    private bool countWorkers;         // this round's row is one the workers' counts are taken from
    private float workersAt;
    private float workerInstructions;  // since the last stats line
    private double totalWorkerInstructions;
    private int frames;
    private float statsAt;
    private double totalInstructions;
    private int rtcSecond = -1;
    private string speedLine = "";
    private bool statsRemote;          // the panel's read-out is another player's machine's (EmuShareHub)
    private uint registerSum;
    private float sampleAt;

    void Start()
    {
        for (int i = 0; i < 4; i++) wanted[i] = "";
        linksPanel.SetActive(false);
        linksButton.SetActive(false);
        if (tickSlider != null) tickSize = 1024 << (int)tickSlider.value;
        if (speedSlider != null) SetBudget(2048 << (int)speedSlider.value);
        ShowLabels();
        if (powerOnAtStart) PowerOn();
        if (fetchTest && fetchTestUrl != null) SendCustomEventDelayedSeconds(nameof(FetchTestRun), 8f);
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

    // The speed slider's step (2,048 instructions a frame doubled per step), and setting it from
    // elsewhere (a classroom tower's slider): the slider's own event applies it.
    public int SpeedStep() { return speedSlider != null ? Mathf.RoundToInt(speedSlider.value) : 4; }
    public void SetSpeedStep(int step)
    {
        if (speedSlider != null) speedSlider.value = Mathf.Clamp(step, (int)speedSlider.minValue, (int)speedSlider.maxValue);
    }

    public void TickChanged()
    {
        tickSize = 1024 << (int)tickSlider.value;
        perRound = tickSize;
        carry = 0f;
        SpeedChanged();
    }

    public void WorkersAuto() { PickWorkers(0); }
    public void WorkersDense() { PickWorkers(1); }
    public void WorkersMedium() { PickWorkers(2); }
    public void WorkersWide() { PickWorkers(3); }

    // Asked for at the next power-on: the layout may change only while no program has workers.
    private void PickWorkers(int preset)
    {
        workerPreset = preset;
        ShowWorkers();
    }

    private static string PresetName(int preset)
    {
        return preset == 1 ? "Dense" : preset == 2 ? "Medium" : preset == 3 ? "Wide" : "Auto";
    }

    // What geotune laid out (the geometry texel at 0x870003d0): its pick and ratio in the first
    // word's high bits, the small workers' cache bits and block width.
    private void ShowWorkers()
    {
        if (workerPlates != null)
            for (int i = 0; i < workerPlates.Length; i++)
                if (workerPlates[i] != null) workerPlates[i].color = i == workerPreset ? workerPicked : workerPlain;
        if (workersLabel == null) return;
        int picked = (int)((geoFirst >> 8) & 0xff), ratio = (int)(geoFirst >> 16), width = (int)((geoHybrid >> 16) & 0xff);
        string auto = picked == 0 ? "Auto: measured at power-on"
                    : "Auto picked " + PresetName(picked == 2 ? 1 : picked == 4 ? 2 : 3) + ", " + (ratio / 100) + "." + (ratio % 100).ToString("00") + " times";
        string running = !powered || picked == 0 ? "" : "\nRunning " + PresetName(width == 2 ? 1 : width == 4 ? 2 : 3);
        if (running != "")
            running += workerPreset != workerPresetBooted ? "; Reset applies " + PresetName(workerPreset)
                     : width == 8 ? ", all workers 2^" + geoBits : ", small workers 2^" + geoBits;
        workersLabel.text = auto + running;
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
        float worth = Mathf.Clamp(perRound, tickSize / 4, tickSize);
        int n = Mathf.Clamp((int)(owed / worth), 1, roundCap);
        int each = owed < tickSize / 2 ? Mathf.Clamp((int)owed, 512, tickSize) : tickSize;
        carry = Mathf.Clamp(owed - n * Mathf.Min(worth, each), -tickSize, tickSize);
        if (each != ticks)
        {
            ticks = each;
            tickMaterial.SetInt("_Ticks", ticks);
        }
        steadyRounds = n;
        return n;
    }

    // A pass ends early at wfi (idle passes count too), pause or a rest, a parallel copy or fill,
    // fence.i, or a full write cache.
    private void ShowTick()
    {
        if (tickLabel == null) return;
        tickLabel.text = tickSize.ToString("N0") + " instructions a tick"
                       + (powered && !paused ? ": " + (reached * 100f).ToString("F0") + "% reached" : "");
    }

    private void SetBudget(int instructionsPerFrame)
    {
        budget = instructionsPerFrame;
        ticks = Mathf.Min(budget, tickSize);
        rounds = Mathf.Clamp(budget / tickSize, 1, MaxRounds);
        tickMaterial.SetInt("_Ticks", ticks);
    }

    private void ShowLabels()
    {
        if (speedLabel != null)
            speedLabel.text = steady ? targetIps.ToString("N0") + " a second, while frames keep to " + minFrameRate.ToString("F0") + " a second"
                                     : "up to " + (rounds * ticks).ToString("N0") + " instructions a frame";
        if (modeLabel != null) modeLabel.text = steady ? "Steady: per second" : "Fixed: per frame";
        ShowTick();
        if (powerLabel != null) powerLabel.text = powered ? "Power off" : "Power on";
        if (powerSign != null) powerSign.text = "POWER";
        if (powerPlate != null) powerPlate.color = powered ? new Color(0.10f, 0.62f, 0.20f) : new Color(0.80f, 0.07f, 0.05f);
        if (powerSays != null) powerSays.text = powered ? "Running. Press to switch off." : "Off. Press to start.";
        if (pauseLabel != null) pauseLabel.text = paused ? "Resume" : "Pause";
        displayMaterial.SetFloat("_Power", powered ? 1f : 0f);
        ShowWorkers();
        ShowDisplay();
    }

    // This machine's own display: its size, 0 while it is off.
    public bool Powered() { return powered; }
    public int OwnWidth() { return powered ? displayWidth : 0; }
    public int OwnHeight() { return powered ? displayHeight : 0; }

    private void ShowDisplay()
    {
        if (state == null) return;
        displayMaterial.SetTexture("_State", state);
        bool theirs = UseRemote();
        displayMaterial.SetVector("_HostPointer", new Vector4(theirs ? remoteX : pointerX, theirs ? remoteY : pointerY,
                                                              theirs || (pointerOn && (!inputAway || pointerOwn)) ? 1 : 0, 0));
        // off: black, as the picture is also the light the display gives the room (LTCGI)
        if (powered || theirs) VRCGraphics.Blit(state, displayTexture, displayMaterial);
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

    // gpu_vm at power-on (gvm_backend11.cpp's init): RAM from the boot image, the ROM's texture from its four, both
    // state sets with core 0 at the kernel's entry, the operations' progress zero, no workers yet.
    private void ApplyImage()
    {
        machineMaterial.SetTexture("_GvmRamR", Pick(0));
        machineMaterial.SetTexture("_GvmRamG", Pick(1));
        machineMaterial.SetTexture("_GvmRamB", Pick(2));
        machineMaterial.SetTexture("_GvmRamA", Pick(3));
        machineMaterial.SetTexture("_GvmRomR", Pick(4));
        machineMaterial.SetTexture("_GvmRomG", Pick(5));
        machineMaterial.SetTexture("_GvmRomB", Pick(6));
        machineMaterial.SetTexture("_GvmRomA", Pick(7));
        VRCGraphics.Blit(blackTexture, state, machineMaterial, GvmInitRam);
        VRCGraphics.Blit(blackTexture, gvmRom, machineMaterial, GvmInitRom);
        machineMaterial.SetInt("_GvmEntryLo", gvmEntry & 0xffff);
        machineMaterial.SetInt("_GvmEntryHi", (gvmEntry >> 16) & 0xffff);
        machineMaterial.SetInt("_GvmBH", gvmBlockH);
        machineMaterial.SetInt("_GvmNTex", gvmStateTexels);
        for (int t = 0; t < 8; t++)
        {
            machineMaterial.SetInt("_GvmTarget", t);
            VRCGraphics.Blit(blackTexture, gvmSetA[t], machineMaterial, GvmInitState);
            VRCGraphics.Blit(blackTexture, gvmSetB[t], machineMaterial, GvmInitState);
        }
        VRCGraphics.Blit(blackTexture, gvmOps[0], machineMaterial, GvmZero);
        VRCGraphics.Blit(blackTexture, gvmOps[1], machineMaterial, GvmZero);
        gvmOpsCur = 0;
        gvmOpsSeen = Time.frameCount;   // (the boot's first copies come before any readback says so)
        gvmCur = 0;
        gvmTime = 0;
        gvmPasses = 0;
        gvmWorkers = 0;
        GvmUniforms();
        for (int i = 0; i < 8; i++)
        {
            gvmBuffersA[i] = gvmSetA[i].colorBuffer;
            gvmBuffersB[i] = gvmSetB[i].colorBuffer;
        }
    }

    // The machine's numbers and textures on its materials, every frame: the editor drops numbers set at run time
    // when a shader is imported in play mode.
    private void GvmUniforms()
    {
        int rows = state.height;
        Material[] tick = GvmTickMaterials();
        for (int i = 0; i < tick.Length; i++)
        {
            tick[i].SetInt("_RamEnd", 0x2000000 + (gvmRamMB << 20));
            tick[i].SetInt("_RamRows", rows);
            tick[i].SetInt("_Flags", 0);
            tick[i].SetTexture("_Ram", state);
            tick[i].SetTexture("_Rom", gvmRom);
        }
        GvmCores();
        gvmWriteMaterial.SetTexture("_Stage", gvmStage);
        gvmWriteMaterial.SetTexture("_Plan", gvmPlan);
        gvmOpsBlitMaterial.SetTexture("_Plan", gvmPlan);
        machineMaterial.SetInt("_GvmRows", rows);
        machineMaterial.SetTexture("_Ram", state);
        machineMaterial.SetTexture("_Zone", gvmZone);
        machineMaterial.SetTexture("_GvmPending", gvmPending);
        machineMaterial.SetInt("_GvmParkLo", 0x470);   // GVM_FW_PARK, GVM_FW_PARK_END (gpu_vm/platform/worker_fw.h)
        machineMaterial.SetInt("_GvmParkHi", 0x520);
        machineMaterial.SetInt("_GvmGeo", 1);
        machineMaterial.SetInt("_GvmListDrawn", 1);
        gvmWritebackMaterial.SetInt("_GvmRows", rows);
        gvmWritebackMaterial.SetTexture("_GvmCtl", gvmCtl);
        gvmWritebackMaterial.SetTexture("_GpuTarget", gpuCamera.targetTexture);
        gpuMaterial.SetTexture("_State", state);
        gpuMaterial.SetTexture("_GvmRom", gvmRom);
        if (sound != null) sound.mixMaterial.SetTexture("_GvmRom", gvmRom);   // samples are played from where they lie in the ROM
        tickMaterial.SetInt("_Ticks", ticks);
        RenderTexture target = gpuCamera.targetTexture;
        machineMaterial.SetInt("_HostFlags", (hostWants & 3) | workerPresetBooted << 2 | (target.width / 16) << 8 | (target.height / 16) << 16);
    }

    // ---- gpu_vm's passes ----
    private const int GvmInitRam = 0, GvmInitRom = 1, GvmInitState = 2, GvmWorkersPass = 3, GvmControl = 4, GvmZoneBack = 5;
    private const int GvmCtlCopy = 6, GvmReadback = 7, GvmZero = 8;
    private const int GvmNone = 8192;   // no second zone in the control pass's row
    private int gvmCur, gvmWorkers;     // the set the next pass draws; the worker cores the kernel laid out
    private int gvmOpsCur, gvmOpsSeen;  // the progress texture the next round reads; the frame operations were last queued
    private double gvmTime;             // TIME, 100 kHz
    private uint gvmPasses;
    private RenderBuffer[] gvmBuffersA = new RenderBuffer[8], gvmBuffersB = new RenderBuffer[8];
    private string[] gvmStateNames = { "_S0", "_S1", "_S2", "_S3", "_S4", "_S5", "_S6", "_S7" };
    private string[] gvmReadNames = { "_GvmR0", "_GvmR1", "_GvmR2", "_GvmR3", "_GvmR4", "_GvmR5", "_GvmR6", "_GvmR7" };

    private Material[] GvmTickMaterials()
    {
        if (gvmTick0Material == null)
        {
            Material[] m = { tickMaterial, gvmCommitMaterial, gvmWriteMaterial, gvmOpsBlitMaterial };
            return m;
        }
        Material[] m0 = { tickMaterial, gvmCommitMaterial, gvmWriteMaterial, gvmOpsBlitMaterial, gvmTick0Material };
        return m0;
    }

    private RenderTexture[] GvmSet(int k) { return k == 0 ? gvmSetA : gvmSetB; }

    private void GvmState(Material m, RenderTexture[] set, string[] names)
    {
        for (int t = 0; t < 8; t++) m.SetTexture(names[t], set[t]);
    }

    // The cores the passes draw: core 0 and the workers.
    private void GvmCores()
    {
        Material[] tick = GvmTickMaterials();
        for (int i = 0; i < tick.Length; i++) tick[i].SetInt("_NCores", 1 + gvmWorkers);
        machineMaterial.SetInt("_GvmWorkers", gvmWorkers);
        cores = 1 + gvmWorkers;
    }

    // One of the passive passes' draws: its mesh alone on the pass camera's layer, into target.
    private void GvmDraw(MeshRenderer mesh, RenderTexture target)
    {
        mesh.enabled = true;
        gvmPassCamera.targetTexture = target;
        gvmPassCamera.Render();
        mesh.enabled = false;
    }

    private void GvmTickUniforms(Material m, int lo16, double lo, double hi, RenderTexture[] read)
    {
        m.SetInt("_TimeL0", lo16);
        m.SetInt("_TimeL1", (int)((lo - lo16) / 65536.0));
        m.SetInt("_TimeH0", (int)(hi % 65536.0));
        m.SetInt("_TimeH1", 0);
        GvmState(m, read, gvmStateNames);
        m.SetTexture("_OpState", gvmOps[gvmOpsCur]);
        m.SetInt("_Ticks", ticks);
    }

    // A pass (gpu_vm/tick/STATE.md): the tick, the commit's four draws, two rounds of the operations.
    private void GvmPass(double wall)
    {
        gvmTime = Math.Max(gvmTime + 1, wall);
        double hi = Math.Floor(gvmTime / 4294967296.0), lo = gvmTime - hi * 4294967296.0;
        int lo16 = (int)(lo % 65536.0);
        RenderTexture[] read = GvmSet(gvmCur ^ 1), drawn = GvmSet(gvmCur);
        GvmTickUniforms(tickMaterial, lo16, lo, hi, read);
        if (gvmTick0Material != null) GvmTickUniforms(gvmTick0Material, lo16, lo, hi, read);
        tickCamera.SetTargetBuffers(gvmCur == 0 ? gvmBuffersA : gvmBuffersB, gvmTickDepth.depthBuffer);
        tickCamera.Render();
        GvmState(gvmCommitMaterial, drawn, gvmStateNames);
        GvmDraw(gvmCommitRenderer, state);
        GvmState(gvmOpsBlitMaterial, drawn, gvmStateNames);
        // more rounds only while operations were queued lately (texel 12 of the readback): a round is a camera's render,
        // and a core waiting on its copy loses a whole pass
        int opRounds = gvmTwoRounds ? 2 : Time.frameCount - gvmOpsSeen < 4 ? gvmBusyRounds : 1;
        for (int oc = 0; oc < opRounds; oc++)
        {
            // only the write scatters, a camera's render; the plan, the stage and the progress are blits
            gvmOpsBlitMaterial.SetTexture("_OpState", gvmOps[gvmOpsCur]);
            VRCGraphics.Blit(blackTexture, gvmPlan, gvmOpsBlitMaterial, 0);
            VRCGraphics.Blit(blackTexture, gvmStage, gvmOpsBlitMaterial, 1);
            GvmDraw(gvmWriteRenderer, state);
            gvmOpsCur ^= 1;
            VRCGraphics.Blit(blackTexture, gvmOps[gvmOpsCur], gvmOpsBlitMaterial, 2);
        }
        gvmOpsBlitMaterial.SetTexture("_OpState", gvmOps[gvmOpsCur]);
        VRCGraphics.Blit(blackTexture, gvmPending, gvmOpsBlitMaterial, 3);
        gvmCur ^= 1;
        gvmPasses++;
    }

    // A zone of the control words (gvm_backend11.cpp's zone, zonePair): RAM texels [t, t + n), and [t2, t2 + n2) in
    // the same row of the scratch when n2 > 0, drawn reading RAM and then copied into it.
    private void GvmZone(int t, int n, int t2, int n2)
    {
        int x = t % 8192, y = t / 8192, w = n, h = 1;
        if (x + n > 8192)
        {
            h = (x + n + 8191) / 8192;
            x = 0;
            w = 8192;
        }
        machineMaterial.SetInt("_GvmZoneX", x);
        machineMaterial.SetInt("_GvmZoneY", y);
        machineMaterial.SetInt("_GvmZoneW", n2 > 0 ? n + n2 : w);
        machineMaterial.SetInt("_GvmZoneH", h);
        machineMaterial.SetInt("_GvmZoneSplit", n2 > 0 ? n : GvmNone);
        machineMaterial.SetInt("_GvmZone2X", t2 % 8192);
        machineMaterial.SetInt("_GvmZone2Y", t2 / 8192);
        VRCGraphics.Blit(blackTexture, gvmZone, machineMaterial, GvmControl);
        VRCGraphics.Blit(blackTexture, state, machineMaterial, GvmZoneBack);
    }

    // The copy the last list asked for, the list (the control pass takes the submit word back), the sound card in a
    // frame's last pass (before the control zone moves its voices on), and the control words with MC_STATS_ALL.
    private void GvmDevices(int i, int n)
    {
        VRCGraphics.Blit(blackTexture, gvmCtl, machineMaterial, GvmCtlCopy);
        for (int c = 0; c < 4; c++) VRCGraphics.Blit(blackTexture, state, gvmWritebackMaterial, c);
        gpuMaterial.SetTexture("_State", state);
        gpuCamera.Render();
        if (i == n - 1 && sound != null) sound.Mix(state, machineMaterial, soundEnabled, soundClock);
        HostClock(i, n);
        machineMaterial.SetTexture("_GvmS0", GvmSet(gvmCur ^ 1)[0]);
        machineMaterial.SetTexture("_GvmS3", GvmSet(gvmCur ^ 1)[3]);
        GvmZone(0x100000, 256, 0x16ba00, gvmWorkers > 0 ? 72 : 0);
    }

    private void GvmPassesOn(Material m)
    {
        m.SetInt("_GvmPassesLo", (int)(gvmPasses & 0xffff));
        m.SetInt("_GvmPassesHi", (int)(gvmPasses >> 16));
    }

    // The workers the kernel laid out (the control words' texel 0x3d), started at the firmware or halted
    // (gvm_backend11.cpp's setWorkers), in the set the next pass reads.
    private void GvmLayout()
    {
        uint g0 = Word(64 + 0x3d, 0), g1 = Word(64 + 0x3d, 1), g2 = Word(64 + 0x3d, 2);
        int want = (g0 & 1) != 0 || g2 < 3 || g2 > 6 ? 0 : g1 > 255u ? 255 : (int)g1;
        if (want == gvmWorkers) return;
        machineMaterial.SetInt("_GvmFrom", gvmWorkers);
        machineMaterial.SetInt("_GvmTo", want);
        RenderTexture[] next = GvmSet(gvmCur ^ 1);
        for (int t = 0; t < 8; t++)
        {
            machineMaterial.SetInt("_GvmTarget", t);
            VRCGraphics.Blit(blackTexture, next[t], machineMaterial, GvmWorkersPass);
        }
        Debug.Log("[GVM] " + want + " worker cores (the kernel's layout)");
        gvmWorkers = want;
        GvmCores();
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
        initLeft = 0;                 // (gpu_vm's start is made at once: ApplyImage)
        ignoreUntil = requestsSent;   // readbacks on their way are of the machine before
        sentTag = 0;
        consumedTag = 0;
        conSeen = false;
        charHead = charTail = keyHead = keyTail = 0;
        zapHead = zapTail = 0;   // what waited for the machine before is not this one's: sixteen flips met a reset machine
        totalInstructions = 0;
        totalWorkerInstructions = 0;
        haveWorkerClock = false;
        displayMode = 0;
        displayWidth = 0;
        displayHeight = 0;
        gpuDrew = true;
        gpuBusyUntil = 0f;
        tickMaterial.SetInt("_Ticks", ticks);
        machineMaterial.SetInt("_UartInLo", 0);
        machineMaterial.SetInt("_UartInHi", 0);
        machineMaterial.SetInt("_UdonUARTInTag", 0);
        machineMaterial.SetInt("_InputKeyCount", 0);
        // the host flags: the desktop at boot, and the largest screen the GPU's target can show
        // (in 16s of pixels: the word must stay within what a material's number holds exactly)
        RenderTexture target = gpuCamera.targetTexture;
        // (bits 2-3: the workers' preset, read by geotune)
        workerPresetBooted = workerPreset;
        geoFirst = geoHybrid = 0;
        machineMaterial.SetInt("_HostFlags", (hostWants & 3) | workerPreset << 2 | (target.width / 16) << 8 | (target.height / 16) << 16);
        VRCPlayerApi player = Networking.LocalPlayer;
        tickMaterial.SetInt("_PlayerID", player != null ? player.playerId : 0);
        if (consoleKeyboard != null) consoleKeyboard.Flush();
        if (gpuKeyboard != null) gpuKeyboard.Flush();
        if (terminal != null) terminal.Clear();
        consoleOut = "";
        consolePrinted = 0;
        fetchState = FetchIdle;
        fetchAnswered = 0;
        heldAddress = "";
        persistWhat = "";
        openDue = "";
        openCount = 0;
        machineMaterial.SetInt("_FetchDeliver", 0);
        // the network: a new machine counts from nothing, and so does its host
        netOutHead = netOutTail = netInHead = netInTail = 0;
        netAcked = netTaken = netRxSeq = 0;
        netRxCount = netBad = 0;
        netSeen = false;
        netId = player != null ? player.playerId & 0xffff : 0;
        lanOut = lanMine = lanOthers = lanOff = lanFull = lanGiven = lanDetail = lanSaid = 0;
        Debug.Log("[Lan] this machine is on: 10.0." + (netId >> 8) + "." + (netId & 255) + " (player " + netId + " of "
                  + VRCPlayerApi.GetPlayerCount() + " here)");
        NetUniforms();
        ShowLabels();
    }

    public void PowerOff()
    {
        if (powered) Debug.Log("[Lan] this machine is off: packets for it are dropped");
        powered = false;
        paused = false;
        soundEnabled = false;
        if (sound != null) sound.Quiet();
        ShowLabels();
        Say("The computer is off.\r\n");
        statsLine = "off";
        if (statsText != null && !statsRemote) statsText.text = "off";
        mipsAll = 0f;
        mipsCore = 0f;
        activity = 0;
        gameId = 0;
    }

    public bool IsOn()
    {
        return powered;
    }

    // ---- the danger zone: the host's hand in RAM, one commit each (src/zap.h, docs/world/danger.md) ----
    private int[] zaps = new int[16 * 9];
    private int zapHead, zapTail;

    // Flips bits of RAM in a commit to come: kind 1 one word of texel (x, y), kind 2 a square
    // reaching `reach` texels each way. False when sixteen wait already.
    public bool Zap(int kind, int x, int y, int reach, int word, int maskLo, int maskHi, int seed, int zones)
    {
        if (!powered || zapTail - zapHead >= 16) return false;
        int at = (zapTail & 15) * 9;
        zaps[at] = kind;
        zaps[at + 1] = x;
        zaps[at + 2] = y;
        zaps[at + 3] = reach;
        zaps[at + 4] = word;
        zaps[at + 5] = maskLo;
        zaps[at + 6] = maskHi;
        zaps[at + 7] = seed;
        zaps[at + 8] = zones;
        zapTail++;
        return true;
    }

    // The next one waiting goes to the commit about to be drawn; true if there was one.
    private bool ZapNext()
    {
        if (zapHead == zapTail) return false;
        int at = (zapHead & 15) * 9;
        zapHead++;
        ZapSet(gatherMaterial, at);   // (the square as it was, into the stage)
        ZapSet(pointsMaterial, at);
        return true;
    }

    private void ZapSet(Material m, int at)
    {
        m.SetInt("_ZapKind", zaps[at]);
        m.SetInt("_ZapX", zaps[at + 1]);
        m.SetInt("_ZapY", zaps[at + 2]);
        m.SetInt("_ZapR", zaps[at + 3]);
        m.SetInt("_ZapWord", zaps[at + 4]);
        m.SetInt("_ZapMaskLo", zaps[at + 5]);
        m.SetInt("_ZapMaskHi", zaps[at + 6]);
        m.SetInt("_ZapSeed", zaps[at + 7]);
        m.SetInt("_ZapZones", zaps[at + 8]);
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

    // Characters (or key events) typed that way and not taken yet.
    public int RemoteWaiting(bool keys)
    {
        return keys ? (keyTail - keyHead) & 255 : (charTail - charHead) & 255;
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
        for (int i = 0; i < Mathf.Min(Mathf.Abs(notches), 8); i++)
        {
            // (over this visitor's own tube or holodeck screen it is their machine's, as the pointer is)
            if (inputAway && pointerOwn) RemoteKey((notches > 0 ? 0x3fe : 0x3ff) | 65536);
            else gpuKeyboard.PushEvent((notches > 0 ? 0x3fe : 0x3ff) | 65536);
        }
    }

    // ---- pages for the guest (docs/system/fetch.md) ----

    // The guest asked for an address: words 16 and 17 of the control block are the request
    // and the answer, the address itself follows from texel 18. Kind 1 is a picture.
    private void FetchAsked(uint seq)
    {
        int length = (int)Mathf.Min(Word(64 + 16, 1), 255);
        string address = "";
        for (int i = 0; i < length; i++) address += (char)((Word(64 + 18 + i / 16, (i / 4) % 4) >> (8 * (i % 4))) & 0xff);
        fetchSeq = seq;
        fetchKind = Small(Word(64 + 16, 2));
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
        if (address == "host:name")
        {
            // the player's name, for the guest's programs (docs/system/persist.md, "The player's name"):
            // what of it a shell and a game can carry, printable ASCII, the rest '_'
            VRCPlayerApi player = Networking.LocalPlayer;
            string name = Utilities.IsValid(player) ? player.displayName : "player";
            int named = Mathf.Min(name.Length, 64);
            for (int i = 0; i < 256; i++) persistPart[i] = 0;
            for (int i = 0; i < named; i++) persistPart[i] = name[i] > ' ' && name[i] < 127 ? (byte)name[i] : (byte)'_';
            hostData.LoadRawTextureData(persistPart);
            hostData.Apply(false);
            heldImage = false;
            fetchOffset = 0;
            fetchLength = named;
            fetchStatus = 200;
            fetchState = FetchReady;
            return;
        }
        if (address == "power:off")
        {
            // the guest has shut down (nxoff): the machine is switched off, as by the panel's key
            fetchState = FetchIdle;
            PowerOff();
            return;
        }
        if (address.StartsWith("persist:"))
        {
            // the guest's kept folder: answered from PlayerData, once VRChat has brought it
            heldImage = false;
            persistWhat = address.Substring(8);
            persistReading = false;
            fetchState = FetchLoading;
            fetchStarted = Time.time;
            if (persist == null)
            {
                // (a world without the storage sign keeps nothing: said at once)
                persistWhat = "";
                fetchStatus = 404;
                fetchState = FetchReady;
            }
            return;
        }
        VRCUrl url = null;
        // an address a visitor has put into one of the fields is theirs to open: the very one
        // the guest asks for, or the first field's for "world:typed"
        for (int i = 0; i < typedUrls.Length; i++)
            if (typedUrls[i].GetUrl().Get() == address || (i == 0 && address == "world:typed")) url = typedUrls[i].GetUrl();
        if (openField != null && openField.GetUrl().Get() == address) url = openField.GetUrl();
        if (placeField != null && placeField.GetUrl().Get() == address) url = placeField.GetUrl();
        if (consoleField != null && consoleField.GetUrl().Get() == address) url = consoleField.GetUrl();
        // the addresses the world was built with are in the order of their characters' numbers
        // (the builder sorts them): thousands of them, found in a dozen steps
        int siteLow = 0, siteHigh = siteAddresses.Length;
        while (siteLow < siteHigh)
        {
            int middle = (siteLow + siteHigh) / 2;
            int order = string.CompareOrdinal(address, siteAddresses[middle]);
            if (order == 0) { url = siteUrls[middle]; break; }
            if (order < 0) siteHigh = middle;
            else siteLow = middle + 1;
        }
        // VRChat throws on an address that is not https, and then calls neither event below
        if (address != "world:typed" && !address.StartsWith("https://"))
        {
            fetchStatus = 3;   // never, in VRChat
            fetchState = FetchReady;
            FetchSays("VRChat opens only https addresses");
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
        FetchSays("Loading " + address);
        heldAddress = address;
        heldImage = fetchKind == 1 || (fetchKind == KindFile && IsPicture(address));
        if (heldImage)
        {
            // a picture: VRChat fetches and decodes it into a texture, whose pixels the control
            // pass copies into the guest's memory as they are, 65,536 a part. Asked for as a file
            // it may carry one in its pixels (docs/system/fetch.md).
            if (imageLoader == null) imageLoader = new VRCImageDownloader();
            TextureInfo info = new TextureInfo();
            info.GenerateMipMaps = false;
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

    // What a fetch has come to: on the wall's panel, and at this visitor's classroom place.
    private void FetchSays(string text)
    {
        if (fetchLabel != null) fetchLabel.text = text;
        if (placeLabel != null) placeLabel.text = text;
    }

    // The button by a link field: the machine is told to open the address in it.
    public void OpenLink() { Open(openField, openLabel); }
    public void OpenPlaceLink() { Open(placeField, placeLabel); }
    public void OpenConsoleLink() { Open(consoleField, consoleLabel); }

    private TextMeshProUGUI openSays;      // the label of the field the last address came from

    private void Open(VRCUrlInputField field, TextMeshProUGUI label)
    {
        if (field == null) return;
        openSays = label;
        string address = field.GetUrl().Get();
        if (address == null || address.Length == 0 || !powered)
        {
            if (label != null) label.text = powered ? "paste an address" : "computer is off";
            return;
        }
        if (!address.StartsWith("https://"))
        {
            if (label != null) label.text = "https only";
            return;
        }
        if (displayWidth == 0)
        {
            // the desktop's bar is what takes the address (docs/system/open.md): nobody would
            if (label != null) label.text = "start the desktop first";
            return;
        }
        openDue =address.Length > 255 ? address.Substring(0, 255) : address;
        if (label != null) label.text = address.Length > 255 ? "too long (255)" : "opening...";
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
        if (any) FetchSays("The browser needs addresses pasted: the button by the display");
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

    public void FetchTestRun()
    {
        fetchTestAt = Time.time;
        Debug.Log("[FetchTest] loading " + fetchTestUrl.Get() + " as text and as a picture");
        VRCStringDownloader.LoadUrl(fetchTestUrl, (IUdonEventReceiver)this);
        fetchTestImages = new VRCImageDownloader();
        TextureInfo info = new TextureInfo();
        info.GenerateMipMaps = false;
        fetchTestImages.DownloadImage(fetchTestUrl, null, (IUdonEventReceiver)this, info);
    }

    private bool FetchTestOf(VRCUrl url)
    {
        return fetchTest && fetchTestUrl != null && url != null && url.Get() == fetchTestUrl.Get() && fetchTestAt > 0f;
    }

    public override void OnImageLoadSuccess(IVRCImageDownload result)
    {
        if (FetchTestOf(result.Url))
            Debug.Log("[FetchTest] picture: " + result.Result.width + " x " + result.Result.height + " after " + (Time.time - fetchTestAt).ToString("F1") + " s");
        if (fetchState != FetchLoading) return;
        hostImage = result.Result;
        // every pixel, four bytes each: the guest asks for the parts after the first
        int w = hostImage.width, h = hostImage.height;
        fetchW = w;
        fetchH = h;
        fetchLength = fetchW * fetchH * 4;
        fetchStatus = 200;
        fetchState = FetchReady;
        FetchSays("Loaded a picture, " + w + " x " + h);
    }

    public override void OnImageLoadError(IVRCImageDownload result)
    {
        if (FetchTestOf(result.Url))
            Debug.Log("[FetchTest] picture failed: " + result.Error + " " + result.ErrorMessage + " after " + (Time.time - fetchTestAt).ToString("F1") + " s");
        if (fetchState != FetchLoading) return;
        fetchLength = 0;
        // a server's refusal reads "HTTP/1.1 404 Not Found": its code, else 415 (not a picture)
        fetchStatus = 415;
        string message = result.ErrorMessage;
        int at = message == null ? -1 : message.IndexOf("HTTP/1.1 ");
        int code;
        if (at >= 0 && message.Length >= at + 12 && int.TryParse(message.Substring(at + 9, 3), out code) && code >= 400)
            fetchStatus = code;
        fetchState = FetchReady;
        FetchSays("Picture not loaded: " + message);
    }

    public override void OnStringLoadSuccess(IVRCStringDownload result)
    {
        if (FetchTestOf(result.Url))
        {
            byte[] got = result.ResultBytes;
            string head = "";
            for (int i = 0; got != null && i < Mathf.Min(8, got.Length); i++) head += got[i].ToString("x2") + " ";
            Debug.Log("[FetchTest] text: " + (got != null ? got.Length : -1) + " bytes, first " + head + "after " + (Time.time - fetchTestAt).ToString("F1") + " s");
        }
        if (fetchState != FetchLoading) return;
        // (a page, a sheet or a file: its whole length is said, and it is handed over in parts
        // of 256 KB as the guest asks for them; a browser of before takes the first and no more)
        fetchBytes = result.ResultBytes;
        fetchLength = fetchBytes.Length;
        fetchStatus = 200;
        // (the part asked for, when a later part had to be loaded again)
        fetchOffset = Mathf.Min(fetchOffset, fetchLength) & ~3;
        fetchPacked = fetchOffset;
        fetchState = FetchPacking;
        FetchSays("Loaded " + fetchLength.ToString("N0") + " bytes");
    }

    public override void OnStringLoadError(IVRCStringDownload result)
    {
        if (FetchTestOf(result.Url))
            Debug.Log("[FetchTest] text failed: " + result.ErrorCode + " " + result.Error + " after " + (Time.time - fetchTestAt).ToString("F1") + " s");
        if (fetchState != FetchLoading) return;
        fetchLength = 0;
        fetchStatus = result.ErrorCode > 1 && result.ErrorCode != 200 ? result.ErrorCode : 2;
        fetchState = FetchReady;
        FetchSays("Not loaded: " + result.Error);
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
            if (openSays != null) openSays.text = "sent";
            openDue = "";
            fetchState = FetchOpening;
        }
        else if (fetchState == FetchDelivering)
        {
            machineMaterial.SetInt("_FetchDeliver", 0);
            fetchAnswered = fetchSeq;
            fetchState = FetchIdle;
            // a picture's texture has been copied: VRChat keeps every one until told. (One with
            // parts still to come is kept until something else is asked for.)
            if (imageLoader != null && fetchKind != KindFile && fetchOffset + FetchMost >= fetchLength)
            {
                imageLoader.Dispose();
                imageLoader = null;
                heldAddress = "";
            }
        }
        else if (fetchState == FetchLoading && persistWhat.Length > 0 && persist != null && persist.Ready())
        {
            PersistStep();
        }
        else if (fetchState == FetchLoading && Time.time - fetchStarted > 20f)
        {
            persistWhat = "";
            // no answer and no error: tell the guest, and be free for its next request
            fetchLength = 0;
            fetchStatus = 2;
            fetchState = FetchReady;
            FetchSays("Not loaded: VRChat gave no answer in 20 seconds");
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

    // The guest's kept folder (docs/system/persist.md): what it asks is answered as a fetch is, its
    // bytes into the answer's texture as they are. A save waits for the readback of what the
    // guest left in the fetch's memory, whose length is the request's offset word.
    private void PersistStep()
    {
        int length = 0;
        fetchStatus = 200;
        if (persistWhat == "save")
        {
            if (!persistReading)
            {
                persistReading = true;
                persist.Read(state, fetchOffset);
            }
            if (persist.reading) return;
            fetchStatus = persist.saveStatus;
        }
        else if (persistWhat == "load" || persistWhat == "load!")
        {
            byte[] blob = persist.Blob();
            if (persistWhat == "load" && !persist.Auto()) fetchStatus = 403;
            else if (blob.Length == 0 || blob.Length > FetchMost) fetchStatus = 404;
            else
            {
                System.Buffer.BlockCopy(blob, 0, persistPart, 0, blob.Length);
                length = blob.Length;
            }
        }
        else if (persistWhat == "clear") persist.Forget();
        else if (persistWhat == "auto=0" || persistWhat == "auto=1") persist.SetAuto(persistWhat == "auto=1");
        else if (persistWhat == "state")
        {
            string text = "stored " + persist.Blob().Length + "\nlimit " + EmuPersist.Limit + "\nauto " + (persist.Auto() ? 1 : 0) + "\n";
            for (length = 0; length < text.Length; length++) persistPart[length] = (byte)text[length];
        }
        else fetchStatus = 400;
        if (length > 0)
        {
            hostData.LoadRawTextureData(persistPart);
            hostData.Apply(false);
        }
        FetchSays(fetchStatus == 200 ? "The kept folder: " + persistWhat : "The kept folder: " + persistWhat + " (" + fetchStatus + ")");
        fetchLength = length;
        fetchOffset = 0;
        persistWhat = "";
        fetchState = FetchReady;
    }

    // The storage sign's Save: the guest is told to pack its folder, as it is told to open an address.
    public bool AskSave()
    {
        if (!powered || displayWidth == 0) return false;
        openSays = null;
        openDue = "persist:save";
        return true;
    }

    // ---- one frame ----

    // A preview camera prepares none of the room's canvases when it renders (82 of them: half
    // of a Render's time on the main thread), and draws its own mesh all the same.
    private void SetPaths()
    {
        pathsSet = oldPaths;
        CameraType kind = (oldPaths & 1) != 0 ? CameraType.Game : CameraType.Preview;
        tickCamera.cameraType = kind;
        gvmPassCamera.cameraType = kind;
        gpuCamera.cameraType = kind;
    }

    void Update()
    {
        LanSay();
        if (!powered || paused)
        {
            activity = 0;
            return;
        }
        frames++;
        GvmUniforms();
        if (pathsSet != oldPaths) SetPaths();
        // While no list has come for a second the GPU's camera draws in a frame's last round
        // only, and the control pass takes a list in no other: the first one waits a frame at most.
        bool gpuAlways = initLeft != 0 || (oldPaths & 2) != 0 || ((oldPaths & 16) == 0 && Time.time < gpuBusyUntil);

        bool running = initLeft == 0;
        if (running && hostInput.Length > 0)
        {
            // as much as the ring of typed characters has room for (256 less one), the rest a later frame
            int typed = Mathf.Clamp(254 - ((charTail - charHead) & 255), 0, hostInput.Length);
            for (int i = 0; i < typed; i++) RemoteChar(hostInput[i] == '\n' ? '\r' : hostInput[i]);
            hostInput = hostInput.Substring(typed);
        }
        if (running) Inputs();
        if (running) FetchStep();
        if (running) NetFrame();
        int n = !running ? 1 : steady ? SteadyRounds() : rounds;
        machineMaterial.SetInt("_SoundMixed", 0);
        // gpu_vm's frame (gvm_backend11.cpp's frame): n passes, each read back into its row of the readback (the
        // console's ring and the control words as it left them), then the devices once
        double wall = (double)Time.timeSinceLevelLoad * 100000.0;
        for (int i = 0; i < n; i++)
        {
            GvmPass(wall);
            GvmState(machineMaterial, GvmSet(gvmCur ^ 1), gvmReadNames);
            GvmPassesOn(machineMaterial);
            machineMaterial.SetInt("_Row", i);
            VRCGraphics.Blit(blackTexture, readbackTexture, machineMaterial, GvmReadback);
            // while lists come, the GPU and the control words after every pass, as the harness does: a list then
            // waits a pass, not a frame
            if (running && i < n - 1 && gpuAlways && gvmDevicesEachPass) GvmDevices(i, n);
        }
        if (running)
        {
            // then the console's host words, the network's ring and the fetch's rows (RISC-V indices less 0x600000)
            GvmDevices(n - 1, n);
            GvmZone(0x16b828, 1, 0, 0);
            GvmZone(0x16b840, 320, 0, 0);
            GvmZone(0x16c000, 0x4000, 0, 0);
        }
        roundsOf[requestsSent & 63] = n;
        VRCAsyncGPUReadback.Request(readbackTexture, 0, (IUdonEventReceiver)this);
        requestsSent++;
        // whoever shows the state reads the texture this frame ended in
        gpuMaterial.SetTexture("_State", state);
        ShowDisplay();
        heatMaterial.SetTexture("_State", state);
        if (stateViewMaterial != null) stateViewMaterial.SetTexture("_State", state);
        volumeMaterial.SetTexture("_State", state);
        if (volumeMaskMaterial != null) volumeMaskMaterial.SetTexture("_State", state);

        if (consoleOut.Length > 32768) consoleOut = consoleOut.Substring(consoleOut.Length - 16384);
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
                machineMaterial.SetInt("_UartInLo", lo);
                machineMaterial.SetInt("_UartInHi", hi);
                machineMaterial.SetInt("_UdonUARTInTag", (int)sentTag);
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
                else if ((!inputAway || keysOwn) && gpuKeyboard.Count() > 0) e = gpuKeyboard.Pop();
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

        frameMs = Time.timeSinceLevelLoad * 1000f;
        frameLength += (Mathf.Min(Time.unscaledDeltaTime, 0.1f) * 1000f - frameLength) * 0.2f;

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

    // The host's clock for round i of a frame's n: the frame's length is spread over its rounds,
    // as the harness's clock moves every pass. A program that waits for the clock to move
    // (Quake's loop after each of its frames) waited for the next Unity frame before.
    private void HostClock(int i, int n)
    {
        uint ms = (uint)(frameMs + frameLength * i / n);
        if (ms < lastMs && lastMs - ms < 1000) ms = lastMs;   // never back (but a scene loaded anew starts again)
        if (ms == lastMs && i > 0) return;
        lastMs = ms;
        machineMaterial.SetInt("_HostMsLo", (int)(ms & 0xffff));
        machineMaterial.SetInt("_HostMsHi", (int)(ms >> 16));
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

    // A guest's word as a number: Udon's cast throws on one past 2^31, which a zapped word can be.
    private int Small(uint w)
    {
        return w > 0x7fffffffu ? 0 : (int)w;
    }

    // ---- the network (docs/system/lan.md) ----

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
                lanGiven += n;
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
            uint number = (uint)NetByte(at + 4) | (uint)NetByte(at + 5) << 8 | (uint)NetByte(at + 6) << 16 | (uint)NetByte(at + 7) << 24;
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
            lanOut++;
            if (lanDetail < LanDetailMost) { lanDetail++; Debug.Log("[Lan] the guest sent " + LanPacket(netOut, to, lengths[k])); }
        }
        netAcked = sent;
    }

    // The next packets are logged one a line again (a test's: the first forty are long past by then).
    public void LanDetailAgain() { lanDetail = 0; }

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
        if (length < 20 || length > NetMost) return;
        if (!powered || netId == 0)
        {
            lanOff++;
            if (lanDetail < LanDetailMost) { lanDetail++; Debug.Log("[Lan] heard " + LanPacket(from, at, length) + ": dropped, this machine is off"); }
            return;
        }
        int a = from[at + 16], b = from[at + 17], c = from[at + 18], d = from[at + 19];
        bool mine = a == 10 && b == 0 && c == (netId >> 8) && d == (netId & 255);
        bool all = (a == 10 && b == 0 && c == 255 && d == 255) || (a == 255 && b == 255 && c == 255 && d == 255);
        if (!mine && !all)
        {
            lanOthers++;
            if (lanDetail < LanDetailMost) { lanDetail++; Debug.Log("[Lan] heard " + LanPacket(from, at, length) + ": not for this machine"); }
            return;
        }
        lanMine++;
        if (lanDetail < LanDetailMost) { lanDetail++; Debug.Log("[Lan] heard " + LanPacket(from, at, length) + ": for this machine"); }
        int next = (netInTail + 1) & (NetQueue - 1);
        if (next == netInHead) { netInHead = (netInHead + 1) & (NetQueue - 1); lanFull++; }   // full: the oldest goes
        System.Buffer.BlockCopy(from, at, netIn, netInTail * NetMost, length);
        for (int i = length; i < ((length + 3) & ~3); i++) netIn[netInTail * NetMost + i] = 0;
        netInLength[netInTail] = length;
        netInTail = next;
    }

    // ---- what the network did, in VRChat's log ([Lan] lines, docs/system/lan.md) ----

    private const int LanDetailMost = 40;   // packets described one by one after power-on; then only the counts
    private int lanOut, lanMine, lanOthers, lanOff, lanFull, lanGiven, lanDetail, lanSaid;
    private float lanAt;

    // A packet in words: its length, what it is, from where to where, and its ports.
    private string LanPacket(byte[] p, int at, int length)
    {
        int kind = p[at + 9], head = (p[at] & 15) * 4;
        string what = kind == 1 ? "ICMP" : kind == 6 ? "TCP" : kind == 17 ? "UDP" : "protocol " + kind;
        if (kind == 1 && length >= head + 1) what += p[at + head] == 8 ? " (ping)" : p[at + head] == 0 ? " (ping's answer)" : " (type " + p[at + head] + ")";
        string ports = "";
        if ((kind == 6 || kind == 17) && length >= head + 4)
            ports = ", port " + (p[at + head] << 8 | p[at + head + 1]) + " to port " + (p[at + head + 2] << 8 | p[at + head + 3]);
        if (kind == 6 && length >= head + 14)
        {
            int flags = p[at + head + 13];
            ports += (flags & 2) != 0 ? ((flags & 16) != 0 ? " (SYN+ACK)" : " (SYN)") : (flags & 4) != 0 ? " (RST)" : (flags & 1) != 0 ? " (FIN)" : "";
        }
        return length + " bytes " + what + " " + p[at + 12] + "." + p[at + 13] + "." + p[at + 14] + "." + p[at + 15] + " to "
               + p[at + 16] + "." + p[at + 17] + "." + p[at + 18] + "." + p[at + 19] + ports;
    }

    // Every five seconds in which something happened: the counts since power-on.
    private void LanSay()
    {
        if (Time.time < lanAt) return;
        lanAt = Time.time + 5f;
        int sum = lanOut + lanMine + lanOthers + lanOff + lanFull + lanGiven + (int)(netTaken & 0xffffff);
        if (sum == lanSaid) return;
        lanSaid = sum;
        Debug.Log("[Lan] 10.0." + (netId >> 8) + "." + (netId & 255) + (powered ? "" : " (off)") + ": the guest sent " + lanOut
                  + "; heard for this machine " + lanMine + ", for others " + lanOthers + ", while off " + lanOff
                  + "; given to the guest " + lanGiven + ", of which it has taken " + netTaken
                  + "; waiting to go out " + NetWaiting() + ", to go in " + ((netInTail - netInHead) & (NetQueue - 1))
                  + (lanFull > 0 ? "; dropped for a full queue " + lanFull : ""));
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
            countWorkers = i == n - 1 && Time.time >= workersAt;
            TakeRound();
        }
        if (countWorkers) workersAt = Time.time + 0.1f;
        if (Time.time >= sampleAt) Sample();
    }

    // Core 0's registers out of the last round's row (texels 28 to 36: x0 is texel 28's last
    // word), and whether they are what they were an eighth of a second ago.
    private void Sample()
    {
        sampleAt = Time.time + 0.125f;
        ShowTick();
        uint sum = 0;
        for (int i = 1; i < 32; i++)
        {
            uint v = Word(28 + (i + 3) / 4, (i + 3) % 4);
            registers[i] = v;
            sum = (sum ^ v) * 0x01000193u;
        }
        registers[0] = pc;
        // a busy machine's lamp is lit three eighths in four, an idle one's (under half a million
        // instructions a second: Linux with nothing to do) one in eight
        int chance = (int)((sum >> 13) & 7);
        bool lit = sum != registerSum && (mipsCore > 0.5f ? (chance & 3) != 0 : chance == 0);
        registerSum = sum;
        activity = (activity << 1 | (lit ? 1 : 0)) & 0xffff;
    }

    private string[] registerNames = { "pc", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
                                       "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };

    // Eight lines of four registers.
    private string RegisterText(uint[] r)
    {
        string text = "";
        for (int i = 0; i < 32; i++)
            text += (i % 4 == 0 ? "\n" : "  ") + registerNames[i].PadRight(3) + " " + r[i].ToString("x8");
        return text;
    }

    // The panel's read-out is another player's machine's, as their packets say it (EmuShareHub).
    public void RemoteStats(string text, uint[] r)
    {
        statsRemote = true;
        if (statsText != null) statsText.text = r == null ? "..." : text + (text == "off" ? "" : RegisterText(r));
    }

    public void OwnStats()
    {
        if (!statsRemote) return;
        statsRemote = false;
        if (statsText != null) statsText.text = statsLine + (powered ? RegisterText(registers) : "");
    }

    // What one round left in the row at rowAt.
    private void TakeRound()
    {

        // state row 0, as rvc_harness.cpp reads it
        uint clock = Word(28, 1), commits = Word(28, 2);
        consumedTag = Word(ConTexel, 0);
        // the control words: display (texel 64), GPU (65), input (66, 67)
        displayMode = Small(Word(64, 0));
        displayWidth = Small(Word(64, 1));
        displayHeight = Small(Word(64, 2));
        if (displayMode < 1 || displayMode > 4 || displayWidth > 2048 || displayHeight > 2048)
        {
            displayWidth = 0;
            displayHeight = 0;
        }
        if (Word(12, 0) != 0) gvmOpsSeen = Time.frameCount;
        gpuFrames = Word(65, 3);
        if (gpuFrames != gpuFramesSeen)
        {
            gpuFramesSeen = gpuFrames;
            gpuBusyUntil = Time.time + 1f;
        }
        keyboardOwned = Word(67, 0) == 0x6b657973u;
        soundEnabled = Word(64 + 0x22, 0) != 0;
        soundClock = Word(64 + 0x23, 0);
        gameId = Small(Word(64 + 6, 2)) & 0xff;
        worldEye = Word(64 + 6, 3);
        worldFrame = Word(64 + 0x30, 0) != 0;
        flatFrame = (Word(64 + 0x30, 3) >> 31) != 0;
        NetWindow();
        uint asked = Word(64 + 16, 0);
        if (fetchState == FetchIdle && asked != Word(64 + 17, 0) && asked != fetchAnswered) FetchAsked(asked);

        if (haveClock)
        {
            uint ran = clock - lastClock;
            if (steady && ticks == tickSize) perRound += (Mathf.Min(ran, tickSize) - perRound) * 0.05f;
            if (!paused) reached += (Mathf.Min(ran, ticks) / (float)ticks - reached) * 0.05f;
            instructions += ran;
            totalInstructions += ran;
        }
        haveClock = true;
        lastClock = clock;
        lastCommits = commits;
        pc = Word(36, 3);
        lastStall = Word(40, 2);
        if (countWorkers) Workers();
        GvmLayout();
        Printed();
    }

    // What the workers ran, for the statistics: the counts only grow, so a row now and then
    // is enough (255 workers a round were most of a frame's script time).
    private void Workers()
    {
        // the cores' counts of instructions, which the control pass publishes at 0x87000380
        cores = Mathf.Clamp(Small(Word(64 + 0x3c, 0)), 1, MostCores);
        // started, and not asleep on its job word: a texel for every 32 cores
        for (int g = 0; g < MostCores / 32; g++) coresRunning[g] = Word(CoresMore + 64 + g, 0) & ~Word(CoresMore + 64 + g, 1);
        for (int c = 1; c < cores; c++)
        {
            uint now = c < 16 ? Word(64 + 0x38 + c / 4, c % 4) : Word(CoresMore + c / 4, c % 4);
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
        // the geometry: what geotune picked and laid out, shown when it changes
        uint first = Word(64 + 0x3d, 0), hybrid = Word(64 + 0x3d, 3);
        if (first != geoFirst || hybrid != geoHybrid)
        {
            geoFirst = first;
            geoHybrid = hybrid;
            geoBits = (int)Word(64 + 0x3d, 2);
            ShowWorkers();
        }
    }

    // What the guest printed in the round at rowAt: the console ring's bytes since the last round.
    private void Printed()
    {
        uint written = Word(ConTexel, 1);
        if (!conSeen)
        {
            conWritten = written;
            conSeen = true;
        }
        if (written - conWritten > ConRing) conWritten = written - ConRing;
        for (; conWritten != written; conWritten++)
        {
            int at = (int)(conWritten & (ConRing - 1));
            int c = (int)((Word(ConTexel + 1 + at / 16, (at / 4) % 4) >> (8 * (at % 4))) & 0xff);
            if (c != 0 && terminal != null) terminal.PutChar(c);
            if (c != 0)
            {
                consoleOut += (char)c;
                consolePrinted++;
            }
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
        for (int c = 1; c < cores; c++) if (((coresRunning[c >> 5] >> (c & 31)) & 1) != 0) busy++;
        string workerLine = cores < 2 ? "one core" :
            (cores - 1) + " workers (" + busy + " running): " + workerIps.ToString("N0") + " instructions/s, all cores " + (ips + workerIps).ToString("N0");
        string display = displayWidth > 0 ? "mode " + displayMode + ", " + displayWidth + " x " + displayHeight : "off";
        statsLine =
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
        if (statsText != null && !statsRemote) statsText.text = statsLine + RegisterText(registers);
    }

    // ---- the network's counts, for the wall's panel (EmuLan) ----
    public int LanOut() { return lanOut; }
    public int LanMine() { return lanMine; }
    public int LanOthers() { return lanOthers; }
    public int LanOff() { return lanOff; }
    public int LanFull() { return lanFull; }
    public int LanGiven() { return lanGiven; }
    public int LanTaken() { return (int)(netTaken & 0xffffff); }
    public int LanQueuedIn() { return (netInTail - netInHead) & (NetQueue - 1); }
}
