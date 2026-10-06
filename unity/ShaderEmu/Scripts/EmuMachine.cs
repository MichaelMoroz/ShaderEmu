using System;
using TMPro;
using UdonSharp;
using UnityEngine;
using UnityEngine.UI;
using VRC.SDK3.Rendering;
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
    public Material machineMaterial;        // Machine.shader: passes CPUTick, Commit, GPUControl
    public Material gpuMaterial;            // the GPU mesh's material
    public Camera gpuCamera;                // disabled: rendered from here, once a round
    public Material displayMaterial;
    public Material heatMaterial;           // the memory view's
    public Material readbackMaterial;
    public RenderTexture readbackTexture;   // 320 x 1, one pixel per word

    public EmuTerminal terminal;
    public EmuKeyboard consoleKeyboard;
    public EmuKeyboard gpuKeyboard;

    // The boot image, twelve textures: RAM r, g, b, a, then ROM, then device tree.
    // A missing texture is left black.
    public Texture2D[] imageTextures;
    public Texture2D blackTexture;

    public TextMeshProUGUI statsText;
    public TextMeshProUGUI speedLabel;
    public TextMeshProUGUI powerLabel;
    public TextMeshProUGUI pauseLabel;
    public Slider speedSlider;         // whole steps: 2,048 instructions a frame, doubled per step
    public bool powerOnAtStart;        // off until someone presses Power

    [HideInInspector] public int displayMode, displayWidth, displayHeight;
    [HideInInspector] public bool keyboardOwned;   // a guest program reads the keyboard device

    private const int Words = 320;
    private const int UartBurst = 4;
    private const int InitFrames = 2;
    private const int TicksPerRound = 8192;   // more per tick pass only fills the write cache
    private const int MaxRounds = 32;
    private const int PassTick = 0, PassCommit = 1, PassControl = 2;

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
        machineMaterial.SetInt("_Ticks", ticks);
    }

    private void ShowLabels()
    {
        if (speedLabel != null) speedLabel.text = "up to " + (rounds * ticks).ToString("N0") + " instructions a frame";
        if (powerLabel != null) powerLabel.text = powered ? "Power off" : "Power on";
        if (pauseLabel != null) pauseLabel.text = paused ? "Resume" : "Pause";
        displayMaterial.SetFloat("_Power", powered ? 1f : 0f);
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
        machineMaterial.SetTexture("_Data_MTD_G", Pick(5));
        machineMaterial.SetTexture("_Data_MTD_B", Pick(6));
        machineMaterial.SetTexture("_Data_MTD_A", Pick(7));
        machineMaterial.SetTexture("_Data_DTB_R", Pick(8));
        machineMaterial.SetTexture("_Data_DTB_G", Pick(9));
        machineMaterial.SetTexture("_Data_DTB_B", Pick(10));
        machineMaterial.SetTexture("_Data_DTB_A", Pick(11));
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
        machineMaterial.SetInt("_InitRaw", 0);
        machineMaterial.SetInt("_DoTick", 0);
        machineMaterial.SetInt("_Ticks", ticks);
        machineMaterial.SetInt("_UartInLo", 0);
        machineMaterial.SetInt("_UartInHi", 0);
        machineMaterial.SetInt("_UdonUARTInTag", 0);
        machineMaterial.SetInt("_InputKeyCount", 0);
        VRCPlayerApi player = Networking.LocalPlayer;
        machineMaterial.SetInt("_PlayerID", player != null ? player.playerId : 0);
        if (consoleKeyboard != null) consoleKeyboard.Flush();
        if (gpuKeyboard != null) gpuKeyboard.Flush();
        if (terminal != null) terminal.Clear();
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
    }

    public void SetPointerButtons(int buttons)
    {
        pointerButtons = buttons;
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
        int n = running ? rounds : 1;
        for (int i = 0; i < n; i++)
        {
            // The tick writes only the CPU's 64 x 64 state area, into a texture of that size;
            // the commit reads it from there and writes the whole state.
            machineMaterial.SetTexture("_SelfTexture2D", current);
            VRCGraphics.Blit(current, tickState, machineMaterial, PassTick);
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
                ignoreUntil = requestsSent;   // init frames leave junk in the UART buffer
            }
        }
        // whoever shows the state reads the texture this frame ended in
        gpuMaterial.SetTexture("_State", current);
        displayMaterial.SetTexture("_State", current);
        heatMaterial.SetTexture("_State", current);

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
            machineMaterial.SetInt("_Rtc0Lo", Bcd(year / 100) | Bcd(now.Second) << 8);
            machineMaterial.SetInt("_Rtc0Hi", Bcd(now.Minute) | Bcd(now.Hour) << 8);
            machineMaterial.SetInt("_Rtc1Lo", Bcd((int)now.DayOfWeek + 1) | Bcd(now.Day) << 8);
            machineMaterial.SetInt("_Rtc1Hi", Bcd(now.Month) | Bcd(year % 100) << 8);
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
