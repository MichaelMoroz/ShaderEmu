// D3D11 backend: FXC bytecode on a double-buffered Custom Render Texture, as Unity runs it.

#include "crt.h"
#include "image.h"
#include "memview.h"
#include "readback.h"
#include "rvc_backend.h"

#include <deque>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

class Backend11 : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader& shader, Material& mat, std::string& err) override {
        if (!gpu_.init(opt.gpu, err)) return false;
        fprintf(stderr, "[harness] D3D11 + %s on %s%s, doubles: %s, extended doubles: %s\n", shaderCompilerName(), gpu_.adapterName.c_str(),
                opt.gpu.warp ? " (WARP)" : "", gpu_.doubles ? "yes" : "NO", gpu_.extendedDoubles ? "yes" : "no");
        if (!gpu_.doubles) fprintf(stderr, "[harness] warning: rvc uses doubles (MULH, timer); this device lacks them\n");

        ShaderBuildOptions bo;
        bo.verbose = opt.verbose;
        bo.settings = opt.compile;
        for (auto& d : opt.compile.defines)
            if (d.first == "COMMIT_BANDS") bands_ = true;
        for (auto& d : opt.compile.defines)
            if (d.first == "CORES") cores_ = (float)(std::max)(1, (std::min)(16, atoi(d.second.c_str())));
        for (auto& d : opt.compile.defines)
            if (d.first == "CORE_PITCH") pitch_ = (float)atoi(d.second.c_str());
        tailWidth_ = (float)opt.workerTailWidth;
        smallRows_ = (float)opt.smallRows;
        smallFrom_ = (float)opt.smallFrom;
        tickRows_ = (float)opt.tickRows;
        if (!buildPasses(gpu_, shader, {"CPUTick", "Commit"}, bo, passes_, err)) return false;
        for (auto& d : opt.compile.defines)
            if (d.first == "RAM_DIRECT") csDirect_ = true;
        for (auto& d : opt.compile.defines)
            if (d.first == "RAM_BUFFER") csBuffer_ = true;
        if (getenv("RVC11_COMPUTE") && !buildComputeTick(shader, bo, err)) return false;
        std::string gpuErr;
        if (opt.gpuShader && (!buildPasses(gpu_, *opt.gpuShader, {"GPUDraw", "GPUControl"}, bo, gpuPasses_, gpuErr) ||
                              !createGpuTarget(gpuErr))) {
            fprintf(stderr, "[harness] GPU device disabled: %s\n", gpuErr.c_str());
            gpuPasses_.clear();
        }
        if (opt.soundShader && !gpuPasses_.empty() &&
            (!buildPasses(gpu_, *opt.soundShader, {"SoundMix"}, bo, soundPasses_, gpuErr) || !createSoundTarget(gpuErr))) {
            fprintf(stderr, "[harness] sound card disabled: %s\n", gpuErr.c_str());
            soundPasses_.clear();
        }

        if (opt.present && !createSwapChain(err)) return false;
        if (opt.profile && !createProfile(mat, err)) return false;

        if (!crt_.init(gpu_.device.Get(), kWidth, kHeight, DXGI_FORMAT_R32G32B32A32_UINT, err)) return false;
        crt_.clear(gpu_.ctx.Get());
        return rows_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 1, 3, err, opt.readbackBatch) &&
               control_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kControlTexels, 1, 3, err, opt.readbackBatch);
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

    bool deliverHostData(Material& mat, const uint8_t* rgba, std::string& err) override {
        ImageRGBA8 img;
        img.width = img.height = kFetchSide;
        img.pixels.assign(rgba, rgba + (size_t)kFetchSide * kFetchSide * 4);
        hostData_ = createTextureRGBA8(gpu_.device.Get(), img, /*flipY=*/false, err);
        if (!hostData_) return false;
        mat.setTexture("_HostData", hostData_.Get(), kFetchSide, kFetchSide);
        deliver_ = true;
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
        if (cs_ && mat.getFloat("_Init") == 0) runComputeTick(mat);
        else if (cores_ == 1) crt_.runZone(gpu_, passes_[0], mat, UpdateZone{32, 4096 - tickRows_ / 2, 64, tickRows_, 0});
        else {
            // Core 0's rectangle, then two for each worker: its first 512 texels (64 x 8) and the
            // 16 x 4 under them that holds the rest of its write cache (MC_ROWS in src/types.h).
            // Every zone is drawn before any is copied back, or each would wait for the last.
            std::vector<UpdateZone> zones{UpdateZone{32, 4096 - tickRows_ / 2, 64, tickRows_, 0}};
            for (float c = 1; c < cores_; c += 1) {
                if (getenv("RVC_MC_FULL")) { zones.push_back(UpdateZone{pitch_ * c + 32, 4096 - tickRows_ / 2, 64, tickRows_, 0}); continue; }
                float rows = c >= smallFrom_ ? smallRows_ : 8;   // (a small core's cache fills fewer)
                zones.push_back(UpdateZone{pitch_ * c + 32, 4096 - rows / 2, 64, rows, 0});
                zones.push_back(UpdateZone{pitch_ * c + tailWidth_ / 2, 4096 - rows - 2, tailWidth_, 4, 0});
            }
            for (auto& z : zones) crt_.runZone(gpu_, passes_[0], mat, z, 6, false);
            for (auto& z : zones) crt_.copyZone(gpu_, z);
        }
        if (timeIt) gpu_.ctx->End(tsQuery_[1].Get());
        if (!gpuPasses_.empty()) mat.setTexture("_GpuTarget", gpuSrv_.Get(), kGpuTarget, kGpuTarget);   // Commit copies it back
        // With COMMIT_BANDS the vertex shader draws the state rows and the bands of RAM that changed;
        // the buffer drawn into holds the state of two commits ago, as on D3D12.
        crt_.runZone(gpu_, passes_[1], mat, UpdateZone{1024, 2048, 2048, 4096, 1}, bands_ ? 6 * kCommitQuads : 6);
        if (timeIt) gpu_.ctx->End(tsQuery_[2].Get());
        if (gpuPasses_.size() == 2) {
            gpuDraw(mat);
            bool mixed = soundMix && !soundPasses_.empty();
            if (mixed) soundDraw(mat, tag);   // before the control zone moves the voices on
            const float* z = kGpuControlZone;
            crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{z[0], z[1], z[2], z[3], 1});
            if (deliver_) {
                const float* f = kFetchZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{f[0], f[1], f[2], f[3], 1});
                deliver_ = false;
            }
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
        control_.request(gpu_.ctx.Get(), crt_.current(), 0, kControlRow, tag);
        // hand the frame to the GPU now, as ExecuteCommandLists does on D3D12: without this it
        // starts when the readback's Map asks for it, after the host's own work for the frame
        if (!getenv("RVC11_NO_FLUSH")) gpu_.ctx->Flush();
        return true;
    }
    bool rowFull() const override { return rows_.full(); }
    size_t rowPending() const override { return rows_.pending(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override {
        std::vector<uint8_t> control;
        uint64_t controlTag;
        if (!rows_.pop(gpu_.ctx.Get(), out, tag) || !control_.pop(gpu_.ctx.Get(), control, controlTag)) return false;
        out.insert(out.end(), control.begin(), control.end());
        lastSound_.clear();
        if (!soundTags_.empty() && soundTags_.front() == tag) {
            soundTags_.pop_front();
            if (!sound_.pop(gpu_.ctx.Get(), lastSound_, controlTag)) lastSound_.clear();
        }
        return true;
    }
    bool takeSound(std::vector<uint8_t>& out) override {
        if (lastSound_.empty()) return false;
        out.swap(lastSound_);
        lastSound_.clear();
        return true;
    }

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
        // depth: tested and written, tested only, unused; blending: none, alpha, additive, multiply
        for (int k = 0; k < 3 && SUCCEEDED(hr); ++k) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = k < 2;
            dd.DepthWriteMask = k == 0 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            hr = gpu_.device->CreateDepthStencilState(&dd, &gpuDepthState_[k]);
        }
        for (int k = 0; k < 4 && SUCCEEDED(hr); ++k) {
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = k != 0;
            rt.SrcBlend = rt.SrcBlendAlpha = k == 3 ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = rt.DestBlendAlpha = k == 1 ? D3D11_BLEND_INV_SRC_ALPHA : k == 2 ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
            if (k == 3) rt.SrcBlendAlpha = D3D11_BLEND_ZERO, rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            hr = gpu_.device->CreateBlendState(&bd, &gpuBlend_[k]);
        }
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
    void gpuDraw(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = gpuPasses_[0];
        ctx->OMSetRenderTargets(1, gpuRtv_.GetAddressOf(), gpuDsv_.Get());
        ctx->ClearDepthStencilView(gpuDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, (float)kGpuTarget, (float)kGpuTarget, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11ShaderResourceView* state = crt_.currentSRV();
        // one draw per pass in use, in order: each has its own blending and depth use
        for (int p = 0; p < 8; ++p) {
            if (!((gpuPasses >> p) & 1)) continue;
            mat.setInt("_GpuPass", p);
            mat.bind(ctx, pass, gpu_);
            // the state texture; the ROM's textures are the material's and already bound
            for (auto& t : pass.vsLayout.textures) ctx->VSSetShaderResources(t.slot, 1, &state);
            for (auto& t : pass.psLayout.textures)
                if (t.name == "_State") ctx->PSSetShaderResources(t.slot, 1, &state);
            ctx->OMSetBlendState(gpuBlend_[p & 3].Get(), nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(gpuDepthState_[p == 0 ? 0 : p < 4 ? 1 : 2].Get(), 0);
            ctx->Draw(kGpuTriangles * 3, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        for (auto& t : pass.vsLayout.textures) ctx->VSSetShaderResources(t.slot, 1, &none);
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // The sound card's mix target: two floats a sample, read back whole.
    bool createSoundTarget(std::string& err) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kSoundSide;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, &soundTex_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(soundTex_.Get(), nullptr, &soundRtv_);
        if (FAILED(hr)) {
            err = "sound target: " + hrToString(hr);
            return false;
        }
        return sound_.init(gpu_.device.Get(), td.Format, 8, kSoundSide, kSoundSide, 3, err);
    }

    // Draws the mix from the committed state and queues its readback.
    void soundDraw(Material& mat, uint64_t tag) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = soundPasses_[0];
        ctx->OMSetRenderTargets(1, soundRtv_.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)kSoundSide, (float)kSoundSide, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mat.bind(ctx, pass, gpu_);
        ID3D11ShaderResourceView* state = crt_.currentSRV();
        for (auto& t : pass.psLayout.textures)
            if (t.name == "_State") ctx->PSSetShaderResources(t.slot, 1, &state);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        sound_.request(ctx, soundTex_.Get(), 0, 0, tag);
        soundTags_.push_back(tag);
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
    std::vector<GpuPass> passes_, gpuPasses_, soundPasses_;
    ComPtr<ID3D11Texture2D> soundTex_;
    ComPtr<ID3D11RenderTargetView> soundRtv_;
    RegionReadback sound_;
    std::deque<uint64_t> soundTags_;   // frames whose mix is still to be read back
    std::vector<uint8_t> lastSound_;
    ComPtr<ID3D11Texture2D> gpuColor_;
    ComPtr<ID3D11RenderTargetView> gpuRtv_;
    ComPtr<ID3D11ShaderResourceView> gpuSrv_;
    ComPtr<ID3D11DepthStencilView> gpuDsv_;
    ComPtr<ID3D11DepthStencilState> gpuDepthState_[3];
    ComPtr<ID3D11BlendState> gpuBlend_[4];
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep_;
    CustomRenderTexture crt_;
    RegionReadback rows_, control_;

    // RVC11_COMPUTE=1 (experiments/rvc_compute): the tick as one thread of a compute shader, the
    // pass's entry tick_cs. It writes the CPU zone's texels into a 64 x 64 texture of its own,
    // which holds the zone before the dispatch and is copied back after it.
    ComPtr<ID3D11ComputeShader> cs_;
    bool csDirect_ = false, csBuffer_ = false, ramImported_ = false;
    static const UINT kRamTexels = 2048 * (4096 - 64);
    ComPtr<ID3D11ComputeShader> csImport_;
    StageLayout csImportLayout_;
    ComPtr<ID3D11Buffer> ramBuf_;
    ComPtr<ID3D11UnorderedAccessView> ramUav_;
    StageLayout csLayout_;
    ComPtr<ID3D11Texture2D> tickTex_;
    ComPtr<ID3D11UnorderedAccessView> tickUav_;
    bool buildComputeTick(const SLShader& shader, const ShaderBuildOptions& bo, std::string& err) {
        const SLPass* p = shader.findPass("CPUTick");
        if (!p) { err = "no CPUTick pass"; return false; }
        std::string rootDir = std::filesystem::u8path(shader.path).parent_path().u8string();
        Defines extra = {{"SHADER_STAGE_COMPUTE", "1"}};
        StageResult r = compileStage(p->code, shader.path, rootDir, "tick_cs", "cs_5_0", extra, bo.settings);
        if (!r.ok) { err = "tick_cs: " + r.log; return false; }
        fprintf(stderr, "[harness] compute tick: %s in %.1fs (%zu bytes)\n", r.fromCache ? "cache hit" : "compiled", r.seconds,
                (size_t)r.bytecode->GetBufferSize());
        HRESULT hr = gpu_.device->CreateComputeShader(r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize(), nullptr, &cs_);
        if (FAILED(hr)) { err = "CreateComputeShader: " + hrToString(hr); return false; }
        if (!csLayout_.reflect(gpu_.device.Get(), r.bytecode.Get(), err)) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        hr = gpu_.device->CreateTexture2D(&td, nullptr, &tickTex_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(tickTex_.Get(), nullptr, &tickUav_);
        if (FAILED(hr)) { err = "compute tick target: " + hrToString(hr); return false; }
        if (csBuffer_) {
            // RAM_BUFFER: RAM as a raw buffer (u1), filled from the state texture by ram_import_cs
            // before the first compute tick
            StageResult ri = compileStage(p->code, shader.path, rootDir, "ram_import_cs", "cs_5_0", extra, bo.settings);
            if (!ri.ok) { err = "ram_import_cs: " + ri.log; return false; }
            hr = gpu_.device->CreateComputeShader(ri.bytecode->GetBufferPointer(), ri.bytecode->GetBufferSize(), nullptr, &csImport_);
            if (FAILED(hr)) { err = "CreateComputeShader (import): " + hrToString(hr); return false; }
            if (!csImportLayout_.reflect(gpu_.device.Get(), ri.bytecode.Get(), err)) return false;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kRamTexels * 16;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            hr = gpu_.device->CreateBuffer(&bd, nullptr, &ramBuf_);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kRamTexels * 4;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(ramBuf_.Get(), &ud, &ramUav_);
            if (FAILED(hr)) { err = "RAM buffer: " + hrToString(hr); return false; }
        }
        return true;
    }
    void runComputeTick(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        mat.setVector("_CustomRenderTextureInfo", crt_.width(), crt_.height(), 1, 0);
        mat.setTexture("_SelfTexture2D", crt_.currentSRV(), crt_.width(), crt_.height());
        D3D11_BOX box{0, 0, 0, 64, 64, 1};
        // RAM_DIRECT: the shader has the state texture itself, to read and to write
        if (!csDirect_) ctx->CopySubresourceRegion(tickTex_.Get(), 0, 0, 0, 0, crt_.current(), 0, &box);
        ctx->CSSetShader(cs_.Get(), nullptr, 0);
        mat.bindCompute(ctx, csLayout_, gpu_);
        ID3D11UnorderedAccessView* uavs[2] = {csDirect_ ? crt_.currentUAV() : tickUav_.Get(), ramUav_.Get()};
        if (csBuffer_ && !ramImported_) {
            // every RAM texel of the state texture into the buffer: 65,536 a row of thread groups
            ctx->CSSetShader(csImport_.Get(), nullptr, 0);
            mat.bindCompute(ctx, csImportLayout_, gpu_);
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            ctx->Dispatch(1024, (kRamTexels + 65535) / 65536, 1);
            ctx->CSSetShader(cs_.Get(), nullptr, 0);
            mat.bindCompute(ctx, csLayout_, gpu_);
            ramImported_ = true;
        }
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* none[2] = {};
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        ID3D11ShaderResourceView* nulls[16] = {};
        ctx->CSSetShaderResources(0, 16, nulls);
        if (!csDirect_) ctx->CopySubresourceRegion(crt_.current(), 0, 0, 0, 0, tickTex_.Get(), 0, &box);
    }
    ComPtr<ID3D11ShaderResourceView> hostData_;
    bool deliver_ = false;   // the fetch zone is drawn in the next frame
    ComPtr<IDXGISwapChain> swapChain_;
    HWND presentWnd_ = nullptr;
    ComPtr<ID3D11Buffer> profBuf_, profStaging_;
    ComPtr<ID3D11UnorderedAccessView> profUav_;
    ComPtr<ID3D11Query> tsDisjoint_, tsQuery_[4];
    bool tsPending_ = false;
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;
    bool bands_ = false;   // the commit draws only the bands of RAM that changed
    float tickRows_ = 64;  // BackendOptions::tickRows
    float tailWidth_ = 16; // BackendOptions::workerTailWidth
    float smallRows_ = 8, smallFrom_ = 1000;
    float pitch_ = 256;    // CORE_PITCH: how far apart the blocks are
    float cores_ = 1;      // CORES: the tick's zone is that many 64 x 64 blocks side by side (docs/multicore.md)
    MemoryView view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend11() { return std::make_unique<Backend11>(); }
