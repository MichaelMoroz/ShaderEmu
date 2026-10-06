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
};

// The GPU device (docs/gpu.md): a mesh of kGpuTriangles triangles drawn into a square colour and
// depth target of kGpuTarget pixels, then one small update zone on the state texture (Unity
// pixel space, y up) holding its control words at RAM 0x87000010.
const UINT kGpuTriangles = 65536;
const UINT kGpuTarget = 2048;
const float kGpuControlZone[4] = {8, 447.5f, 16, 1};
// The machine's control words (display, GPU, input: RAM from 0x87000000) as a row of the
// state texture. popRow() returns them after the 64 texels of row 0.
const unsigned kControlRow = 64 + 0x700000 / 2048, kControlTexels = 16;
// The display's RAM framebuffer (RAM 0x87000000, 128 texture rows), for writing the picture back.

class RvcBackend {
public:
    static const UINT kWidth = 2048, kHeight = 4096;   // rvc's vm_state_crt.asset
    static const UINT kProfCount = 256;

    virtual ~RvcBackend() {}
    // Creates the device and compiles both passes. Prints its own progress lines.
    virtual bool init(const BackendOptions& opt, const SLShader& shader, Material& mat, std::string& err) = 0;
    // Loads <dir>/<prefix>.{r,g,b,a}.png as <propBase>_{R,G,B,A}; prefix "none" leaves them black.
    virtual bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                             std::string& err) = 0;
    // Sets both state buffers from width*height RGBA32_UINT texels, row 0 first; null = all zero.
    virtual bool setState(const void* texels, std::string& err) = 0;

    // One emulator frame; also queues a readback of state row 0 tagged `tag`. Pop first if rowFull().
    virtual bool frame(Material& mat, uint64_t tag, bool timeIt) = 0;
    virtual bool rowFull() const = 0;
    virtual size_t rowPending() const = 0;
    // Blocks until the oldest queued row (64 texels) is available.
    virtual bool popRow(std::vector<uint8_t>& out, uint64_t& tag) = 0;
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
