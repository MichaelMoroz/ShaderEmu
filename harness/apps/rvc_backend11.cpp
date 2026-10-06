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
        std::string gpuErr;
        if (opt.gpuShader && (!buildPasses(gpu_, *opt.gpuShader, {"GPUDraw", "GPUControl"}, bo, gpuPasses_, gpuErr) ||
                              !createGpuTarget(gpuErr))) {
            fprintf(stderr, "[harness] GPU device disabled: %s\n", gpuErr.c_str());
            gpuPasses_.clear();
        }

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
        if (timeIt) gpu_.ctx->End(tsQuery_[2].Get());
        if (gpuPasses_.size() == 2) {
            gpuDraw();
            const float* z = kGpuControlZone;
            crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{z[0], z[1], z[2], z[3], 1});
        }
        if (timeIt) {
            gpu_.ctx->End(tsQuery_[3].Get());
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

    bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) override {
        if (tsPending_) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ts[4] = {};
            const UINT fl = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (gpu_.ctx->GetData(tsDisjoint_.Get(), &dj, sizeof dj, fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[3].Get(), &ts[3], sizeof ts[3], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[2].Get(), &ts[2], sizeof ts[2], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[1].Get(), &ts[1], sizeof ts[1], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[0].Get(), &ts[0], sizeof ts[0], fl) == S_OK) {
                if (!dj.Disjoint && dj.Frequency) {
                    tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
                    commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)dj.Frequency;
                    deviceMs_ = (double)(ts[3] - ts[2]) * 1000.0 / (double)dj.Frequency;
                }
                tsPending_ = false;
            }
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        deviceMs = deviceMs_;
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
    void viewRender(bool present) override { view_.render(gpu_.ctx.Get(), crt_.currentSRV(), present, gpuSrv_.Get()); }
    bool gpuCapture(const std::string& path) override {
        if (!gpuColor_) return false;
        D3D11_TEXTURE2D_DESC td{};
        gpuColor_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.device->CreateTexture2D(&td, nullptr, &staging))) return false;
        gpu_.ctx->CopyResource(staging.Get(), gpuColor_.Get());
        if (FAILED(gpu_.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        bool ok = memoryViewWriteBmp(path, td.Width, td.Height, (const uint8_t*)m.pData, m.RowPitch);
        gpu_.ctx->Unmap(staging.Get(), 0);
        return ok;
    }
    void viewText(const std::vector<std::vector<std::string>>& columns) override { view_.setText(gpu_.ctx.Get(), columns); }
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

    // The GPU device's colour and depth target, with the fixed depth test of its draw pass.
    bool createGpuTarget(std::string& err) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kGpuTarget;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, &gpuColor_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(gpuColor_.Get(), nullptr, &gpuRtv_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateShaderResourceView(gpuColor_.Get(), nullptr, &gpuSrv_);
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateTexture2D(&td, nullptr, &depth);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateDepthStencilView(depth.Get(), nullptr, &gpuDsv_);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateDepthStencilState(&dd, &gpuDepthState_);
        if (FAILED(hr)) {
            err = "GPU target: " + hrToString(hr);
            return false;
        }
        const float black[4] = {0, 0, 0, 1};
        gpu_.ctx->ClearRenderTargetView(gpuRtv_.Get(), black);
        return true;
    }

    // Draws the GPU device's mesh. Depth starts fresh every frame; colour is kept, so the
    // picture stays until the guest submits another list.
    void gpuDraw() {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = gpuPasses_[0];
        ctx->OMSetRenderTargets(1, gpuRtv_.GetAddressOf(), gpuDsv_.Get());
        ctx->ClearDepthStencilView(gpuDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, (float)kGpuTarget, (float)kGpuTarget, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpuDepthState_.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(pass.vs.Get(), nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(pass.ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* state = crt_.currentSRV();
        for (auto& t : pass.vsLayout.textures) ctx->VSSetShaderResources(t.slot, 1, &state);
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &state);
        ctx->Draw(kGpuTriangles * 3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        for (auto& t : pass.vsLayout.textures) ctx->VSSetShaderResources(t.slot, 1, &none);
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    bool ensureQueries() {
        if (!tsDisjoint_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            gpu_.device->CreateQuery(&qd, &tsDisjoint_);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& q : tsQuery_) gpu_.device->CreateQuery(&qd, &q);
        }
        return tsDisjoint_ && tsQuery_[3];
    }

    Gpu gpu_;
    std::vector<GpuPass> passes_, gpuPasses_;
    ComPtr<ID3D11Texture2D> gpuColor_;
    ComPtr<ID3D11RenderTargetView> gpuRtv_;
    ComPtr<ID3D11ShaderResourceView> gpuSrv_;
    ComPtr<ID3D11DepthStencilView> gpuDsv_;
    ComPtr<ID3D11DepthStencilState> gpuDepthState_;
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep_;
    CustomRenderTexture crt_;
    RegionReadback rows_;
    ComPtr<IDXGISwapChain> swapChain_;
    HWND presentWnd_ = nullptr;
    ComPtr<ID3D11Buffer> profBuf_, profStaging_;
    ComPtr<ID3D11UnorderedAccessView> profUav_;
    ComPtr<ID3D11Query> tsDisjoint_, tsQuery_[4];
    bool tsPending_ = false;
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;
    MemoryView view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend11() { return std::make_unique<Backend11>(); }
