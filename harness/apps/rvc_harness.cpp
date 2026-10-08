// rvc_harness: runs pimaker's rvc (RISC-V Linux in a VRChat pixel shader) outside Unity.
//
// It loads rvc's unmodified _Nix/rvc/main.shader, compiles both passes with FXC, and drives
// them the way the Unity scene does: a 2048x4096 RGBA32_UINT double-buffered Custom Render
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
#include <vector>

namespace fs = std::filesystem;

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
    std::vector<std::pair<std::string, std::string>> expectSend;
    std::string initialInput;
    std::string until;
    int ticks = 16384;
    int initFrames = 2;
    uint64_t maxFrames = 0;
    double maxSeconds = 0;
    double fixedDt = 0;  // 0 = real time
    double statsInterval = 0;
    int64_t benchWarmup = -1;  // >= 0: benchmark mode, this many unmeasured frames first
    bool readStdin = true;
    bool verbose = false;
    bool profile = false;
    bool present = false;
    bool viz = false;         // memory view window
    int uartBurst = 0;        // input characters per handshake; 0 = what the shader declares
    bool resume = false;      // with no other arguments: resume the shell snapshot instead of booting
    bool desktop = false;     // ask the guest to start its desktop: the terminal mode, unless --no-desktop
    std::string vizCapture;   // BMP of the memory view, written at exit
    std::string gpuCapture;   // BMP of the GPU device's colour target, written at exit
    std::string pcLog;        // per frame: the guest's pc and the instructions it ran, as two uint32
    std::string frameLog;     // per frame, four uint32: pc, instructions, last stall, microseconds since start
    double statsAfter = -1;   // >= 0: print a STATS line for the part of the run after this many seconds
    bool noDoubles = false;
    bool noBands = false;     // --no-bands: the commit rewrites all of RAM, as a CustomRenderTexture does
    bool ourKernel = false;   // the RAM image is this project's (kernel at +4 MiB, device tree at +34 MiB)
    bool sbi = false;         // compile with SBI_HLE
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
  --ticks N            emulated instructions per tick pass (default 16384; upstream uses 2048). Large values on a real
                       GPU can trigger a driver timeout (TDR); WARP has no timeout.
  --frames N           stop after N frames          --seconds S   stop after S seconds
  --until TEXT         stop (exit 0) once the UART output contains TEXT; exit 3 if a limit hits first
  --input TEXT         type TEXT at start (escapes: \n \r \t \\ \xHH)
  --expect A --send B  after output contains A, type B (pairs, processed in order)
  --no-stdin           do not forward console input
  --fixed-dt S         drive _Time as frame*S seconds (deterministic) instead of wall clock
  --uart-log FILE      append all UART output to FILE
  --dump-state FILE    write the 64x64 state area (raw uint32 RGBA) at exit
  --save-state FILE    write a snapshot of the whole machine (128 MB) at exit
  --load-state FILE    resume from a snapshot instead of booting
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
  --image NAME         boot a named image instead of --payload/--ram/--mtd/--dtb; --image list shows them.
                       Without it (and without --payload, --ram or --load-state) a menu asks at start
                       when run from a console; scripts with redirected input or --no-stdin get linux-net.
  --machine M          how much machine the shader is compiled with (experiments/rvc_opt only):
                         full      everything; needed for Linux
                         nopaging  NO_PAGING: satp hardwired to 0, no translation or TLBs
                         mmode     M_MODE_ONLY: also no supervisor/user mode or trap delegation
                         auto      (default) the smallest one the chosen image runs on
  --no-gpu             leave out the GPU device (the passes in <rvc>/gpu.shader, docs/gpu.md)
  --no-bands           have the commit rewrite all of RAM instead of only the bands written to
  --gpu-capture FILE   save the GPU device's whole colour target as a BMP at exit
  --no-sound           leave out the sound card (<rvc>/sound.shader, docs/sound.md)
  --sound              play what the sound card mixes (the terminal mode does; other runs are silent)
  --volume P           how loud the host plays it, 0 to 100 (default 10). In the window Ctrl+F11 and
                       Ctrl+F12 change it and Ctrl+F9 turns the sound card off and on
  --sound-capture FILE save what it mixed as a WAV at exit, and FILE.frames for tools/sound_reference.py
  --pc-log FILE        sample the guest's pc once a frame, for tools/pc_profile.py
  --frame-log FILE     per frame: pc, instructions, last stall and time, for tools/boot_profile.py
  --stats-after S      print instructions/s and frames/s for the run after its first S seconds
                       (and start the pc log there)
  --no-desktop         terminal mode: boot to the shell only (by default the guest starts its desktop);
                       --desktop asks for it in a run with other arguments
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
        else if (a == "--ticks") o.ticks = atoi(next("--ticks").c_str());
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
        else if (a == "--uart-log") o.uartLog = next("--uart-log");
        else if (a == "--dump-state") o.dumpState = next("--dump-state");
        else if (a == "--save-state") o.saveState = next("--save-state");
        else if (a == "--load-state") o.loadState = next("--load-state");
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
        else if (a == "--desktop") o.desktop = true;
        else if (a == "--viz-capture") { o.vizCapture = next("--viz-capture"); o.viz = true; }
        else if (a == "--no-doubles") o.noDoubles = true;
        else if (a == "--no-gpu") o.noGpu = true;
        else if (a == "--gpu-capture") o.gpuCapture = next("--gpu-capture");
        else if (a == "--no-sound") o.noSound = true;
        else if (a == "--sound") o.sound = true;
        else if (a == "--volume") o.volume = (std::max)(0, (std::min)(100, atoi(next("--volume").c_str())));
        else if (a == "--sound-capture") o.soundCapture = next("--sound-capture");
        else if (a == "--pc-log") o.pcLog = next("--pc-log");
        else if (a == "--frame-log") o.frameLog = next("--frame-log");
        else if (a == "--stats-after") o.statsAfter = atof(next("--stats-after").c_str());
        else if (a == "--define") o.defines.push_back(next("--define"));
        else if (a == "--no-bands") o.noBands = true;
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

void applyImage(Options& o, const BootImage& im) {
    o.image = im.name;
    o.payloadDir = imageDir(im);
    o.ramPrefix = im.ram;
    o.mtdPrefix = im.mtd;
    o.dtbPrefix = im.dtb;
    // Our build of the Linux image (tools/make_linux_image.py): upstream's root filesystem with
    // our programs added, and a device tree that keeps the kernel out of the GPU's memory.
    std::error_code ec;
    if (o.image == "linux-net" && fs::exists("build/images/linux/rootfs.bin", ec) && fs::exists("build/images/linux/dts.bin", ec)) {
        o.mtdPrefix = "build/images/linux/rootfs";
        o.dtbPrefix = "build/images/linux/dts";
        // and our kernel, if it has been built (linux/kernel/build.sh)
        if (fs::exists("build/images/linux/linux_payload.bin", ec)) {
            o.ramPrefix = "build/images/linux/linux_payload";
            o.ourKernel = true;
        }
    }
}

// Start menu, shown when the command line does not say what to boot. False = the user backed out.
bool chooseImage(Options& o, bool canResume) {
    std::vector<const BootImage*> list;
    for (auto& im : kImages)
        if (imageAvailable(im)) list.push_back(&im);
    fprintf(stderr, "\n  rvc: a RISC-V machine in a pixel shader    [%s]\n\n  Boot which image?\n\n",
            o.dxc ? "D3D12 + DXC" : "D3D11 + FXC");
    for (size_t i = 0; i < list.size(); ++i)
        fprintf(stderr, "    %zu  %-13s %s%s\n", i + 1, list[i]->name, list[i]->title,
                list[i]->machine == 0 ? "  [full machine]" : list[i]->machine == 1 ? "  [no paging]" : "  [machine mode only]");
    if (canResume) fprintf(stderr, "    r  %-13s %s\n", "resume", "Linux at the shell prompt, from the saved snapshot");
    const char* kMachine[4] = {"auto: the smallest the image runs on (shown in brackets)", "full",
                               "no paging (Linux will not boot)", "machine mode only (OpenSBI images will not boot)"};
    fprintf(stderr, "\n    m  machine: %s\n", kMachine[o.machine + 1]);
    fprintf(stderr, "\n  Press a key (Enter = 1, Esc = quit): ");
    for (;;) {
        int c = _getch();
        if (c == 0 || c == 0xE0) { _getch(); continue; }  // function and arrow keys come as two codes
        if (c == 'm' || c == 'M') {
            o.machine = o.machine == 2 ? -1 : o.machine + 1;
            fprintf(stderr, "\n    m  machine: %s\n\n  Press a key (Enter = 1, Esc = quit): ", kMachine[o.machine + 1]);
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

    // Opened with no arguments (or only --resume / --no-viz): act as a terminal on the emulated
    // machine. Fastest shader, a cold boot you can watch, memory view on.
    bool terminalMode = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--image") ++i;
        else if (a != "--resume" && a != "--no-viz" && a != "--dxc" && a != "--d3d11" && a != "--no-desktop") terminalMode = false;
    }
    std::error_code ec;
    bool haveOpt = fs::exists("experiments/rvc_opt/main.shader", ec);
    if ((terminalMode || opt.dxc) && !opt.rvcDirSet && haveOpt) opt.rvcDir = "experiments/rvc_opt";
    const char* shellSnap = "build/snapshots/rvc_shell.snap";
    bool canResume = terminalMode && fs::exists(shellSnap, ec);
    // Nothing on the command line says what to boot: ask, if there is someone to ask.
    DWORD conMode = 0;
    bool interactive = opt.readStdin && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &conMode) != 0;
    bool chosen = !opt.image.empty() || opt.payloadSet || !opt.loadState.empty() || opt.resume;
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
    if (!opt.image.empty() && !opt.payloadSet) {
        const BootImage* im = findImage(opt.image);
        if (!im) {
            fprintf(stderr, "[harness] unknown image '%s' (try --image list)\n", opt.image.c_str());
            return 1;
        }
        applyImage(opt, *im);
    }
    // The full machine unless a named image is known to need less, or the user said so.
    if (opt.machine < 0) {
        const BootImage* im = opt.loadState.empty() ? findImage(opt.image) : nullptr;
        opt.machine = im && opt.rvcDir == "experiments/rvc_opt" ? im->machine : 0;
    }
    // Our Linux image, and snapshots (which are of it), have no firmware: the machine answers
    // the kernel's calls to it (docs/boot.md).
    opt.sbi = opt.machine == 0 && opt.rvcDir == "experiments/rvc_opt" && (opt.ourKernel || !opt.loadState.empty());
    if (opt.machine == 1) opt.defines.push_back("NO_PAGING");
    if (opt.machine == 2) opt.defines.push_back("M_MODE_ONLY");
    if (opt.machine > 0)
        fprintf(stderr, "[harness] machine: %s\n", opt.machine == 1 ? "no paging (NO_PAGING)" : "machine mode only (M_MODE_ONLY)");
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
    bo.present = opt.present;
    bo.dxcOpt = opt.dxcOpt;
    bo.dxcSm = opt.dxcSm;
    bo.dxcDir = opt.dxcDir;
    // The GPU device is a second shader next to the CPU's; upstream rvc has none.
    SLShader gpuShader;
    std::string gpuPath = (fs::u8path(opt.rvcDir) / "gpu.shader").u8string();
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
    std::string soundPath = (fs::u8path(opt.rvcDir) / "sound.shader").u8string();
    if (bo.gpuShader && !opt.noSound && fs::exists(fs::u8path(soundPath), ec)) {
        if (!loadShaderLab(soundPath, soundShader, err)) {
            fprintf(stderr, "[harness] %s\n", err.c_str());
            return 1;
        }
        bo.soundShader = &soundShader;
    }
    // both backends keep two state buffers, which is what lets the commit skip unwritten bands
    if (opt.sbi) opt.defines.push_back("SBI_HLE");
    if (!opt.noBands) opt.defines.push_back("COMMIT_BANDS");
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

    // --- Material and backend ---
    Material mat;
    mat.applyDefaults(shader);
    std::unique_ptr<RvcBackend> backendPtr = opt.dxc ? makeBackend12() : makeBackend11();
    RvcBackend& backend = *backendPtr;
    if (!backend.init(bo, shader, mat, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
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
    uint32_t sentTag = 0, consumedTag = 0, sentChar = 0;
    if (!opt.loadState.empty()) {
        std::string snap;
        SnapshotHeader hdr{};
        size_t need = sizeof hdr + (size_t)W * H * 16;
        bool ok = readFileBinary(opt.loadState, snap) && snap.size() == need;
        if (ok) memcpy(&hdr, snap.data(), sizeof hdr);
        if (!ok || memcmp(hdr.magic, kSnapshotMagic, 8) != 0 || hdr.width != W || hdr.height != H) {
            fprintf(stderr, "[harness] %s is not a %ux%u snapshot\n", opt.loadState.c_str(), W, H);
            return 1;
        }
        if (!backend.setState(snap.data() + sizeof hdr, err)) {
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
    uint64_t ovInstr = 0, ovFrames = 0;
    double gpuTickMs = -1, gpuCommitMs = -1, gpuDeviceMs = 0;
    unsigned lastKey = 0;   // last character handed to the guest
    std::deque<uint32_t> keyEvents;   // raw key events from the window, waiting for the input device
    uint32_t keySeq = 0;              // events delivered so far
    MemoryViewInput pointer;
    auto withCommas = [](uint64_t v) {
        std::string s = std::to_string(v);
        for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
        return s;
    };
    size_t expectIdx = 0, expectScanFrom = 0;
    std::string transcript;
    bool untilHit = false;
    bool haveClock = false;
    uint32_t lastClock = 0, commits = 0;
    uint64_t guestInstructions = 0;

    auto t0 = std::chrono::steady_clock::now();
    double lastStats = 0;
    uint64_t statsInstr = 0, statsFrames = 0;
    uint64_t frame = 0;
    double guestTime = timeBase;

    std::vector<uint32_t> pcSamples;
    std::vector<uint32_t> frameSamples;
    bool statsStarted = false;
    double timeSum[3] = {0, 0, 0};
    int timeSamples = 0;
    uint64_t statsInstr0 = 0, statsFrame0 = 0;
    auto statsT0 = t0;
    bool keyboardOwned = false;   // a guest program reads the keyboard device (docs/input.md)
    // A page the guest asked for (docs/fetch.md): fetched on a thread, handed over in one frame.
    struct Fetch {
        std::mutex lock;
        bool busy = false, ready = false;
        uint32_t seq = 0, status = 0, info = 0;
        std::string body;
    } fetch;
    uint32_t fetchAnswered = 0;
    bool fetchDelivering = false;
    int gpuPassLife[8] = {};      // frames each of the GPU's passes 1-7 is still drawn for
    // The sound card (docs/sound.md). While the guest has it enabled the host mixes: it names a
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
    auto processRow = [&](const std::vector<uint8_t>& raw, uint64_t rowFrame) {
        const uint32_t* t = (const uint32_t*)raw.data();
        auto texel = [&](int x, int c) { return t[x * 4 + c]; };
        uint32_t clock = texel(28, 1);
        keyboardOwned = raw.size() >= (64 + 4) * 16 && texel(64 + 3, 0) == 0x6b657973u;
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
        // passes a submitted list asks for: drawn from now on, and for a while after the last use
        if (raw.size() >= (64 + 2) * 16)
            for (int p = 1; p < 8; ++p)
                if ((texel(64 + 1, 0) >> (8 + p)) & 1) gpuPassLife[p] = 600;
        if (raw.size() >= (64 + 34) * 16 && texel(64 + 16, 0) != texel(64 + 17, 0) && texel(64 + 16, 0) != fetchAnswered) {
            std::lock_guard<std::mutex> lock(fetch.lock);
            if (!fetch.busy && !fetch.ready) {
                uint32_t length = (std::min)(texel(64 + 16, 1), 255u);
                std::string url((const char*)raw.data() + (64 + 18) * 16, length);
                fetch.busy = true;
                fetch.seq = texel(64 + 16, 0);
                bool picture = texel(64 + 16, 2) == 1;
                fprintf(stderr, "[harness] the guest asks for %s\n", url.c_str());
                std::thread([&fetch, url, picture] {
                    std::string body;
                    uint32_t status = 0, info = 0;
                    HINTERNET net = InternetOpenA("ShaderEmu", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
                    HINTERNET page = net && url.rfind("http", 0) == 0
                        ? InternetOpenUrlA(net, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0) : nullptr;
                    if (page) {
                        DWORD size = sizeof(status), got = 0;
                        char buf[8192];
                        HttpQueryInfoA(page, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr);
                        while (body.size() < kFetchSide * kFetchSide * 4 && InternetReadFile(page, buf, sizeof(buf), &got) && got)
                            body.append(buf, got);
                        InternetCloseHandle(page);
                    }
                    if (net) InternetCloseHandle(net);
                    if (picture && status == 200) {
                        // The guest has no decoder: it gets pixels, a word each, of the picture
                        // scaled to fit the 256 KB it can be handed.
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
                        if (info) body.swap(pixels);
                        else status = 415;   // not a picture this host can read
                    }
                    std::lock_guard<std::mutex> lock(fetch.lock);
                    fetch.body.swap(body);
                    fetch.status = status;
                    fetch.info = info;
                    fetch.busy = false;
                    fetch.ready = true;
                }).detach();
            }
        }
        commits = texel(28, 2);
        consumedTag = texel(9, 3);
        if (rowFrame < (uint64_t)opt.initFrames) return;  // cpu_init leaves junk in the UART buffer
        if (haveClock) guestInstructions += (uint32_t)(clock - lastClock);
        if (haveClock && !opt.pcLog.empty() && (opt.statsAfter < 0 || statsStarted)) {
            pcSamples.push_back(texel(36, 3));
            pcSamples.push_back((uint32_t)(clock - lastClock));
        }
        if (haveClock && !opt.frameLog.empty()) {
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            frameSamples.insert(frameSamples.end(), {texel(36, 3), (uint32_t)(clock - lastClock), texel(40, 2), (uint32_t)us});
        }
        lastClock = clock;
        haveClock = true;

        uint32_t ptr = texel(11, 3);
        if (ptr == 0xFFFFFFFFu) return;
        uint32_t n = (std::min)(ptr + 1, 64u);
        std::string out;
        for (uint32_t i = 0; i < n; ++i) {
            char c = (char)(texel(12 + i / 4, i % 4) & 0xFF);
            if (c) out += c;
        }
        if (out.empty()) return;
        fwrite(out.data(), 1, out.size(), stdout);
        fflush(stdout);
        if (uartLog) { fwrite(out.data(), 1, out.size(), uartLog); fflush(uartLog); }
        size_t before = transcript.size();
        transcript += out;
        if (!opt.until.empty() && !untilHit) {
            size_t from = before >= opt.until.size() ? before - opt.until.size() + 1 : 0;
            if (transcript.find(opt.until, from) != std::string::npos) untilHit = true;
        }
        while (expectIdx < opt.expectSend.size()) {
            size_t p = transcript.find(opt.expectSend[expectIdx].first, expectScanFrom);
            if (p == std::string::npos) break;
            expectScanFrom = p + opt.expectSend[expectIdx].first.size();
            for (char c : opt.expectSend[expectIdx].second) scriptQueue.push_back(c);
            ++expectIdx;
        }
    };

    std::vector<uint8_t> row, mix;
    // The oldest frame's row, and its mix when it drew one.
    auto takeRow = [&]() {
        uint64_t f;
        if (!backend.popRow(row, f)) return false;
        processRow(row, f);
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
    for (;;) {
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double t = timeBase + (opt.fixedDt > 0 ? (double)frame * opt.fixedDt : wall);
        guestTime = t;
        if (opt.statsAfter >= 0 && !statsStarted && wall >= opt.statsAfter) {
            statsStarted = true;
            statsInstr0 = guestInstructions;
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

        // Feed one input character per handshake.
        if (frame >= (uint64_t)opt.initFrames && sentTag == consumedTag) {
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
            }
        }

        // The input device: the pointer every frame, and up to four key events.
        {
            uint32_t batch[4] = {0, 0, 0, 0};
            int n = 0;
            for (; n < 4 && !keyEvents.empty(); ++n) {
                batch[n] = keyEvents.front();
                keyEvents.pop_front();
            }
            mat.setVector("_InputPointer", pointer.x, pointer.y, pointer.panelW, pointer.panelH);
            mat.setInt("_InputButtons", pointer.buttons);
            mat.setInt("_HostMs", (int64_t)(uint32_t)(t * 1000.0));   // a clock programs read without a system call
            mat.setInt("_HostFlags", opt.desktop ? 1 : 0);
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
                std::vector<uint8_t> bytes((size_t)kFetchSide * kFetchSide * 4, 0);
                size_t n = (std::min)(fetch.body.size(), bytes.size());
                memcpy(bytes.data(), fetch.body.data(), n);
                std::string err;
                if (backend.deliverHostData(mat, bytes.data(), err)) {
                    mat.setInt("_FetchDeliver", 1);
                    mat.setInt("_FetchSeq", fetch.seq);
                    mat.setInt("_FetchLength", (int64_t)n);
                    mat.setInt("_FetchStatus", fetch.status);
                    mat.setInt("_FetchInfo", fetch.info);
                    fetchDelivering = true;
                    fprintf(stderr, "[harness] answered with %zu bytes, status %u\n", n, fetch.status);
                }
                fetchAnswered = fetch.seq;
                fetch.ready = false;
            }
        }

        if (opt.benchWarmup >= 0 && frame == (uint64_t)opt.benchWarmup) {
            // Start the measurement from an idle GPU with every earlier frame accounted for.
            while (backend.rowPending() > 0 && takeRow()) {}
            benchInstr0 = guestInstructions;
            backend.readProf(profBase);  // warm-up events are subtracted at the end
            benchT0 = std::chrono::steady_clock::now();
        }
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
        bool sampleTimes = statsStarted && (frame & 63) == 0;
        if (sampleTimes && backend.gpuTimes(gpuTickMs, gpuCommitMs, gpuDeviceMs)) {
            timeSum[0] += gpuTickMs;
            timeSum[1] += gpuCommitMs;
            timeSum[2] += gpuDeviceMs;
            ++timeSamples;
        }
        {
            uint32_t passes = 1;
            for (int p = 1; p < 8; ++p)
                if (gpuPassLife[p] > 0) {
                    --gpuPassLife[p];
                    passes |= 1u << p;
                }
            backend.gpuPasses = passes;
            mat.setInt("_GpuPasses", passes);
        }
        backend.frame(mat, frame, sampleTimes || (backend.viewOpen() && wall - ovLast >= 0.2));
        ++frame;

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
            double fps = (double)(frame - statsFrames) / dt;
            char title[256];
            snprintf(title, sizeof title, "rvc_harness | %.1fk IPS | %.1f frames/s | frame %llu | commits %u", ips / 1000.0,
                     fps, (unsigned long long)frame, commits);
            SetConsoleTitleA(title);
            backend.viewTitle(std::string("memory | ") + (title + 14));
            if (opt.statsInterval > 0 && wall - lastStats >= opt.statsInterval)
                fprintf(stderr, "\n[harness] %s\n", title + 14);
            if (opt.statsInterval <= 0 || wall - lastStats >= opt.statsInterval) {
                lastStats = wall;
                statsInstr = guestInstructions;
                statsFrames = frame;
            }
        }

        if (backend.viewOpen() && wall - ovLast >= 0.25) {
            backend.gpuTimes(gpuTickMs, gpuCommitMs, gpuDeviceMs);
            double dt = wall - ovLast;
            double ips = (double)(guestInstructions - ovInstr) / dt, fps = (double)(frame - ovFrames) / dt;
            char l1[96], l2[96], l3[96], l4[96], l5[96];
            snprintf(l1, sizeof l1, "%s IPS    %.0f frames/s", withCommas((uint64_t)ips).c_str(), fps);
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
            snprintf(l5, sizeof l5, "%s instructions    %s commits", withCommas(guestInstructions).c_str(), withCommas(commits).c_str());
            overlayLines = {l1, l2, l3, l6, l4, l5};
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
                if (l.size() > 72) l = l.substr(l.size() - 72);
            char i1[64], i2[64], i3[64], i4[64];
            size_t queued;
            {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                queued = scriptQueue.size() + g_stdinQueue.size();
            }
            snprintf(i1, sizeof i1, "input: window%s, %s", keyboardOwned ? " (keys to the keyboard device only)" : "",
                     g_rawConsole ? "console" : (opt.readStdin ? "stdin" : "script"));
            snprintf(i2, sizeof i2, "queued %zu   sent %u", queued, sentTag);
            snprintf(i3, sizeof i3, "taken by guest %u", consumedTag);
            if (lastKey >= 0x20 && lastKey < 0x7f) snprintf(i4, sizeof i4, "last key '%c' (0x%02x)", lastKey, lastKey);
            else if (lastKey) snprintf(i4, sizeof i4, "last key 0x%02x", lastKey);
            else snprintf(i4, sizeof i4, "last key -");
            backend.viewText({overlayLines, console, {i1, i2, i3, i4}});
            ovLast = wall;
            ovInstr = guestInstructions;
            ovFrames = frame;
        }
        // The memory view samples the state at about 60 Hz, however fast the emulator runs.
        if (backend.viewOpen() && wall - lastViz >= 1.0 / 60) {
            backend.viewRender();
            lastViz = wall;
            pointer = memoryViewTakeInput();
            keyEvents.insert(keyEvents.end(), pointer.keys.begin(), pointer.keys.end());
            pointer.keys.clear();
            // keys typed into the window go to the guest's console too, unless a program there
            // has the keyboard device open; Ctrl+] quits
            for (char c : memoryViewTakeHostKeys()) {
                if (c == 's') soundSwitch = !soundSwitch;
                else opt.volume = (std::max)(0, (std::min)(100, opt.volume + (c == '+' ? 5 : -5)));
                if (audioOpen) audio.setVolume(opt.volume / 100.0f);
                if (audioOpen && !soundSwitch) audio.silence();
            }
            std::string typed = memoryViewTakeKeys();
            if (!typed.empty()) {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                for (char c : typed) {
                    if (c == kQuitKey) g_quit = true;
                    else if (!keyboardOwned) g_stdinQueue.push_back(c);
                }
            }
        }
        if (viewShown && !backend.viewOpen()) break;  // closing the memory window quits
        if (g_quit) break;
        if (untilHit) break;
        if (opt.maxFrames && frame >= opt.maxFrames) { if (!opt.until.empty()) exitCode = 3; break; }
        if (opt.maxSeconds > 0 && wall >= opt.maxSeconds) { if (!opt.until.empty()) exitCode = 3; break; }
    }

    // Drain outstanding readbacks so trailing output is not lost.
    while (exitCode != 1 && backend.rowPending() > 0 && takeRow()) {}
    if (audioOpen) audio.close();
    if (untilHit) exitCode = 0;

    if (opt.benchWarmup >= 0 && exitCode != 1 && frame > (uint64_t)opt.benchWarmup) {
        // The drain above blocked on the last frame, so this interval covers all GPU work.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchT0).count();
        uint64_t frames = frame - (uint64_t)opt.benchWarmup, instr = guestInstructions - benchInstr0;
        uint64_t hash = 0;
        std::vector<uint8_t> data;
        if (backend.readState(64, 64, data)) hash = fnv1a64(data.data(), data.size());
        fprintf(stderr, "\nBENCH frames=%llu seconds=%.3f instructions=%llu ips=%.0f fps=%.1f per_frame=%.1f state=%016llx\n",
                (unsigned long long)frames, secs, (unsigned long long)instr, instr / secs, frames / secs,
                (double)instr / frames, (unsigned long long)hash);
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
        hdr.width = W;
        hdr.height = H;
        hdr.time = guestTime;
        hdr.sentTag = sentTag;
        hdr.sentChar = sentChar;
        bool ok = backend.readState(W, H, data);
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
        if (timeSamples > 0)
            fprintf(stderr, "STATS gpu per frame: tick %.3f ms, commit %.3f ms, device %.3f ms; wall %.3f ms\n", timeSum[0] / timeSamples,
                    timeSum[1] / timeSamples, timeSum[2] / timeSamples, 1000.0 * secs / (double)frames);
    }
    if (!opt.frameLog.empty()) writeFileBinary(opt.frameLog, (const uint8_t*)frameSamples.data(), frameSamples.size() * 4);
    if (!opt.pcLog.empty()) writeFileBinary(opt.pcLog, (const uint8_t*)pcSamples.data(), pcSamples.size() * 4);
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
    fprintf(stderr, "\n[harness] %llu frames in %.1fs, %llu guest instructions, avg %.1fk IPS, %u commits%s\n",
            (unsigned long long)frame, wall, (unsigned long long)guestInstructions,
            wall > 0 ? guestInstructions / wall / 1000.0 : 0.0, commits,
            untilHit ? ", --until matched" : (exitCode == 3 ? ", --until NOT matched" : ""));
    if (uartLog) fclose(uartLog);
    fflush(stdout);
    // The stdin thread may be blocked in getchar(); exit without joining it.
    _exit(exitCode);
}
