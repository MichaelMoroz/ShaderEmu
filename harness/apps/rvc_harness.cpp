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
#include "crt.h"
#include "gpu.h"
#include "image.h"
#include "material.h"
#include "readback.h"
#include "shaderlab.h"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifndef SHADERX86_UNITY_INCLUDE_DIR
#define SHADERX86_UNITY_INCLUDE_DIR "harness/unity_include"
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
    int ticks = 2048;
    int initFrames = 2;
    uint64_t maxFrames = 0;
    double maxSeconds = 0;
    double fixedDt = 0;  // 0 = real time
    double statsInterval = 0;
    int64_t benchWarmup = -1;  // >= 0: benchmark mode, this many unmeasured frames first
    bool readStdin = true;
    bool verbose = false;
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
  --ticks N            emulated instructions per tick pass (default 2048). Large values on a real
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
        else if (a == "--rvc") o.rvcDir = next("--rvc");
        else if (a == "--payload") o.payloadDir = next("--payload");
        else if (a == "--ram") o.ramPrefix = next("--ram");
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
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return false; }
    }
    if (havePendingExpect) { fprintf(stderr, "--expect without --send\n"); return false; }
    if (o.payloadDir.empty()) o.payloadDir = (fs::u8path(o.rvcDir) / "data-net").u8string();
    return true;
}

// Loads <dir>/<prefix>.{r,g,b,a}.png into <propBase>_{R,G,B,A}.
bool loadLaneTextures(Gpu& gpu, Material& mat, const std::string& dir, const std::string& prefix,
                      const std::string& propBase, std::vector<ComPtr<ID3D11ShaderResourceView>>& keep) {
    if (prefix == "none") return true;
    const char* lanes[4] = {"r", "g", "b", "a"};
    const char* props[4] = {"_R", "_G", "_B", "_A"};
    for (int i = 0; i < 4; ++i) {
        std::string path = (fs::u8path(dir) / (prefix + "." + lanes[i] + ".png")).u8string();
        ImageRGBA8 img;
        std::string err;
        if (!loadImageRGBA8(path, img, err)) {
            fprintf(stderr, "[harness] %s\n", err.c_str());
            return false;
        }
        auto srv = createTextureRGBA8(gpu.device.Get(), img, /*flipY=*/true, err);
        if (!srv) {
            fprintf(stderr, "[harness] %s: %s\n", path.c_str(), err.c_str());
            return false;
        }
        mat.setTexture(propBase + props[i], srv.Get(), img.width, img.height);
        keep.push_back(srv);
        if (i == 0) fprintf(stderr, "[harness] %s%s <- %s.*.png (%ux%u)\n", propBase.c_str(), "_{R,G,B,A}", prefix.c_str(), img.width, img.height);
    }
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

void stdinThread() {
    for (;;) {
        int c = getchar();
        if (c == EOF) return;
        if (c == '\r') continue;  // console gives CRLF; the guest tty wants a single newline
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_stdinQueue.push_back((char)c);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hrCo;

    // Raw byte output (the guest already emits CRLF), with ANSI escape support on consoles.
    _setmode(_fileno(stdout), _O_BINARY);
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    Gpu gpu;
    std::string err;
    if (!gpu.init(opt.gpu, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    fprintf(stderr, "[harness] device: %s%s, doubles: %s, extended doubles: %s\n", gpu.adapterName.c_str(),
            opt.gpu.warp ? " (WARP)" : "", gpu.doubles ? "yes" : "NO", gpu.extendedDoubles ? "yes" : "no");
    if (!gpu.doubles) fprintf(stderr, "[harness] warning: rvc uses doubles (MULH, timer); this device lacks them\n");

    // --- Shader ---
    std::string shaderPath = (fs::u8path(opt.rvcDir) / "main.shader").u8string();
    SLShader shader;
    if (!loadShaderLab(shaderPath, shader, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    ShaderBuildOptions bo;
    bo.verbose = opt.verbose;
    bo.settings.flags = opt.fxcFlags;
    bo.settings.cacheDir = opt.cacheDir;
    bo.settings.includeDirs = {SHADERX86_UNITY_INCLUDE_DIR};
    bo.settings.defines = {{"SHADER_API_D3D11", "1"}, {"SHADER_TARGET", "50"}, {"UNITY_COMPILER_HLSL", "1"},
                           {"UNITY_VERSION", "202235"}};
    std::vector<GpuPass> passes;
    if (!buildPasses(gpu, shader, {"CPUTick", "Commit"}, bo, passes, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }

    // --- Material ---
    Material mat;
    mat.applyDefaults(shader);
    std::vector<ComPtr<ID3D11ShaderResourceView>> keepAlive;
    if (!loadLaneTextures(gpu, mat, opt.payloadDir, opt.ramPrefix, "_Data_RAM", keepAlive) ||
        !loadLaneTextures(gpu, mat, opt.payloadDir, opt.mtdPrefix, "_Data_MTD", keepAlive) ||
        !loadLaneTextures(gpu, mat, opt.payloadDir, opt.dtbPrefix, "_Data_DTB", keepAlive))
        return 1;
    mat.setInt("_Ticks", opt.ticks);
    mat.setInt("_TicksDivisor", 1);
    mat.setInt("_DoTick", 0);
    mat.setInt("_InitRaw", 0);
    mat.setInt("_UdonUARTInChar", 0);
    mat.setInt("_UdonUARTInTag", 0);
    mat.setVector("unity_OrthoParams", 1, 1, 0, 1);

    // --- Render texture, as configured in rvc's vm_state_crt.asset ---
    const UINT W = 2048, H = 4096;
    CustomRenderTexture crt;
    if (!crt.init(gpu.device.Get(), W, H, DXGI_FORMAT_R32G32B32A32_UINT, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }
    crt.clear(gpu.ctx.Get());
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
        crt.load(gpu.ctx.Get(), snap.data() + sizeof hdr, W * 16);
        timeBase = hdr.time;
        sentTag = consumedTag = hdr.sentTag;
        sentChar = hdr.sentChar;
        mat.setInt("_UdonUARTInChar", (int)sentChar);
        mat.setInt("_UdonUARTInTag", (int)sentTag);
        opt.initFrames = 0;
        fprintf(stderr, "[harness] resumed from %s (guest time %.1fs)\n", opt.loadState.c_str(), timeBase);
    }
    const UpdateZone tickZone{32, 4064, 64, 64, 0};
    const UpdateZone commitZone{1024, 2048, 2048, 4096, 1};

    RegionReadback rowReadback;
    const int kRing = 3;
    if (!rowReadback.init(gpu.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 1, kRing, err)) {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        return 1;
    }

    FILE* uartLog = nullptr;
    if (!opt.uartLog.empty()) uartLog = _wfopen(widen(opt.uartLog).c_str(), L"ab");
    if (uartLog) {
        // Marks where this run starts for anyone following the log live.
        fprintf(uartLog, "\r\n\x1b[7m=== rvc_harness: %s ===\x1b[0m\r\n",
                opt.loadState.empty() ? "cold boot" : ("resumed from " + opt.loadState).c_str());
        fflush(uartLog);
    }

    if (opt.readStdin) std::thread(stdinThread).detach();

    // --- State shared by the loop ---
    std::deque<char> scriptQueue(opt.initialInput.begin(), opt.initialInput.end());
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

    auto processRow = [&](const std::vector<uint8_t>& raw, uint64_t rowFrame) {
        const uint32_t* t = (const uint32_t*)raw.data();
        auto texel = [&](int x, int c) { return t[x * 4 + c]; };
        uint32_t clock = texel(28, 1);
        commits = texel(28, 2);
        consumedTag = texel(9, 3);
        if (rowFrame < (uint64_t)opt.initFrames) return;  // cpu_init leaves junk in the UART buffer
        if (haveClock) guestInstructions += (uint32_t)(clock - lastClock);
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

    std::vector<uint8_t> row;
    int exitCode = 0;
    auto benchT0 = t0;
    uint64_t benchInstr0 = 0;
    for (;;) {
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double t = timeBase + (opt.fixedDt > 0 ? (double)frame * opt.fixedDt : wall);
        guestTime = t;
        mat.setVector("_Time", t / 20, t, t * 2, t * 3);
        mat.setVector("_SinTime", sin(t / 8), sin(t / 4), sin(t / 2), sin(t));
        mat.setVector("_CosTime", cos(t / 8), cos(t / 4), cos(t / 2), cos(t));
        mat.setInt("_Init", frame < (uint64_t)opt.initFrames ? 1 : 0);

        // Feed one input character per handshake.
        if (frame >= (uint64_t)opt.initFrames && sentTag == consumedTag) {
            int c = -1;
            if (!scriptQueue.empty()) { c = (unsigned char)scriptQueue.front(); scriptQueue.pop_front(); }
            else {
                std::lock_guard<std::mutex> lock(g_inputMutex);
                if (!g_stdinQueue.empty()) { c = (unsigned char)g_stdinQueue.front(); g_stdinQueue.pop_front(); }
            }
            if (c > 0) {
                ++sentTag;
                sentChar = (uint32_t)c;
                mat.setInt("_UdonUARTInChar", c);
                mat.setInt("_UdonUARTInTag", sentTag);
            }
        }

        crt.runZone(gpu, passes[0], mat, tickZone);
        crt.runZone(gpu, passes[1], mat, commitZone);

        if (opt.benchWarmup >= 0 && frame == (uint64_t)opt.benchWarmup) {
            // Start the measurement from an idle GPU with every earlier frame accounted for.
            uint64_t f;
            while (rowReadback.pending() > 0 && rowReadback.pop(gpu.ctx.Get(), row, f)) processRow(row, f);
            benchInstr0 = guestInstructions;
            benchT0 = std::chrono::steady_clock::now();
        }
        if (rowReadback.full()) {
            uint64_t f;
            if (!rowReadback.pop(gpu.ctx.Get(), row, f)) { exitCode = 1; break; }
            processRow(row, f);
        }
        rowReadback.request(gpu.ctx.Get(), crt.current(), 0, 0, frame);
        ++frame;

        std::string removed = gpu.deviceRemovedReason();
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
            if (opt.statsInterval > 0 && wall - lastStats >= opt.statsInterval)
                fprintf(stderr, "\n[harness] %s\n", title + 14);
            if (opt.statsInterval <= 0 || wall - lastStats >= opt.statsInterval) {
                lastStats = wall;
                statsInstr = guestInstructions;
                statsFrames = frame;
            }
        }

        if (untilHit) break;
        if (opt.maxFrames && frame >= opt.maxFrames) { if (!opt.until.empty()) exitCode = 3; break; }
        if (opt.maxSeconds > 0 && wall >= opt.maxSeconds) { if (!opt.until.empty()) exitCode = 3; break; }
    }

    // Drain outstanding readbacks so trailing output is not lost.
    while (exitCode != 1 && rowReadback.pending() > 0) {
        uint64_t f;
        if (!rowReadback.pop(gpu.ctx.Get(), row, f)) break;
        processRow(row, f);
    }
    if (untilHit) exitCode = 0;

    if (opt.benchWarmup >= 0 && exitCode != 1 && frame > (uint64_t)opt.benchWarmup) {
        // The drain above blocked on the last frame, so this interval covers all GPU work.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchT0).count();
        uint64_t frames = frame - (uint64_t)opt.benchWarmup, instr = guestInstructions - benchInstr0;
        uint64_t hash = 0;
        RegionReadback area;
        std::vector<uint8_t> data;
        uint64_t f;
        if (area.init(gpu.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 64, 1, err)) {
            area.request(gpu.ctx.Get(), crt.current(), 0, 0, 0);
            if (area.pop(gpu.ctx.Get(), data, f)) hash = fnv1a64(data.data(), data.size());
        }
        fprintf(stderr, "\nBENCH frames=%llu seconds=%.3f instructions=%llu ips=%.0f fps=%.1f per_frame=%.1f state=%016llx\n",
                (unsigned long long)frames, secs, (unsigned long long)instr, instr / secs, frames / secs,
                (double)instr / frames, (unsigned long long)hash);
    }

    if (!opt.dumpState.empty() && exitCode != 1) {
        RegionReadback full;
        if (full.init(gpu.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 64, 1, err)) {
            full.request(gpu.ctx.Get(), crt.current(), 0, 0, 0);
            uint64_t f;
            std::vector<uint8_t> data;
            if (full.pop(gpu.ctx.Get(), data, f) && writeFileBinary(opt.dumpState, data.data(), data.size()))
                fprintf(stderr, "\n[harness] state area written to %s\n", opt.dumpState.c_str());
        }
    }

    if (!opt.saveState.empty() && exitCode != 1) {
        RegionReadback full;
        std::vector<uint8_t> data;
        uint64_t f;
        SnapshotHeader hdr{};
        memcpy(hdr.magic, kSnapshotMagic, 8);
        hdr.width = W;
        hdr.height = H;
        hdr.time = guestTime;
        hdr.sentTag = sentTag;
        hdr.sentChar = sentChar;
        bool ok = full.init(gpu.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, W, H, 1, err);
        if (ok) {
            full.request(gpu.ctx.Get(), crt.current(), 0, 0, 0);
            ok = full.pop(gpu.ctx.Get(), data, f);
        }
        if (ok) {
            data.insert(data.begin(), (const uint8_t*)&hdr, (const uint8_t*)&hdr + sizeof hdr);
            ok = writeFileBinary(opt.saveState, data.data(), data.size());
        }
        fprintf(stderr, "\n[harness] snapshot %s %s\n", ok ? "written to" : "FAILED:", opt.saveState.c_str());
        if (!ok) exitCode = 1;
    }

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
