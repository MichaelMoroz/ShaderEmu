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
        if (opt.gpuShader) {
            buildPasses12(dx_, *opt.gpuShader, opt.compile, b, passes_ + 2, "GPUControl", "", 1);
            passCount_ = 3;
            buildGpuDraw(*opt.gpuShader, opt.compile, b);
            textures_["_GpuTarget"] = gpuColor_;
        }

        const uint8_t black[4] = {0, 0, 0, 0};
        blackTex_ = uploadTexture(dx_, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        for (UINT s = 0; s < kSlots; ++s) {
            check(dx_.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs_[s])), "allocator");
            rowBuf_[s] = readbackBuffer(rowFootprint_, 64, 2);   // row 0, then the control texels
        }
        check(dx_.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs_[0].Get(), nullptr, IID_PPV_ARGS(&cl_)), "command list");
        cl_->Close();
        auto cbd = bufferDesc((UINT64)kSlots * 16 * kCbBytes);
        auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        check(dx_.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&cbuf_)), "constant buffer");
        check(cbuf_->Map(0, nullptr, (void**)&cbMapped_), "Map constants");

        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 4;
        auto tsd = bufferDesc(4 * sizeof(UINT64));
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
            cl_->RSSetScissorRects(1, &scissor);
            Pass12& P = passes_[p];
            mat.setVector("CustomRenderTextureCenters", cx, cy, 0.5, 0);
            mat.setVector("CustomRenderTextureSizesAndRotations", zw, zh, 1, 0);
            mat.fillGlobals(P.vs);
            mat.fillGlobals(P.ps);
            UINT64 base = ((UINT64)slot * 16 + (UINT64)p * 2) * kCbBytes;   // 16 per frame: two a pass, then the GPU draws
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
            if (P.vsSelf < 0) {
                cl_->DrawInstanced(6, 1, 0, 0);
                return;
            }
            // the vertex shader picks the quads from the state: it needs the non-pixel state too
            D3D12_GPU_DESCRIPTOR_HANDLE self = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            self.ptr += (UINT64)(8 * kTableSize + cur) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(3, self);
            const D3D12_RESOURCE_STATES both =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            auto toBoth = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, both);
            cl_->ResourceBarrier(1, &toBoth);
            cl_->DrawInstanced(6 * kCommitQuads, 1, 0, 0);
            auto toPixel = transition(state_[cur].Get(), both, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl_->ResourceBarrier(1, &toPixel);
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
        // Commit reads the GPU's picture, to copy it into RAM when the guest asked.
        const D3D12_RESOURCE_STATES gpuRt = D3D12_RESOURCE_STATE_RENDER_TARGET, gpuRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (passCount_ == 3) {
            auto toRead = transition(gpuColor_.Get(), gpuRt, gpuRead);
            cl_->ResourceBarrier(1, &toRead);
        }
        draw(1, 1024, 2048, 2048, 4096);
        if (passCount_ == 3) {
            auto toDraw = transition(gpuColor_.Get(), gpuRead, gpuRt);
            cl_->ResourceBarrier(1, &toDraw);
        }
        stamp(2);
        // The GPU device's zone reads the committed state and writes part of it: it renders
        // into the other buffer and its rows are copied back, as the CPUTick zone does.
        if (passCount_ == 3) {
            D3D12_RESOURCE_BARRIER swapIn[2] = {
                transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
            cl_->ResourceBarrier(2, swapIn);
            std::swap(cur, dst);   // `cur` is the committed state from here on
            // The mesh, into the GPU's own target. Depth starts fresh every frame; colour is
            // kept, so the picture stays until the guest submits another list.
            D3D12_CPU_DESCRIPTOR_HANDLE grtv = gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE gdsv = gpuDsvHeap_->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT gvp{0, 0, (float)kGpuTarget, (float)kGpuTarget, 0, 1};
            D3D12_RECT gsc{0, 0, (LONG)kGpuTarget, (LONG)kGpuTarget};
            cl_->RSSetViewports(1, &gvp);
            cl_->RSSetScissorRects(1, &gsc);
            cl_->OMSetRenderTargets(1, &grtv, FALSE, &gdsv);
            cl_->ClearDepthStencilView(gdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cl_->SetGraphicsRootSignature(gpuRoot_.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE gtable = srvHeap_->GetGPUDescriptorHandleForHeapStart();
            gtable.ptr += (UINT64)(8 * kTableSize + 2 + cur * kGpuTable) * srvStep_;
            cl_->SetGraphicsRootDescriptorTable(0, gtable);
            // the vertex shader reads the state texture too, which needs the non-pixel state
            const D3D12_RESOURCE_STATES anyStage =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            auto toAny = transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, anyStage);
            cl_->ResourceBarrier(1, &toAny);
            // one draw per pass in use, in order: each has its own blending and depth use
            for (int p = 0; p < 8; ++p) {
                if (!((gpuPasses >> p) & 1)) continue;
                if (gpuVl_.hasGlobals) {
                    mat.setInt("_GpuPass", p);
                    mat.fillGlobals(gpuVl_);
                    UINT64 at = ((UINT64)slot * 16 + 8 + (UINT64)p) * kCbBytes;
                    memcpy(cbMapped_ + at, gpuVl_.scratch.data(), gpuVl_.scratch.size());
                    cl_->SetGraphicsRootConstantBufferView(1, cbuf_->GetGPUVirtualAddress() + at);
                }
                cl_->SetPipelineState(gpuPso_[p].Get());
                cl_->DrawInstanced(kGpuTriangles * 3, 1, 0, 0);
            }
            auto toPixel = transition(state_[cur].Get(), anyStage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cl_->ResourceBarrier(1, &toPixel);
            cl_->RSSetViewports(1, &vp);
            // then the control zone on the state texture: mark the list drawn, deliver input
            {
                const float* zn = kGpuControlZone;
                draw(2, zn[0], zn[1], zn[2], zn[3]);
                D3D12_RESOURCE_BARRIER in[2] = {
                    transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
                cl_->ResourceBarrier(2, in);
                UINT top = kHeight - (UINT)(zn[1] + zn[3] / 2), left = (UINT)(zn[0] - zn[2] / 2);
                D3D12_TEXTURE_COPY_LOCATION zd{}, zs{};
                zd.pResource = state_[cur].Get();
                zs.pResource = state_[dst].Get();
                D3D12_BOX zbox{left, top, 0, left + (UINT)zn[2], top + (UINT)zn[3], 1};
                cl_->CopyTextureRegion(&zd, left, top, 0, &zs, &zbox);
                D3D12_RESOURCE_BARRIER out[2] = {
                    transition(state_[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                    transition(state_[cur].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
                cl_->ResourceBarrier(2, out);
            }
            // back to the names the rest of the frame uses: dst = the new state, in RENDER_TARGET
            D3D12_RESOURCE_BARRIER swapOut[2] = {
                transition(state_[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
            cl_->ResourceBarrier(2, swapOut);
            std::swap(cur, dst);
        }
        stamp(3);
        if (timeIt) cl_->ResolveQueryData(tsHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, tsBuf_.Get(), 0);
        auto toRead = transition(state_[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl_->ResourceBarrier(1, &toRead);
        D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
        rdst.pResource = rowBuf_[slot].Get();
        rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rdst.PlacedFootprint = rowFootprint_;
        rsrc.pResource = state_[dst].Get();
        D3D12_BOX rowBox{0, 0, 0, 64, 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, &rowBox);
        D3D12_BOX controlBox{0, kControlRow, 0, kControlTexels, kControlRow + 1, 1};
        cl_->CopyTextureRegion(&rdst, 0, 1, 0, &rsrc, &controlBox);
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
        const uint8_t* base = p + rowFootprint_.Offset;
        out.assign(base, base + 64 * 16);
        out.insert(out.end(), base + rowFootprint_.Footprint.RowPitch, base + rowFootprint_.Footprint.RowPitch + kControlTexels * 16);
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

    bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) override {
        UINT64 freq = 0;
        if (tsPending_ && dx_.fence->GetCompletedValue() >= tsFence_) {
            UINT64* ts = nullptr;
            if (SUCCEEDED(dx_.queue->GetTimestampFrequency(&freq)) && freq && SUCCEEDED(tsBuf_->Map(0, nullptr, (void**)&ts))) {
                tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)freq;
                commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)freq;
                deviceMs_ = (double)(ts[3] - ts[2]) * 1000.0 / (double)freq;
                tsBuf_->Unmap(0, nullptr);
            }
            tsPending_ = false;
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        deviceMs = deviceMs_;
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
        view_.render(state_[cur_].Get(), present, gpuColor_.Get());
    }
    bool gpuCapture(const std::string& path) override {
        if (!gpuColor_) return false;
        dx_.flush();
        auto td = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dx_.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> rb;
        auto bd = bufferDesc(total);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        check(dx_.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb)), "readback buffer");
        OneShot os(dx_);
        auto b0 = transition(gpuColor_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        os.list->ResourceBarrier(1, &b0);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = gpuColor_.Get();
        os.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b1 = transition(gpuColor_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        os.list->ResourceBarrier(1, &b1);
        os.run();
        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, (void**)&p))) return false;
        bool ok = memoryViewWriteBmp(path, kGpuTarget, kGpuTarget, p + fp.Offset, fp.Footprint.RowPitch);
        rb->Unmap(0, nullptr);
        return ok;
    }
    void viewText(const std::vector<std::vector<std::string>>& columns) override { view_.setText(columns); }
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
        // the tables, each state texture alone, then the GPU draw's tables (state and ROM)
        hd.NumDescriptors = 8 * kTableSize + 2 + 2 * kGpuTable;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)), "SRV heap");
        UINT rtvStep = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStep_ = dx_.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (int i = 0; i < 2; ++i) {
            rtv_[i] = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
            rtv_[i].ptr += (SIZE_T)i * rtvStep;
            dx_.dev->CreateRenderTargetView(state_[i].Get(), nullptr, rtv_[i]);
        }
        for (int p = 0; p < passCount_; ++p) {
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
        for (int i = 0; i < 2; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)(8 * kTableSize + i) * srvStep_;
            dx_.dev->CreateShaderResourceView(state_[i].Get(), nullptr, h);
        }
        for (int cur = 0; cur < 2; ++cur) {
            for (UINT s = 0; s < kGpuTable; ++s) {
                ID3D12Resource* res = s == 0 ? state_[cur].Get() : blackTex_.Get();
                for (auto& t : gpuPl_.textures)
                    if (t.slot == s && textures_.count(t.name)) res = textures_[t.name].Get();
                D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)(8 * kTableSize + 2 + cur * kGpuTable + s) * srvStep_;
                dx_.dev->CreateShaderResourceView(res, nullptr, h);
            }
        }
        tablesBuilt_ = true;
    }

    Dx dx_;
    Pass12 passes_[3];   // CPUTick, Commit, and the GPU device's control zone if present
    int passCount_ = 2;

    // The GPU device's draw: its own pipeline (depth test, state texture visible to both stages)
    // and its own colour and depth target.
    void buildGpuDraw(const SLShader& shader, const CompileSettings& cs, const Build12& b) {
        const SLPass* sp = shader.findPass("GPUDraw");
        if (!sp) die("gpu.shader has no GPUDraw pass");
        Dxc dxc;
        dxc.init(b.dxcDir);
        std::string err, vsText, psText, rootDir = fs::u8path(shader.path).parent_path().u8string();
        if (!preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_VERTEX", "1"}}, cs, vsText, err) ||
            !preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs, psText, err))
            die(err.c_str());
        StageLayout &vl = gpuVl_, &pl = gpuPl_;
        std::vector<uint8_t> vs = dxcCompile(dxc, vsText, sp->vertexEntry, "vs_" + b.dxcSm, b.dxcOpt, vl);
        std::vector<uint8_t> ps = dxcCompile(dxc, psText, sp->fragmentEntry, "ps_" + b.dxcSm, b.dxcOpt, pl);
        for (auto& t : vl.textures)
            if (t.slot != 0) die("GPUDraw: the state texture must be t0");
        for (auto& t : pl.textures)
            if ((t.slot == 0) != (t.name == "_State") || t.slot >= kGpuTable) die("GPUDraw: the state texture must be t0, the ROM t1-t4");
        if (pl.hasGlobals) die("GPUDraw: uniforms in the vertex shader only");
        if (vl.globalsSize > kCbBytes) die("GPUDraw: $Globals larger than the upload slot");

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kGpuTable;
        // [0] the state texture for both stages and the ROM, [1] the vertex shader's uniforms
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[1].Descriptor.ShaderRegister = vl.globalsSlot;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = vl.hasGlobals ? 2 : 1;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> blob, rsErr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rsErr))) {
            fprintf(stderr, "[d3d12] GPU root signature (uniforms: slot %u, %u bytes): %s\n", vl.globalsSlot, vl.globalsSize,
                    rsErr ? (const char*)rsErr->GetBufferPointer() : "?");
            die("GPU root signature");
        }
        check(dx_.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gpuRoot_)), "GPU root signature");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = gpuRoot_.Get();
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        // The eight passes (FRAGMENT_PASS in gpu.h): opaque, alpha, additive, multiply with the
        // depth test, then the same four without. Only the first writes depth.
        for (int p = 0; p < 8; ++p) {
            auto& rt = pd.BlendState.RenderTarget[0];
            int blend = p & 3;
            rt.BlendEnable = blend != 0;
            rt.SrcBlend = rt.SrcBlendAlpha = blend == 3 ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = rt.DestBlendAlpha = blend == 1 ? D3D12_BLEND_INV_SRC_ALPHA : blend == 2 ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (blend == 3) rt.SrcBlendAlpha = D3D12_BLEND_ZERO, rt.DestBlendAlpha = D3D12_BLEND_ONE;
            pd.DepthStencilState.DepthEnable = p < 4;
            pd.DepthStencilState.DepthWriteMask = p == 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            check(dx_.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gpuPso_[p])), "GPU pipeline state");
        }
        fprintf(stderr, "[d3d12] pass 'GPUDraw': vs %zu bytes, ps %zu bytes\n", vs.size(), ps.size());

        auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto cd = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto dd = texDesc(kGpuTarget, kGpuTarget, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;
        check(dx_.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&gpuColor_)), "GPU colour target");
        check(dx_.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                               IID_PPV_ARGS(&gpuDepth_)), "GPU depth target");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuRtvHeap_)), "GPU RTV heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        check(dx_.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuDsvHeap_)), "GPU DSV heap");
        dx_.dev->CreateRenderTargetView(gpuColor_.Get(), nullptr, gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart());
        dx_.dev->CreateDepthStencilView(gpuDepth_.Get(), nullptr, gpuDsvHeap_->GetCPUDescriptorHandleForHeapStart());
        OneShot os(dx_);
        const float black[4] = {0, 0, 0, 1};
        os.list->ClearRenderTargetView(gpuRtvHeap_->GetCPUDescriptorHandleForHeapStart(), black, 0, nullptr);
        os.run();
    }

    ComPtr<ID3D12RootSignature> gpuRoot_;
    ComPtr<ID3D12PipelineState> gpuPso_[8];
    StageLayout gpuVl_, gpuPl_;
    static constexpr UINT kGpuTable = 5;   // the GPU draw's textures: the state, then the ROM's four
    ComPtr<ID3D12Resource> gpuColor_, gpuDepth_;
    ComPtr<ID3D12DescriptorHeap> gpuRtvHeap_, gpuDsvHeap_;
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
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;

    MemoryView12 view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend12() { return std::make_unique<Backend12>(); }
