// What rvc_harness needs from a graphics API: run one emulator frame (CPUTick, then Commit),
// read the state texture back, and show the memory view. Two implementations: D3D11 with FXC
// bytecode (what VRChat runs) and D3D12 with DXC-compiled DXIL (faster, and seconds to compile).
#pragma once

#include "common.h"
#include "gpu.h"
#include "image.h"
#include "material.h"
#include "shaderlab.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct BackendOptions {
    CompileSettings compile;      // defines, include dirs, FXC flags, cache folder
    bool verbose = false;
    GpuOptions gpu;
    bool profile = false;         // D3D11 only: PROF() counters through a UAV
    bool present = false;         // D3D11 only: hidden swapchain presented once per frame
    std::string dxcOpt = "-O3", dxcSm = "6_6", dxcDir;
    const SLShader* gpuShader = nullptr;   // the GPU device's passes (gpu.shader), if the machine has one
    const SLShader* soundShader = nullptr; // the sound card's mix pass (sound.shader); needs the GPU device
    unsigned tickRows = 64;       // rows of the 64 texels wide state block that the tick pass draws (16: rvc_opt)
    int readbackBatch = 1;        // D3D11: frames whose rows are read back with one Map (--readback-batch)
    bool stateLog = false;        // D3D12 only: read the CPU's 64 x 64 state texels back every frame (--l1-log)
    std::string computeShader;    // D3D11 only, --compute: the processor as this compute shader (experiments/rv32_min)
    bool tickDirect = false;      // --unpack-in-place: the unpack draws into the state texture itself, no zone copied in and back (a tick that does not read the texture there)
    bool ramInplace = false;      // D3D11: RAM in one texture written in place, the commit's passes with RAM_INPLACE (src/ram.h)
    bool laneField = false;       // D3D11, RAM in place: the tick has the lanes' field (LANE_FIELD, experiments/rvc_lean; docs/machine/lanes.md)
    // --gvm DIR (gpu_vm/platform.md): a gpu_vm machine (gvm_backend11.cpp): its image's folder (vmlinux, gvm.dtb,
    // rootfs.romfs, ram_mb), its tick (--gvm-tick; gpu_vm/tick/steps/x18.hlsl), RAM in MB (0: the folder's ram_mb, or
    // 192), bundles a pass (0: 8192)
    std::string gvmDir, gvmTick;
    unsigned gvmRamMB = 0, gvmBudget = 0;
};

// The state texture's rows: 64 of the cores' state, then RAM from 0x80000000 to 0x92000000, 32 KB a
// row (STATE_TEX_HEIGHT in src/types.h). Zones are in Unity's pixel space, y up from its bottom.
const unsigned kStateHeight = 9280;
// The GPU device (docs/machine/gpu.md): a mesh of kGpuTriangles triangles drawn into a square colour and
// depth target of kGpuTarget pixels, then one small update zone on the state texture (Unity
// pixel space, y up) holding the control words: RAM 0x87000000 to 0x87000fff.
const UINT kGpuTriangles = 65536;
const UINT kGpuTarget = 2048;
const float kGpuControlZone[4] = {128, kStateHeight - (64 + 0x700000 / 2048) - 0.5f, 256, 1};
// The sound card's mix target (docs/machine/sound.md): kSoundSide squared samples, two floats each.
const UINT kSoundSide = 128;
// The rows of RAM an answer to the guest's request is written to (docs/system/fetch.md), as a zone.
const float kFetchZone[4] = {1024, kStateHeight - (64 + 0x76c000 / 2048) - 4, 2048, 8};
const UINT kFetchSide = 256;   // the answer's texture: one word a texel, 256 KB
// The network device (docs/system/lan.md): a row of RAM at 0x876b8000. Its first kNetSlot texels are the
// packet the guest sends, which popRow() returns after the control words; from kNetRing on is
// the ring of kNetSlots packets for the guest, written as a zone when deliverNet() was called.
const unsigned kNetRow = 64 + 0x76b800 / 2048, kNetSlot = 40, kNetRing = 64, kNetSlots = 8, kNetMost = 4;
// What every core ran (MC_STATS_ALL in src/gpu.h): 72 texels after the ring, which the control
// pass writes every frame. kNetRead texels of the row are read back: the window, the ring, those.
const unsigned kCoreStatsAt = 0x200, kNetRead = kCoreStatsAt + 72;
const float kCoreStatsZone[4] = {kCoreStatsAt + 36.0f, kStateHeight - (64 + 0x76b800 / 2048) - 0.5f, 72, 1};
const float kNetZone[4] = {kNetRing + kNetSlot * kNetSlots / 2.0f, kStateHeight - kNetRow - 0.5f, (float)(kNetSlot * kNetSlots), 1};
// The counters' totals (programs/mc/mc.h, MC_COUNT_*): RAM 0x86C10000 to 0x86C11000, drawn after
// the commit when the control words read back say a core added (src/gpu.h).
const float kCountZone[4] = {128, kStateHeight - (64 + 0x6c1000 / 2048) - 0.5f, 256, 1};
// Quads the Commit pass draws when its vertex shader chooses them: the state rows and RAM's 4 MB bands.
const unsigned kCommitQuads = 1 + (kStateHeight - 64) / 128;
// The machine's control words (display, GPU, input, sound: RAM from 0x87000000) as a row of the
// state texture. popRow() returns them after the 64 texels of row 0.
const unsigned kControlRow = 64 + 0x700000 / 2048, kControlTexels = 256;
// The console (docs/machine/console.md): texel kConHost of the network's row is the host's words, the
// next the guest's, then the ring of kConRing bytes; the control pass writes the first.
const unsigned kConHost = 40, kConRing = 256;
const float kConZone[4] = {kConHost + 0.5f, kStateHeight - kNetRow - 0.5f, 1, 1};
// The lanes' field (docs/machine/lanes.md, experiments/rvc_lean/tick.hlsl): its job's texel and its groups'
// words in the mailboxes' page (RAM 0x86C01900 and 0x86C01A00), which popRow() returns after the network's
// texels: kFieldRead of them from the job's. The tick's primitive after the cores' draws it; the commit has
// a point for each of its buckets, before the picture's.
const unsigned kFieldRow = 64 + 0x6c0000 / 2048, kFieldAt = 0x190, kFieldRead = 0x10 + 62;
const unsigned kFieldLanes = 3968, kFieldPoints = 2 * kFieldLanes;
const int kCoresMost = 256;   // what the shader, the kernel and the library have room for
const int kCores = 256;       // core 0 and 255 workers: the only machine there is (docs/machine/multicore.md)
// The display's RAM framebuffer (RAM 0x87000000, 128 texture rows), for writing the picture back.

class RvcBackend {
public:
    // The GPU device's passes to draw in the next frame, one bit each (docs/machine/gpu.md); pass 0 always.
    uint32_t gpuPasses = 1;
    // Whether the machine's geometry has any worker (the harness reads its texel back with the
    // control words): while it has none, and is not being laid out, nothing of the workers'
    // is drawn or copied, which is a third of what a pass costs besides its instructions.
    // (Workers are started long after the geometry that has them is set: shaderemu_gpu.c.)
    bool workersNow = true;
    bool countsAdded = false;   // the counters' head read back says a core added (kCountZone)
    bool fieldNow = false;      // a job of the lanes' field is posted (read back): its primitive and points are drawn
    bool conDelivered = false;  // the console's input changed: kConZone is drawn in the next frame
    // Draw the sound card's mix in the next frame and read it back with that frame's row.
    bool soundMix = false;
    static const UINT kWidth = 2048, kHeight = kStateHeight;   // rvc's vm_state_crt.asset, taller
    static const UINT kProfCount = 256;

    // --cpu (docs/machine/cpu-harness.md): the instructions run on the processor. The tick pass is not
    // drawn; RAM comes from the host before a frame and the devices' part of it goes back after.
    bool cpuMode = false;
    virtual bool uploadRows(UINT row, UINT rows, const void* texels) { (void)row; (void)rows; (void)texels; return false; }
    virtual bool uploadTexel(UINT x, UINT y, const uint32_t* words) { (void)x; (void)y; (void)words; return false; }
    virtual bool readRows(UINT row, UINT rows, std::vector<uint8_t>& out) { (void)row; (void)rows; (void)out; return false; }
    // --replay: the tick (and its unpack) of another machine's folder, built beside the first (variant 0);
    // useTickVariant puts variant i in the tick's place
    virtual bool addTickVariant(const SLShader& shader, std::string& err) { (void)shader; err = "tick variants are D3D11's"; return false; }
    virtual bool useTickVariant(size_t i) { (void)i; return false; }
    // --compute (experiments/rv32_min/README.md, "The whole machine"): as --cpu, but the processor is
    // a compute shader with RAM in a buffer of its own on the card. A frame runs one of its passes,
    // puts the bands of RAM it stored to into the state texture (the commit carries them over as
    // the processor's), and after the devices' passes takes back what they wrote: the control
    // words, the picture the commit copied back, the console's, the network's and a fetch's words.
    bool computeMode = false;
    uint32_t computeMask = 0xffffffffu;   // the bands that may go into the texture this frame
    uint32_t computeCores = 1;   // core 0 and the workers: a thread group each (set before computeStart)
    uint32_t computePasses = 1;  // the processor's passes a frame at most
    uint32_t computeSplit = 0;
    uint32_t tickRounds = 1;     // the fragment tick: its rounds (a tick and its commit) a frame
    uint32_t tickSplit = 0;      // of them the first so many always run, the rest while core 0 rests (0: all)   // of them the first so many always run, a core's at most the frame's instructions over them
    // Loads <imageDir>/linux_payload.bin into RAM and rootfs.bin as the ROM, and starts the hart.
    virtual bool computeStart(const std::string& imageDir, std::string& err) { (void)imageDir; err = "--compute is for the D3D11 backend"; return false; }
    // The processor's words (rv32_min*.hlsl's State), read back now.
    virtual bool computeState(std::vector<uint32_t>& out) { (void)out; return false; }

    virtual ~RvcBackend() {}
    // Creates the device and compiles both passes. Prints its own progress lines.
    virtual bool init(const BackendOptions& opt, const SLShader& shader, Material& mat, std::string& err) = 0;
    // Loads <dir>/<prefix>.{r,g,b,a}.png as <propBase>_{R,G,B,A}; prefix "none" leaves them black.
    virtual bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                             std::string& err) = 0;
    // Sets both state buffers from width*height RGBA32_UINT texels, row 0 first; null = all zero.
    virtual bool setState(const void* texels, std::string& err) = 0;
    // An answer for the guest: kFetchSide squared RGBA8 texels, row 0 first. The next frame's
    // control pass writes them into RAM; set the _Fetch uniforms for that frame.
    virtual bool deliverHostData(Material& mat, const uint8_t* rgba, std::string& err) = 0;

    // Packets for the guest: kNetMost rows of kNetSlot * 4 RGBA8 texels, a ring place each (four
    // bytes a texel). The next frame's control pass writes the rows _NetRxCount says.
    virtual bool deliverNet(Material& mat, const uint8_t* rgba, std::string& err) { (void)mat; (void)rgba; (void)err; return false; }

    // One emulator frame; also queues a readback of state row 0 tagged `tag`. Pop first if rowFull().
    virtual bool frame(Material& mat, uint64_t tag, bool timeIt) = 0;
    virtual bool rowFull() const = 0;
    virtual size_t rowPending() const = 0;
    // Blocks until the oldest queued row (64 texels) is available.
    virtual bool popRow(std::vector<uint8_t>& out, uint64_t& tag) = 0;
    // The mix drawn in the frame whose row popRow() returned last: kSoundSide squared pairs of
    // floats, row 0 first. False when that frame drew none.
    virtual bool takeSound(std::vector<uint8_t>& out) { (void)out; return false; }
    // The CPU's state texels (64 x 64, 16 bytes each) of that same frame, when stateLog is set.
    virtual bool takeState(std::vector<uint8_t>& out) { (void)out; return false; }
    // Blocking read of the top-left w x h texels of the current state.
    virtual bool readState(UINT w, UINT h, std::vector<uint8_t>& out) = 0;
    // Time on the graphics card of the last frame run with timeIt, once it is known: the CPUTick
    // draw, the Commit draw, and the emulated GPU device's two draws together (0 without one).
    virtual bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) = 0;
    virtual bool readProf(std::vector<uint32_t>& out) { (void)out; return false; }
    // Saves the GPU device's whole colour target as a BMP.
    virtual bool gpuCapture(const std::string& path) { (void)path; return false; }
    // Empty while the device is fine.
    virtual std::string deviceRemoved() = 0;

    virtual bool viewInit(std::string& err) = 0;
    virtual bool viewOpen() const = 0;
    virtual void viewRender(bool present = true) = 0;
    // Columns of the bar under the picture: speed, console, input.
    virtual void viewText(const std::vector<std::vector<std::string>>& columns) = 0;
    virtual void viewTitle(const std::string& title) = 0;
    virtual bool viewCapture(const std::string& path) = 0;
    virtual void viewClose() = 0;
};

// One lane (0..3 = r, g, b, a) of a payload as rvc's importer lays it out: 2048 texels per row,
// the lane's word of each 16-byte texel as one RGBA8 pixel. Comes from <dir>/<prefix>.bin (a raw
// memory image, as our own programs are built) if that exists, else <dir>/<prefix>.<lane>.png.
// A prefix with a slash in it is a path of its own, and <dir> is not used.
inline bool loadPayloadLane(const std::string& dir, const std::string& prefix, int lane, ImageRGBA8& img, std::string& err) {
    namespace fs = std::filesystem;
    std::string bin;
    fs::path base = prefix.find_first_of("/\\") == std::string::npos ? fs::u8path(dir) / prefix : fs::u8path(prefix);
    if (readFileBinary(base.u8string() + ".bin", bin)) {
        size_t texels = (bin.size() + 15) / 16;
        img.width = 2048;
        img.height = (UINT)((texels + 2047) / 2048);
        img.pixels.assign((size_t)img.width * img.height * 4, 0);
        for (size_t t = 0; t < texels; ++t)
            for (size_t k = 0; k < 4; ++k)
                if (t * 16 + lane * 4 + k < bin.size()) img.pixels[t * 4 + k] = (uint8_t)bin[t * 16 + lane * 4 + k];
        return true;
    }
    const char* lanes[4] = {"r", "g", "b", "a"};
    return loadImageRGBA8(base.u8string() + "." + lanes[lane] + ".png", img, err);
}

std::unique_ptr<RvcBackend> makeBackend11();
std::unique_ptr<RvcBackend> makeBackend12();
std::unique_ptr<RvcBackend> makeBackendGvm();
