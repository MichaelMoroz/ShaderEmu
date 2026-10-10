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
};

// The GPU device (docs/gpu.md): a mesh of kGpuTriangles triangles drawn into a square colour and
// depth target of kGpuTarget pixels, then one small update zone on the state texture (Unity
// pixel space, y up) holding the control words: RAM 0x87000000 to 0x87000fff.
const UINT kGpuTriangles = 65536;
const UINT kGpuTarget = 2048;
const float kGpuControlZone[4] = {128, 447.5f, 256, 1};
// The sound card's mix target (docs/sound.md): kSoundSide squared samples, two floats each.
const UINT kSoundSide = 128;
// The rows of RAM an answer to the guest's request is written to (docs/fetch.md), as a zone.
const float kFetchZone[4] = {1024, 4096 - (64 + 0x76c000 / 2048) - 4, 2048, 8};
const UINT kFetchSide = 256;   // the answer's texture: one word a texel, 256 KB
// The network device (docs/lan.md): a row of RAM at 0x876b8000. Its first kNetSlot texels are the
// packet the guest sends, which popRow() returns after the control words; from kNetRing on is
// the ring of kNetSlots packets for the guest, written as a zone when deliverNet() was called.
const unsigned kNetRow = 64 + 0x76b800 / 2048, kNetSlot = 40, kNetRing = 64, kNetSlots = 8, kNetMost = 4;
// What cores 16 and up ran (MC_STATS_MORE in src/gpu.h): 13 texels after the window, which the
// control pass writes every frame. kNetRead texels of the row are read back: the window and those.
const unsigned kCoreStatsAt = 40, kNetRead = 64;
const float kCoreStatsZone[4] = {kCoreStatsAt + 6.5f, 4096 - (64 + 0x76b800 / 2048) - 0.5f, 13, 1};
const float kNetZone[4] = {kNetRing + kNetSlot * kNetSlots / 2.0f, 4096 - kNetRow - 0.5f, (float)(kNetSlot * kNetSlots), 1};
// Quads the Commit pass draws when its vertex shader chooses them: the state rows and 32 bands of RAM.
const unsigned kCommitQuads = 33;
// The machine's control words (display, GPU, input, sound: RAM from 0x87000000) as a row of the
// state texture. popRow() returns them after the 64 texels of row 0.
const unsigned kControlRow = 64 + 0x700000 / 2048, kControlTexels = 256;
const int kCores = 64;   // core 0 and 63 workers: the only machine there is (docs/multicore.md)
// The display's RAM framebuffer (RAM 0x87000000, 128 texture rows), for writing the picture back.

class RvcBackend {
public:
    // The GPU device's passes to draw in the next frame, one bit each (docs/gpu.md); pass 0 always.
    uint32_t gpuPasses = 1;
    // Draw the sound card's mix in the next frame and read it back with that frame's row.
    bool soundMix = false;
    static const UINT kWidth = 2048, kHeight = 4096;   // rvc's vm_state_crt.asset
    static const UINT kProfCount = 256;

    // --cpu (docs/cpu-harness.md): the instructions run on the processor. The tick pass is not
    // drawn; RAM comes from the host before a frame and the devices' part of it goes back after.
    bool cpuMode = false;
    virtual bool uploadRows(UINT row, UINT rows, const void* texels) { (void)row; (void)rows; (void)texels; return false; }
    virtual bool uploadTexel(UINT x, UINT y, const uint32_t* words) { (void)x; (void)y; (void)words; return false; }
    virtual bool readRows(UINT row, UINT rows, std::vector<uint8_t>& out) { (void)row; (void)rows; (void)out; return false; }

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
