// D3D12 backend: the same two draws as D3D11, with the shader compiled by DXC to DXIL.

#include "image.h"
#include "rvc_backend.h"
#include "rvc_d3d12.h"
#include "rvc_memview12.h"

#include <deque>
#include <map>

namespace fs = std::filesystem;

namespace {

class Backend12 : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader& shader, Material&, std::string& err) override {
        if (opt.profile || opt.present) {
            err = "--profile and --present are D3D11 only (use rvc_trace12 for D3D12 profilers)";
            return false;
        }
        if (opt.gpu.debug) {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        if (opt.gpu.warp) check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
        else if (opt.gpu.adapterIndex >= 0 && FAILED(factory->EnumAdapters1((UINT)opt.gpu.adapterIndex, &adapter))) {
            err = "adapter index " + std::to_string(opt.gpu.adapterIndex) + " not found";
            return false;
        }
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx_.dev)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(dx_.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx_.queue)), "queue");
        check(dx_.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_.fence)), "fence");
        dx_.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fprintf(stderr, "[harness] D3D12 + DXC (shader model %s)%s\n", opt.dxcSm.c_str(), opt.gpu.warp ? " on WARP" : "");

        Build12 b;
        b.dxc = true;
        b.dxcOpt = opt.dxcOpt;
        b.dxcSm = opt.dxcSm;
        b.dxcDir = opt.dxcDir;
        buildPasses12(dx_, shader, opt.compile, b, passes_);

        const uint8_t black[4] = {0, 0, 0, 0};
        blackTex_ = uploadTexture(dx_, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        for (UINT s = 0; s < kSlots; ++s) {
            check(dx_.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs_[s])), "allocator");
            rowBuf_[s] = readbackBuffer(rowFootprint_, 64, 1);
        }
        check(dx_.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[0].Get(), nullptr, IID_PPV_ARGS(&cl_)), "command list");
        cl_->Close();
        auto cbd = bufferDesc((UINT64)kSlots * 4 * kCbBytes);
        auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        check(dx_.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&cbuf_)), "constant buffer");
        check(cbuf_->Map(0, nullptr, (void**)&cbMapped_), "Map constants");

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 3;
        auto tsd = bufferDesc(3 * sizeof(UINT64));
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(dx_.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&tsHeap_))) ||
            FAILED(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &tsd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&tsBuf_))))
            tsHeap_.Reset();
        return true;
    }

    bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                     std::string& err) override {
        if (prefix == "none") return true;
        const char* lanes[4] = {"r", "g", "b", "a"};
        const char* props[4] = {"_R", "_G", "_B", "_A"};
        for (int i = 0; i < 4; ++i) {
            ImageRGBA8 img;
            std::string path = (fs::u8path(dir) / (prefix + "." + lanes[i] + ".png")).u8string();
            if (!loadPayloadLane(dir, prefix, i, img, err)) return false;
            std::vector<uint8_t> flipped(img.pixels.size());  // Unity layout: row 0 = bottom of the image
            size_t pitch = (size_t)img.width * 4;
            for (UINT y = 0; y < img.height; ++y)
                memcpy(flipped.data() + y * pitch, img.pixels.data() + (img.height - 1 - y) * pitch, pitch);
            std::string name = propBase + props[i];
            textures_[name] = uploadTexture(dx_, img.width, img.height, DXGI_FORMAT_R8G8B8A8_UNORM, 4, flipped.data(),
                                            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            mat.setVector(name + "_TexelSize", 1.0 / img.width, 1.0 / img.height, img.width, img.height);
            if (i == 0)
                fprintf(stderr, "[harness] %s_{R,G,B,A} <- %s (%ux%u)\n", propBase.c_str(), prefix.c_str(), img.width, img.height);
        }
        tablesBuilt_ = false;
        return true;
    }

    bool setState(const void* texels, std::string&) override {
        dx_.flush();
        std::vector<uint8_t> zero;
        if (!texels) {
            zero.assign((size_t)kWidth * kHeight * 16, 0);
            texels = zero.data();
        }
        for (int i = 0; i < 2; ++i)
            state_[i] = uploadTexture(dx_, kWidth, kHeight, kStateFormat, 16, (const uint8_t*)texels,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                      i == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET);
        cur_ = 0;
        tablesBuilt_ = false;
        return true;
    }

    bool frame(Material& mat, uint64_t tag, bool timeIt) override {
        prepare();
        UINT slot = (UINT)(frameCount_ % kSlots);
        dx_.waitFor(slotFence_[slot]);
        check(allocs_[slot]->Reset(), "allocator reset");
        check(cl_->Reset(allocs_[slot].Get(), nullptr), "list reset");
        timeIt = timeIt && tsHeap_ && !tsPending_;

        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
        cl_->SetDescriptorHeaps(1, heaps);
        D3D12_VIEWPORT vp{0, 0, (float)kWidth, (float)kHeight, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)kWidth, (LONG)kHeight};
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        int cur = cur_, dst = 1 - cur_;
        mat.setVector("CustomRenderTextureParameters", 1, 0, 0, 0);
        mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
        mat.setVector("_CustomRenderTextureInfo", kWidth, kHeight, 1, 0);

        auto draw = [&](int p, double cx, double cy, double zw, double zh) {
            Pass12& P = passes_[p];
            mat.setVector("CustomRenderTextureCenters", cx, cy, 0.5, 0);
            mat.setVector("CustomRenderTextureSizesAndRotations", zw, zh, 1, 0);
            mat.fillGlobals(P.vs);
            mat.fillGlobals(P.ps);
            UINT64 base = ((UINT64)slot * 4 + (UINT64)p * 2) * kCbBytes;
            if (P.vs.hasGlobals) memcpy(cbMapped_ + base, P.vs.scratch.data(), P.vs.scratch.size());
            if (P.ps.hasGlobals) memcpy(cbMapped_ + base + kCbBytes, P.ps.scratch.data(), P.ps.scratch.size());
            cl_->SetPipelineState(P.pso.Get());
            cl_->SetGraphicsRootSignature(P.root.Get());
            cl_->SetGraphicsRootConstantBufferView(0, cbuf_->GetGPUVirtualAddress() + base);
            cl_->SetGraphicsRootConstantBufferView(1, cbuf_->GetGPUVirtualAddress() + base + kCbBytes);
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            table.ptr += (UINT64)tableIndex(p, cur) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(2, table);
            cl_->OMSetRenderTargets(1, &rtv_[dst], FALSE, nullptr);
            cl_->DrawInstanced(6, 1, 0, 0);
        };
        auto stamp = [&](UINT i) {
            if (timeIt) cl_->EndQuery(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
        };

        // Zone 1: CPUTick on the 64x64 state area, then copy that area back so `cur` stays complete.
        stamp(0);
        draw(0, 32, 4064, 64, 64);
        stamp(1);
        D3D12_RESOURCE_BARRIER toCopy[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        cl_->ResourceBarrier(2, toCopy);
        D3D12_TEXTURE_COPY_LOCATION cdst{}, csrc{};
        cdst.pResource = state_[cur].Get();
        csrc.pResource = state_[dst].Get();
        D3D12_BOX box{0, 0, 0, 64, 64, 1};
        cl_->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, &box);
        D3D12_RESOURCE_BARRIER fromCopy[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        cl_->ResourceBarrier(2, fromCopy);

        // Zone 2: Commit on the whole texture; row 0 of the result goes to this slot's readback.
        draw(1, 1024, 2048, 2048, 4096);
        stamp(2);
        if (timeIt) cl_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, tsBuf_.Get(), 0);
        auto toRead = transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl_->ResourceBarrier(1, &toRead);
        D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
        rdst.pResource = rowBuf_[slot].Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint = rowFootprint_;
        rsrc.pResource = state_[dst].Get();
        D3D12_BOX rowBox{0, 0, 0, 64, 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, &rowBox);
        D3D12_RESOURCE_BARRIER swapStates[2] = {
            transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        cl_->ResourceBarrier(2, swapStates);
        check(cl_->Close(), "Close");
        ID3D12CommandList* lists[] = {cl_.Get()};
        dx_.queue->ExecuteCommandLists(1, lists);
        slotFence_[slot] = dx_.signal();
        if (timeIt) {
            tsPending_ = true;
            tsFence_ = slotFence_[slot];
        }
        rows_.push_back({slot, slotFence_[slot], tag});
        cur_ = dst;
        ++frameCount_;
        return true;
    }
    bool rowFull() const override { return rows_.size() >= kSlots; }
    size_t rowPending() const override { return rows_.size(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override {
        if (rows_.empty()) return false;
        PendingRow r = rows_.front();
        rows_.pop_front();
        dx_.waitFor(r.fence);
        if (FAILED(dx_.dev->GetDeviceRemovedReason())) return false;
        uint8_t* p = nullptr;
        if (FAILED(rowBuf_[r.slot]->Map(0, nullptr, (void**)&p))) return false;
        out.assign(p + rowFootprint_.Offset, p + rowFootprint_.Offset + 64 * 16);
        rowBuf_[r.slot]->Unmap(0, nullptr);
        tag = r.tag;
        return true;
    }

    bool readState(UINT w, UINT h, std::vector<uint8_t>& out) override {
        prepare();
        dx_.flush();
        out = readRegion(dx_, state_[cur_].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, w, h);
        return true;
    }

    bool gpuTimes(double& tickMs, double& commitMs) override {
        UINT64 freq = 0;
        if (tsPending_ && dx_.fence->GetCompletedValue() >= tsFence_) {
            UINT64* ts = nullptr;
            if (SUCCEEDED(dx_.queue->GetTimestampFrequency(&freq)) && freq && SUCCEEDED(tsBuf_->Map(0, nullptr, (void**)&ts))) {
                tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)freq;
                commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)freq;
                tsBuf_->Unmap(0, nullptr);
            }
            tsPending_ = false;
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        return tickMs_ >= 0;
    }

    std::string deviceRemoved() override {
        HRESULT hr = dx_.dev->GetDeviceRemovedReason();
        return FAILED(hr) ? hrToString(hr) : std::string();
    }

    bool viewInit(std::string& err) override { return view_.init(dx_, kWidth, kHeight, 64, err); }
    bool viewOpen() const override { return view_.open(); }
    void viewRender(bool present) override {
        if (!view_.open()) return;
        prepare();
        dx_.flush();
        view_.render(state_[cur_].Get(), present);
    }
    void viewText(const std::vector<std::string>& lines) override { view_.setText(lines); }
    void viewTitle(const std::string& title) override { view_.setTitle(title); }
    bool viewCapture(const std::string& path) override { return view_.capture(path); }
    void viewClose() override { view_.close(); }

private:
    struct PendingRow {
        UINT slot;
        UINT64 fence;
        uint64_t tag;
    };

    static UINT tableIndex(int pass, int cur) { return (UINT)(pass * 2 + cur) * kTableSize; }

    ComPtr<ID3D12Resource> readbackBuffer(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT w, UINT h) {
        auto td = texDesc(w, h, kStateFormat, D3D12_RESOURCE_FLAG_NONE);
        UINT64 total = 0;
        dx_.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> rb;
        auto bd = bufferDesc(total);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        check(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb)), "readback buffer");
        return rb;
    }

    // State textures (an empty machine unless setState ran) and the views that depend on them
    // and on the payload textures: RTV per state texture, one SRV table per (pass, current).
    void prepare() {
        if (tablesBuilt_) return;
        std::string err;
        if (!state_[0]) setState(nullptr, err);
        dx_.flush();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)), "RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4 * kTableSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)), "SRV heap");
        UINT rtvStep = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStep_ = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = 0; i < 2; ++i) {
            rtv_[i] = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            rtv_[i].ptr += (SIZE_T)i * rtvStep;
            dx_.dev->CreateRenderTargetView(state_[i].Get(), nullptr, rtv_[i]);
        }
        for (int p = 0; p < 2; ++p) {
            for (int cur = 0; cur < 2; ++cur) {
                for (UINT s = 0; s < kTableSize; ++s) {
                    ID3D12Resource* res = blackTex_.Get();
                    for (auto& t : passes_[p].ps.textures) {
                        if (t.slot != s) continue;
                        if (t.name == "_SelfTexture2D") res = state_[cur].Get();
                        else if (textures_.count(t.name)) res = textures_[t.name].Get();
                    }
                    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                    h.ptr += (SIZE_T)(tableIndex(p, cur) + s) * srvStep_;
                    dx_.dev->CreateShaderResourceView(res, nullptr, h);
                }
            }
        }
        tablesBuilt_ = true;
    }

    Dx dx_;
    Pass12 passes_[2];
    ComPtr<ID3D12Resource> state_[2], blackTex_;
    std::map<std::string, ComPtr<ID3D12Resource>> textures_;
    int cur_ = 0;
    bool tablesBuilt_ = false;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, srvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_[2]{};
    UINT srvStep_ = 0;
    ComPtr<ID3D12CommandAllocator> allocs_[kSlots];
    UINT64 slotFence_[kSlots] = {};
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12Resource> cbuf_;
    uint8_t* cbMapped_ = nullptr;
    uint64_t frameCount_ = 0;
    ComPtr<ID3D12Resource> rowBuf_[kSlots];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rowFootprint_{};
    std::deque<PendingRow> rows_;
    ComPtr<ID3D12QueryHeap> tsHeap_;
    ComPtr<ID3D12Resource> tsBuf_;
    bool tsPending_ = false;
    UINT64 tsFence_ = 0;
    double tickMs_ = -1, commitMs_ = -1;

    MemoryView12 view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend12() { return std::make_unique<Backend12>(); }
