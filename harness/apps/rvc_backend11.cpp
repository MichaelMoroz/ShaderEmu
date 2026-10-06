// D3D11 backend: FXC bytecode on a double-buffered Custom Render Texture, as Unity runs it.

#include "crt.h"
#include "image.h"
#include "memview.h"
#include "readback.h"
#include "rvc_backend.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace {

class Backend11 : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader& shader, Material& mat, std::string& err) override {
        if (!gpu_.init(opt.gpu, err)) return false;
        fprintf(stderr, "[harness] D3D11 + FXC on %s%s, doubles: %s, extended doubles: %s\n", gpu_.adapterName.c_str(),
                opt.gpu.warp ? " (WARP)" : "", gpu_.doubles ? "yes" : "NO", gpu_.extendedDoubles ? "yes" : "no");
        if (!gpu_.doubles) fprintf(stderr, "[harness] warning: rvc uses doubles (MULH, timer); this device lacks them\n");

        ShaderBuildOptions bo;
        bo.verbose = opt.verbose;
        bo.settings = opt.compile;
        if (!buildPasses(gpu_, shader, {"CPUTick", "Commit"}, bo, passes_, err)) return false;

        if (opt.present && !createSwapChain(err)) return false;
        if (opt.profile && !createProfile(mat, err)) return false;

        if (!crt_.init(gpu_.device.Get(), kWidth, kHeight, DXGI_FORMAT_R32G32B32A32_UINT, err)) return false;
        crt_.clear(gpu_.ctx.Get());
        return rows_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 1, 3, err);
    }

    bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                     std::string& err) override {
        if (prefix == "none") return true;
        const char* lanes[4] = {"r", "g", "b", "a"};
        const char* props[4] = {"_R", "_G", "_B", "_A"};
        for (int i = 0; i < 4; ++i) {
            std::string path = (fs::u8path(dir) / (prefix + "." + lanes[i] + ".png")).u8string();
            ImageRGBA8 img;
            if (!loadPayloadLane(dir, prefix, i, img, err)) return false;
            auto srv = createTextureRGBA8(gpu_.device.Get(), img, /*flipY=*/true, err);
            if (!srv) {
                err = path + ": " + err;
                return false;
            }
            mat.setTexture(propBase + props[i], srv.Get(), img.width, img.height);
            keep_.push_back(srv);
            if (i == 0)
                fprintf(stderr, "[harness] %s_{R,G,B,A} <- %s (%ux%u)\n", propBase.c_str(), prefix.c_str(), img.width, img.height);
        }
        return true;
    }

    bool setState(const void* texels, std::string&) override {
        if (texels) crt_.load(gpu_.ctx.Get(), texels, kWidth * 16);
        else crt_.clear(gpu_.ctx.Get());
        return true;
    }

    bool frame(Material& mat, uint64_t tag, bool timeIt) override {
        timeIt = timeIt && ensureQueries() && !tsPending_;
        if (timeIt) {
            gpu_.ctx->Begin(tsDisjoint_.Get());
            gpu_.ctx->End(tsQuery_[0].Get());
        }
        crt_.runZone(gpu_, passes_[0], mat, UpdateZone{32, 4064, 64, 64, 0});
        if (timeIt) gpu_.ctx->End(tsQuery_[1].Get());
        crt_.runZone(gpu_, passes_[1], mat, UpdateZone{1024, 2048, 2048, 4096, 1});
        if (timeIt) {
            gpu_.ctx->End(tsQuery_[2].Get());
            gpu_.ctx->End(tsDisjoint_.Get());
            tsPending_ = true;
        }
        if (swapChain_) {
            swapChain_->Present(0, 0);
            MSG msg;
            while (PeekMessageW(&msg, presentWnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        rows_.request(gpu_.ctx.Get(), crt_.current(), 0, 0, tag);
        return true;
    }
    bool rowFull() const override { return rows_.full(); }
    size_t rowPending() const override { return rows_.pending(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override { return rows_.pop(gpu_.ctx.Get(), out, tag); }

    bool readState(UINT w, UINT h, std::vector<uint8_t>& out) override {
        RegionReadback rb;
        std::string err;
        uint64_t tag;
        if (!rb.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, w, h, 1, err)) return false;
        rb.request(gpu_.ctx.Get(), crt_.current(), 0, 0, 0);
        return rb.pop(gpu_.ctx.Get(), out, tag);
    }

    bool gpuTimes(double& tickMs, double& commitMs) override {
        if (tsPending_) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ts[3] = {};
            const UINT fl = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (gpu_.ctx->GetData(tsDisjoint_.Get(), &dj, sizeof dj, fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[2].Get(), &ts[2], sizeof ts[2], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[1].Get(), &ts[1], sizeof ts[1], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[0].Get(), &ts[0], sizeof ts[0], fl) == S_OK) {
                if (!dj.Disjoint && dj.Frequency) {
                    tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
                    commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)dj.Frequency;
                }
                tsPending_ = false;
            }
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        return tickMs_ >= 0;
    }

    bool readProf(std::vector<uint32_t>& out) override {
        if (!profBuf_) return false;
        out.assign(kProfCount, 0);
        gpu_.ctx->CopyResource(profStaging_.Get(), profBuf_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.ctx->Map(profStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        memcpy(out.data(), m.pData, kProfCount * 4);
        gpu_.ctx->Unmap(profStaging_.Get(), 0);
        return true;
    }

    std::string deviceRemoved() override { return gpu_.deviceRemovedReason(); }

    bool viewInit(std::string& err) override { return view_.init(gpu_.device.Get(), kWidth, kHeight, 64, err); }
    bool viewOpen() const override { return view_.open(); }
    void viewRender(bool present) override { view_.render(gpu_.ctx.Get(), crt_.currentSRV(), present); }
    void viewText(const std::vector<std::string>& lines) override { view_.setText(gpu_.ctx.Get(), lines); }
    void viewTitle(const std::string& title) override { view_.setTitle(title); }
    bool viewCapture(const std::string& path) override { return view_.capture(gpu_.ctx.Get(), path); }
    void viewClose() override { view_.close(); }

private:
    // Hidden swapchain, only to give external profilers a Present per frame.
    bool createSwapChain(std::string& err) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"rvc_harness_present";
        RegisterClassW(&wc);
        presentWnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"rvc_harness", WS_POPUP,
                                      0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);  // never shown
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = presentWnd_;
        sd.Windowed = TRUE;
        HRESULT hr = presentWnd_ ? gpu_.device.As(&dxgiDev) : E_FAIL;
        if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(gpu_.device.Get(), &sd, &swapChain_);
        if (FAILED(hr)) err = "creating the hidden swapchain failed: " + hrToString(hr);
        return SUCCEEDED(hr);
    }

    // Profiling counters: a uint buffer the tick pass increments through a UAV.
    bool createProfile(Material& mat, std::string& err) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kProfCount * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        HRESULT hr = gpu_.device->CreateBuffer(&bd, nullptr, &profBuf_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(profBuf_.Get(), nullptr, &profUav_);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateBuffer(&bd, nullptr, &profStaging_);
        if (FAILED(hr)) {
            err = "creating the profile buffer failed: " + hrToString(hr);
            return false;
        }
        const UINT zero[4] = {0, 0, 0, 0};
        gpu_.ctx->ClearUnorderedAccessViewUint(profUav_.Get(), zero);
        mat.setUav("_Prof", profUav_.Get());
        return true;
    }

    bool ensureQueries() {
        if (!tsDisjoint_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            gpu_.device->CreateQuery(&qd, &tsDisjoint_);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& q : tsQuery_) gpu_.device->CreateQuery(&qd, &q);
        }
        return tsDisjoint_ && tsQuery_[2];
    }

    Gpu gpu_;
    std::vector<GpuPass> passes_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep_;
    CustomRenderTexture crt_;
    RegionReadback rows_;
    ComPtr<IDXGISwapChain> swapChain_;
    HWND presentWnd_ = nullptr;
    ComPtr<ID3D11Buffer> profBuf_, profStaging_;
    ComPtr<ID3D11UnorderedAccessView> profUav_;
    ComPtr<ID3D11Query> tsDisjoint_, tsQuery_[3];
    bool tsPending_ = false;
    double tickMs_ = -1, commitMs_ = -1;
    MemoryView view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend11() { return std::make_unique<Backend11>(); }
