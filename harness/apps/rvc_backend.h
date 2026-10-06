// What rvc_harness needs from a graphics API: run one emulator frame (CPUTick, then Commit),
// read the state texture back, and show the memory view. Two implementations: D3D11 with FXC
// bytecode (what VRChat runs) and D3D12 with DXC-compiled DXIL (faster, and seconds to compile).
#pragma once

#include "common.h"
#include "gpu.h"
#include "material.h"
#include "shaderlab.h"

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
};

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
    // GPU time of the two draws of the last frame run with timeIt, once it is known.
    virtual bool gpuTimes(double& tickMs, double& commitMs) = 0;
    virtual bool readProf(std::vector<uint32_t>& out) { (void)out; return false; }
    // Empty while the device is fine.
    virtual std::string deviceRemoved() = 0;

    virtual bool viewInit(std::string& err) = 0;
    virtual bool viewOpen() const = 0;
    virtual void viewRender(bool present = true) = 0;
    virtual void viewText(const std::vector<std::string>& lines) = 0;
    virtual void viewTitle(const std::string& title) = 0;
    virtual bool viewCapture(const std::string& path) = 0;
    virtual void viewClose() = 0;
};

std::unique_ptr<RvcBackend> makeBackend11();
std::unique_ptr<RvcBackend> makeBackend12();
