using TMPro;
using UdonSharp;
using UnityEngine;
using VRC.SDK3.Rendering;
using VRC.SDKBase;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;

// The machine's second processor (experiments/udon_aot): the kernel and programs translated to Udon.
// With it picked, EmuMachine runs as it always does but its own processor is held at reset, and
// this runs the guest on `mem`: the pages it wrote go into the state in one upload a frame
// (UdonAotScatter), and the words the control pass writes come back from a readback (UdonAotWords).
// So the display, the GPU, sound, input, the network, fetches and the holodeck are EmuMachine's.
[UdonBehaviourSyncMode(BehaviourSyncMode.None)]
public class EmuUdonCpu : UdonSharpBehaviour
{
    public UdonBehaviour cpu;
    public EmuMachine machine;
    public Texture2D[] ramLanes, romLanes;
    public Material unpackMaterial;       // UdonAotUnpack.shader
    public RenderTexture unpackTarget;    // 2048 x 2048: four million words a readback
    public TextMeshProUGUI label, powerLabel;   // the toggle's status line, and the toggle's own text
    public int sliceBudget = 2000;        // loop heads and calls a slice
    public float[] frameMs = { 1f, 2f, 4f, 6f, 8f, 10f, 12f, 14f };   // Udon time a frame, by the speed slider's step
    public int textSum;                   // of the translated kernel's text: another image is refused
    public int textWords;
    public Texture2D[] pageStages;        // 1024 wide RGBA32, 64, 256 and 1024 rows: row 0 the page numbers, then a page a row
    public Material scatterMaterial;      // UdonAotScatter.shader
    public Material wordsMaterial;        // UdonAotWords.shader
    public RenderTexture devWords;        // 1024 x 3: the control page's 256 texels and the console's row's 512
    public RenderTexture fetchWords;      // 1024 x 64: a fetch's answer
    public RenderTexture copyWords;       // 1024 x 2048: a GPU copy's rows
    public bool udonMode;                 // the processor the next power-on gets
    public bool selfTest;                 // for a test client: boots, types and draws by itself, logging "[UdonCpuTest]"
    [HideInInspector] public double cpuMs, uploadMs, uploadPages;   // summed, for a test

    private const int RamWords = 0x12000000 / 4, ChunkWords = 2048 * 2048, Pages = 0x12000000 / 4096;
    private const int TextAt = 0x02000000 / 4;          // the kernel in the boot image (0x82000000)
    private const int Dev = 0x100000 * 4;                 // 0x81000000: the control page's first word
    private const int ConRow = 0x16b800 * 4;              // the console's and the network's
    private const int ConGuest = ConRow + 0x29 * 4, ConRing = ConRow + 0x2a * 4;
    private const int FetchData = 0x16c000 * 4;

    private int[] mem, rom, payload, pdirty, plist, pageHead = new int[1024];
    private int[] dev = new int[3072], answer = new int[65536], region;
    private byte[] chunk, devBytes, answerBytes, regionBytes, stageS, stageM, stageL;
    private bool loaded, loading, running;
    private int part, chunkAt, readbacks, waitFrames;
    private long guestTime;
    private int conRead, slices;
    private System.Diagnostics.Stopwatch watch, clock;
    // a GPU list: sent to the state once, done when the control pass has counted it (and its copy made)
    private bool listOut, copyOut, regionAsked, copyReady;
    private int sentFrame;
    private int listFrames, listCopies, copyAt, copyW, copyH, copyRow;
    private int heldSeq;                  // a fetch's answer being read back: its reply waits for its bytes
    private bool answerAsked;
    private int[] kinds = new int[32];    // what each readback on its way is: 1 an image, 2 the device words (5: a copy follows), 3 an answer, 4 a copy
    private int kindHead, kindCount;
    private int ranBefore, statsFrames;
    private float ranAt, shownAt;
    private uint registerSum;

    public bool Running() { return running; }

    // The panel's toggle: the machine starts again with the other processor.
    public void ToggleCpu()
    {
        udonMode = !udonMode;
        running = false;
        if (machine.Powered()) machine.PowerOn();
        Show();
    }

    // ---- the boot images, a readback of four million words at a time

    private void StartLoading()
    {
        loading = true;
        chunk = new byte[ChunkWords * 4];
        payload = new int[ramLanes[0].width * ramLanes[0].height * 4];
        rom = new int[romLanes[0].width * romLanes[0].height * 4];
        part = 0; chunkAt = 0; readbacks = 0;
        Unpack();
    }

    private void Unpack()
    {
        Texture2D[] lanes = part == 0 ? ramLanes : romLanes;
        unpackMaterial.SetTexture("_LaneR", lanes[0]);
        unpackMaterial.SetTexture("_LaneG", lanes[1]);
        unpackMaterial.SetTexture("_LaneB", lanes[2]);
        unpackMaterial.SetTexture("_LaneA", lanes[3]);
        unpackMaterial.SetFloat("_Texel0", chunkAt / 4);
        VRCGraphics.Blit(lanes[0], unpackTarget, unpackMaterial);
        Ask(unpackTarget, 1);
    }

    private void Ask(RenderTexture rt, int kind)
    {
        kinds[(kindHead + kindCount) & 31] = kind;
        kindCount++;
        VRCAsyncGPUReadback.Request(rt, 0, TextureFormat.RGBA32, (IUdonEventReceiver)this);
    }

    public override void OnAsyncGpuReadbackComplete(VRCAsyncGPUReadbackRequest request)
    {
        if (kindCount == 0) return;
        int kind = kinds[kindHead];   // requests complete in the order made
        kindHead = (kindHead + 1) & 31;
        kindCount--;
        if (kind == 1) ImageBack(request);
        else if (kind == 2 || kind == 5) DeviceBack(request, kind == 5);
        else if (kind == 3) AnswerBack(request);
        else if (kind == 4) CopyBack(request);
    }

    private void ImageBack(VRCAsyncGPUReadbackRequest request)
    {
        if (request.hasError || !request.TryGetData(chunk)) { Unpack(); return; }
        readbacks++;
        int[] into = part == 0 ? payload : rom;
        int n = Mathf.Min(ChunkWords, into.Length - chunkAt);
        System.Buffer.BlockCopy(chunk, 0, into, chunkAt * 4, n * 4);
        chunkAt += n;
        if (chunkAt >= into.Length)
        {
            part++; chunkAt = 0;
            if (part == 2)
            {
                loading = false;
                chunk = null;
                int s = 0;   // every 61st word of the kernel's text, as the editor summed the image it translated
                for (int i = 0; i < textWords; i += 61) s = s * 31 + payload[TextAt + i];
                loaded = s == textSum;
                Show();
                return;
            }
        }
        Unpack();
        Show();
    }

    // ---- running: after EmuMachine's frame, so its processor stays at reset and its readback is of this frame

    private void LateUpdate()
    {
        if (selfTest) TestStep();
        if (!udonMode) { running = false; return; }
        if (!machine.Powered()) { running = false; if (Time.time - shownAt > 1f) Show(); return; }
        machine.tickMaterial.SetInt("_Init", 1);       // EmuMachine's processor: at reset, every pass
        if (machine.machineMaterial.GetInt("_Init") != 0) { running = false; waitFrames = 2; return; }   // its power-on copies the image
        if (!running)
        {
            if (!loaded) { if (!loading) StartLoading(); if (Time.time - shownAt > 1f) Show(); return; }
            if (--waitFrames > 0) return;
            Boot();
            if (!running) return;
        }
        if (!cpu.enabled) { running = false; if (label != null) label.text = "Udon CPU: Udon halted the program"; return; }
        if (watch == null) watch = new System.Diagnostics.Stopwatch();
        watch.Restart();
        // what the control pass left in this frame's state, before this frame's pages go in
        wordsMaterial.SetTexture("_State", machine.state);
        wordsMaterial.SetFloat("_TexelA", 0x100000);
        wordsMaterial.SetFloat("_CountA", 256);
        wordsMaterial.SetFloat("_TexelB", 0x16b800);
        VRCGraphics.Blit(machine.state, devWords, wordsMaterial);
        // a copy on its way is read beside them, from the same state: when they say it is made, it is here
        bool copyToo = listOut && copyOut && !regionAsked && !copyReady && Time.frameCount - sentFrame >= 2;
        Ask(devWords, copyToo ? 5 : 2);
        if (copyToo) BlitCopy();
        Rtc();
        float budget = frameMs[Mathf.Clamp(machine.SpeedStep(), 0, frameMs.Length - 1)];
        while (watch.Elapsed.TotalMilliseconds < budget)
        {
            // the guest's timer (5000 Hz) is real time since power on; a wfi ends the frame unless it is due
            long real = (long)(clock.Elapsed.TotalMilliseconds * 5.0);
            if (real > guestTime) guestTime = real;
            cpu.SetProgramVariable("now", (int)guestTime);
            cpu.SetProgramVariable("budget", sliceBudget);
            cpu.SetProgramVariable("status", 0);
            cpu.SendCustomEvent("Slice");
            slices++;
            Printed();
            int status = (int)cpu.GetProgramVariable("status");
            // a rest (the pause hint) waiting on the GPU: nothing changes until the upload and a readback
            if (status == 3 && (listOut || (mem[Dev + 4] & 1) != 0)) break;
            if (status == 2)
            {
                long tcmp = ((long)U((int)cpu.GetProgramVariable("th")) << 32) | U((int)cpu.GetProgramVariable("tl"));
                if (tcmp > (long)(clock.Elapsed.TotalMilliseconds * 5.0)) break;
            }
        }
        double t0 = watch.Elapsed.TotalMilliseconds;
        Upload();
        cpuMs += t0;
        uploadMs += watch.Elapsed.TotalMilliseconds - t0;
        statsFrames++;
        Stats();
    }

    private void Boot()
    {
        if (mem == null) mem = new int[RamWords];
        else System.Array.Clear(mem, 0, RamWords);
        System.Array.Copy(payload, mem, payload.Length);   // the state has the same image, from EmuMachine's power-on
        for (int r = 1; r < 32; r++) cpu.SetProgramVariable("x" + r, 0);
        cpu.SetProgramVariable("x11", -2082471936);      // 0x83e00000, the device tree
        cpu.SetProgramVariable("priv", 1);
        cpu.SetProgramVariable("mode", 1);
        cpu.SetProgramVariable("res", -1);
        cpu.SetProgramVariable("rpc", -2113929216);      // 0x82000000
        cpu.SetProgramVariable("resume", cpu.GetProgramVariable("resume0"));
        string[] zero = { "th", "tl", "fcsr", "msrc", "mdst", "mn", "ipc", "icount", "gcount", "pn" };
        foreach (string z in zero) cpu.SetProgramVariable(z, 0);
        cpu.SetProgramVariable("mem", mem);
        cpu.SetProgramVariable("rom", rom);
        cpu.SetProgramVariable("romlen", rom.Length);
        cpu.SetProgramVariable("xr", new int[32]);
        cpu.SetProgramVariable("fr", new int[32]);
        cpu.SetProgramVariable("csr", new int[16]);
        pdirty = (int[])cpu.GetProgramVariable("pdirty");
        plist = (int[])cpu.GetProgramVariable("plist");
        if (pdirty == null || plist == null)
        {
            if (label != null) label.text = "Udon CPU: this translation marks no pages (l2u with L2U_DIRTY=1)";
            return;
        }
        System.Array.Clear(pdirty, 0, pdirty.Length);
        listOut = copyOut = regionAsked = copyReady = answerAsked = false;
        heldSeq = mem[0x100011 * 4];
        guestTime = 1; slices = 0; conRead = 0; ranBefore = 0; ranAt = 0f;
        if (clock == null) clock = new System.Diagnostics.Stopwatch();
        clock.Restart();
        running = true;
        Show();
    }

    // ---- the pages the guest wrote, into the state

    private void MarkWords(int word, int count)
    {
        if (count <= 0 || word < 0) return;
        int n = (int)cpu.GetProgramVariable("pn");
        int last = Mathf.Min((word + count - 1) >> 10, Pages - 1);
        for (int p = word >> 10; p <= last; p++)
            if (pdirty[p] == 0) { pdirty[p] = 1; plist[n++] = p; }
        cpu.SetProgramVariable("pn", n);
    }

    // A BlockCopy a page into a stage, one texture upload and one draw a stage (and four small ones
    // for the texels the guest shares with the device); more than 1,023 pages take more stages.
    private void Upload()
    {
        bool send = !listOut && (mem[Dev + 4] & 1) != 0;
        if (send)
        {
            // the list goes once; until the control pass has counted it, the submit word is the state's
            listOut = true;
            sentFrame = Time.frameCount;
            listFrames = mem[Dev + 7];
            listCopies = mem[Dev + 14];
            copyOut = (mem[Dev + 4] & 6) != 0;
            if ((mem[Dev + 4] & 4) != 0) { copyAt = mem[Dev + 20]; copyW = mem[Dev + 21]; copyH = mem[Dev + 22]; copyRow = Mathf.Max(mem[Dev + 23], copyW); }
            else { copyAt = -2130706432 + 0x1000; copyW = mem[Dev + 1]; copyH = mem[Dev + 2]; copyRow = copyW; }   // 0x81001000
            MarkWords(Dev, 1024);
        }
        int n = (int)cpu.GetProgramVariable("pn");
        if (n == 0) return;
        if (stageS == null)
        {
            stageS = new byte[pageStages[0].width * pageStages[0].height * 4];
            stageM = new byte[pageStages[1].width * pageStages[1].height * 4];
            stageL = new byte[pageStages[2].width * pageStages[2].height * 4];
        }
        scatterMaterial.SetFloat("_Send", send ? 1 : 0);
        int done = 0, draws = 0;
        while (done < n && draws < 8)
        {
            int k = Mathf.Min(n - done, pageStages[2].height - 1);
            int which = k < pageStages[0].height ? 0 : k < pageStages[1].height ? 1 : 2;
            byte[] bytes = which == 0 ? stageS : which == 1 ? stageM : stageL;
            bool shared = false;
            for (int i = 0; i < k; i++)
            {
                int p = plist[done + i];
                pdirty[p] = 0;
                pageHead[i] = p;
                if (p == 0x7000 || p == 0x76b8 || p == 0x76b9) shared = true;
                System.Buffer.BlockCopy(mem, p << 12, bytes, (i + 1) << 12, 4096);
            }
            System.Buffer.BlockCopy(pageHead, 0, bytes, 0, k * 4);
            Texture2D stage = pageStages[which];
            stage.LoadRawTextureData(bytes);
            stage.Apply(false);
            scatterMaterial.SetFloat("_Pages", k);
            VRCGraphics.Blit(stage, machine.state, scatterMaterial, 0);
            if (shared)
                for (int pass = 1; pass <= 4; pass++) VRCGraphics.Blit(stage, machine.state, scatterMaterial, pass);
            done += k;
            draws++;
        }
        uploadPages += done;
        if (done < n) System.Array.Copy(plist, done, plist, 0, n - done);
        cpu.SetProgramVariable("pn", n - done);
    }

    // ---- the device's words, as the control pass left them

    private void DeviceBack(VRCAsyncGPUReadbackRequest request, bool copyFollows)
    {
        if (!running) return;
        if (devBytes == null) devBytes = new byte[devWords.width * devWords.height * 4];
        if (request.hasError || !request.TryGetData(devBytes)) return;   // the next frame's comes
        System.Buffer.BlockCopy(devBytes, 0, dev, 0, 3072 * 4);
        // whole texels: input, keys, the address to open, host state, the network's, the voices
        System.Array.Copy(dev, 0x02 * 4, mem, Dev + 0x02 * 4, 4);
        System.Array.Copy(dev, 0x08 * 4, mem, Dev + 0x08 * 4, 32);
        System.Array.Copy(dev, 0x25 * 4, mem, Dev + 0x25 * 4, 44);
        System.Array.Copy(dev, 0x31 * 4, mem, Dev + 0x31 * 4, 4);
        System.Array.Copy(dev, 0x33 * 4, mem, Dev + 0x33 * 4, 20);
        System.Array.Copy(dev, 0x3f * 4, mem, Dev + 0x3f * 4, 4);
        System.Array.Copy(dev, 0xc0 * 4, mem, Dev + 0xc0 * 4, 256);
        // words of texels the guest shares: the clock and flags, the passes, the sound card's cursor, HOST_OPEN
        mem[Dev + 0x03 * 4 + 1] = dev[0x03 * 4 + 1];
        mem[Dev + 0x03 * 4 + 3] = dev[0x03 * 4 + 3];
        mem[Dev + 0x32 * 4 + 3] = dev[0x32 * 4 + 3];
        mem[Dev + 0x23 * 4] = dev[0x23 * 4];
        mem[Dev + 0x23 * 4 + 1] = dev[0x23 * 4 + 1];
        mem[Dev + 0x24 * 4] = dev[0x24 * 4];
        mem[Dev + 0x24 * 4 + 1] = dev[0x24 * 4 + 1];
        // the console's input and the network's ring
        System.Array.Copy(dev, 1024 + 0x28 * 4, mem, ConRow + 0x28 * 4, 4);
        System.Array.Copy(dev, 1024 + 0x40 * 4, mem, ConRow + 0x40 * 4, 320 * 4);
        // a fetch answered: its 256 KB first, then the words that say so
        if (!answerAsked && dev[0x11 * 4] != heldSeq)
        {
            answerAsked = true;
            wordsMaterial.SetFloat("_TexelA", 0x16c000);
            wordsMaterial.SetFloat("_CountA", 0x4000);
            VRCGraphics.Blit(machine.state, fetchWords, wordsMaterial);
            Ask(fetchWords, 3);
            for (int i = 0; i < 4; i++) answer[i] = dev[0x11 * 4 + i];   // (kept until the bytes come)
            heldSeq = dev[0x11 * 4];
        }
        // the list sent: counted, and its copy made, then the guest is told
        if (listOut && dev[0x01 * 4 + 3] != listFrames)
        {
            if (!copyOut) ListDone();
            else if (!regionAsked && !copyReady && dev[0x03 * 4 + 2] != listCopies)
            {
                if (copyFollows) copyReady = true;
                else { regionAsked = true; BlitCopy(); }
            }
        }
    }

    private void ListDone()
    {
        mem[Dev + 0x01 * 4] = dev[0x01 * 4];
        mem[Dev + 0x01 * 4 + 3] = dev[0x01 * 4 + 3];
        mem[Dev + 0x03 * 4 + 2] = dev[0x03 * 4 + 2];
        listOut = copyOut = regionAsked = copyReady = false;
    }

    private void AnswerBack(VRCAsyncGPUReadbackRequest request)
    {
        if (!running) return;
        if (answerBytes == null) answerBytes = new byte[fetchWords.width * fetchWords.height * 4];
        if (request.hasError || !request.TryGetData(answerBytes)) { Ask(fetchWords, 3); return; }
        int seq = answer[0], length = answer[1], status = answer[2], info = answer[3];
        System.Buffer.BlockCopy(answerBytes, 0, mem, FetchData * 4, 0x4000 * 16);
        mem[0x100011 * 4 + 1] = length; mem[0x100011 * 4 + 2] = status; mem[0x100011 * 4 + 3] = info;
        mem[0x100011 * 4] = seq;
        answerAsked = false;
    }

    // the copy's rows, from the texel its first word is in
    private void BlitCopy()
    {
        int first = (copyAt & 0x7fffffff) >> 2, span = (Mathf.Max(copyH, 1) - 1) * copyRow + copyW;
        if (copyW <= 0 || span <= 0 || span + 4 > copyWords.width * copyWords.height) { ListDone(); return; }
        wordsMaterial.SetFloat("_TexelA", first >> 2);
        wordsMaterial.SetFloat("_CountA", (span + 7) / 4 + 1);
        VRCGraphics.Blit(machine.state, copyWords, wordsMaterial);
        Ask(copyWords, 4);
    }

    private void CopyBack(VRCAsyncGPUReadbackRequest request)
    {
        if (!running) return;
        int size = copyWords.width * copyWords.height;
        if (regionBytes == null) { regionBytes = new byte[size * 4]; region = new int[size]; }
        bool wanted = regionAsked || copyReady;   // else read ahead of its being made
        if (!wanted) return;
        if (request.hasError || !request.TryGetData(regionBytes)) { regionAsked = true; copyReady = false; Ask(copyWords, 4); return; }
        int first = (copyAt & 0x7fffffff) >> 2, off = first & 3, span = (Mathf.Max(copyH, 1) - 1) * copyRow + copyW;
        System.Buffer.BlockCopy(regionBytes, 0, region, 0, Mathf.Min(size, span + off + 4) * 4);
        for (int y = 0; y < copyH; y++) System.Array.Copy(region, off + y * copyRow, mem, first + y * copyRow, copyW);
        ListDone();
    }

    // ---- the console's output, from its ring in the control row after every slice

    private void Printed()
    {
        int written = mem[ConGuest + 1];
        if (written - conRead > 256) conRead = written - 256;
        EmuTerminal terminal = machine.terminal;
        while (conRead != written)
        {
            int c = (mem[ConRing + ((conRead & 255) >> 2)] >> ((conRead & 3) * 8)) & 255;
            if (c != 0 && terminal != null) terminal.PutChar(c);
            lastFour = (lastFour << 8) | c;
            if (lastFour == 0x2f202320) prompts++;    // "/ # "
            conRead++;
        }
    }

    // the clock chip (DS1742) as the harness sets it: local time in BCD, the century in the control byte
    private void Rtc()
    {
        System.DateTime t = System.DateTime.Now;
        int year = t.Year;
        cpu.SetProgramVariable("rtc0", Bcd(year / 100) | Bcd(t.Second) << 8 | Bcd(t.Minute) << 16 | Bcd(t.Hour) << 24);
        cpu.SetProgramVariable("rtc1", Bcd((int)t.DayOfWeek + 1) | Bcd(t.Day) << 8 | Bcd(t.Month) << 16 | Bcd(year % 100) << 24);
    }

    private int Bcd(int v) { return (v / 10) << 4 | (v % 10); }

    // Udon's numeric casts throw past the other type's range: this keeps the bits
    private uint U(int v) { return (uint)(v & 0x7fffffff) | (v < 0 ? 0x80000000u : 0u); }

    // EmuMachine's read-out is of its own processor, at reset: the speed, registers and lamp are this one's
    private float mips;

    private void Stats()
    {
        machine.mipsAll = mips;
        machine.mipsCore = mips;
        float now = Time.realtimeSinceStartup;
        if (now - ranAt < 0.5f) return;
        int ran = (int)cpu.GetProgramVariable("gcount") + (int)cpu.GetProgramVariable("icount");
        mips = ranAt > 0f ? (ran - ranBefore) / (now - ranAt) / 1e6f : 0f;
        float frames = statsFrames / Mathf.Max(now - ranAt, 0.001f);
        ranBefore = ran; ranAt = now; statsFrames = 0;
        machine.mipsAll = mips;
        machine.mipsCore = mips;
        uint sum = 0;
        for (int r = 1; r < 32; r++)
        {
            uint v = U((int)cpu.GetProgramVariable("x" + r));
            machine.registers[r] = v;
            sum = (sum ^ v) * 0x01000193u;
        }
        machine.registers[0] = U((int)cpu.GetProgramVariable("rpc"));
        machine.activity = (machine.activity << 4 | (sum != registerSum ? (mips > 0.5f ? 0xe : 0x8) : 0)) & 0xffff;
        registerSum = sum;
        if (label != null)
            label.text = string.Format("Udon CPU: {0:F2} MIPS, {1:F0} frames/s, {2:N0} slices", mips, frames, slices);
        shownAt = Time.time;
    }

    private void Show()
    {
        shownAt = Time.time;
        if (powerLabel != null) powerLabel.text = udonMode ? "CPU: Udon VM" : "CPU: shader";
        if (label == null) return;
        if (!udonMode) label.text = "The shader CPU runs the machine";
        else if (loading) label.text = "Udon CPU: loading the images, " + readbacks + " of " + ((payload.Length + rom.Length + ChunkWords - 1) / ChunkWords);
        else if (!loaded && readbacks > 0) label.text = "Udon CPU: these images are not the kernel it was translated from";
        else if (!machine.Powered()) label.text = "Udon CPU: picked, the computer is off";
        else label.text = "Udon CPU: starting";
    }

    // ---- the self-test (docs: the world's test flags): power on with the Udon CPU, type, measure

    private int lastFour, prompts, testStep, testPrompts;
    private float testAt;

    private void TestStep()
    {
        float now = Time.realtimeSinceStartup;
        if (testStep == 0 && Time.time > 5f)
        {
            Debug.Log("[UdonCpuTest] power on with the Udon CPU");
            udonMode = true;
            machine.PowerOn();
            testStep = 1; testAt = now;
        }
        else if (testStep == 1 && prompts > 0)
        {
            Debug.Log("[UdonCpuTest] prompt after " + (now - testAt).ToString("F1") + " s; " + label.text);
            TypeLine("uname -a; dd if=/dev/urandom of=/dev/fb0 bs=4096 count=50");
            testStep = 2; testAt = now; testPrompts = prompts;
        }
        else if (testStep == 2 && (prompts > testPrompts || now - testAt > 120f))
        {
            Debug.Log("[UdonCpuTest] after " + (now - testAt).ToString("F1") + " s: display mode " + machine.displayMode + " " + machine.displayWidth + "x" + machine.displayHeight + "; " + label.text);
            TypeLine("nano-X -p & sleep 1; demo-hello & sleep 8; glxgears & sleep 10; kill %3; date; nxpersist name");
            testStep = 3; testAt = now;
        }
        else if (testStep == 3 && now - testAt > 40f)
        {
            Debug.Log("[UdonCpuTest] window system: display mode " + machine.displayMode + " " + machine.displayWidth + "x" + machine.displayHeight
                      + ", GPU lists " + mem[Dev + 7] + ", copies " + mem[Dev + 14] + ", pages " + uploadPages + " in " + uploadMs.ToString("F0") + " ms; " + label.text);
            testStep = 4;
        }
    }

    private void TypeLine(string text)
    {
        if (machine.consoleKeyboard != null) machine.consoleKeyboard.TypeText(text + "\n");
    }
}
