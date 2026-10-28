// rvc_harness: runs pimaker's rvc (RISC-V Linux in a VRChat pixel shader) outside Unity.
//
// It loads rvc's unmodified _Nix/rvc/main.shader, compiles both passes with FXC, and drives
// them the way the Unity scene does: a 2048x9280 RGBA32_UINT double-buffered Custom Render
// Texture with two update zones per frame (CPUTick on the 64x64 state area, then Commit on
// the whole texture). The emulated UART is wired to this console.
//
// Frame protocol, mirrored from NixControl.cs / NixDebug.cs:
//   * _Init = 1 for the first frames: the tick pass runs cpu_init(), the commit pass loads RAM
//     from the _Data_RAM_{R,G,B,A} textures.
//   * After each frame, state texel row 0 is read back. Texel (11,0).a is the UART buffer
//     pointer (chars written this frame minus 1), texels (12..27,0) hold up to 64 chars,
//     (9,0).a is the input tag the guest last consumed, (28,0) = stall, clock, commits.
//   * Input: bump _UdonUARTInTag with _UdonUARTInChar set; the guest latches it when its RBR
//     is empty, and echoes the tag back through (9,0).a.

#include "common.h"
#include "material.h"
#include "memview.h"
#include "rvc_audio.h"
#include "rvc_backend.h"
#include "rvc_net.h"
#include "rvc_time.h"
#include "shaderlab.h"

#include <conio.h>
#include <wininet.h>
#include <wincodec.h>
#pragma comment(lib, "wininet.lib")
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// The machine as an interpreter (rvc_cpu.cpp, docs/machine/cpu-harness.md), for --cpu: the same file
// that is bin\\rvc_cpu.exe, without its main().
#include "run_lock.h"
#define RVC_CPU_LIBRARY
#include "rvc_persist.h"
namespace cpuemu {
#include "rvc_cpu.cpp"
}
static std::string g_cpuConsole;   // what the interpreter's console said since it was last looked at
static void cpuConsoleHook(char c) { g_cpuConsole.push_back(c); }

// --cpu --save-state: a snapshot the shader machine (experiments/rvc_opt, 256 cores, TICK_MRT) resumes from.
// `data` is the state texture as drawn, W texels wide: the GPU's memory and the control words are kept as
// they are there; Linux's RAM (shown four times a second) is the processor's, and every core's state is
// written where the shader keeps it (src/types.h: decode, encode_l1, mc_geo_read, mc_state_at; src/mc.h),
// with empty write caches. The interpreter knows only the geometry of workers all alike, so geotune leaves
// that one there; `width` (2, 4 or 8) lays them out as geotune does on the card instead (its hybrid: 16 big
// workers, the rest small in blocks of `width` columns), which the empty caches allow. Returns how many
// workers it wrote; -1 when a core is in a state the shader cannot be given (a copy still being made).
static int cpuSnapshotCores(std::vector<uint8_t>& data, uint32_t W, uint32_t width) {
    using namespace cpuemu;
    auto tex = [&](uint32_t x, uint32_t y) { return (uint32_t*)&data[((size_t)y * W + x) * 16]; };
    for (uint32_t a = RAM_BASE; a - RAM_BASE < RAM_SIZE; a += 0x400000u) {
        if (a >= 0x86000000u && a < 0x88000000u) continue;
        uint32_t lin = (a - RAM_BASE) / 16;
        memcpy(tex(lin % 2048, 64 + lin / 2048), &ram[a - RAM_BASE], 0x400000);
    }
    // the workers' geometry (MC_HYBRID; STATE_ROWS 16, MC_STRIP_TILES 248, TICK_COLS 1024, 6 strip bands, 2 tick bands)
    const uint32_t L1_AT = 44, TILES = 248, COLS = 1024, SBANDS = 6, TBANDS = 2, LAST = 255;
    auto buckets = [](uint32_t bits) { return (7u << bits) >> 2; };
    auto rowsOf = [&](uint32_t bits) { return bits == 0 ? 0u : (L1_AT + 8 + buckets(bits) * 4 + 63) / 64; };
    auto wide = [](uint32_t rows, uint32_t block, uint32_t pad) { return (rows + pad + block - 1) / block * block; };
    uint32_t geo[4];
    memcpy(geo, &ram[0x870003d0u - RAM_BASE], 16);
    // (none yet, where no program has asked for workers: geotune lays it out at boot on the card, all 255)
    if (width && (geo[1] > 16 || geo[1] == 0) && geo[3] == 0 && (geo[0] & 1) == 0) {
        uint32_t laid[4] = {width << 8, geo[1] ? geo[1] : 255u, width == 8 ? 5u : 3u, 16u | 5u << 8 | width << 16 | 8u << 24};   // (geotune.c: hybrid(1, width, WIDE))
        memcpy(geo, laid, 16);
        memcpy(tex(0x70003du % 2048, 64 + 0x70003du / 2048), laid, 16);
    }
    uint32_t ws = (geo[3] >> 16) & 0xff, wb = geo[3] >> 24;
    uint32_t rows = geo[2] < 3 || geo[2] > 6 ? 0 : rowsOf(geo[2]);
    uint32_t cols = ws != 0 && ws <= 64 ? wide(rows, ws, 0) : wide(rows, 4, 2);
    uint32_t nb = 0, per0 = 0, count = 0, bb = (geo[3] >> 8) & 0xff, rowsB = 0;
    if ((geo[3] & 0xff) != 0 && bb >= 3 && bb <= 6 && rows != 0) {
        rowsB = rowsOf(bb);
        uint32_t colsB = wb != 0 && wb <= 64 ? wide(rowsB, wb, 0) : wide(rowsB, 8, 0);
        nb = (std::min)({geo[3] & 0xff, TILES / rowsB, COLS / colsB, geo[1], LAST});
        if (nb != 0) per0 = (COLS - nb * colsB) / cols;
    }
    uint32_t sb0 = nb != 0 ? 1 : 0;
    if (rows != 0 && (geo[0] & 1) == 0)
        count = nb + (std::min)((std::min)(geo[1], LAST) - nb, (std::min)((SBANDS - sb0) * (TILES / rows), (TBANDS - sb0) * (COLS / cols) + per0));
    for (uint32_t core = 0; core <= count && core < cores.size(); core++) {
        Core& c = cores[core];
        if (c.copying) return -1;
        uint32_t bits = core == 0 ? 6 : core <= nb ? bb : geo[2];
        uint32_t cacheEnd = L1_AT + buckets(bits) * 4, tile0 = 0;
        if (core != 0) {
            uint32_t j = core - 1 - nb, per = TILES / (std::max)(rows, 1u);
            tile0 = core <= nb ? (core - 1) * rowsB : (sb0 + j / per) * TILES + (j % per) * rows;
        }
        // texel lin as core 0 numbers them (its float registers' 8 from cacheEnd on), where this core keeps it
        auto at = [&](uint32_t lin) {
            if (core == 0) return tex(lin % 64, lin / 64);
            uint32_t t = tile0 + (lin >> 6);
            return tex(64 + 8 * (t % TILES) + (lin & 7), 8 * (t / TILES) + ((lin >> 3) & 7));
        };
        auto put = [&](uint32_t lin, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
            uint32_t* p = at(lin);
            p[0] = r, p[1] = g, p[2] = b, p[3] = a;
        };
        for (uint32_t lin = 0; lin < cacheEnd; lin++) put(lin, 0, 0, 0, 0);
        put(0, c.mstatus, c.mie, c.mip, c.stvec);
        put(1, c.sscratch, c.sepc, c.scause, c.stval);
        put(2, c.scounteren, c.fcsr, 0, 0);
        put(8, 0xffffffffu, 3735931358u, c.priv, 0);
        put(10, 0, (uint32_t)c.timecmp, (uint32_t)(c.timecmp >> 32), 0);
        put(11, 0, 0, 0, 0xffffffffu);
        put(28, 0, (uint32_t)c.clock, 0, 0);
        for (uint32_t k = 0; k < 8; k++)
            put(29 + k, c.x[4 * k + 1], c.x[4 * k + 2], c.x[4 * k + 3], k == 7 ? c.pc : c.x[4 * k + 4]);
        put(37, c.reserved ? 1 : 0, c.reservation, c.memop_src, c.memop_dst);
        put(38, 0, 0, c.memop_n, 0);
        uint32_t word = 0;
        if (core != 0) {
            const Worker& w = workers[core];
            uint32_t start = cpuemu::word(mc_start_at((int)core));
            word = w.state == MC_RUNNING ? 1 : w.state == MC_ASLEEP ? 3 | (w.job_seen & 0xffffff) << 8
                 : w.state == MC_FAULTED ? 5 | (w.seen & 0xffffff) << 8 : w.state == MC_CALLING ? 9 | (w.seen & 0xffffff) << 8
                 : (start == MC_STOP ? 2u : 1u) << 8;
        }
        put(41, 0, 0, 0, word);
        for (uint32_t k = 0; k < 8; k++)
            put(cacheEnd + k, c.f[4 * k], c.f[4 * k + 1], c.f[4 * k + 2], c.f[4 * k + 3]);
    }
    return (int)count;
}

#ifndef SHADEREMU_UNITY_INCLUDE_DIR
#define SHADEREMU_UNITY_INCLUDE_DIR "harness/unity_include"
#endif

namespace {

struct Options {
    std::string rvcDir = "rvc/_Nix/rvc";
    std::string payloadDir;  // default <rvc>/data-net
    std::string ramPrefix = "linux_payload", mtdPrefix = "rootfs", dtbPrefix = "dts";
    std::string cacheDir = "build/shadercache";
    std::string uartLog;
    std::string dumpState;
    std::string saveState, loadState;
    std::string recordInput, replayInput;   // the window's input to a file with its times, or from one instead of the window's
    bool printStamp = false;
    std::string screenLog, screenFrom;   // --screen-log: the display, every frame, into a file
    int screenFrames = 0, screenEvery = 0;
    std::vector<std::pair<std::string, std::string>> expectSend;
    std::string initialInput;
    std::string until;
    std::vector<std::string> open;   // --open: addresses or files the guest is told to open (docs/system/open.md)
    std::string openAfter;           // --open-after: not before the console has shown this
    std::string persist;             // --persist: the file the guest's /persistence is kept in (docs/system/persist.md)
    int ticks = 8192;
    int initFrames = 2;
    uint64_t maxFrames = 0;
    double maxSeconds = 0;
    double fixedDt = 0;  // 0 = real time
    long long pointerSweep = -1;  // from this frame on the pointer goes round the display; -1 = it does not
    int pointerButton = 0;        // --pointer-button: the buttons word, between these seconds of the clock
    double pointerButtonFrom = 0, pointerButtonTo = 0;
    double statsInterval = 0;
    int64_t benchWarmup = -1;  // >= 0: benchmark mode, this many unmeasured frames first
    bool readStdin = true;
    bool verbose = false;
    bool profile = false;
    bool present = false;
    bool viz = false;         // memory view window
    int uartBurst = 0;        // input characters per handshake; 0 = what the shader declares
    bool resume = false;      // with no other arguments: resume the shell snapshot instead of booting
    bool tabs = false;        // ask the guest for its console as several terminals (docs/machine/console.md)
    int workers = 0;          // --workers: the small workers' blocks geotune lays out (0 its own pick; 1-3: 2, 4, 8 columns)
    bool desktop = false;     // ask the guest to start its desktop: the terminal mode, unless --no-desktop
    std::string kernel;       // Linux's: "mmu" or "nommu" (docs/system/nommu.md); unset, nommu once its image is made
    std::string isa = "base"; // "base" RV32IMAF, or "zb" with Zba and Zbb (the shader's ZB)
    std::string vizCapture;   // BMP of the memory view, written at exit
    std::string gpuCapture;   // BMP of the GPU device's colour target, written at exit
    std::string raLog;        // per frame: pc, the return address register (who called a leaf function) and instructions
    std::string pcLog;        // per frame: the guest's pc and the instructions it ran, as two uint32
    std::string frameLog;     // per frame, four uint32: pc, instructions, last stall, microseconds since start
    std::string l1Log;        // per frame: instructions, last stall, a count, then the RAM texels its write cache held
    std::string statsFrom;    // --stats-from TEXT: the STATS line is for the run from where the console says TEXT (no GPU timing)
    double statsAfter = -1;   // >= 0: print a STATS line for the part of the run after this many seconds
    bool noDoubles = false;
    bool holodeck = false;    // --holodeck: tell the guest its owner sits in a holodeck's seat (docs/world/holodeck.md)
    double holodeckAim = 0;   // --holodeck-aim DEGREES: with it, a tracked right hand that points up by so much
    bool holodeckHands = false;
    unsigned host[8] = {};    // --host: the host's eight words as they are (hex), in place of those
    std::string zap;          // --zap KIND,X,Y,REACH,WORD,MASK,SEED,ZONES: the host's hand in RAM (src/zap.h, docs/world/danger.md)
    long long zapFrame = -1;  // --zap-frame N: in the commit of that frame, and no other
    int netPort = 0;          // --net PORT: the network device's link to other machines here (docs/system/lan.md)
    int netId = 1;            // --net-id N: this machine's number, its address 10.0.0.N
    double netLoss = 0, netDelay = 0;   // --net-loss percent, --net-delay milliseconds: of every packet sent
    int cores = 1;            // core 0 and the worker cores: kCores wherever the machine has workers (docs/machine/multicore.md)
    bool lazy = false;        // --lazy: a fill or copy is left to the commit and the pass goes on (LAZY_MEMOP)
    bool noBands = false;     // --no-bands: the commit rewrites all of RAM, not only the bands written to (COMMIT_BANDS off)
    bool unpackInPlace = false;   // --unpack-in-place: a tick that can (an rvc_lean variant) has its unpack drawn into the state texture, no zone copies
    bool pingPong = false;    // --ping-pong: two state textures, as before RAM was written in place (RAM_INPLACE off)
    // --replay DIR;DIR;...: with --load-state, the tick of each folder (its CPUTick pass) beside the machine's own, each
    // run --replay-frames frames from the snapshot's state, --replay-rounds times in turn: wall time a frame, instructions a frame
    std::vector<std::string> replayDirs;
    int replayFrames = 600, replayRounds = 3;
    bool noMrt = false;       // --no-mrt: the tick in one target (D3D11)
    int readbackBatch = 1;    // --readback-batch N: D3D11 maps the rows of N frames at once
    bool ourKernel = false;   // the RAM image is this project's (kernel at +4 MiB, device tree at +34 MiB)
    std::string linuxFolder;  // --image DIR: a folder of our Linux (a private image) instead of build/images/...
    bool sbi = false;         // compile with SBI_HLE
    bool cpu = false;         // --cpu: the instructions run on the processor, the devices on the graphics card
    double cpuIps = 0;        // --cpu-ips N: no more than N instructions a second (0: as fast as it goes)
    std::string compute;      // --compute FILE: the processor as a compute shader, the devices the shader's (experiments/rv32_min)
    int computeCores = 256;   // --compute-cores N: core 0 and N - 1 workers, a thread group each
    int computePasses = 0;    // --compute-passes N: the processor's passes a frame at most (0: 4 for the fast shader, else 1)
    int computeSplit = -1;    // --compute-split F: of them the first F always run, a core's at most --ticks over F (-1: 2 for the fast shader)
    bool ticksSet = false;
    int tickRounds = 1;       // --tick-rounds N: the tick and its commit N times a frame (the fragment machine)
    double unityMs = 0, unityOverMs = 1.4;   // --unity F[,U[,R]]: a world's frame of F ms, U of them Unity's own (below)
    double unityRoundMs = 0;  // R: Unity's CPU a round (its cameras and blits), which the card's time a round cannot go under
    int tickSplit = 0;        // --tick-split F: of them the first F always run (0: all), the rest while core 0 rests    // --ticks was given (--compute's fast shader then takes 8192 a pass of its split)
    bool noGpu = false;       // --no-gpu: leave out the GPU device's passes (gpu.shader)
    bool noSound = false;     // --no-sound: leave out the sound card (sound.shader)
    bool sound = false;       // play the sound card: the terminal mode, or --sound
    std::string soundCapture; // WAV of what the sound card played, written at exit
    int volume = 10;          // --volume: how loud the host plays it, in percent
    bool doubles = false;     // --doubles: keep the shader's double math under DXC too
#ifdef RVC_DEFAULT_DXC
    bool dxc = true;          // D3D12 + DXC instead of D3D11 + FXC
#else
    bool dxc = false;
#endif
    std::string dxcOpt = "-O3", dxcSm = "6_6", dxcDir;
    bool rvcDirSet = false, payloadSet = false;
    std::string image;        // --image: a named boot image from kImages
    // --gvm DIR: a gpu_vm machine (gpu_vm/platform.md, harness/apps/gvm_backend11.cpp) booting the image in DIR
    // (vmlinux, gvm.dtb, rootfs.romfs); --gvm-tick FILE its tick, --gvm-ram MB its RAM
    std::string gvm, gvmTick;
    unsigned gvmRamMB = 0;
    int machine = -1;         // --machine: 0 full, 1 no paging, 2 machine mode only, -1 = most the image allows
    std::vector<std::string> defines;
    GpuOptions gpu;
    UINT fxcFlags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
};

void usage() {
    fprintf(stderr,
R"(usage: rvc_harness [options]

Runs rvc's main.shader (RISC-V Linux) headlessly on D3D11 and connects its UART to this console.

  --rvc DIR            rvc shader folder (default rvc/_Nix/rvc)
  --payload DIR        folder with the payload PNGs (default <rvc>/data-net)
  --ram/--mtd/--dtb P  PNG name prefixes in the payload folder (default linux_payload, rootfs, dts;
                       "none" leaves that texture black)
  --ticks N            emulated instructions per tick pass (default 8192; upstream uses 2048). Large values on a real
                       GPU can trigger a driver timeout (TDR); WARP has no timeout.
  --frames N           stop after N frames          --seconds S   stop after S seconds
  --until TEXT         stop (exit 0) once the UART output contains TEXT; exit 3 if a limit hits first
  --input TEXT         type TEXT at start (escapes: \n \r \t \\ \xHH)
  --expect A --send B  after output contains A, type B (pairs, processed in order)
  --no-stdin           do not forward console input
  --fixed-dt S         drive _Time as frame*S seconds (deterministic) instead of wall clock
  --pointer-sweep N    from frame N on the pointer goes round the middle of the display, a pixel
                       every few frames: what a program pays for a pointer that moves
  --pointer-button B,S,T  the buttons word is B (2: the right one) from second S to T of the clock
  --uart-log FILE      append all UART output to FILE
  --dump-state FILE    write the 64x64 state area (raw uint32 RGBA) at exit
  --save-state FILE    write a snapshot of the whole machine (290 MB) at exit
  --record-input FILE  write the window's keys, pointer and typing to FILE, each with its second of the run
  --replay-input FILE  play FILE's input at its seconds instead of the window's (Ctrl+] still quits)
  --stamp              print the processor folder's WARP stamp (its sources hashed) and exit: the card runs
                       a folder only when its warp_ok holds it (tools/test_card.py --warp --mark-ok)
  --load-state FILE    resume from a snapshot instead of booting
  --replay DIR;DIR...  with --load-state: the tick (CPUTick pass) of each folder beside the machine's own,
                       each run --replay-frames N (600) frames from the snapshot's state, in turn,
                       --replay-rounds R (3) times; prints ms a frame and instructions a frame for each
  --screen-log FILE    with --cpu: the display as it is shown (its layers, the cursor over them), every
                       frame: 16 bytes (width, height, frame, the guest's milliseconds), then red, green,
                       blue a pixel. --screen-from TEXT: not before the console has shown TEXT;
                       --screen-frames N: stop (exit 0) after N of them; --screen-every MS: one not
                       sooner than MS of the guest's milliseconds after the last
  --bench N            benchmark: skip N warm-up frames, then time the rest (GPU fully drained at both
                       ends) and print a BENCH line with IPS and a hash of the state area
  --present            present each frame to a hidden 64x64 swapchain, so tools that define a frame
                       by Present (Nsight GPU Trace) can see frame boundaries
  --no-doubles         compile with NO_DOUBLES (shader must support it): timer value from the host,
                       exact integer MULH. Not bit-identical to the double build.
  --define NAME        add a preprocessor define to the shader build (repeatable)
  --uart-burst N       input characters per handshake (default: the shader's _UartBurst property, else 1)
  --viz                open a window showing memory live (writes glow); --no-viz turns it off
  --viz-capture FILE   save the memory view as a BMP at exit
  --resume             when started without other arguments: resume the shell snapshot instead of booting
  --gvm DIR            a gpu_vm machine (gpu_vm/platform.md): DIR's vmlinux, gvm.dtb and rootfs.romfs (as
                       gpu_vm/userland/romroot.sh or apps/image.sh make them), the tick of --gvm-tick FILE
                       (gpu_vm/tick/steps/x18.hlsl), --gvm-ram MB of RAM (DIR's ram_mb, or 192); --ticks its bundles a pass
  --image NAME|DIR     boot a named image instead of --payload/--ram/--mtd/--dtb; --image list shows them.
                       DIR: a folder of our Linux without an MMU (a private image, build\\images\\NAME)
                       Without it (and without --payload, --ram or --load-state) a menu asks at start
                       when run from a console; scripts with redirected input or --no-stdin get linux-net.
  --machine M          how much machine the shader is compiled with (experiments/rvc_opt only):
                         full      everything; needed for Linux
                         nopaging  NO_PAGING: satp hardwired to 0, no translation or TLBs
                         mmode     M_MODE_ONLY: also no supervisor/user mode or trap delegation
                         auto      (default) the smallest one the chosen image runs on
  --no-gpu             leave out the GPU device (the passes in <rvc>/gpu.shader, docs/machine/gpu.md)
  --no-bands           have the commit rewrite all of RAM instead of only the bands written to
  --ping-pong          D3D11: two state textures, the commit drawing one from the other, in place of
                       RAM in one texture written in place (docs/machine/gpu.md, "RAM in one texture")
  --lazy               leave a parallel fill or copy to the commit and run on (LAZY_MEMOP, docs/machine/boot.md):
                       fewer passes end early, every instruction is some 1.5 percent slower
  --readback-batch N   D3D11: read the rows of N frames back with one Map (1); the console's
                       output and typed input then come N frames later
  --gpu-capture FILE   save the GPU device's whole colour target as a BMP at exit
  --no-sound           leave out the sound card (<rvc>/sound.shader, docs/machine/sound.md)
  --sound              play what the sound card mixes (the terminal mode does; other runs are silent)
  --volume P           how loud the host plays it, 0 to 100 (default 10). In the window Ctrl+F11 and
                       Ctrl+F12 change it and Ctrl+F9 turns the sound card off and on
  --sound-capture FILE save what it mixed as a WAV at exit, and FILE.frames for tools/sound_reference.py
  --ra-log FILE        per frame: pc, return address and instructions, for tools/pc_callers.py
  --pc-log FILE        sample the guest's pc once a frame, for tools/pc_profile.py
  --frame-log FILE     per frame: pc, instructions, last stall and time, for tools/boot_profile.py
                       (with --stats-from TEXT: from where the console says TEXT)
  --cpu                run the instructions on the processor (docs/machine/cpu-harness.md): some thirty times
                       faster; the display, the GPU, the keys and the pointer are the shader's as before
                       (D3D11, the Linux image in build\\images\\linux)
  --cpu-ips N          the same, at no more than N instructions a second (3000000: the world's speed)
  --compute FILE       the processor as one thread of a compute shader (experiments/rv32_min/README.md,
                       "The whole machine"): fast or plain for rv32_min_fast.hlsl or rv32_min.hlsl.
                       A pass a frame; the display, the GPU, sound, keys and pointer as ever
  --compute-cores N    with --compute: core 0 and N - 1 workers (256; plain has core 0 alone)
  --compute-passes N   with --compute: the processor's passes a frame at most, before the devices' (fast: 4, plain: 1)
  --tick-rounds N      the tick and its commit N times a frame before the devices' passes (1)
  --unity F[,U[,R]]    run as a world does: the guest's clock F ms a frame, and each frame as many rounds as fit
                       F - U ms on the card (U: Unity's own part of a frame, default 1.4), the devices once a frame;
                       R: Unity's CPU a round in ms (default 0): a frame's rounds cost at least R each
  --tick-split F       of them the first F always run; the others give core 0 the rest of its F x --ticks,
                       and run the workers only while core 0 rests (experiments/rvc_lean: 0, all)
  --compute-split F    with --compute: the first F of them always run, each core's at most --ticks / F; the rest
                       only while core 0 rests (fast: 2, and --ticks 16384 unless given)
  --open ADDRESS       tell the guest to open an address, or a file of this computer (file:// and its path);
                       several are told ten seconds apart (docs/system/open.md)
  --open-after TEXT    not before the console has shown TEXT
  --persist FILE       the file the guest's /persistence is kept in (docs/system/persist.md)
  --holodeck           tell the guest its owner sits in a holodeck's seat (docs/world/holodeck.md)
  --zap K,X,Y,R,W,M,S,Z  flip bits of RAM in one commit (docs/world/danger.md): kind 1 (word W of texel X,Y) or 2
                       (a square reaching R texels each way), mask M and seed S (hex), zones Z (a bit each)
  --zap-frame N        the frame whose commit does it
  --host W0,...,W7     the host's eight words in hex, as rvc_cpu's (docs/world/holodeck.md)
  --holodeck-aim D     and has tracked hands, the right one pointing up by D degrees
  --net PORT           the network (docs/system/lan.md): this machine is 10.0.0.N and hears UDP port PORT + N of
                       this computer, where the other machines started with the same PORT send (D3D11)
  --net-id N           its number N, 1 to 16 (default 1)
  --net-loss P         lose P percent of the packets it sends   --net-delay MS   and hold the rest back MS ms
  --l1-log FILE        per frame: the addresses in the write cache, for tools/l1_study.py (D3D12)
  --stats-after S      print instructions/s and frames/s for the run after its first S seconds
                       (and start the pc log there)
  --no-desktop         terminal mode: boot to the shell only (by default the guest starts its desktop);
                       --desktop asks for it in a run with other arguments
  --kernel mmu|nommu   Linux with the MMU (build\\images\\linux) or without one (build\\images\\nommu, the machine
                       without paging; docs/system/nommu.md); the default is nommu once that image has been made
  --isa base|zb        RV32IMAF, or with Zba and Zbb (the shader's ZB; without an MMU build\\images\\nommu-zb,
                       whose programs use them)
  --tabs               the console as four terminals: 0x1e and a digit, either way, says whose bytes follow
  --workers auto|2|4|8 how geotune lays out the workers at boot (docs/machine/multicore.md, "Packing the small workers")
  --dxc / --d3d11      backend: D3D12 with DXC-compiled DXIL, or D3D11 with FXC bytecode (what VRChat
                       runs). rvc_harness_dxc.exe defaults to --dxc. DXC implies NO_DOUBLES and, unless
                       --rvc is given, the experiments/rvc_opt shader (upstream does not compile with DXC).
  --doubles            with --dxc: keep the shader's double math (about 15 percent slower)
  --dxc-opt "FLAGS"    DXC optimisation flags (default -O3)   --dxc-sm 6_x   --dxc-dir DIR
  --profile            compile with PROFILE defined and print the shader's PROF() event counters at
                       exit (names from <rvc>/src/prof.h; with --bench, counted after the warm-up)
  --stats S            print speed stats to stderr every S seconds (title bar always shows them)
  --warp               use WARP (software) instead of the GPU
  --adapter N          use DXGI adapter N        --list-adapters
  --debug              enable the D3D11 debug layer
  --fxc-flags HEX      override FXC flags (default 0x%X = backwards-compat | O3)
  --skip-opt           compile with D3DCOMPILE_SKIP_OPTIMIZATION (much faster to compile)
  --cache DIR          shader bytecode cache (default build/shadercache)
  --init-frames N      frames with _Init=1 at startup (default 2)
  --verbose            print FXC warnings
)", (unsigned)(D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3));
}

std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { r += s[i]; continue; }
        char c = s[++i];
        switch (c) {
            case 'n': r += '\n'; break;
            case 'r': r += '\r'; break;
            case 't': r += '\t'; break;
            case 'e': r += '\x1b'; break;
            case '\\': r += '\\'; break;
            case 'x':
                if (i + 2 < s.size()) {
                    r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
                    i += 2;
                }
                break;
            default: r += '\\'; r += c; break;
        }
    }
    return r;
}

bool parseArgs(int argc, char** argv, Options& o) {
    std::string pendingExpect;
    bool havePendingExpect = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); exit(0); }
        else if (a == "--list-adapters") { listAdapters(); exit(0); }
        else if (a == "--rvc") {
            o.rvcDir = next("--rvc");
            std::replace(o.rvcDir.begin(), o.rvcDir.end(), '\\', '/');   // compared with "experiments/rvc_opt"
            o.rvcDirSet = true;
        }
        else if (a == "--payload") { o.payloadDir = next("--payload"); o.payloadSet = true; }
        else if (a == "--image") o.image = next("--image");
        else if (a == "--gvm") o.gvm = next("--gvm");
        else if (a == "--gvm-tick") o.gvmTick = next("--gvm-tick");
        else if (a == "--gvm-ram") o.gvmRamMB = (unsigned)atoi(next("--gvm-ram").c_str());
        else if (a == "--machine") {
            std::string v = next("--machine");
            const char* names[4] = {"auto", "full", "nopaging", "mmode"};
            int k = 0;
            while (k < 4 && v != names[k]) ++k;
            if (k == 4) { fprintf(stderr, "--machine takes auto, full, nopaging or mmode\n"); return false; }
            o.machine = k - 1;
        }
        else if (a == "--dxc") o.dxc = true;
        else if (a == "--d3d11") o.dxc = false;
        else if (a == "--doubles") o.doubles = true;
        else if (a == "--dxc-opt") o.dxcOpt = next("--dxc-opt");
        else if (a == "--dxc-sm") o.dxcSm = next("--dxc-sm");
        else if (a == "--dxc-dir") o.dxcDir = next("--dxc-dir");
        else if (a == "--ram") { o.ramPrefix = next("--ram"); o.payloadSet = true; }
        else if (a == "--mtd") o.mtdPrefix = next("--mtd");
        else if (a == "--dtb") o.dtbPrefix = next("--dtb");
        else if (a == "--ticks") { o.ticks = atoi(next("--ticks").c_str()); o.ticksSet = true; }
        else if (a == "--frames") o.maxFrames = strtoull(next("--frames").c_str(), nullptr, 10);
        else if (a == "--seconds") o.maxSeconds = atof(next("--seconds").c_str());
        else if (a == "--until") o.until = unescape(next("--until"));
        else if (a == "--input") o.initialInput += unescape(next("--input"));
        else if (a == "--expect") { pendingExpect = unescape(next("--expect")); havePendingExpect = true; }
        else if (a == "--send") {
            std::string s = unescape(next("--send"));
            if (!havePendingExpect) { fprintf(stderr, "--send needs a preceding --expect\n"); return false; }
            o.expectSend.push_back({pendingExpect, s});
            havePendingExpect = false;
        }
        else if (a == "--no-stdin") o.readStdin = false;
        else if (a == "--fixed-dt") o.fixedDt = atof(next("--fixed-dt").c_str());
        else if (a == "--pointer-sweep") o.pointerSweep = atoll(next("--pointer-sweep").c_str());
        else if (a == "--pointer-button") sscanf(next("--pointer-button").c_str(), "%d,%lf,%lf", &o.pointerButton, &o.pointerButtonFrom, &o.pointerButtonTo);
        else if (a == "--uart-log") o.uartLog = next("--uart-log");
        else if (a == "--dump-state") o.dumpState = next("--dump-state");
        else if (a == "--save-state") o.saveState = next("--save-state");
        else if (a == "--record-input") o.recordInput = next("--record-input");
        else if (a == "--stamp") o.printStamp = true;
        else if (a == "--replay-input") o.replayInput = next("--replay-input");
        else if (a == "--screen-log") o.screenLog = next("--screen-log");
        else if (a == "--screen-from") o.screenFrom = unescape(next("--screen-from"));
        else if (a == "--screen-frames") o.screenFrames = atoi(next("--screen-frames").c_str());
        else if (a == "--screen-every") o.screenEvery = atoi(next("--screen-every").c_str());
        else if (a == "--load-state") o.loadState = next("--load-state");
        else if (a == "--replay") {
            std::string list = next("--replay");
            for (size_t s = 0; s <= list.size();) {
                size_t e = list.find(';', s);
                if (e == std::string::npos) e = list.size();
                if (e > s) o.replayDirs.push_back(list.substr(s, e - s));
                s = e + 1;
            }
        }
        else if (a == "--replay-frames") o.replayFrames = atoi(next("--replay-frames").c_str());
        else if (a == "--replay-rounds") o.replayRounds = atoi(next("--replay-rounds").c_str());
        else if (a == "--bench") o.benchWarmup = atoll(next("--bench").c_str());
        else if (a == "--stats") o.statsInterval = atof(next("--stats").c_str());
        else if (a == "--warp") o.gpu.warp = true;
        else if (a == "--adapter") o.gpu.adapterIndex = atoi(next("--adapter").c_str());
        else if (a == "--debug") o.gpu.debug = true;
        else if (a == "--fxc-flags") o.fxcFlags = (UINT)strtoul(next("--fxc-flags").c_str(), nullptr, 16);
        else if (a == "--skip-opt") o.fxcFlags = (o.fxcFlags & ~(UINT)D3DCOMPILE_OPTIMIZATION_LEVEL3) | D3DCOMPILE_SKIP_OPTIMIZATION;
        else if (a == "--cache") o.cacheDir = next("--cache");
        else if (a == "--init-frames") o.initFrames = atoi(next("--init-frames").c_str());
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--profile") o.profile = true;
        else if (a == "--present") o.present = true;
        else if (a == "--uart-burst") o.uartBurst = atoi(next("--uart-burst").c_str());
        else if (a == "--viz") o.viz = true;
        else if (a == "--no-viz") o.viz = false;
        else if (a == "--resume") o.resume = true;
        else if (a == "--no-desktop") o.desktop = false;
        else if (a == "--kernel") {
            o.kernel = next("--kernel");
            if (o.kernel != "mmu" && o.kernel != "nommu") { fprintf(stderr, "--kernel takes mmu or nommu\n"); return false; }
        }
        else if (a == "--isa") {
            o.isa = next("--isa");
            if (o.isa != "base" && o.isa != "zb") { fprintf(stderr, "--isa takes base or zb\n"); return false; }
        }
        else if (a == "--desktop") o.desktop = true;
        else if (a == "--tabs") o.tabs = true;
        else if (a == "--workers") { std::string w = next("--workers"); o.workers = w == "2" ? 1 : w == "4" ? 2 : w == "8" ? 3 : 0; }
        else if (a == "--open") o.open.push_back(next("--open"));
        else if (a == "--open-after") o.openAfter = unescape(next("--open-after"));
        else if (a == "--persist") o.persist = next("--persist");
        else if (a == "--viz-capture") { o.vizCapture = next("--viz-capture"); o.viz = true; }
        else if (a == "--no-doubles") o.noDoubles = true;
        else if (a == "--no-gpu") o.noGpu = true;
        else if (a == "--gpu-capture") o.gpuCapture = next("--gpu-capture");
        else if (a == "--no-sound") o.noSound = true;
        else if (a == "--sound") o.sound = true;
        else if (a == "--volume") o.volume = (std::max)(0, (std::min)(100, atoi(next("--volume").c_str())));
        else if (a == "--sound-capture") o.soundCapture = next("--sound-capture");
        else if (a == "--pc-log") o.pcLog = next("--pc-log");
        else if (a == "--ra-log") o.raLog = next("--ra-log");
        else if (a == "--frame-log") o.frameLog = next("--frame-log");
        else if (a == "--l1-log") o.l1Log = next("--l1-log");
        else if (a == "--stats-after") o.statsAfter = atof(next("--stats-after").c_str());
        else if (a == "--stats-from") o.statsFrom = unescape(next("--stats-from"));
        else if (a == "--define") o.defines.push_back(next("--define"));
        else if (a == "--no-bands") o.noBands = true;
        else if (a == "--ping-pong") o.pingPong = true;
        else if (a == "--unpack-in-place") o.unpackInPlace = true;
        else if (a == "--lazy") o.lazy = true;
        else if (a == "--no-mrt") o.noMrt = true;
        else if (a == "--readback-batch") o.readbackBatch = atoi(next("--readback-batch").c_str());
        else if (a == "--holodeck") o.holodeck = true;
        else if (a == "--host") sscanf(next("--host").c_str(), "%x,%x,%x,%x,%x,%x,%x,%x", &o.host[0], &o.host[1], &o.host[2], &o.host[3],
                                       &o.host[4], &o.host[5], &o.host[6], &o.host[7]);
        else if (a == "--zap") o.zap = next("--zap");
        else if (a == "--zap-frame") o.zapFrame = atoll(next("--zap-frame").c_str());
        else if (a == "--holodeck-aim") { o.holodeckAim = atof(next("--holodeck-aim").c_str()); o.holodeckHands = true; }
        else if (a == "--net") o.netPort = atoi(next("--net").c_str());
        else if (a == "--net-id") o.netId = atoi(next("--net-id").c_str());
        else if (a == "--net-loss") o.netLoss = atof(next("--net-loss").c_str());
        else if (a == "--net-delay") o.netDelay = atof(next("--net-delay").c_str());
        else if (a == "--cpu") o.cpu = true;
        else if (a == "--compute") {
            o.compute = next("--compute");
            if (o.compute == "fast" || o.compute == "plain")
                o.compute = o.compute == "fast" ? "experiments/rv32_min/rv32_min_fast.hlsl" : "experiments/rv32_min/rv32_min.hlsl";
        }
        else if (a == "--compute-cores") o.computeCores = (std::max)(1, (std::min)(256, atoi(next("--compute-cores").c_str())));
        else if (a == "--compute-passes") o.computePasses = (std::max)(1, (std::min)(64, atoi(next("--compute-passes").c_str())));
        else if (a == "--tick-split") o.tickSplit = (std::max)(0, (std::min)(16, atoi(next("--tick-split").c_str())));
        else if (a == "--unity") {
            std::string v = next("--unity");
            o.unityMs = atof(v.c_str());
            size_t c1 = v.find(',');
            if (c1 != std::string::npos) {
                o.unityOverMs = atof(v.c_str() + c1 + 1);
                size_t c2 = v.find(',', c1 + 1);
                if (c2 != std::string::npos) o.unityRoundMs = atof(v.c_str() + c2 + 1);
            }
            o.fixedDt = o.unityMs / 1000.0;
            // (a world draws the GPU's list in every frame's devices: gpu_vm's lazy list waited for a readback frames behind)
            if (!getenv("GVM_LIST_LAZY")) _putenv("GVM_LIST_LAZY=0");
        }
        else if (a == "--tick-rounds") o.tickRounds = (std::max)(1, (std::min)(16, atoi(next("--tick-rounds").c_str())));
        else if (a == "--compute-split") o.computeSplit = (std::max)(0, (std::min)(64, atoi(next("--compute-split").c_str())));
        else if (a == "--cpu-ips") { o.cpu = true; o.cpuIps = atof(next("--cpu-ips").c_str()); }
        else if (a == "--core-pitch" || a == "--small-cores") next(a.c_str());   // (of the layouts before the geometry: taken and ignored)
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
    }
    if (havePendingExpect) { fprintf(stderr, "--expect without --send\n"); return false; }
    if (o.dxc && !o.doubles) o.noDoubles = true;
    return true;
}

// Snapshot file: this header, then width*height RGBA32_UINT texels, row 0 first.
struct SnapshotHeader {
    char magic[8];
    uint32_t width, height;
    double time;  // guest wall clock (_Time.y), so the timer does not jump back on resume
    uint32_t sentTag, sentChar;
};
const char kSnapshotMagic[8] = {'S', 'X', '8', '6', 'S', 'N', 'A', 'P'};

std::mutex g_inputMutex;
std::deque<char> g_stdinQueue;

std::atomic<bool> g_quit{false};

// Raw console pass-through: every key goes to the guest as the bytes a terminal would send
// (arrows as escape sequences, Ctrl+C as 0x03). Ctrl+] is the one key kept for the host.
bool g_rawConsole = false;
HANDLE g_conIn = nullptr, g_conOut = nullptr;
DWORD g_oldInMode = 0, g_oldOutMode = 0;
UINT g_oldOutCp = 0;
const char kQuitKey = 0x1d;  // Ctrl+]

void restoreConsole();

bool beginRawConsole() {
    g_conIn = GetStdHandle(STD_INPUT_HANDLE);
    g_conOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(g_conIn, &g_oldInMode)) return false;  // stdin is a pipe or file
    GetConsoleMode(g_conOut, &g_oldOutMode);
    g_oldOutCp = GetConsoleOutputCP();
    // No line buffering, echo or Ctrl+C handling: the guest's tty does all of that.
    if (!SetConsoleMode(g_conIn, ENABLE_VIRTUAL_TERMINAL_INPUT)) return false;
    SetConsoleOutputCP(CP_UTF8);
    g_rawConsole = true;
    atexit(restoreConsole);  // error paths that exit() must not leave the user's shell in raw mode
    return true;
}

void restoreConsole() {
    if (!g_rawConsole) return;
    SetConsoleMode(g_conIn, g_oldInMode);
    SetConsoleMode(g_conOut, g_oldOutMode);
    SetConsoleOutputCP(g_oldOutCp);
    g_rawConsole = false;
}

void stdinThread() {
    if (g_rawConsole) {
        char buf[256];
        DWORD n = 0;
        while (ReadFile(g_conIn, buf, sizeof buf, &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lock(g_inputMutex);
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] == kQuitKey) { g_quit = true; return; }
                g_stdinQueue.push_back(buf[i]);
            }
        }
        return;
    }
    for (;;) {
        int c = getchar();
        if (c == EOF) return;
        if (c == '\r') continue;  // piped text has CRLF; the guest tty wants a single newline
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_stdinQueue.push_back((char)c);
    }
}

// Started from Explorer the working directory is bin\; the default paths are relative to the
// repository root one level up.
void findRepoRoot(const Options& o) {
    if (fs::exists(fs::u8path(o.rvcDir) / "main.shader")) return;
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    fs::path root = fs::path(exe).parent_path().parent_path();
    std::error_code ec;
    if (fs::exists(root / fs::u8path(o.rvcDir) / "main.shader", ec)) fs::current_path(root, ec);
}

// What the emulated machine can boot: payload PNG sets shipped with rvc, under kUpstream.
const char* const kUpstream = "rvc/_Nix/rvc";

// The patched shader's folder, or a copy of it beside other work (any folder named rvc_opt...).
bool isOpt(const std::string& dir) {
    // (experiments/rvc_lean: the same machine with the processor rewritten in the compute shader's shape)
    std::string name = fs::u8path(dir).filename().u8string();
    return dir == "experiments/rvc_opt" || name.rfind("rvc_opt", 0) == 0 || name.rfind("rvc_lean", 0) == 0;
}
const char* const kPrograms = "programs/bin";   // our own programs, as raw memory images
struct BootImage {
    const char* name;
    const char* title;
    const char* dir;
    const char* ram;
    const char* mtd;
    const char* dtb;
    int machine;   // the smallest machine it runs on: 0 full, 1 no paging, 2 machine mode only
};
const BootImage kImages[] = {
    {"linux-net", "Linux with a shell; glxgears runs on the GPU device", "data-net", "linux_payload", "rootfs", "dts", 0},
    {"linux", "Linux, kernel with built-in initramfs", "data", "linux_payload", "none", "dts", 0},
    {"micropython", "MicroPython on OpenSBI (bare metal REPL)", "data", "mprv_payload", "none", "dts", 1},
    {"rust", "Rust test payload on OpenSBI (bare metal)", "data", "rust_payload", "none", "dts", 1},
    {"gears", "Gears: three lit, textured gears drawn by the GPU device (bare metal C)", nullptr, "gears", "none", "none", 2},
    {"rects", "GPU test card: 3,600 rectangles in one list, written back to RAM (bare metal C)", nullptr, "rects", "none", "none", 2},
    {"blend", "GPU test card: the eight passes, blending with and without depth (bare metal C)", nullptr, "blend", "none", "none", 2},
    {"sound", "Sound test card: every kind of voice for half a second (bare metal C)", nullptr, "sound", "none", "none", 2},
    {"raycast", "Raycaster: walk a textured maze on the display (bare metal C)", nullptr, "raycast", "none", "none", 2},
    {"raytrace", "Raytracer drawing to the display (bare metal C)", nullptr, "raytrace", "none", "none", 2},
    {"classes", "What each kind of instruction costs: loops of one kind, a mark between them (bare metal C)", nullptr, "classes", "none", "none", 0},
    {"fpcheck", "Float instructions on the fast path: prints a sum to compare between shader builds (bare metal C)", nullptr, "fpcheck", "none", "none", 0},
    {"rvc-raytrace", "rvc's Rust raytracer, drawing into raw memory", "data", "rust_raytrace", "none", "dts", 1},
    {"bare", "C bare-metal test (no firmware)", "data", "bare", "none", "dts", 1},
};

// dir = nullptr: one of our programs.
std::string imageDir(const BootImage& im) {
    return im.dir ? (fs::u8path(kUpstream) / im.dir).u8string() : std::string(kPrograms);
}

bool imageAvailable(const BootImage& im) {
    std::error_code ec;
    return fs::exists(fs::u8path(imageDir(im)) / (std::string(im.ram) + (im.dir ? ".r.png" : ".bin")), ec);
}

const BootImage* findImage(const std::string& name) {
    for (auto& im : kImages)
        if (name == im.name) return &im;
    return nullptr;
}

// Our Linux's folder for the kernel and instruction set asked for (linux/nommu/image.py makes the
// no-MMU ones; with the MMU the programs are RV32IMAF whatever the shader runs).
std::string linuxDir(const Options& o) {
    if (!o.linuxFolder.empty()) return o.linuxFolder;
    if (o.kernel != "nommu") return "build/images/linux";
    return o.isa == "zb" ? "build/images/nommu-zb" : "build/images/nommu";
}

bool linuxBuilt(const Options& o) {
    std::error_code ec;
    return fs::exists(fs::u8path(linuxDir(o)) / "linux_payload.bin", ec);
}

void applyImage(Options& o, const BootImage& im) {
    o.image = im.name;
    o.payloadDir = imageDir(im);
    o.ramPrefix = im.ram;
    o.mtdPrefix = im.mtd;
    o.dtbPrefix = im.dtb;
    // Our build of the Linux image (tools/make_linux_image.py): upstream's root filesystem with
    // our programs added, and a device tree that keeps the kernel out of the GPU's memory.
    std::error_code ec;
    std::string dir = linuxDir(o);
    if (o.image == "linux-net" && fs::exists(dir + "/rootfs.bin", ec) && fs::exists(dir + "/dts.bin", ec)) {
        o.mtdPrefix = dir + "/rootfs";
        o.dtbPrefix = dir + "/dts";
        // and our kernel, if it has been built (linux/kernel/build.sh)
        if (fs::exists(dir + "/linux_payload.bin", ec)) {
            o.ramPrefix = dir + "/linux_payload";
            o.ourKernel = true;
        }
    }
}

// Start menu, shown when the command line does not say what to boot. False = the user backed out.
// What the menu's k and i say of Linux: the kernel, the instructions, and the folder they boot.
void printLinuxChoice(const Options& o) {
    fprintf(stderr, "    k  kernel: %s\n", o.kernel == "nommu" ? "Linux without an MMU (bFLT programs, the machine without paging)" : "Linux with the MMU (Sv32 paging)");
    fprintf(stderr, "    i  instructions: %s\n", o.isa == "zb" ? "RV32IMAF + Zba, Zbb" : "RV32IMAF");
    fprintf(stderr, "       Linux boots %s%s\n", linuxDir(o).c_str(),
            !linuxBuilt(o) ? "  (not made: docs/system/nommu.md)" : o.isa == "zb" && o.kernel != "nommu" ? "  (its programs are RV32IMAF)" : "");
}

bool chooseImage(Options& o, bool canResume) {
    std::vector<const BootImage*> list;
    for (auto& im : kImages)
        if (imageAvailable(im)) list.push_back(&im);
    fprintf(stderr, "\n  rvc: a RISC-V machine in a pixel shader    [%s]\n\n  Boot which image?\n\n",
            o.dxc ? "D3D12 + DXC" : "D3D11 + FXC");
    for (size_t i = 0; i < list.size(); ++i) {
        int machine = strcmp(list[i]->name, "linux-net") == 0 && o.kernel == "nommu" ? 1 : list[i]->machine;
        fprintf(stderr, "    %zu  %-13s %s%s\n", i + 1, list[i]->name, list[i]->title,
                machine == 0 ? "  [full machine]" : machine == 1 ? "  [no paging]" : "  [machine mode only]");
    }
    if (canResume) fprintf(stderr, "    r  %-13s %s\n", "resume", "Linux at the shell prompt, from the saved snapshot (with the MMU)");
    const char* kMachine[4] = {"auto: the smallest the image runs on (shown in brackets)", "full",
                               "no paging (only Linux without an MMU boots)", "machine mode only (OpenSBI images will not boot)"};
    fprintf(stderr, "\n");
    printLinuxChoice(o);
    fprintf(stderr, "    m  machine: %s\n", kMachine[o.machine + 1]);
    fprintf(stderr, "\n  Press a key (Enter = 1, Esc = quit): ");
    for (;;) {
        int c = _getch();
        if (c == 0 || c == 0xE0) { _getch(); continue; }  // function and arrow keys come as two codes
        if (c == 'm' || c == 'M') {
            o.machine = o.machine == 2 ? -1 : o.machine + 1;
            fprintf(stderr, "\n    m  machine: %s\n\n  Press a key (Enter = 1, Esc = quit): ", kMachine[o.machine + 1]);
            continue;
        }
        if (c == 'k' || c == 'K' || c == 'i' || c == 'I') {
            if (c == 'k' || c == 'K') o.kernel = o.kernel == "nommu" ? "mmu" : "nommu";
            else o.isa = o.isa == "zb" ? "base" : "zb";
            fprintf(stderr, "\n\n");
            printLinuxChoice(o);
            fprintf(stderr, "\n  Press a key (Enter = 1, Esc = quit): ");
            continue;
        }
        if (c == 27 || c == 3 || c == kQuitKey) { fprintf(stderr, "\n"); return false; }
        if (c == '\r') c = '1';
        if ((c == 'r' || c == 'R') && canResume) {
            o.resume = true;
            fprintf(stderr, "resume\n\n");
            return true;
        }
        if (c >= '1' && (size_t)(c - '1') < list.size()) {
            applyImage(o, *list[c - '1']);
            fprintf(stderr, "%s\n\n", o.image.c_str());
            return true;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;
    findRepoRoot(opt);
    if (opt.image == "list") {
        for (auto& im : kImages)
            printf("%-12s %s%s\n", im.name, im.title, imageAvailable(im) ? "" : "  (files missing)");
        return 0;
    }
    if (!opt.printStamp && !run_lock_take(!opt.cpu, opt.cpu ? (opt.gpu.warp ? "rvc_harness --cpu --warp" : "rvc_harness --cpu") : "rvc_harness"))
        return kRunLockRefused;

    // Opened with no arguments (or only --resume / --no-viz): act as a terminal on the emulated
    // machine. Fastest shader, a cold boot you can watch, memory view on.
    bool terminalMode = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--image" || a == "--cpu-ips" || a == "--kernel" || a == "--isa" || a == "--compute" || a == "--compute-cores" || a == "--compute-passes") ++i;
        // (--cpu is the same terminal on the interpreter: its desktop and view come up too)
        else if (a != "--resume" && a != "--no-viz" && a != "--dxc" && a != "--d3d11" && a != "--no-desktop" && a != "--cpu") terminalMode = false;
    }
    std::error_code ec;
    bool haveOpt = fs::exists("experiments/rvc_opt/main.shader", ec);
    if (!opt.gvm.empty()) {
        terminalMode = false;
        if (opt.cpu || !opt.compute.empty() || opt.dxc) { fprintf(stderr, "[harness] --gvm is D3D11's, with no --cpu or --compute\n"); return 2; }
        opt.rvcDir = "experiments/rvc_opt";
        opt.rvcDirSet = true;
        opt.kernel = "nommu";
    }
    // (--cpu needs the shader with the GPU device and the display: upstream's has neither)
    if ((terminalMode || opt.dxc || opt.cpu || !opt.compute.empty()) && !opt.rvcDirSet && haveOpt) opt.rvcDir = "experiments/rvc_opt";
    const char* shellSnap = "build/snapshots/rvc_shell.snap";
    bool canResume = terminalMode && fs::exists(shellSnap, ec);
    // Nothing on the command line says what to boot: ask, if there is someone to ask.
    DWORD conMode = 0;
    bool interactive = opt.readStdin && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &conMode) != 0;
    if ((opt.cpu || !opt.compute.empty()) && opt.image.empty()) opt.image = "linux-net";   // (the interpreter boots that one: nothing to ask)
    // Linux is the no-MMU build once it has been made, the MMU one (legacy) only by --kernel mmu
    if (opt.kernel.empty()) {
        opt.kernel = "nommu";
        if (!linuxBuilt(opt)) opt.kernel = "mmu";
    }
    if (terminalMode && opt.image.empty() && !opt.resume && !interactive) opt.image = "linux-net";
    bool chosen = !opt.image.empty() || opt.payloadSet || !opt.loadState.empty() || opt.resume || !opt.gvm.empty();
    if (!chosen && interactive && !chooseImage(opt, canResume)) return 0;
    if (terminalMode) {
        if (opt.resume && canResume) {
            opt.loadState = shellSnap;
            if (opt.image.empty()) opt.image = "linux-net";   // the snapshot is of that image
        }
        fs::create_directories("logs", ec);
        opt.uartLog = "logs/uart.log";
        opt.viz = true;
        opt.sound = true;
        for (int i = 1; i < argc; ++i)
            if (strcmp(argv[i], "--no-viz") == 0) opt.viz = false;
        opt.desktop = true;
        for (int i = 1; i < argc; ++i)
            if (strcmp(argv[i], "--no-desktop") == 0) opt.desktop = false;
        // a resumed shell is past the point where the guest decides: type the command instead
        if (opt.desktop && !opt.loadState.empty()) opt.initialInput += "nx\n";
    }
    // a folder with our Linux in it (tools/private_image.py's): booted as linux-net without the MMU
    if (!opt.image.empty() && !findImage(opt.image) && fs::exists(fs::u8path(opt.image) / "linux_payload.bin", ec)) {
        opt.linuxFolder = opt.image, opt.image = "linux-net", opt.kernel = "nommu";
        std::replace(opt.linuxFolder.begin(), opt.linuxFolder.end(), '\\', '/');
    }
    // (Linux without an MMU runs on the patched shader only)
    if (opt.kernel == "nommu" && opt.image == "linux-net" && !opt.rvcDirSet && haveOpt) opt.rvcDir = "experiments/rvc_opt";
    if (!opt.image.empty() && !opt.payloadSet) {
        const BootImage* im = findImage(opt.image);
        if (!im) {
            fprintf(stderr, "[harness] unknown image '%s' (try --image list)\n", opt.image.c_str());
            return 1;
        }
        applyImage(opt, *im);
    }
    // The full machine unless a named image is known to need less, or the user said so.
    bool nommuLinux = opt.kernel == "nommu" && opt.image == "linux-net" && opt.loadState.empty();
    if (opt.machine < 0) {
        const BootImage* im = opt.loadState.empty() ? findImage(opt.image) : nullptr;
        opt.machine = im && (isOpt(opt.rvcDir) || opt.rvcDir == "experiments/rvc_compute") ? (nommuLinux ? 1 : im->machine) : 0;
    }
    if (nommuLinux && !linuxBuilt(opt)) {
        fprintf(stderr, "[harness] %s has not been made (docs/system/nommu.md)\n", linuxDir(opt).c_str());
        return 1;
    }
    if (opt.isa == "zb" && isOpt(opt.rvcDir)) opt.defines.push_back("ZB");
    // Our Linux image, and snapshots (which are of it), have no firmware: the machine answers
    // the kernel's calls to it (docs/machine/boot.md).
    // (Linux without an MMU is our kernel on the machine without paging, its SBI and float unit kept)
    bool linuxMachine = opt.machine == 0 || (opt.machine == 1 && opt.ourKernel);
    opt.sbi = linuxMachine && (isOpt(opt.rvcDir) || opt.rvcDir == "experiments/rvc_compute") && (opt.ourKernel || !opt.loadState.empty());
    if (opt.machine == 1) opt.defines.push_back("NO_PAGING");
    if (opt.machine == 2) opt.defines.push_back("M_MODE_ONLY");
    // The full machine has single-precision float instructions (docs/machine/fpu.md).
    if (linuxMachine && (isOpt(opt.rvcDir) || opt.rvcDir == "experiments/rvc_compute") &&
        std::find(opt.defines.begin(), opt.defines.end(), "FPU") == opt.defines.end())
        opt.defines.push_back("FPU");
    if (opt.lazy && opt.machine == 0 && isOpt(opt.rvcDir)) opt.defines.push_back("LAZY_MEMOP");
    if (opt.machine > 0)
        fprintf(stderr, "[harness] machine: %s\n", opt.machine == 1 ? "no paging (NO_PAGING)" : "machine mode only (M_MODE_ONLY)");
    if (opt.image == "linux-net" && opt.loadState.empty())
        fprintf(stderr, "[harness] Linux %s, %s: %s\n", opt.kernel == "nommu" ? "without an MMU" : "with the MMU",
                opt.isa == "zb" ? "RV32IMAF + Zba, Zbb" : "RV32IMAF", linuxDir(opt).c_str());
    // Payloads live with upstream rvc; a patched shader folder usually has none of its own.
    if (opt.payloadDir.empty()) {
        opt.payloadDir = (fs::u8path(opt.rvcDir) / "data-net").u8string();
        if (!fs::exists(fs::u8path(opt.payloadDir), ec)) opt.payloadDir = (fs::u8path(kUpstream) / "data-net").u8string();
    }
    if (opt.readStdin && beginRawConsole())
        fprintf(stderr, "[harness] console attached to the guest: every key goes to it, Ctrl+C included. Ctrl+] quits.\n");

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hrCo;

    // Raw byte output (the guest already emits CRLF), with ANSI escape support on consoles.
    _setmode(_fileno(stdout), _O_BINARY);
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    std::string err;

    // --- Shader ---
    std::string shaderPath = (fs::u8path(opt.rvcDir) / "main.shader").u8string();
    SLShader shader;
    if (!loadShaderLab(shaderPath, shader, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    BackendOptions bo;
    bo.verbose = opt.verbose;
    bo.gpu = opt.gpu;
    bo.profile = opt.profile;
    bo.stateLog = !opt.l1Log.empty();
    if (isOpt(opt.rvcDir)) {
        // its tick keeps one rectangle from the top of the state block (STATE_ROWS in src/types.h)
        // The harness's machine: the shader's own write cache (384 texels), and on D3D11 the
        // tick in eight targets, a pixel for every eight texels of state (docs/machine/multicore.md).
        // With the eight targets the machine has all its cores, and so has the interpreter.
        if (!opt.dxc && !opt.noMrt && !opt.cpu && opt.compute.empty() && !opt.profile && !getenv("RVC11_COMPUTE")) {
            opt.defines.push_back("TICK_MRT");
            // (RVC_CORES=N: a machine of fewer cores, to measure what the others cost core 0)
            int cores = getenv("RVC_CORES") ? atoi(getenv("RVC_CORES")) : kCores;
            if (cores > 1) opt.defines.push_back("CORES=" + std::to_string(cores));
            opt.cores = cores;
        }
        if (opt.cpu) opt.cores = kCores;
        // (the plain shader is core 0 alone)
        if (!opt.compute.empty()) opt.cores = opt.compute.find("rv32_min.hlsl") != std::string::npos ? 1 : opt.computeCores;
        if (!opt.compute.empty()) {
            // the fast shader: two passes of 8,192 a frame, and up to two more while core 0 rests
            // (experiments/rv32_min/README.md, "Passes a frame"); the plain one a pass a frame
            bool plain = opt.compute.find("rv32_min.hlsl") != std::string::npos;
            if (opt.computePasses == 0) opt.computePasses = plain ? 1 : 4;
            if (opt.computeSplit < 0) opt.computeSplit = plain ? 0 : 2;
            opt.computeSplit = (std::min)(opt.computeSplit, opt.computePasses);
            if (!opt.ticksSet && opt.computeSplit > 0) opt.ticks = 8192 * opt.computeSplit;
        }
        unsigned ways = 3, tableBits = 6;
        for (auto& d : opt.defines) {
            if (d.rfind("L1_WAYS=", 0) == 0) ways = (unsigned)atoi(d.c_str() + 8);
            if (d.rfind("L1_TABLE_BITS=", 0) == 0) tableBits = (unsigned)atoi(d.c_str() + 14);
        }
        bo.tickRows = ways == 3 && tableBits == 6 ? 16 : 32;
        for (auto& d : opt.defines)   // (a machine with more state says how many rows: STATE_ROWS in src/types.h)
            if (d.rfind("STATE_ROWS=", 0) == 0) bo.tickRows = (unsigned)atoi(d.c_str() + 11);
    }
    bo.present = opt.present;
    bo.readbackBatch = opt.readbackBatch;
    bo.dxcOpt = opt.dxcOpt;
    bo.dxcSm = opt.dxcSm;
    bo.dxcDir = opt.dxcDir;
    bo.computeShader = opt.compute;
    // The GPU device is a second shader next to the CPU's; upstream rvc has none.
    SLShader gpuShader;
    // (a processor folder without devices of its own, experiments/rvc_lean, has experiments/rvc_opt's)
    fs::path deviceDir = fs::exists(fs::u8path(opt.rvcDir) / "gpu.shader", ec) ? fs::u8path(opt.rvcDir) : fs::u8path("experiments/rvc_opt");
    std::string gpuPath = (deviceDir / "gpu.shader").u8string();
    if (!opt.noGpu && fs::exists(fs::u8path(gpuPath), ec)) {
        if (!loadShaderLab(gpuPath, gpuShader, err)) {
            fprintf(stderr, "[harness] %s\n", err.c_str());
            return 1;
        }
        bo.gpuShader = &gpuShader;
        opt.defines.push_back("GPU_DEVICE");   // the Commit pass copies the GPU's picture back
    }
    // The sound card is a third: its mix pass. Its words are kept by the GPU device's control pass.
    SLShader soundShader;
    std::string soundPath = (deviceDir / "sound.shader").u8string();
    if (bo.gpuShader && !opt.noSound && fs::exists(fs::u8path(soundPath), ec)) {
        if (!loadShaderLab(soundPath, soundShader, err)) {
            fprintf(stderr, "[harness] %s\n", err.c_str());
            return 1;
        }
        bo.soundShader = &soundShader;
    }
    // both backends keep two state buffers, which is what lets the commit skip unwritten bands
    if (opt.sbi) opt.defines.push_back("SBI_HLE");
    if (opt.cpu || !opt.compute.empty()) {
        // the interpreter has the cores: the shader is one core's, and runs none
        // (--compute: the control pass reports no cores; the compute shader's sync pass reports its own)
        opt.defines.erase(std::remove_if(opt.defines.begin(), opt.defines.end(), [](const std::string& d) {
            return d.rfind("CORES=", 0) == 0 || d.rfind("CORE_PITCH=", 0) == 0 || d.rfind("MC_", 0) == 0; }), opt.defines.end());
        if (opt.dxc) { fprintf(stderr, "[harness] --cpu and --compute are for the D3D11 backend\n"); return 2; }
        if (opt.cpu && !opt.compute.empty()) { fprintf(stderr, "[harness] --cpu or --compute, not both\n"); return 2; }
    }
    if (!opt.noBands) opt.defines.push_back("COMMIT_BANDS");
    // RAM in one texture, written in place (src/ram.h): D3D11, with no fill or copy left to the commit
    bool lazyOn = std::find(opt.defines.begin(), opt.defines.end(), "LAZY_MEMOP") != opt.defines.end();
    // (--compute, and a rvc_lean folder with a commit of its own, keep two state textures: their commits have no
    // RAM_INPLACE passes; experiments/rvc_lean now takes rvc_opt's commit, whose main.shader brings in src/ram.h)
    bool lean = fs::u8path(opt.rvcDir).filename().u8string().rfind("rvc_lean", 0) == 0;
    std::string shaderText;
    readFileBinary(shaderPath, shaderText);
    bool ownCommit = lean && shaderText.find("ram.h") == std::string::npos;
    bo.tickDirect = ownCommit && opt.unpackInPlace && opt.compute.empty();
    bo.laneField = lean && shaderText.find("LANE_FIELD") != std::string::npos && opt.compute.empty() && !opt.cpu;
    if (!opt.dxc && !opt.pingPong && isOpt(opt.rvcDir) && !ownCommit && opt.compute.empty() && !getenv("RVC11_COMPUTE")) {
        if (lazyOn) fprintf(stderr, "[harness] --lazy has two state textures (RAM_INPLACE off)\n");
        else bo.ramInplace = true;
    }
    // under DXC a local array is not zeroed at the start of every tick, a static one is
    if (opt.dxc) opt.defines.push_back("L1_LOCAL");
    bo.compile.flags = opt.fxcFlags;
    bo.compile.cacheDir = opt.cacheDir;
    bo.compile.includeDirs = {SHADEREMU_UNITY_INCLUDE_DIR};
    bo.compile.defines = {{"SHADER_API_D3D11", "1"}, {"SHADER_TARGET", "50"}, {"UNITY_COMPILER_HLSL", "1"},
                          {"UNITY_VERSION", "202235"}};
    if (opt.profile) bo.compile.defines.push_back({"PROFILE", "1"});
    if (opt.noDoubles) bo.compile.defines.push_back({"NO_DOUBLES", "1"});
    for (auto& d : opt.defines) {  // NAME or NAME=VALUE
        size_t eq = d.find('=');
        bo.compile.defines.push_back({d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1)});
    }

    // The WARP stamp (gpu_vm's tick.hlsl.warp_ok): the card runs a processor folder only when its warp_ok holds the
    // hash of its sources (and of the devices' folder), written by tools/test_card.py --warp --mark-ok after WARP
    // passed its cases: a tick the card has never run is checked in software first. --stamp prints the hash.
    if (opt.gvm.empty() && opt.compute.empty() && !opt.cpu && (opt.printStamp || !opt.gpu.warp)) {
        std::vector<std::pair<std::string, fs::path>> files;
        for (const fs::path& dir : {fs::u8path(opt.rvcDir), deviceDir}) {
            for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::string ext = it->path().extension().u8string();
                if (it->is_regular_file(ec) && (ext == ".shader" || ext == ".cginc" || ext == ".h" || ext == ".hlsl"))
                    files.push_back({fs::relative(it->path(), dir, ec).generic_u8string(), it->path()});
            }
            if (fs::equivalent(fs::u8path(opt.rvcDir), deviceDir, ec)) break;
        }
        std::sort(files.begin(), files.end());
        uint64_t h = 0xcbf29ce484222325ull;
        for (auto& f : files) {
            std::string text;
            readFileBinary(f.second.u8string(), text);
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());   // (a checkout's line ends are not the tick)
            h = fnv1a64(f.first.data(), f.first.size(), h);
            h = fnv1a64(text.data(), text.size(), h);
        }
        char stamp[32];
        snprintf(stamp, sizeof stamp, "%016llx", (unsigned long long)h);
        if (opt.printStamp) { printf("stamp %s (%zu files of %s)\n", stamp, files.size(), opt.rvcDir.c_str()); return 0; }
        std::string ok;
        readFileBinary((fs::u8path(opt.rvcDir) / "warp_ok").u8string(), ok);
        if (ok.find(stamp) == std::string::npos) {
            fprintf(stderr, "[harness] %s (stamp %s) has not passed WARP's checks as it is: not run on the card "
                            "(python tools/test_card.py machine --warp --mark-ok --rvc %s)\n", opt.rvcDir.c_str(), stamp, opt.rvcDir.c_str());
            return 3;
        }
    }

    // --- Material and backend ---
    Material mat;
    mat.applyDefaults(shader);
    if (!opt.gvm.empty()) {
        bo.gvmDir = opt.gvm;
        bo.gvmTick = opt.gvmTick;
        bo.gvmRamMB = opt.gvmRamMB;
        bo.gvmBudget = opt.ticksSet ? (unsigned)opt.ticks : 4096u;   // (a pass half RISC-V's: lighter in VRChat, and the games between 2K and 8K; gpu_vm/bench_games/README.md)
        opt.cores = kCores;   // (the workers' counts come with the control words)
        opt.initFrames = 0;
    }
    std::unique_ptr<RvcBackend> backendPtr = !opt.gvm.empty() ? makeBackendGvm() : opt.dxc ? makeBackend12() : makeBackend11();
    RvcBackend& backend = *backendPtr;
    if (!backend.init(bo, shader, mat, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    for (auto& dir : opt.replayDirs) {
        SLShader variant;
        if (!loadShaderLab((fs::u8path(dir) / "main.shader").u8string(), variant, err) || !backend.addTickVariant(variant, err)) {
            fprintf(stderr, "[harness] --replay %s: %s\n", dir.c_str(), err.c_str());
            return 1;
        }
    }
    // RVC_TICK_ALT=DIR;DIR...: other ticks of the same layout beside the machine's own (variant 0), each frame's tick
    // picked by a hash of the frame (RVC_TICK_ALT_SHIFT=N: 2^N frames a pick), every frame timed (with --stats-after)
    // and each timing kept by its tick: ticks compared in one run, on one card's clock (gpu_vm's GVM_TICK_ALT, 5fa6f70)
    std::vector<std::string> altDirs;
    if (const char* alt = getenv("RVC_TICK_ALT"); alt && *alt) {
        if (!opt.replayDirs.empty()) { fprintf(stderr, "[harness] RVC_TICK_ALT or --replay, not both\n"); return 2; }
        for (std::string list = alt; !list.empty();) {
            size_t e = list.find(';');
            if (e) altDirs.push_back(list.substr(0, e));
            list = e == std::string::npos ? "" : list.substr(e + 1);
        }
        for (auto& dir : altDirs) {
            SLShader variant;
            if (!loadShaderLab((fs::u8path(dir) / "main.shader").u8string(), variant, err) || !backend.addTickVariant(variant, err)) {
                fprintf(stderr, "[harness] RVC_TICK_ALT %s: %s\n", dir.c_str(), err.c_str());
                return 1;
            }
        }
    }
    static const int altShift = getenv("RVC_TICK_ALT_SHIFT") ? atoi(getenv("RVC_TICK_ALT_SHIFT")) : 0;
    std::vector<double> altMs(altDirs.size() + 1);
    std::vector<uint64_t> altN(altDirs.size() + 1), altPicked(altDirs.size() + 1);
    uint64_t altSeen = 0;
    if (!backend.loadPayload(mat, opt.payloadDir, opt.ramPrefix, "_Data_RAM", err) ||
        !backend.loadPayload(mat, opt.payloadDir, opt.mtdPrefix, "_Data_MTD", err) ||
        !backend.loadPayload(mat, opt.payloadDir, opt.dtbPrefix, "_Data_DTB", err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    mat.setInt("_Ticks", opt.ticks);
    mat.setInt("_TicksDivisor", 1);
    mat.setInt("_DoTick", 0);
    mat.setInt("_InitRaw", 0);
    mat.setInt("_UdonUARTInChar", 0);
    mat.setInt("_UdonUARTInTag", 0);
    mat.setVector("unity_OrthoParams", 1, 1, 0, 1);

    const UINT W = RvcBackend::kWidth, H = RvcBackend::kHeight, kProfCount = RvcBackend::kProfCount;
    std::vector<uint32_t> profBase(kProfCount, 0);
    double timeBase = 0;
    std::string replaySnap;   // --replay: the snapshot's texels, set again before each run
    uint32_t sentTag = 0, consumedTag = 0, sentChar = 0;
    uint32_t conWritten = 0;   // the console's bytes taken so far (docs/machine/console.md)
    bool conSeen = false;
    if (!opt.loadState.empty()) {
        std::string snap;
        SnapshotHeader hdr{};
        bool ok = readFileBinary(opt.loadState, snap) && snap.size() > sizeof hdr;
        if (ok) memcpy(&hdr, snap.data(), sizeof hdr);
        // (one from before RAM reached 0x92000000 has 4096 rows: the rest is zeros; gpu_vm's is the backend's blob, 0 x 0)
        bool gvmSnap = ok && !opt.gvm.empty() && memcmp(hdr.magic, kSnapshotMagic, 8) == 0 && hdr.width == 0;
        if (!gvmSnap && (!ok || !opt.gvm.empty() || memcmp(hdr.magic, kSnapshotMagic, 8) != 0 || hdr.width != W || hdr.height > H ||
                         snap.size() != sizeof hdr + (size_t)W * hdr.height * 16)) {
            fprintf(stderr, "[harness] %s is not a %ux%u snapshot\n", opt.loadState.c_str(), W, H);
            return 1;
        }
        if (!gvmSnap) snap.resize(sizeof hdr + (size_t)W * H * 16, '\0');
        if (!opt.replayDirs.empty()) replaySnap.assign(snap.data() + sizeof hdr, snap.size() - sizeof hdr);
        if (gvmSnap ? !backend.loadSnapshot((const uint8_t*)snap.data() + sizeof hdr, snap.size() - sizeof hdr, err)
                    : !backend.setState(snap.data() + sizeof hdr, err)) {
            fprintf(stderr, "[harness] %s\n", err.c_str());
            return 1;
        }
        timeBase = hdr.time;
        sentTag = consumedTag = hdr.sentTag;
        sentChar = hdr.sentChar;
        mat.setInt("_UdonUARTInChar", (int)sentChar);
        mat.setInt("_UdonUARTInTag", (int)sentTag);
        opt.initFrames = 0;
        fprintf(stderr, "[harness] resumed from %s (guest time %.1fs)\n", opt.loadState.c_str(), timeBase);
    }
    FILE* uartLog = nullptr;
    if (!opt.uartLog.empty()) uartLog = _wfopen(widen(opt.uartLog).c_str(), L"ab");
    if (uartLog) {
        // Marks where this run starts for anyone following the log live.
        std::string what = opt.loadState.empty() ? "cold boot of " + opt.payloadDir + "/" + opt.ramPrefix : "resumed from " + opt.loadState;
        fprintf(uartLog, "\r\n\x1b[7m=== rvc_harness (%s): %s ===\x1b[0m\r\n", opt.dxc ? "D3D12/DXC" : "D3D11/FXC", what.c_str());
        fflush(uartLog);
    }

    if (opt.readStdin) std::thread(stdinThread).detach();

    // --- State shared by the loop ---
    std::deque<char> scriptQueue(opt.initialInput.begin(), opt.initialInput.end());
    // A resumed guest is sitting at its prompt; a newline makes it print one.
    if (g_rawConsole && !opt.loadState.empty() && scriptQueue.empty()) scriptQueue.push_back('\r');

    int uartBurst = opt.uartBurst > 0 ? opt.uartBurst : (int)mat.getFloat("_UartBurst", 1);
    if (uartBurst < 1) uartBurst = 1;
    if (uartBurst > 4) uartBurst = 4;

    if (opt.viz && !backend.viewInit(err)) fprintf(stderr, "[harness] memory view: %s\n", err.c_str());
    double lastViz = -1;
    const bool viewShown = backend.viewOpen();

    // Counters for the memory view's text box, refreshed four times a second.
    double ovLast = 0;
    std::vector<std::string> overlayLines;
    uint64_t ovInstr = 0, ovFrames = 0, ovWorkers = 0;
    double gpuTickMs = -1, gpuCommitMs = -1, gpuDeviceMs = 0;
    // --unity: the rounds of the next frame, from the last timed frame's card time; and what the run had
    double unityRounds = 4, unityMsSum = 0, unityRoundCost = 0, unitySmoothMs = 0;
    int unityCap = 4, unityGuard = 0;
    uint64_t unitySeen = 0, unityTimed = 0, unityRoundSum = 0, unityFrames = 0;
    unsigned lastKey = 0;   // last character handed to the guest
    std::deque<uint32_t> keyEvents;   // raw key events from the window, waiting for the input device
    uint32_t keySeq = 0;              // events delivered so far
    MemoryViewInput pointer;
    // --record-input / --replay-input (gpu_vm's a338987): a line an event, "SECONDS P x y panelW panelH buttons" (the
    // pointer as it changes), "SECONDS K code" (a key event, hex), "SECONDS T bytes" (typed, hex)
    struct InputEvent { double t; char kind; float p[5]; uint32_t code; std::string typed; };
    std::vector<InputEvent> replay;
    size_t inputNext = 0;
    MemoryViewInput replayPointer, recorded;
    FILE* inputLog = opt.recordInput.empty() ? nullptr : fopen(opt.recordInput.c_str(), "w");
    if (!opt.recordInput.empty() && !inputLog) { fprintf(stderr, "[harness] cannot write %s\n", opt.recordInput.c_str()); return 1; }
    if (!opt.replayInput.empty()) {
        FILE* f = fopen(opt.replayInput.c_str(), "r");
        if (!f) { fprintf(stderr, "[harness] cannot read %s\n", opt.replayInput.c_str()); return 1; }
        char line[4096], hex[4000];
        for (InputEvent e{}; fgets(line, sizeof line, f); e = {}) {
            if (sscanf(line, "%lf P %f %f %f %f %f", &e.t, &e.p[0], &e.p[1], &e.p[2], &e.p[3], &e.p[4]) == 6) e.kind = 'P';
            else if (sscanf(line, "%lf K %x", &e.t, &e.code) == 2) e.kind = 'K';
            else if (sscanf(line, "%lf T %3999s", &e.t, hex) == 2) {
                e.kind = 'T';
                for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) e.typed += (char)strtoul(std::string(hex + i, 2).c_str(), nullptr, 16);
            } else continue;
            replay.push_back(e);
        }
        fclose(f);
        fprintf(stderr, "[harness] replaying %zu input events from %s\n", replay.size(), opt.replayInput.c_str());
    }
    auto withCommas = [](uint64_t v) {
        std::string s = std::to_string(v);
        for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
        return s;
    };
    size_t expectIdx = 0, expectScanFrom = 0;
    std::string transcript;
    bool untilHit = false, statsFromHit = false, screenOn = false;
    int screensKept = 0;
    double screenAt = -1e9;
    FILE* screenFile = nullptr;
    bool haveClock = false;
    uint32_t lastClock = 0, commits = 0;
    uint64_t guestInstructions = 0;
    // with worker cores: what they ran, from the counts the control pass publishes
    uint64_t workerInstructions = 0, workerInstr0 = 0;
    uint32_t workerClock[kCoresMost] = {};
    bool haveWorkerClock = false;

    auto t0 = std::chrono::steady_clock::now();
    double lastStats = 0;
    uint64_t statsInstr = 0, statsFrames = 0, statsWorkers = 0;
    uint64_t frame = 0;
    double guestTime = timeBase;

    std::vector<uint32_t> pcSamples, raSamples;
    std::vector<uint32_t> frameSamples, workerSamples, workerMost, roundSamples;
    std::vector<float> timedSamples;
    std::vector<uint32_t> l1Samples;
    uint32_t lastInstructions = 0, lastStall = 0;
    bool statsStarted = false;
    double timeSum[3] = {0, 0, 0};
    int timeSamples = 0;
    // RVC_MARK=TEXT: at each TEXT on the console, the card's tick time a frame since the last one (every frame timed
    // with RVC_STATS_EVERY=1; from --stats-after's start): two programs run in turn in one run are timed apart, on the
    // same card's clock (gpu_vm's GVM_MARK)
    const char* markText = getenv("RVC_MARK");
    double segMs = 0;
    int segN = 0, markN = 0;
    uint64_t statsInstr0 = 0, statsFrame0 = 0;
    auto statsT0 = t0;
    bool keyboardOwned = false;   // a guest program reads the keyboard device (docs/machine/input.md)
    uint32_t displayMode = 0;     // the guest's display (docs/machine/display.md): 0 while it shows nothing
    // keys typed into the window go to the guest's console too while it shows no display
    // and no program there has the keyboard device open; Ctrl+] quits
    auto typedToGuest = [&](const std::string& typed) {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        for (char c : typed) {
            if (c == kQuitKey) g_quit = true;
            else if (!keyboardOwned && displayMode == 0) g_stdinQueue.push_back(c);
        }
    };
    // --cpu: when the interpreter started, its passes, the bands of Linux's RAM still to show,
    // the bands sent with the last frame, and the copy the commit of this frame makes
    double cpuT0 = 0, cpuLowAt = 0;
    uint64_t cpuPasses = 0;
    uint64_t cpuLow = 0, cpuUploaded = 0;   // rvc_cpu's bands (dirty_bands)
    uint32_t cpuCopy[4] = {}, cpuDisplay[2] = {};
    bool cpuFirst = true;
    // A page the guest asked for (docs/system/fetch.md): fetched on a thread, handed over in one frame.
    // The whole answer is kept: one larger than the 256 KB handed over at a time is asked for
    // again part by part, with an offset.
    struct Fetch {
        std::mutex lock;
        bool busy = false, ready = false;
        uint32_t seq = 0, status = 0, info = 0, offset = 0;
        std::string body, url;
    } fetch;
    uint32_t fetchAnswered = 0;
    bool fetchDelivering = false;
    // "Open this" for the guest (--open): one at a time, each a count up of the guest's word.
    size_t openNext = 0;
    uint32_t openCount = 0;
    double openDue = 0;       // the guest's bar looks once a second and takes one at a time
    uint32_t openWord = 0;    // the guest's count as the control row has it (a snapshot's may not be 0)
    // The network device (docs/system/lan.md): the guest's packets come back with the row, the others'
    // go into its ring in the control pass.
    NetLink net;
    if (opt.netPort) {
        std::string why;
        if (opt.netId < 1 || opt.netId > (int)NetLink::kBroadcastTo) why = "--net-id is 1 to 16";
        else if (opt.dxc || !bo.gpuShader) why = "it needs the GPU device on D3D11";
        else net.open(opt.netPort, (unsigned)opt.netId, opt.netLoss, opt.netDelay, why);
        if (!net.on()) { fprintf(stderr, "[harness] no network: %s\n", why.c_str()); return 1; }
        fprintf(stderr, "[harness] network: this machine is 10.0.0.%d, UDP port %d\n", opt.netId, opt.netPort + opt.netId);
    }
    bool netDelivered = false;
    int gpuPassLife[8] = {};      // frames each of the GPU's passes 1-7 is still drawn for
    // The sound card (docs/machine/sound.md). While the guest has it enabled the host mixes: it names a
    // cursor, the device draws the samples from there on and moves its voices up to it.
    SoundOut audio;
    bool audioOpen = false;
    if (bo.soundShader && opt.sound) {
        audioOpen = audio.open(err);
        if (!audioOpen) fprintf(stderr, "[harness] no sound: %s\n", err.c_str());
        audio.setVolume(opt.volume / 100.0f);
    }
    bool soundSwitch = true;   // off: the host mixes nothing, and the card costs nothing
    SoundCapture soundCapture;
    const uint32_t kSoundLead = 1024;   // samples the cursor is ahead of the audio thread: the readback's delay
    bool soundEnabled = false;
    uint32_t soundCursor = 0, soundBase = 0, soundMixes = 0;
    double soundT0 = 0;
    std::deque<std::pair<uint64_t, uint32_t>> soundFrames;   // frames whose mix is on its way back, and their cursors
    bool cpuRunning = false;   // --cpu, past the frames that set the state texture up
    // RVC_LINE_LOG=FILE (a measure, off by default; gpu_vm/PROFILE_Q3.md): each console line with the frame (pass) whose
    // row brought it. RVC_CORES_LOG=FILE: every pass a record of 260 words: the frame, core 0's instructions (gpu_vm: ops),
    // its last stall word, microseconds since the start, then cores 1 to 255's instructions (ops) in the pass (MC_STATS)
    static FILE* lineLog = getenv("RVC_LINE_LOG") ? fopen(getenv("RVC_LINE_LOG"), "w") : nullptr;
    static FILE* coresLog = getenv("RVC_CORES_LOG") ? fopen(getenv("RVC_CORES_LOG"), "wb") : nullptr;
    uint64_t rowNow = 0;
    std::string linePart;
    auto consoleOut = [&](const std::string& out) {
        if (out.empty()) return;
        if (lineLog)
            for (char c : out) {
                if (c == '\n') fprintf(lineLog, "%llu\t%s\n", (unsigned long long)rowNow, linePart.c_str()), fflush(lineLog), linePart.clear();
                else if (c != '\r') linePart += c;
            }
        fwrite(out.data(), 1, out.size(), stdout);
        fflush(stdout);
        if (uartLog) { fwrite(out.data(), 1, out.size(), uartLog); fflush(uartLog); }
        size_t before = transcript.size();
        transcript += out;
        if (!opt.until.empty() && !untilHit) {
            size_t from = before >= opt.until.size() ? before - opt.until.size() + 1 : 0;
            if (transcript.find(opt.until, from) != std::string::npos) untilHit = true;
        }
        if (!opt.statsFrom.empty() && !statsFromHit) {
            size_t from = before >= opt.statsFrom.size() ? before - opt.statsFrom.size() + 1 : 0;
            if (transcript.find(opt.statsFrom, from) != std::string::npos) statsFromHit = true;
        }
        if (markText && *markText) {
            size_t n = strlen(markText), from = before >= n ? before - n + 1 : 0, p;
            while ((p = transcript.find(markText, from)) != std::string::npos) {
                fprintf(stderr, "[harness] mark %d: tick %.4f ms a frame over %d frames\n", markN++, segN ? segMs / segN : 0.0, segN);
                segMs = 0, segN = 0, from = p + n;
            }
        }
        while (expectIdx < opt.expectSend.size()) {
            size_t p = transcript.find(opt.expectSend[expectIdx].first, expectScanFrom);
            if (p == std::string::npos) break;
            expectScanFrom = p + opt.expectSend[expectIdx].first.size();
            for (char c : opt.expectSend[expectIdx].second) scriptQueue.push_back(c);
            ++expectIdx;
        }
    };
    auto processRow = [&](const std::vector<uint8_t>& raw, uint64_t rowFrame) {
        const uint32_t* t = (const uint32_t*)raw.data();
        auto texel = [&](int x, int c) { return t[x * 4 + c]; };
        uint32_t clock = texel(28, 1);
        rowNow = rowFrame;
        {   // RVC_FRAMES_LOG=FILE: the frame (host frame) whose row first shows each new count of lists drawn (GPU_CTRL's .w)
            static FILE* framesLog = getenv("RVC_FRAMES_LOG") ? fopen(getenv("RVC_FRAMES_LOG"), "w") : nullptr;
            static uint32_t framesSeen = 0xffffffffu;
            if (framesLog && raw.size() >= (64 + 2) * 16 && texel(65, 3) != framesSeen)
                framesSeen = texel(65, 3), fprintf(framesLog, "%llu\t%u\n", (unsigned long long)rowFrame, framesSeen), fflush(framesLog);
        }
        keyboardOwned = raw.size() >= (64 + 4) * 16 && texel(64 + 3, 0) == 0x6b657973u;
        displayMode = raw.size() >= (64 + 1) * 16 ? texel(64, 0) : 0;
        bool soundOn = bo.soundShader && raw.size() >= (64 + 0x24) * 16 && texel(64 + 0x22, 0) != 0;
        if (soundOn && !soundEnabled) {
            // carry on from the device's own clock (a snapshot's is not zero), or from the last
            // cursor it was given if that is later: it may not have acted on it
            uint32_t clock = texel(64 + 0x23, 0);
            soundBase = soundCursor = soundMixes && (int32_t)(soundCursor - clock) > 0 ? soundCursor : clock;
            soundT0 = guestTime;
            if (audioOpen) audio.restart(soundBase);
        }
        if (!soundOn && soundEnabled && audioOpen) audio.silence();
        soundEnabled = soundOn;
        // the geometry's texel: its first word's low bit while the workers are laid out anew, its second how many there are
        if (raw.size() >= (64 + 0x3e) * 16) backend.workersNow = (texel(64 + 0x3d, 0) & 1) != 0 || texel(64 + 0x3d, 1) != 0;
        if (raw.size() >= (64 + 0x33) * 16 && texel(64 + 0x32, 0) != 0) backend.countsAdded = true;
        // passes a submitted list asks for: drawn from now on, and for a while after the last use
        if (raw.size() >= (64 + 2) * 16 && texel(64 + 1, 0) != 0) backend.listDue = 4;   // (RVC_LIST_LAZY)
        if (raw.size() >= (64 + 2) * 16)
            for (int p = 1; p < 8; ++p)
                if ((texel(64 + 1, 0) >> (8 + p)) & 1) gpuPassLife[p] = 600;
        if (net.on() && raw.size() >= (64 + kControlTexels + kNetSlot) * 16)
            net.fromGuest(texel(64 + 0x3e, 0), texel(64 + 0x3e, 1), t + (64 + kControlTexels) * 4);
        if (raw.size() >= (64 + 0x25) * 16) openWord = texel(64 + 0x24, 0);
        if (raw.size() >= (64 + 34) * 16 && texel(64 + 16, 0) != texel(64 + 17, 0) && texel(64 + 16, 0) != fetchAnswered) {
            std::lock_guard<std::mutex> lock(fetch.lock);
            if (!fetch.busy && !fetch.ready) {
                uint32_t length = (std::min)(texel(64 + 16, 1), 255u);
                std::string url((const char*)raw.data() + (64 + 18) * 16, length);
                uint32_t kind = texel(64 + 16, 2), offset = texel(64 + 16, 3);
                fetch.seq = texel(64 + 16, 0);
                fetch.offset = offset;
                bool kept = PersistHost::is(url);
                bool held = !kept && offset != 0 && url == fetch.url;   // a further part of what is held
                if (kept) {
                    // the guest's kept folder (docs/system/persist.md): answered here, from the file; what
                    // it hands over lies in the fetch's own 256 KB (four rows are the 100,000 bytes)
                    PersistHost keeper;
                    std::vector<uint8_t> data(PersistHost::kLimit + 32768, 0), rows, reply;
                    keeper.file = opt.persist;
                    bool read = true;
                    if (opt.cpu) memcpy(data.data(), &cpuemu::ram[0x876c0000u - cpuemu::RAM_BASE], PersistHost::kLimit);
                    else if (url == "persist:save" && (read = backend.readRows(64 + 0x76c000 / 2048, 4, rows)))
                        memcpy(data.data(), rows.data(), (std::min)(rows.size(), data.size()));
                    fetch.status = read ? keeper.answer(url, offset, data.data(), reply) : 500;
                    fetch.offset = 0;
                    fetch.body.assign(reply.begin(), reply.end());
                    fetch.url = url;
                    fetch.info = 0;
                    fetch.ready = true;
                    fprintf(stderr, "[harness] the guest's kept folder: %s, status %u\n", url.c_str(), fetch.status);
                    held = true;   // (no thread)
                } else
                (held ? fetch.ready : fetch.busy) = true;
                bool picture = kind == 1, file = kind == 3;
                if (!held) fprintf(stderr, "[harness] the guest asks for %s\n", url.c_str());
                // (a file of the host's own only if this run was told to have the guest open it)
                bool own = std::find(opt.open.begin(), opt.open.end(), url) != opt.open.end();
                if (!held) std::thread([&fetch, url, picture, file, own] {
                    const size_t most = 16u << 20;
                    std::string body;
                    uint32_t status = 0, info = 0;
                    if (url.rfind("http", 0) != 0 && !own) status = 3;   // never: docs/system/fetch.md
                    else if (url.rfind("http", 0) != 0) {
                        // a file of the host's own (--open): its path, or file:// and its path
                        std::string path = url.rfind("file://", 0) == 0 ? url.substr(7) : url;
                        if (FILE* f = fopen(path.c_str(), "rb")) {
                            char buf[65536];
                            size_t got;
                            while (body.size() < most && (got = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, got);
                            fclose(f);
                            status = 200;
                        } else status = 404;
                    } else {
                        HINTERNET net = InternetOpenA("ShaderEmu", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
                        HINTERNET page = net ? InternetOpenUrlA(net, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0) : nullptr;
                        if (page) {
                            DWORD size = sizeof(status), got = 0;
                            char buf[8192];
                            HttpQueryInfoA(page, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr);
                            while (body.size() < most && InternetReadFile(page, buf, sizeof(buf), &got) && got)
                                body.append(buf, got);
                            InternetCloseHandle(page);
                        }
                        if (net) InternetCloseHandle(net);
                    }
                    if ((picture || (file && body.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)) && status == 200) {
                        // A picture: the guest gets pixels, a word each, scaled to fit the 256 KB
                        // it can be handed. Or, asked for as a file, a PNG may carry one in its
                        // pixels (docs/system/fetch.md): then the guest gets that file, header first.
                        std::string pixels;
                        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        ComPtr<IWICImagingFactory> wic;
                        ComPtr<IWICStream> stream;
                        ComPtr<IWICBitmapDecoder> decoder;
                        ComPtr<IWICBitmapFrameDecode> frame;
                        ComPtr<IWICBitmapScaler> scaler;
                        ComPtr<IWICFormatConverter> converter;
                        UINT w = 0, h = 0;
                        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                            SUCCEEDED(wic->CreateStream(&stream)) &&
                            SUCCEEDED(stream->InitializeFromMemory((BYTE*)body.data(), (DWORD)body.size())) &&
                            SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
                            if (file) {
                                // its pixels as they are, three bytes each: a file if they begin with the mark
                                std::string packed((size_t)w * h * 3, 0);
                                if (SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                                    SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone, nullptr, 0,
                                                                    WICBitmapPaletteTypeCustom)) &&
                                    SUCCEEDED(converter->CopyPixels(nullptr, w * 3, (UINT)packed.size(), (BYTE*)packed.data())) &&
                                    packed.size() >= 256 && packed.compare(0, 12, "SHADEREMU\x1a" "F1") == 0) {
                                    uint32_t held = 0;
                                    memcpy(&held, packed.data() + 12, 4);
                                    if (256 + (size_t)held <= packed.size()) {
                                        packed.resize(256 + (size_t)held);
                                        body.swap(packed);
                                        status = 201;   // a file with its header
                                    }
                                }
                            } else {
                                UINT tw = w, th = h;
                                while (tw > 256 || th > 256 || tw * th > 65536) {
                                    tw = (std::max)(1u, tw * 7 / 8);
                                    th = (std::max)(1u, h * tw / w);
                                }
                                if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                                    SUCCEEDED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeFant)) &&
                                    SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                                    SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                                                    WICBitmapPaletteTypeCustom))) {
                                    pixels.resize((size_t)tw * th * 4);
                                    if (SUCCEEDED(converter->CopyPixels(nullptr, tw * 4, (UINT)pixels.size(), (BYTE*)pixels.data()))) {
                                        for (size_t i = 0; i < pixels.size(); i += 4) {
                                            // over white, as the page is; the word is 0x00RRGGBB
                                            unsigned a = (unsigned char)pixels[i + 3];
                                            for (int c = 0; c < 3; ++c)
                                                pixels[i + c] = (char)(((unsigned char)pixels[i + c] * a + 255 * (255 - a)) / 255);
                                            pixels[i + 3] = 0;
                                        }
                                        info = tw | th << 16;
                                    }
                                }
                            }
                        }
                        if (picture) {
                            if (info) body.swap(pixels);
                            else status = 415;   // not a picture this host can read
                        }
                    }
                    std::lock_guard<std::mutex> lock(fetch.lock);
                    fetch.body.swap(body);
                    fetch.url = url;
                    fetch.status = status;
                    fetch.info = info;
                    fetch.busy = false;
                    fetch.ready = true;
                }).detach();
            }
        }
        if (cpuRunning) return;   // (the rest is the shader's processor, which is not running)
        commits = texel(28, 2);
        // what the guest has taken of the input: the UART's tag, or the console's word (docs/machine/console.md)
        const unsigned con = 64 + kControlTexels + kConHost + 1;   // the guest's words, then the ring
        bool hasCon = raw.size() >= (con + 1 + kConRing / 16) * 16;
        consumedTag = texel(9, 3) + (hasCon ? texel(con, 0) : 0);        if (rowFrame < (uint64_t)opt.initFrames) return;  // cpu_init leaves junk in the UART buffer
        if (haveClock) guestInstructions += (uint32_t)(clock - lastClock);
        if (opt.cores > 1 && raw.size() >= (64 + kControlTexels + kNetRead) * 16) {
            // this pass: the workers' instructions, how many ran, how many a whole pass, how many only looked for a job
            uint32_t ranNow = 0, coresNow = 0, fullNow = 0, waitNow = 0, mostNow = 0;
            uint32_t perCore[260] = {};
            for (int c = 1; c < opt.cores; ++c) {   // (MC_STATS, and MC_STATS_ALL from core 16)
                uint32_t now = c < 16 ? texel(64 + 0x38 + c / 4, c % 4) : texel(64 + kControlTexels + kCoreStatsAt + c / 4, c % 4);
                // (a geometry laid out anew starts the counts again: one that went down is all new)
                uint32_t ran = !haveWorkerClock ? 0 : now < workerClock[c] ? now : (uint32_t)(now - workerClock[c]);
                if (c < 256) perCore[4 + c - 1] = ran;
                workerInstructions += ran;
                if (ran != 0 && ran < 64) { waitNow++; ran = 0; }   // (a worker awake with no job runs some 18 a pass)
                ranNow += ran;
                mostNow = (std::max)(mostNow, ran);
                coresNow += ran != 0;
                fullNow += ran >= (uint32_t)opt.ticks;
                workerClock[c] = now;
            }
            haveWorkerClock = true;
            if (coresLog && haveClock) {
                perCore[0] = (uint32_t)rowFrame, perCore[1] = (uint32_t)(clock - lastClock), perCore[2] = texel(40, 2);
                perCore[3] = (uint32_t)std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                fwrite(perCore, 4, 260, coresLog);
                static uint32_t n = 0;
                if (++n % 512 == 0) fflush(coresLog);
            }
            if (!opt.frameLog.empty() && (opt.statsFrom.empty() || statsStarted)) workerSamples.insert(workerSamples.end(), {ranNow, coresNow, fullNow, waitNow}), workerMost.push_back(mostNow);
        }
        // the lanes' field (docs/machine/lanes.md): whether a job is posted, and what its lanes ran, by its
        // groups' words (each the sixteenths of its lanes' instructions for the job of the number it says)
        if (bo.laneField && raw.size() >= (64 + kControlTexels + kNetRead + kFieldRead) * 16) {
            static uint32_t groupSeq[64], groupSum[64];
            const unsigned f0 = 64 + kControlTexels + kNetRead;
            backend.fieldNow = texel(f0, 0) != 0;
            for (unsigned g = 0; g < 62; ++g) {
                uint32_t seq = texel(f0 + 0x10 + g, 0), sum = texel(f0 + 0x10 + g, 2);
                if (seq == groupSeq[g] && sum >= groupSum[g]) workerInstructions += 16ull * (sum - groupSum[g]);
                else if (seq != groupSeq[g]) workerInstructions += 16ull * sum;
                groupSeq[g] = seq, groupSum[g] = sum;
            }
        }
        if (haveClock && !opt.pcLog.empty() && (opt.statsAfter < 0 || statsStarted)) {
            pcSamples.push_back(texel(36, 3));
            pcSamples.push_back((uint32_t)(clock - lastClock));
        }
        if (haveClock && !opt.raLog.empty() && (opt.statsAfter < 0 || statsStarted)) {
            raSamples.push_back(texel(36, 3));
            raSamples.push_back(texel(29, 0));   // x1
            raSamples.push_back((uint32_t)(clock - lastClock));
        }
        if (haveClock && !opt.frameLog.empty() && (opt.statsFrom.empty() || statsStarted)) {
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            frameSamples.insert(frameSamples.end(), {texel(36, 3), (uint32_t)(clock - lastClock), texel(40, 2), (uint32_t)us});
            roundSamples.push_back(backend.tickRounds);   // (the rounds the frame was given: --unity fits them to the card's time)
        }
        lastInstructions = haveClock ? (uint32_t)(clock - lastClock) : 0;
        lastStall = texel(40, 2);
        lastClock = clock;
        haveClock = true;

        if (hasCon) {
            // the console's ring, written since the last row
            uint32_t written = texel(con, 1);
            if (!conSeen) conWritten = written, conSeen = true;
            if (written - conWritten > kConRing) conWritten = written - kConRing;
            std::string out;
            for (; conWritten != written; ++conWritten) {
                uint32_t at = conWritten & (kConRing - 1);
                char c = (char)((texel(con + 1 + at / 16, (at / 4) % 4) >> (8 * (at % 4))) & 0xFF);
                if (c) out += c;
            }
            if (!out.empty()) consoleOut(out);
        }
        uint32_t ptr = texel(11, 3);
        if (ptr == 0xFFFFFFFFu) return;
        uint32_t n = (std::min)(ptr + 1, 64u);
        std::string out;
        for (uint32_t i = 0; i < n; ++i) {
            char c = (char)(texel(12 + i / 4, i % 4) & 0xFF);
            if (c) out += c;
        }
        consoleOut(out);
    };

    std::vector<uint8_t> row, mix, zone;
    // The oldest frame's row, and its mix when it drew one.
    auto takeRow = [&]() {
        uint64_t f;
        if (!backend.popRow(row, f)) return false;
        processRow(row, f);
        if (backend.takeState(zone)) {
            // the write cache is state texels from 44 on (1068 in upstream's layout): buckets of a texel of tags (RAM texel
            // number + 1, 0 when free) and then that many RAM texels. The log gets each cached
            // texel's address: which of its four words were written is not kept anywhere.
            size_t at = l1Samples.size();
            l1Samples.insert(l1Samples.end(), {lastInstructions, lastStall, 0u});
            unsigned ways = 3, tableBits = 6;   // the shader's defaults (L1_WAYS, L1_TABLE_BITS)
            for (auto& d : opt.defines) {
                if (d.rfind("L1_WAYS=", 0) == 0) ways = (unsigned)atoi(d.c_str() + 8);
                if (d.rfind("L1_TABLE_BITS=", 0) == 0) tableBits = (unsigned)atoi(d.c_str() + 14);
            }
            for (size_t b = 0; b < ((7u << tableBits) >> 2); ++b) {
                const uint32_t* w = (const uint32_t*)(zone.data() + ((bo.tickRows == 64 ? 1068 : 44) + b * (ways + 1)) * 16);
                for (unsigned e = 0; e < ways; ++e)
                    if (w[e]) l1Samples.push_back((w[e] - 1) << 4);
            }
            l1Samples[at + 2] = (uint32_t)(l1Samples.size() - at - 3);
        }
        if (backend.takeSound(mix) && !soundFrames.empty() && soundFrames.front().first == f) {
            uint32_t cursor = soundFrames.front().second;
            soundFrames.pop_front();
            if (audioOpen) audio.submit(cursor, (const float*)mix.data());
            if (!opt.soundCapture.empty() && ((const uint32_t*)row.data())[(64 + 0x22) * 4] != 0)   // only what the device mixed
                soundCapture.add(cursor, (const float*)mix.data(), (const uint32_t*)row.data() + 64 * 4, kControlTexels * 4);
        }
        return true;
    };
    int exitCode = 0;
    auto benchT0 = t0;
    uint64_t benchInstr0 = 0;
    // (the card's clock is not the same all through a run: the fastest stretch of 1,024 frames is reported too)
    std::vector<std::pair<double, uint64_t>> benchMarks;
    // --replay: each tick from the same state in turn, inside this loop (every device as in a live run): every
    // --replay-frames frames the time and instructions are noted, the next tick put in place and the state set again
    size_t replayNv = opt.replayDirs.size() + 1, replayAt = 0;
    int replayRound = 0;
    uint64_t replayNext = opt.replayDirs.empty() ? ~0ull : 0, replayInstr0 = 0;
    std::vector<std::vector<double>> replayMs(replayNv), replayIpf(replayNv);
    for (;;) {
        if (frame == replayNext) {
            if (replaySnap.empty()) { fprintf(stderr, "[harness] --replay needs --load-state\n"); exitCode = 1; break; }
            while (backend.rowPending() > 0 && takeRow()) {}
            if (frame > 0) {
                double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                replayMs[replayAt].push_back(1000.0 * sec / opt.replayFrames);
                replayIpf[replayAt].push_back((double)(guestInstructions - replayInstr0) / opt.replayFrames);
                if (++replayAt == replayNv) { replayAt = 0; ++replayRound; }
            }
            if (replayRound == opt.replayRounds) {
                for (size_t v = 0; v < replayNv; ++v) {
                    std::vector<double> s = replayMs[v];
                    std::sort(s.begin(), s.end());
                    double mean = 0, ipf = 0;
                    for (double x : replayMs[v]) mean += x;
                    for (double x : replayIpf[v]) ipf += x;
                    printf("replay %s %.4f ms a frame (median %.4f, best %.4f), %.0f instructions a frame\n",
                           v == 0 ? opt.rvcDir.c_str() : opt.replayDirs[v - 1].c_str(), mean / s.size(), s[s.size() / 2], s[0], ipf / replayIpf[v].size());
                }
                fflush(stdout);
                break;
            }
            backend.useTickVariant(replayAt);
            if (!backend.setState(replaySnap.data(), err)) { fprintf(stderr, "[harness] %s\n", err.c_str()); exitCode = 1; break; }
            haveClock = false;   // (the clock goes back to the snapshot's)
            replayInstr0 = guestInstructions;
            t0 = std::chrono::steady_clock::now();
            replayNext = frame + opt.replayFrames;
        }
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double t = timeBase + (opt.fixedDt > 0 ? (double)frame * opt.fixedDt : wall);
        guestTime = t;
        if (!statsStarted && ((opt.statsAfter >= 0 && wall >= opt.statsAfter) || statsFromHit)) {
            statsStarted = true;
            statsInstr0 = guestInstructions;
            workerInstr0 = workerInstructions;
            statsFrame0 = frame;
            statsT0 = std::chrono::steady_clock::now();
        }
        mat.setVector("_Time", t / 20, t, t * 2, t * 3);
        if (opt.fixedDt <= 0) {
            // the machine's clock chip (DS1742): local time as BCD, century in the control byte.
            // Fixed-timestep runs leave it at zero so that they stay repeatable.
            auto bcd = [](int v) { return (uint32_t)((v / 10) << 4 | (v % 10)); };
            time_t now = time(nullptr);
            struct tm lt{};
            localtime_s(&lt, &now);
            int year = lt.tm_year + 1900;
            mat.setInt("_RTC0", (int64_t)(bcd(year / 100) | bcd(lt.tm_sec) << 8 | bcd(lt.tm_min) << 16 | bcd(lt.tm_hour) << 24));
            mat.setInt("_RTC1", (int64_t)(bcd(lt.tm_wday + 1) | bcd(lt.tm_mday) << 8 | bcd(lt.tm_mon + 1) << 16 | bcd(year % 100) << 24));
        }
        uint32_t mtimeLo, mtimeHi;
        rvcMtime(t, mtimeLo, mtimeHi);
        mat.setInt("_HostMtimeLo", mtimeLo);
        mat.setInt("_HostMtimeHi", mtimeHi);
        mat.setVector("_SinTime", sin(t / 8), sin(t / 4), sin(t / 2), sin(t));
        mat.setVector("_CosTime", cos(t / 8), cos(t / 4), cos(t / 2), cos(t));
        mat.setInt("_Init", frame < (uint64_t)opt.initFrames ? 1 : 0);
        mat.setInt("_CommitAlways", opt.cpu ? 0xff000000u : 1u << 28);   // the bands of the GPU's words, or of all it has

        // --cpu: the processor runs passes until the devices have something to do (a list to
        // draw, a picture to copy back) or a sixtieth of a second is over, and what it wrote of
        // RAM goes into the state texture, where the shader's devices and the view read it.
        if (opt.cpu && frame >= (uint64_t)opt.initFrames) {
            using namespace cpuemu;
            if (!cpuRunning) {
                bare_workers = bare_workers || opt.kernel == "nommu";   // (a worker of root 0 runs untranslated)
                zb_on = opt.isa == "zb";
                if (!machine_boot(linuxDir(opt), opt.cores)) {
                    fprintf(stderr, "[harness] --cpu: no image in %s\n", linuxDir(opt).c_str());
                    exitCode = 1;
                    break;
                }
                external_devices = true;
                // (the host's flags at once: the guest's init asks for them a tenth of a second in,
                // which can be before the control pass's first row has come back)
                word(CTRL + 0x3c) = (opt.desktop ? 1u : 0u) | (opt.tabs ? 2u : 0u) | (uint32_t)(kGpuTarget / 16) << 8 | (uint32_t)(kGpuTarget / 16) << 16;
                console_hook = cpuConsoleHook;
                cpuRunning = true;
                backend.cpuMode = true;
                cpuT0 = wall;
            }
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                // a few characters a frame, as a keyboard would: a shell that sets its terminal
                // up throws away what was typed before it had
                int typed = 0;
                for (std::deque<char>* q : {&scriptQueue, &g_stdinQueue})
                    while (!q->empty() && typed < 2 && console_in.size() < 8) {
                        unsigned char c = (unsigned char)q->front();
                        q->pop_front();
                        if (c) { console_in.push_back(c); lastKey = c; typed++; }
                    }
            }
            {
                // the clock chip, as the shader machine is given it
                auto bcd = [](int v) { return (uint32_t)((v / 10) << 4 | (v % 10)); };
                time_t clock = time(nullptr);
                struct tm lt{};
                localtime_s(&lt, &clock);
                int year = lt.tm_year + 1900;
                rtc0 = bcd(year / 100) | bcd(lt.tm_sec) << 8 | bcd(lt.tm_min) << 16 | bcd(lt.tm_hour) << 24;
                rtc1 = bcd(lt.tm_wday + 1) | bcd(lt.tm_mday) << 8 | bcd(lt.tm_mon + 1) << 16 | bcd(year % 100) << 24;
            }
            double due = wall + 1.0 / 60;
            for (;;) {
                double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                guest_seconds = now - cpuT0;
                uint64_t most = machine_pass((uint32_t)opt.ticks);
                cpuPasses++;
                if (word(CTRL + 0x10) || word(CTRL + 0x70) || now >= due) break;
                if (most == 0 && cores[0].waiting) Sleep(1);   // an idle machine
                else if (opt.cpuIps > 0 && (double)(cores[0].clock + worker_instr) / opt.cpuIps > now - cpuT0) Sleep(1);
            }
            guestInstructions = cores[0].clock;
            workerInstructions = worker_instr;
            commits = (uint32_t)cpuPasses;
            consoleOut(g_cpuConsole);
            g_cpuConsole.clear();
            // RAM: the GPU's memory and the control words whenever they were written, Linux's own
            // (which the memory view shows and GPU draws may read) four times a second
            const uint64_t devices = 0xffull << 24;   // 0x86000000 to 0x88000000
            uint64_t upload = dirty_bands & devices;
            cpuLow |= dirty_bands & ~devices;
            dirty_bands = 0;
            if (cpuFirst || wall - cpuLowAt >= 0.25) {
                upload |= cpuLow;
                cpuLow = 0;
                cpuLowAt = wall;
            }
            if (cpuFirst) upload = ~0ull;
            cpuFirst = false;
            for (unsigned b = 0; b < (kStateHeight - 64) / 128; ++b)
                if (upload >> (b & 63) & 1) backend.uploadRows(64 + 128 * b, 128, ram.data() + (size_t)b * 0x400000);
            // which bands the commit is to carry over into the buffer it draws: these, and for
            // the buffer of two frames ago the last ones, as the shader's 32 bits (ram_bands in
            // src/types.h); the GPU's are drawn every commit (_CommitAlways)
            auto fold = [&](uint64_t m) { return (uint32_t)(m & ~devices) | (uint32_t)(m >> 32); };
            uint32_t bands[4] = {1, fold(upload), fold(cpuUploaded), 0};
            backend.uploadTexel(41, 0, bands);
            cpuUploaded = upload;
            memcpy(cpuCopy, &word(CTRL + 0x70), sizeof cpuCopy);
            cpuDisplay[0] = word(CTRL + 4), cpuDisplay[1] = word(CTRL + 8);
        }

        backend.tickRounds = opt.unityMs > 0 ? (uint32_t)unityRounds : (uint32_t)opt.tickRounds; backend.tickSplit = (uint32_t)opt.tickSplit;   // (--tick-rounds: the fragment tick's rounds a frame)
        // --compute: the processor's RAM and hart, once the frames that set the state texture up are
        // over; from then on a frame is a pass of it, and the rest is the harness's as without it
        if (!opt.compute.empty() && frame >= (uint64_t)opt.initFrames && !backend.computeMode &&
            ((backend.computeCores = (uint32_t)opt.cores), (backend.computePasses = (uint32_t)(std::max)(1, opt.computePasses)), (backend.computeSplit = (uint32_t)opt.computeSplit),
             !backend.computeStart(linuxDir(opt), err))) {
            fprintf(stderr, "[harness] --compute: %s\n", err.c_str());
            exitCode = 1;
            break;
        }

        // Feed one input character per handshake.
        if (frame >= (uint64_t)opt.initFrames && sentTag == consumedTag && !cpuRunning) {
            // Up to uartBurst characters per handshake, first one in the low byte. NUL cannot be
            // sent (the guest treats 0 as "no character") and is dropped.
            uint32_t group = 0;
            int count = 0;
            std::lock_guard<std::mutex> lock(g_inputMutex);
            std::deque<char>& q = !scriptQueue.empty() ? scriptQueue : g_stdinQueue;
            while (count < uartBurst && !q.empty()) {
                unsigned char c = (unsigned char)q.front();
                q.pop_front();
                if (c == 0) continue;
                lastKey = c;
                group |= (uint32_t)c << (8 * count++);
            }
            if (count > 0) {
                sentTag += (uint32_t)count;
                sentChar = group;
                mat.setInt("_UdonUARTInChar", (int64_t)group);
                mat.setInt("_UdonUARTInTag", sentTag);
                backend.conDelivered = true;
            }
        }

        // The input device: the pointer every frame, and up to four key events.
        {
            uint32_t batch[4] = {0, 0, 0, 0};
            int n = 0;
            for (; inputNext < replay.size() && replay[inputNext].t <= wall; ++inputNext) {
                const InputEvent& e = replay[inputNext];
                if (e.kind == 'P') replayPointer.x = e.p[0], replayPointer.y = e.p[1], replayPointer.panelW = e.p[2], replayPointer.panelH = e.p[3],
                                   replayPointer.buttons = (unsigned)e.p[4];
                else if (e.kind == 'K') keyEvents.push_back(e.code);
                else typedToGuest(e.typed);
            }
            for (; n < 4 && !keyEvents.empty(); ++n) {
                batch[n] = keyEvents.front();
                keyEvents.pop_front();
            }
            if (opt.pointerSweep >= 0 && (long long)frame >= opt.pointerSweep) {
                double turn = (double)(frame - (uint64_t)opt.pointerSweep) * 0.005;
                pointer.panelW = 800, pointer.panelH = 600;
                pointer.x = 400 + 80 * (float)cos(turn);
                pointer.y = 300 + 60 * (float)sin(turn);
            }
            if (opt.pointerButton) pointer.buttons = t >= opt.pointerButtonFrom && t < opt.pointerButtonTo ? opt.pointerButton : 0;
            if (!replay.empty()) pointer.x = replayPointer.x, pointer.y = replayPointer.y, pointer.panelW = replayPointer.panelW,
                                 pointer.panelH = replayPointer.panelH, pointer.buttons = replayPointer.buttons;
            if (inputLog && (pointer.x != recorded.x || pointer.y != recorded.y || pointer.panelW != recorded.panelW ||
                             pointer.panelH != recorded.panelH || pointer.buttons != recorded.buttons)) {
                fprintf(inputLog, "%.4f P %g %g %g %g %u\n", wall, pointer.x, pointer.y, pointer.panelW, pointer.panelH, pointer.buttons);
                recorded = pointer, recorded.keys.clear();
            }
            mat.setVector("_InputPointer", pointer.x, pointer.y, pointer.panelW, pointer.panelH);
            mat.setInt("_InputButtons", pointer.buttons);
            mat.setInt("_HostMs", (int64_t)(uint32_t)(t * 1000.0));   // a clock programs read without a system call
            {
                // the host's hand in RAM: in one frame's commit, nothing in every other
                unsigned z[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                if (!opt.zap.empty() && (long long)frame == opt.zapFrame)
                    sscanf(opt.zap.c_str(), "%u,%u,%u,%u,%u,%x,%x,%u", &z[0], &z[1], &z[2], &z[3], &z[4], &z[5], &z[6], &z[7]);
                mat.setInt("_ZapKind", z[0]);
                mat.setInt("_ZapX", z[1]);
                mat.setInt("_ZapY", z[2]);
                mat.setInt("_ZapR", z[3]);
                mat.setInt("_ZapWord", z[4]);
                mat.setInt("_ZapMaskLo", z[5] & 0xffff);
                mat.setInt("_ZapMaskHi", z[5] >> 16);
                mat.setInt("_ZapSeed", z[6]);
                mat.setInt("_ZapZones", z[7]);
            }
            // bit 0: start the desktop at boot; then the largest screen this host shows, in 16s of pixels
            mat.setInt("_HostFlags", (opt.desktop ? 1 : 0) | (opt.tabs ? 2 : 0) | opt.workers << 2 | (kGpuTarget / 16) << 8 | (kGpuTarget / 16) << 16);
            // the host's words: the harness, one visitor (number 1); a head that looks straight on from the seat's eye point
            mat.setInt("_HostState0", 1 | (opt.holodeck ? 16 : 0) | (opt.holodeckHands ? 32 : 0) | 1 << 8 | 1 << 16);
            mat.setInt("_HostState1", 0);
            mat.setInt("_HostState7", (int64_t)(((uint32_t)(int32_t)(opt.holodeckAim * 65536.0 / 360.0) & 0xffffu) << 16));
            mat.setInt("_HostState2", 0);
            mat.setInt("_HostState3", 0);
            if (opt.host[0]) {
                static const char* const names[8] = { "_HostState0", "_HostState1", "_HostState2", "_HostState3", "_HostState4", "_HostState5",
                                                      "_HostState6", "_HostState7" };
                for (int i = 0; i < 8; i++) mat.setInt(names[i], (int64_t)opt.host[i]);
            }
            mat.setInt("_InputKeySeq", keySeq);
            mat.setInt("_InputKeyCount", n);
            mat.setInt("_InputKey0", batch[0]);
            mat.setInt("_InputKey1", batch[1]);
            mat.setInt("_InputKey2", batch[2]);
            mat.setInt("_InputKey3", batch[3]);
            keySeq += (uint32_t)n;
        }

        // An answer the fetch thread has ready goes into the guest's memory in this frame.
        if (fetchDelivering) {
            mat.setInt("_FetchDeliver", 0);
            fetchDelivering = false;
        }
        {
            std::lock_guard<std::mutex> lock(fetch.lock);
            if (fetch.ready) {
                // the part from the offset asked for; the length said is the whole answer's
                std::vector<uint8_t> bytes((size_t)kFetchSide * kFetchSide * 4, 0);
                size_t from = (std::min)((size_t)fetch.offset, fetch.body.size());
                size_t n = (std::min)(fetch.body.size() - from, bytes.size());
                memcpy(bytes.data(), fetch.body.data() + from, n);
                std::string err;
                if (backend.deliverHostData(mat, bytes.data(), err)) {
                    // (--cpu: the processor's own memory has only the control words copied back,
                    // so the answer's bytes are put there here, before its words say it has come)
                    if (opt.cpu) {
                        memcpy(&cpuemu::ram[0x876c0000u - cpuemu::RAM_BASE], bytes.data(), bytes.size());
                        cpuemu::dirty(0x876c0000u);
                    }
                    mat.setInt("_FetchDeliver", 1);
                    mat.setInt("_FetchSeq", fetch.seq);
                    mat.setInt("_FetchLength", (int64_t)fetch.body.size());
                    mat.setInt("_FetchStatus", fetch.status);
                    mat.setInt("_FetchInfo", fetch.info);
                    fetchDelivering = true;
                    fprintf(stderr, "[harness] answered with %zu bytes from %zu of %zu, status %u\n", n, from, fetch.body.size(), fetch.status);
                }
                fetchAnswered = fetch.seq;
                fetch.ready = false;
            } else if (!fetchDelivering && openNext < opt.open.size() && frame > (uint64_t)opt.initFrames + 4 && t >= openDue &&
                       (opt.openAfter.empty() || transcript.find(opt.openAfter) != std::string::npos)) {
                // the next thing to open: its address in the answer's texture, written to the guest's words
                std::vector<uint8_t> bytes((size_t)kFetchSide * kFetchSide * 4, 0);
                const std::string& address = opt.open[openNext];
                size_t n = (std::min)(address.size(), (size_t)255);
                memcpy(bytes.data(), address.data(), n);
                std::string err;
                if (backend.deliverHostData(mat, bytes.data(), err)) {
                    mat.setInt("_FetchDeliver", 4);
                    openCount = (std::max)(openCount, openWord) + 1;
                    mat.setInt("_FetchSeq", openCount);
                    mat.setInt("_FetchLength", (int64_t)n);
                    fetchDelivering = true;
                    fprintf(stderr, "[harness] the guest is told to open %s\n", address.c_str());
                }
                ++openNext;
                openDue = t + 10;
            }
        }

        netDelivered = false;
        if (net.on()) {
            uint8_t places[NetLink::kMost * NetLink::kSlotBytes];
            uint32_t before = 0;
            unsigned n = net.toGuest(places, before);
            std::string err;
            if (n && !backend.deliverNet(mat, places, err)) n = 0;   // (those packets are lost)
            netDelivered = n != 0;
            mat.setInt("_NetRxSeq", before);
            mat.setInt("_NetRxCount", n);
            mat.setInt("_NetTxAck", net.txAck());
            mat.setInt("_NetId", net.id());
        }

        if (opt.benchWarmup >= 0 && frame == (uint64_t)opt.benchWarmup) {
            // Start the measurement from an idle GPU with every earlier frame accounted for.
            while (backend.rowPending() > 0 && takeRow()) {}
            benchInstr0 = guestInstructions;
            backend.readProf(profBase);  // warm-up events are subtracted at the end
            benchT0 = std::chrono::steady_clock::now();
        }
        if (opt.benchWarmup >= 0 && frame >= (uint64_t)opt.benchWarmup && ((frame - (uint64_t)opt.benchWarmup) & 255) == 0)
            benchMarks.push_back({std::chrono::duration<double>(std::chrono::steady_clock::now() - benchT0).count(), guestInstructions - benchInstr0});
        if (backend.rowFull() && !takeRow()) { exitCode = 1; break; }
        {
            // Mix when the cursor has moved on: with a device it follows the audio thread, without
            // one the clock (every frame of a fixed-timestep run, so that those stay repeatable).
            bool mixed = false;
            if (soundEnabled && soundSwitch) {
                uint32_t want = audioOpen ? audio.position() + kSoundLead
                                          : soundBase + (uint32_t)(int64_t)(floor(t * kSoundRate) - floor(soundT0 * kSoundRate));
                uint32_t ahead = want - soundCursor;
                if (ahead >= (opt.fixedDt > 0 ? 1u : 240u) && ahead < 0x80000000u) {
                    soundCursor = want;
                    mixed = true;
                    ++soundMixes;
                }
            }
            mat.setInt("_SoundCursor", soundCursor);
            mat.setInt("_SoundMixed", mixed ? 1 : 0);
            mat.setInt("_SoundRate", kSoundRate);
            // drawn and read back only when someone listens; the voices move on either way
            backend.soundMix = mixed && (audioOpen || !opt.soundCapture.empty());
            if (backend.soundMix) soundFrames.push_back({frame, soundCursor});
        }
        // The GPU times the two draws of one frame per refresh of the text box.
        // with --stats-after, one frame in 64 is timed on the GPU and the times are averaged
        static const uint64_t statsMask = (getenv("RVC_STATS_EVERY") && atoi(getenv("RVC_STATS_EVERY"))) || !altDirs.empty() ? 0u : 63u;   // (every frame timed)
        bool sampleTimes = statsStarted && opt.statsFrom.empty() && (frame & statsMask) == 0;
        if (sampleTimes && backend.gpuTimes(gpuTickMs, gpuCommitMs, gpuDeviceMs)) {
            timeSum[0] += gpuTickMs;
            segMs += gpuTickMs, segN++;
            timeSum[1] += gpuCommitMs;
            timeSum[2] += gpuDeviceMs;
            ++timeSamples;
            if (!altDirs.empty() && backend.frameTimes != altSeen && backend.frameVariant < altMs.size()) {   // (a timing just in, of its tick)
                altSeen = backend.frameTimes;
                altMs[backend.frameVariant] += gpuTickMs, altN[backend.frameVariant]++;
            }
        }
        if (!altDirs.empty()) {
            uint64_t x = (frame >> altShift) * 0x9e3779b97f4a7c15ull;
            x ^= x >> 31, x *= 0xbf58476d1ce4e5b9ull, x ^= x >> 29;
            size_t v = (size_t)(x % altMs.size());
            backend.useTickVariant(v);
            if (statsStarted) altPicked[v]++;
        }
        {
            // every pass every frame, as a world draws them (Unity's _GpuPasses 0xff): a list that asked for a pass the
            // host drew only after a readback saw it waited submitted for it, and the machine with it, three frames
            // (gpu_vm's Doom, 27 October 2026). RVC_PASS_LIFE=1: the passes seen lately only
            static const bool passLife = getenv("RVC_PASS_LIFE") && atoi(getenv("RVC_PASS_LIFE")) != 0;
            uint32_t passes = passLife ? 1u : 0xffu;
            for (int p = 1; p < 8; ++p)
                if (gpuPassLife[p] > 0) {
                    --gpuPassLife[p];
                    passes |= 1u << p;
                }
            backend.gpuPasses = passes;
            mat.setInt("_GpuPasses", passes);
        }
        if (opt.unityMs > 0) {   // (--unity: a frame timed whenever the last timing is in; the rounds follow the card's time)
            static const bool worldPace = getenv("RVC_UNITY_PACE") && !strcmp(getenv("RVC_UNITY_PACE"), "world");   // (the world's steady mode)
            double a, b, c;
            backend.gpuTimes(a, b, c);
            if (backend.frameTimes != unitySeen && backend.frameMs > 0 && backend.frameRounds) {
                unitySeen = backend.frameTimes;
                if (!opt.frameLog.empty()) timedSamples.insert(timedSamples.end(), {(float)frame, (float)backend.frameMs, (float)backend.frameRounds});   // (each timing as it came: the frame, its card ms, its rounds)
                // (a bursty game's frames overshoot or undershoot F - U on the card, as the world's steady mode does: compare
                // machines by fps at the card time a frame they took, bench_games/grid.py's "at budget")
                // (with R, a frame costs the larger of its card time and its rounds' CPU: Unity's CPU and the card overlap)
                double cost = (std::max)(backend.frameMs, backend.frameRounds * opt.unityRoundMs);
                double want = backend.frameRounds * (opt.unityMs - opt.unityOverMs) / cost;
                unityRoundCost = cost / backend.frameRounds;
                if (!worldPace) unityRounds = (std::max)(1.0, (std::min)(64.0, unityRounds + (want - unityRounds) * 0.5));
                if (statsStarted) unityMsSum += cost, unityTimed++;
            }
            if (worldPace && unityRoundCost > 0) {   // (EmuMachine.SteadyRounds: a frame's time smoothed a tenth a frame, the
                // rounds' cap one down or an eighth more every sixth frame over F, one up under 0.8 F, as at the fastest speed)
                double dt = opt.unityOverMs + unityRounds * unityRoundCost;
                unitySmoothMs += (dt - unitySmoothMs) * 0.1;
                if (--unityGuard <= 0) {
                    unityGuard = 6;
                    if (unitySmoothMs > opt.unityMs) unityCap = (std::max)(1, unityCap - 1 - unityCap / 8);
                    else if (unitySmoothMs < opt.unityMs * 0.8 && unityCap < 64) unityCap++;
                }
                unityRounds = unityCap;
            }
            if (statsStarted) unityRoundSum += (uint32_t)unityRounds, unityFrames++;
        }
        backend.frame(mat, frame, sampleTimes || opt.unityMs > 0 || (backend.viewOpen() && wall - ovLast >= 0.2));
        ++frame;
        if (cpuRunning) {
            // the devices' answer, before the processor goes on: the control words as the control
            // pass left them, and the picture the commit copied back for the list drawn last
            using namespace cpuemu;
            // (every row still on its way, to the last: an older frame's words would undo what
            // the processor has written since)
            while (backend.rowPending() > 0)
                if (!takeRow()) Sleep(0);
            if (row.size() >= (64 + kControlTexels) * 16) memcpy(&word(CTRL), row.data() + 64 * 16, kControlTexels * 16);
            if (netDelivered) {
                // the ring as the control pass left it
                std::vector<uint8_t> rows;
                size_t ring = (size_t)kNetRing * 16, bytes = (size_t)kNetSlot * kNetSlots * 16;
                if (backend.readRows(kNetRow, 1, rows) && rows.size() >= ring + bytes)
                    memcpy(ram.data() + (size_t)(kNetRow - 64) * 32768 + ring, rows.data() + ring, bytes);
            }
            if (cpuCopy[0]) {
                uint32_t address = CTRL + 0x1000, bytes = cpuDisplay[0] * cpuDisplay[1] * 4;
                if (cpuCopy[0] & 4) {
                    uint32_t width = cpuCopy[2] & 0xffff, height = cpuCopy[2] >> 16;
                    address = (cpuCopy[1] & 0x7ffffffcu) | 0x80000000u;
                    bytes = (std::max)(cpuCopy[3], width) * height * 4;
                }
                if (address >= 0x86000000u && bytes && address + bytes <= 0x87e00000u) {   // (where a copy may go: src/gpu.h)
                    uint32_t offset = address - RAM_BASE, first = offset / 32768, last = (offset + bytes - 1) / 32768;
                    std::vector<uint8_t> rows;
                    if (backend.readRows(64 + first, last - first + 1, rows) && rows.size() >= (size_t)(last - first + 1) * 32768)
                        memcpy(ram.data() + offset, rows.data() + (offset - first * 32768), bytes);
                }
            }
            // --screen-log: the picture as tools/snapshot_screen.py makes it of a snapshot, from the
            // state texture (the GPU draws there, not in the host's copy of RAM)
            if (!opt.screenLog.empty() && !screenOn && (opt.screenFrom.empty() || transcript.find(opt.screenFrom) != std::string::npos)) screenOn = true;
            uint32_t mode = word(CTRL), sw = word(CTRL + 4), sh = word(CTRL + 8);
            if (screenOn && sw > 0 && sh > 0 && sw <= 2048 && sh <= 2048 && (guest_seconds - screenAt) * 1000.0 >= opt.screenEvery) {
                screenAt = guest_seconds;
                auto words = [&](uint32_t address, size_t count, std::vector<uint32_t>& out) {
                    uint32_t offset = (address & 0x7ffffffcu), first = offset / 32768, last = (uint32_t)((offset + count * 4 - 1) / 32768);
                    std::vector<uint8_t> rows;
                    out.assign(count, 0);
                    if (address < RAM_BASE || offset + count * 4 > 2048u * (kStateHeight - 64) * 16) return false;
                    if (!backend.readRows(64 + first, last - first + 1, rows) || rows.size() < (size_t)(last - first + 1) * 32768) return false;
                    memcpy(out.data(), rows.data() + (offset - first * 32768), count * 4);
                    return true;
                };
                std::vector<uint32_t> screen((size_t)sw * sh, 0), part;
                if (mode == 4) {
                    std::vector<uint32_t> table;
                    words(word(CTRL + 0xc), 4 + 8 * 16, table);
                    for (uint32_t k = 0; k < table[0] && k < 16; k++) {
                        const uint32_t* l = &table[4 + 8 * k];
                        int x = (int)l[0], y = (int)l[1], lw = (int)l[2], lh = (int)l[3];
                        if (lw <= 0 || lh <= 0 || lw > 2048 || lh > 2048 || !words(l[4], (size_t)lw * lh, part)) continue;
                        for (int j = (std::max)(0, -y); j < lh && y + j < (int)sh; j++)
                            for (int i = (std::max)(0, -x); i < lw && x + i < (int)sw; i++) screen[(size_t)(y + j) * sw + x + i] = part[(size_t)j * lw + i];
                    }
                } else words(CTRL + 0x1000, (size_t)sw * sh, screen);
                if (word(CTRL + 0x48) & 1) {
                    int x = (int)word(CTRL + 0x40), y = (int)word(CTRL + 0x44);
                    if (words(word(CTRL + 0x4c), 32 * 32, part))
                        for (int j = 0; j < 32; j++)
                            for (int i = 0; i < 32; i++)
                                if (part[j * 32 + i] >> 24 && x + i >= 0 && y + j >= 0 && x + i < (int)sw && y + j < (int)sh) screen[(size_t)(y + j) * sw + x + i] = part[j * 32 + i];
                }
                if (!screenFile) screenFile = fopen(opt.screenLog.c_str(), "wb");
                if (screenFile) {
                    uint32_t head[4] = {sw, sh, (uint32_t)frame, (uint32_t)(guest_seconds * 1000.0)};
                    std::vector<uint8_t> rgb((size_t)sw * sh * 3);
                    for (size_t i = 0; i < screen.size(); i++) {
                        rgb[i * 3] = (uint8_t)(screen[i] >> 16);
                        rgb[i * 3 + 1] = (uint8_t)(screen[i] >> 8);
                        rgb[i * 3 + 2] = (uint8_t)screen[i];
                    }
                    fwrite(head, 4, 4, screenFile);
                    fwrite(rgb.data(), 1, rgb.size(), screenFile);
                    if (++screensKept == opt.screenFrames) untilHit = true;
                }
            }
        }

        std::string removed = backend.deviceRemoved();
        if (!removed.empty()) {
            fprintf(stderr, "\n[harness] GPU device lost: %s. If this is a timeout (TDR), lower --ticks or use --warp.\n",
                    removed.c_str());
            exitCode = 1;
            break;
        }

        wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (wall - lastStats >= 1.0) {
            double dt = wall - lastStats;
            double ips = (double)(guestInstructions - statsInstr) / dt;
            double all = ips + (double)(workerInstructions - statsWorkers) / dt;
            double fps = (double)(frame - statsFrames) / dt;
            char title[256];
            snprintf(title, sizeof title, "rvc_harness | all cores %.2fM IPS | core 0 %.2fM | %.1f frames/s | frame %llu | commits %u",
                     all / 1e6, ips / 1e6, fps, (unsigned long long)frame, commits);
            SetConsoleTitleA(title);
            backend.viewTitle(std::string("memory | ") + (title + 14));
            if (opt.statsInterval > 0 && wall - lastStats >= opt.statsInterval)
                fprintf(stderr, "\n[harness] %s\n", title + 14);
            if (opt.statsInterval <= 0 || wall - lastStats >= opt.statsInterval) {
                lastStats = wall;
                statsInstr = guestInstructions;
                statsWorkers = workerInstructions;
                statsFrames = frame;
            }
        }

        if (backend.viewOpen() && wall - ovLast >= 0.25) {
            backend.gpuTimes(gpuTickMs, gpuCommitMs, gpuDeviceMs);
            double dt = wall - ovLast;
            double ips = (double)(guestInstructions - ovInstr) / dt, fps = (double)(frame - ovFrames) / dt;
            double workerIps = (double)(workerInstructions - ovWorkers) / dt;
            char l1[96], l2[96], l3[96], l4[96], l5[96], l0[96];
            snprintf(l0, sizeof l0, "all cores %s IPS    %.0f frames/s", withCommas((uint64_t)(ips + workerIps)).c_str(), fps);
            snprintf(l1, sizeof l1, "core 0 %s    workers %s", withCommas((uint64_t)ips).c_str(), withCommas((uint64_t)workerIps).c_str());
            snprintf(l2, sizeof l2, "frame %.3f ms    %.0f instr/frame", fps > 0 ? 1000.0 / fps : 0.0, fps > 0 ? ips / fps : 0.0);
            char l6[96];
            if (!bo.soundShader) snprintf(l6, sizeof l6, "gpu device %.3f ms", gpuDeviceMs);
            else if (!soundSwitch) snprintf(l6, sizeof l6, "gpu device %.3f ms    sound off (Ctrl+F9)", gpuDeviceMs);
            else snprintf(l6, sizeof l6, "gpu device %.3f ms    sound %s, volume %d%%", gpuDeviceMs,
                          !audioOpen ? "not played" : soundEnabled ? "playing" : "idle", opt.volume);
            if (gpuTickMs >= 0) snprintf(l3, sizeof l3, "tick %.3f ms    commit %.3f ms", gpuTickMs, gpuCommitMs);
            else snprintf(l3, sizeof l3, "tick -    commit -");
            unsigned up = (unsigned)wall;
            snprintf(l4, sizeof l4, "up %02u:%02u:%02u    guest clock %.1f s", up / 3600, up / 60 % 60, up % 60, guestTime);
            snprintf(l5, sizeof l5, "%s instructions    %s commits", withCommas(guestInstructions + workerInstructions).c_str(), withCommas(commits).c_str());
            overlayLines = {l0, l1, l2, l3, l6, l4, l5};
            // The console column: the last lines the guest printed, without escape sequences.
            std::vector<std::string> console(1);
            size_t from = transcript.size() > 2000 ? transcript.size() - 2000 : 0;
            for (size_t k = from; k < transcript.size(); ++k) {
                unsigned char ch = (unsigned char)transcript[k];
                if (ch == 0x1b) {   // ESC [ ... letter
                    if (k + 1 < transcript.size() && transcript[k + 1] == '[')
                        for (k += 2; k < transcript.size() && !isalpha((unsigned char)transcript[k]); ++k) {}
                } else if (ch == '\n') {
                    console.emplace_back();
                } else if (ch == '\b') {
                    if (!console.back().empty()) console.back().pop_back();
                } else if (ch >= 0x20 && ch < 0x7f) {
                    console.back() += (char)ch;
                }
            }
            if (console.size() > 8) console.erase(console.begin(), console.end() - 8);
            for (auto& l : console)
                if (l.size() > 62) l = l.substr(l.size() - 62);
            char i1[64], i2[64], i3[64], i4[64];
            size_t queued;
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                queued = scriptQueue.size() + g_stdinQueue.size();
            }
            snprintf(i1, sizeof i1, "input: window%s, %s", keyboardOwned || displayMode ? " (keys to the keyboard device only)" : "",
                     g_rawConsole ? "console" : (opt.readStdin ? "stdin" : "script"));
            snprintf(i2, sizeof i2, "queued %zu   sent %u", queued, sentTag);
            snprintf(i3, sizeof i3, "taken by guest %u", consumedTag);
            if (lastKey >= 0x20 && lastKey < 0x7f) snprintf(i4, sizeof i4, "last key '%c' (0x%02x)", lastKey, lastKey);
            else if (lastKey) snprintf(i4, sizeof i4, "last key 0x%02x", lastKey);
            else snprintf(i4, sizeof i4, "last key -");
            backend.viewText({overlayLines, console, {i1, i2, i3, i4}});
            ovLast = wall;
            ovInstr = guestInstructions;
            ovWorkers = workerInstructions;
            ovFrames = frame;
        }
        // The memory view samples the state at about 60 Hz, however fast the emulator runs.
        if (backend.viewOpen() && wall - lastViz >= 1.0 / 60) {
            backend.viewRender();
            lastViz = wall;
            pointer = memoryViewTakeInput();
            if (inputLog)
                for (uint32_t k : pointer.keys) fprintf(inputLog, "%.4f K %x\n", wall, k);
            if (replay.empty()) keyEvents.insert(keyEvents.end(), pointer.keys.begin(), pointer.keys.end());
            pointer.keys.clear();
            for (char c : memoryViewTakeHostKeys()) {
                if (c == 's') soundSwitch = !soundSwitch;
                else opt.volume = (std::max)(0, (std::min)(100, opt.volume + (c == '+' ? 5 : -5)));
                if (audioOpen) audio.setVolume(opt.volume / 100.0f);
                if (audioOpen && !soundSwitch) audio.silence();
            }
            std::string typed = memoryViewTakeKeys();
            if (inputLog && !typed.empty()) {
                fprintf(inputLog, "%.4f T ", wall);
                for (unsigned char c : typed) fprintf(inputLog, "%02x", c);
                fputc(10, inputLog);
            }
            if (!replay.empty()) typed = typed.find(kQuitKey) != std::string::npos ? std::string(1, kQuitKey) : "";
            typedToGuest(typed);
        }
        if (viewShown && !backend.viewOpen()) break;  // closing the memory window quits
        if (g_quit) break;
        if (untilHit) break;
        if (opt.maxFrames && frame >= opt.maxFrames) { if (!opt.until.empty()) exitCode = 3; break; }
        if (opt.maxSeconds > 0 && wall >= opt.maxSeconds) { if (!opt.until.empty()) exitCode = 3; break; }
    }

    if (inputLog) fclose(inputLog);
    // Drain outstanding readbacks so trailing output is not lost.
    while (exitCode != 1 && backend.rowPending() > 0 && takeRow()) {}
    if (audioOpen) audio.close();
    if (untilHit) exitCode = 0;
    if (screenFile) fclose(screenFile);

    if (opt.benchWarmup >= 0 && exitCode != 1 && frame > (uint64_t)opt.benchWarmup) {
        // The drain above blocked on the last frame, so this interval covers all GPU work.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchT0).count();
        uint64_t frames = frame - (uint64_t)opt.benchWarmup, instr = guestInstructions - benchInstr0;
        uint64_t hash = 0;
        std::vector<uint8_t> data;
        if (backend.readState(64, 64, data)) hash = fnv1a64(data.data(), data.size());
        double best = 0, bestMs = 0;
        for (size_t i = 4; i < benchMarks.size(); ++i) {
            double span = benchMarks[i].first - benchMarks[i - 4].first;
            best = (std::max)(best, (double)(benchMarks[i].second - benchMarks[i - 4].second) / span);
            if (bestMs == 0 || span * 1000.0 / 1024.0 < bestMs) bestMs = span * 1000.0 / 1024.0;
        }
        fprintf(stderr, "\nBENCH frames=%llu seconds=%.3f instructions=%llu ips=%.0f fps=%.1f per_frame=%.1f state=%016llx best=%.0f best_ms=%.4f\n",
                (unsigned long long)frames, secs, (unsigned long long)instr, instr / secs, frames / secs,
                (double)instr / frames, (unsigned long long)hash, best, bestMs);
    }

    std::vector<uint32_t> profNow;
    if (opt.profile && exitCode != 1 && backend.readProf(profNow)) {
        // Counter names come from the shader's own prof.h: '#define PROF_<name> <id>'.
        std::map<uint32_t, std::string> names;
        std::string text;
        if (readFileBinary((fs::u8path(opt.rvcDir) / "src" / "prof.h").u8string(), text)) {
            size_t p = 0;
            while ((p = text.find("#define PROF_", p)) != std::string::npos) {
                p += 13;
                size_t e = text.find_first_of(" \t", p);
                if (e == std::string::npos) break;
                std::string name = text.substr(p, e - p);
                const char* num = text.c_str() + e;
                while (*num == ' ' || *num == '\t') ++num;
                if (*num >= '0' && *num <= '9') names[(uint32_t)strtoul(num, nullptr, 10)] = name;
            }
        }
        std::vector<uint32_t>& c = profNow;
        {
            for (uint32_t i = 0; i < kProfCount; ++i) c[i] -= profBase[i];
            std::vector<std::pair<uint32_t, uint32_t>> rows;  // count, id
            for (uint32_t i = 0; i < kProfCount; ++i)
                if (c[i]) rows.push_back({c[i], i});
            std::sort(rows.rbegin(), rows.rend());
            double ticks = c[0] ? (double)c[0] : 1.0;
            fprintf(stderr, "\nPROFILE (events, and events per emulated instruction)\n");
            for (auto& r : rows) {
                auto it = names.find(r.second);
                std::string n = it != names.end() ? it->second : "id" + std::to_string(r.second);
                fprintf(stderr, "PROF %-22s %10u %8.4f\n", n.c_str(), r.first, r.first / ticks);
            }
        }
    }

    if (!opt.dumpState.empty() && exitCode != 1) {
        std::vector<uint8_t> data;
        if (backend.readState(64, 64, data) && writeFileBinary(opt.dumpState, data.data(), data.size()))
            fprintf(stderr, "\n[harness] state area written to %s\n", opt.dumpState.c_str());
    }

    if (!opt.saveState.empty() && exitCode != 1) {
        std::vector<uint8_t> data;
        SnapshotHeader hdr{};
        memcpy(hdr.magic, kSnapshotMagic, 8);
        hdr.width = opt.gvm.empty() ? W : 0;
        hdr.height = opt.gvm.empty() ? H : 0;
        hdr.time = guestTime;
        hdr.sentTag = sentTag;
        hdr.sentChar = sentChar;
        bool ok = opt.gvm.empty() ? backend.readState(W, H, data) : backend.saveSnapshot(data);
        if (ok && cpuRunning) {
            // (--cpu: the processor's state put where the shader machine keeps it, its clock the guest's)
            int workers = cpuSnapshotCores(data, W, opt.workers ? 1u << opt.workers : 2u);   // (2: geotune's pick on this card)
            hdr.time = cpuemu::guest_seconds;
            hdr.sentTag = cpuemu::word(0x876b8280u);
            hdr.sentChar = cpuemu::word(0x876b8284u);
            if (workers < 0) fprintf(stderr, "\n[harness] --cpu: a core is in the middle of a copy: no snapshot\n");
            else fprintf(stderr, "\n[harness] --cpu: core 0 and %d workers in the snapshot, guest time %.1fs\n", workers, hdr.time);
            ok = workers >= 0;
        }
        if (ok) {
            data.insert(data.begin(), (const uint8_t*)&hdr, (const uint8_t*)&hdr + sizeof hdr);
            ok = writeFileBinary(opt.saveState, data.data(), data.size());
        }
        fprintf(stderr, "\n[harness] snapshot %s %s\n", ok ? "written to" : "FAILED:", opt.saveState.c_str());
        if (!ok) exitCode = 1;
    }

    if (statsStarted) {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - statsT0).count();
        uint64_t instr = guestInstructions - statsInstr0, frames = frame - statsFrame0;
        fprintf(stderr, "\nSTATS seconds=%.2f instructions=%llu ips=%.0f frames=%llu fps=%.1f per_frame=%.1f\n", secs,
                (unsigned long long)instr, instr / secs, (unsigned long long)frames, frames / secs, (double)instr / (double)frames);
        if (opt.cores > 1)
            fprintf(stderr, "STATS workers: instructions=%llu ips=%.0f; all cores ips=%.0f\n", (unsigned long long)(workerInstructions - workerInstr0),
                    (workerInstructions - workerInstr0) / secs, (instr + workerInstructions - workerInstr0) / secs);
        if (opt.unityMs > 0 && unityFrames)
            fprintf(stderr, "STATS unity: a frame %.2f ms of the guest's clock, %.2f ms of rounds; %.2f rounds a frame, %.3f ms on the card a frame (%llu timed; the rounds' CPU at R %.2f ms when larger)\n",
                    opt.unityMs, opt.unityMs - opt.unityOverMs, (double)unityRoundSum / (double)unityFrames, unityTimed ? unityMsSum / (double)unityTimed : 0.0, (unsigned long long)unityTimed, opt.unityRoundMs);
        for (size_t v = 0; v < altMs.size() && !altDirs.empty(); ++v)
            fprintf(stderr, "STATS tick %zu %s: %.4f ms a frame's tick over %llu frames timed (%llu frames run)\n", v,
                    v ? altDirs[v - 1].c_str() : opt.rvcDir.c_str(), altN[v] ? altMs[v] / (double)altN[v] : 0.0,
                    (unsigned long long)altN[v], (unsigned long long)altPicked[v]);
        if (timeSamples > 0)
            fprintf(stderr, "STATS gpu per frame: tick %.3f ms, commit %.3f ms, device %.3f ms; wall %.3f ms\n", timeSum[0] / timeSamples,
                    timeSum[1] / timeSamples, timeSum[2] / timeSamples, 1000.0 * secs / (double)frames);
    }
    if (opt.cores > 1) {
        // each core's state: what it is doing as the run ends. Core 0's is at the left; a
        // worker's rows of the strip beside it are where the geometry (control texel 0x3d) puts them.
        std::vector<uint8_t> blocks;
        if (backend.readState(2048, 64, blocks) && blocks.size() >= (size_t)2048 * 64 * 16 && row.size() >= (64 + 0x40) * 16) {
            const uint32_t* all = (const uint32_t*)blocks.data();
            const uint32_t* geo = (const uint32_t*)row.data() + (64 + 0x3d) * 4;
            // the geometry: how many workers, and their caches' size (all alike)
            unsigned workers = geo[1], bits = geo[2] >= 3 && geo[2] <= 6 ? geo[2] : 0;
            unsigned rows = bits ? (44 + 8 + ((7u << bits) >> 2) * 4 + 63) / 64 : 0, per = rows ? 248 / rows : 1;
            uint64_t workersRan = 0;
            for (int c = 0; c < opt.cores; ++c) {
                if (c != 0 && (!rows || (unsigned)c > workers)) continue;
                // texel w (below 64) of the core's state: a worker's is in the first 8 x 8 tile of its strip
                unsigned band = (unsigned)(c - 1) / per, at = (unsigned)(c - 1) % per * rows;
                auto texel = [&](unsigned w) {
                    size_t x = c == 0 ? w : 64 + at * 8 + (w & 7), y = c == 0 ? 0 : band * 8 + (w >> 3);
                    return all + (y * 2048 + x) * 4;
                };
                uint32_t word = texel(41)[3];
                const uint32_t pc = texel(36)[3], ran = texel(28)[1], committed = texel(28)[2];
                if (c != 0) workersRan += ran;
                if (c == opt.cores - 1 && opt.cores > 16)
                    fprintf(stderr, "CORES: the workers ran %llu instructions in all\n", (unsigned long long)workersRan);
                if (c >= 4 && c < opt.cores - 1 && opt.cores > 16) continue;   // (the first few and the last say enough)
                fprintf(stderr, "CORE %d: pc %08x, %u instructions, %u commits, %s (state word %08x%s)\n", c, pc, ran,
                        committed, c == 0 ? "the machine" : (word & 8) ? "waiting for a system call" : (word & 4) ? "stopped by a fault" : (word & 2) ? "asleep" :
                        (word & 1) ? "running" : (word >> 8) == 0 ? "parked, never started" : (word >> 8) == 2 ? "parked by the kernel" : "parked by ebreak", word,
                        c == 0 ? "" : bits == 6 ? "; 6 KB of stores a pass" : bits == 5 ? "; 3 KB of stores a pass" : bits == 4 ? "; 1.5 KB of stores a pass" : "; 0.75 KB of stores a pass");
            }
        }
    }
    if (!opt.frameLog.empty()) writeFileBinary(opt.frameLog, (const uint8_t*)frameSamples.data(), frameSamples.size() * 4);
    // (beside it, each frame's rounds, and each timing as it came: frame, card ms, rounds as floats)
    if (!opt.frameLog.empty()) writeFileBinary(opt.frameLog + ".rounds", (const uint8_t*)roundSamples.data(), roundSamples.size() * 4);
    if (!opt.frameLog.empty()) writeFileBinary(opt.frameLog + ".timed", (const uint8_t*)timedSamples.data(), timedSamples.size() * 4);
    // (beside it, a pass's worker cores: their instructions, how many ran, how many a whole pass, how many only waited)
    if (!opt.frameLog.empty() && !workerSamples.empty())
        writeFileBinary(opt.frameLog + ".workers", (const uint8_t*)workerSamples.data(), workerSamples.size() * 4),
        writeFileBinary(opt.frameLog + ".most", (const uint8_t*)workerMost.data(), workerMost.size() * 4);   // (the most one worker ran)
    if (!opt.l1Log.empty()) writeFileBinary(opt.l1Log, (const uint8_t*)l1Samples.data(), l1Samples.size() * 4);
    if (!opt.pcLog.empty()) writeFileBinary(opt.pcLog, (const uint8_t*)pcSamples.data(), pcSamples.size() * 4);
    if (!opt.raLog.empty()) writeFileBinary(opt.raLog, (const uint8_t*)raSamples.data(), raSamples.size() * 4);
    if (!opt.soundCapture.empty() && exitCode != 1)
        fprintf(stderr, "\n[harness] sound (%u mixes) %s %s\n", soundMixes, soundCapture.write(opt.soundCapture) ? "written to" : "capture FAILED:",
                opt.soundCapture.c_str());
    if (!opt.gpuCapture.empty() && exitCode != 1)
        fprintf(stderr, "\n[harness] GPU target %s %s\n", backend.gpuCapture(opt.gpuCapture) ? "written to" : "capture FAILED:", opt.gpuCapture.c_str());
    if (!opt.vizCapture.empty() && backend.viewOpen() && exitCode != 1) {
        backend.viewRender(false);
        bool ok = backend.viewCapture(opt.vizCapture);
        for (auto& l : overlayLines) fprintf(stderr, "\n[harness] overlay: %s", l.c_str());
        fprintf(stderr, "\n[harness] memory view %s %s\n", ok ? "written to" : "capture FAILED:", opt.vizCapture.c_str());
    }
    backend.viewClose();
    restoreConsole();

    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "\n[harness] %llu frames in %.3fs, %llu guest instructions, avg %.1fk IPS, %u commits%s\n",
            (unsigned long long)frame, wall, (unsigned long long)guestInstructions,
            wall > 0 ? guestInstructions / wall / 1000.0 : 0.0, commits,
            untilHit ? ", --until matched" : (exitCode == 3 ? ", --until NOT matched" : ""));
    std::vector<uint32_t> hart;
    if (backend.computeMode && backend.computeState(hart))
        fprintf(stderr, "[harness] the compute processor: pc %08x, privilege %u, %llu instructions in %u passes\n", hart[80], hart[81],
                (unsigned long long)hart[93] | (unsigned long long)hart[94] << 32, hart[95]);
    if (opt.cores > 1)
        fprintf(stderr, "[harness] %d workers: %llu instructions more, all cores avg %.1fk IPS\n", opt.cores - 1,
                (unsigned long long)workerInstructions, wall > 0 ? (guestInstructions + workerInstructions) / wall / 1000.0 : 0.0);
    if (net.on())
        fprintf(stderr, "[harness] network: %llu packets sent, %llu lost on the way (--net-loss), %llu received, %llu never taken\n",
                (unsigned long long)net.sent, (unsigned long long)net.lost, (unsigned long long)net.received, (unsigned long long)net.dropped);
    if (uartLog) fclose(uartLog);
    fflush(stdout);
    // The stdin thread may be blocked in getchar(); exit without joining it.
    _exit(exitCode);
}
