// The memory view (see memview.h) on D3D12: same shader, window and info bar, drawn by the
// emulator's own device straight from its state texture. Each call runs synchronously.
#pragma once

#include "memview.h"
#include "rvc_d3d12.h"

#include <d3dcompiler.h>

namespace {

class MemoryView12 {
public:
    bool init(Dx& dx, UINT texWidth, UINT texHeight, UINT stateRows, std::string& err) {
        dx_ = &dx;
        texWidth_ = texWidth;
        texHeight_ = texHeight;
        stateRows_ = stateRows;
        ComPtr<ID3DBlob> vsb, psb, errors;
        const char* src = memoryViewShader();
        HRESULT hr = D3DCompile(src, strlen(src), "memview", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &errors);
        if (SUCCEEDED(hr)) hr = D3DCompile(src, strlen(src), "memview", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &errors);
        if (FAILED(hr)) {
            err = "memory view shader: " + hrToString(hr);
            return false;
        }

        // Root: [0] constants b0, [1] textures t0..t3 (current, previous, heat, text).
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 5;
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
        if (SUCCEEDED(hr)) hr = dx.dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&root_));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
        pd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
        pd.SampleDesc.Count = 1;
        if (SUCCEEDED(hr)) hr = dx.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;  // two back buffers, two heat textures
        if (SUCCEEDED(hr)) hr = dx.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_));
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 5;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (SUCCEEDED(hr)) hr = dx.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_));
        rtvStep_ = dx.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStep_ = dx.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT), hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto refDesc = texDesc(texWidth, texHeight, kStateFormat, D3D12_RESOURCE_FLAG_NONE);
        auto textDesc = texDesc(kMemoryViewTextW, kMemoryViewTextH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        UINT64 textBytes = 0;
        dx.dev->GetCopyableFootprints(&textDesc, 0, 1, 0, &textFp_, nullptr, nullptr, &textBytes);
        auto textUp = bufferDesc(textBytes), cbDesc = bufferDesc(256);
        const D3D12_RESOURCE_STATES psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, gr = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &refDesc, psr, nullptr, IID_PPV_ARGS(&ref_));
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &textDesc, psr, nullptr, IID_PPV_ARGS(&text_));
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &textUp, gr, nullptr, IID_PPV_ARGS(&textUpload_));
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &cbDesc, gr, nullptr, IID_PPV_ARGS(&cb_));
        if (SUCCEEDED(hr)) hr = cb_->Map(0, nullptr, (void**)&cbMapped_);
        if (SUCCEEDED(hr)) hr = textUpload_->Map(0, nullptr, (void**)&textMapped_);
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_));
        if (SUCCEEDED(hr)) hr = dx.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_));
        if (FAILED(hr)) {
            err = "memory view resources: " + hrToString(hr);
            return false;
        }
        list_->Close();

        hwnd_ = memoryViewCreateWindow();
        ComPtr<IDXGIFactory4> factory;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swap1;
        hr = hwnd_ ? CreateDXGIFactory1(IID_PPV_ARGS(&factory)) : E_FAIL;
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(dx.queue.Get(), hwnd_, &sd, nullptr, nullptr, &swap1);
        if (SUCCEEDED(hr)) hr = swap1.As(&swap_);
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr) || !createTargets()) {
            err = "memory view swapchain: " + hrToString(hr);
            close();
            return false;
        }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);  // never takes the keyboard from the console
        return true;
    }

    // Draws one frame from `cur` (in PIXEL_SHADER_RESOURCE state, GPU idle) and presents it.
    // gpu: the GPU device's colour target (in RENDER_TARGET state), or null.
    void render(ID3D12Resource* cur, bool present, ID3D12Resource* gpu = nullptr) {
        if (!hwnd_) return;
        MSG msg;
        while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetPropW(hwnd_, L"closed")) {
            close();
            return;
        }
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;  // minimised
        if (((UINT)rc.right != width_ || (UINT)rc.bottom != height_) && !createTargets()) return;

        MemoryViewConstants c{};
        c.winSize[0] = (float)width_;
        c.winSize[1] = (float)height_;
        c.frame = frame_++;
        c.strips = 2;
        c.texWidth = texWidth_;
        c.ramRows = texHeight_ - stateRows_;
        c.stateRows = stateRows_;
        c.bar = (float)kMemoryViewBar;
        c.inset = (float)(kMemoryViewBar - 16);
        c.split = (float)width_ * kMemoryViewDispW / (kMemoryViewDispW + kMemoryViewMemW);
        c.textSize[0] = (float)textW_;
        c.textSize[1] = (float)textH_;
        memcpy(cbMapped_, &c, sizeof c);

        // The first frame has nothing to compare with, so it compares the state with itself.
        int dst = 1 - heatCur_;
        ID3D12Resource* srvs[5] = {cur, refValid_ ? ref_.Get() : cur, heat_[heatCur_].Get(), text_.Get(), gpu ? gpu : text_.Get()};
        for (UINT i = 0; i < 5; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * srvStep_;
            dx_->dev->CreateShaderResourceView(srvs[i], nullptr, h);
        }

        alloc_->Reset();
        list_->Reset(alloc_.Get(), nullptr);
        const D3D12_RESOURCE_STATES psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, rt = D3D12_RESOURCE_STATE_RENDER_TARGET;
        auto move = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            auto b = transition(r, from, to);
            list_->ResourceBarrier(1, &b);
        };
        if (textDirty_) {
            move(text_.Get(), psr, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION td{}, ts{};
            td.pResource = text_.Get();
            ts.pResource = textUpload_.Get();
            ts.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ts.PlacedFootprint = textFp_;
            list_->CopyTextureRegion(&td, 0, 0, 0, &ts, nullptr);
            move(text_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, psr);
            textDirty_ = false;
        }
        UINT back = swap_->GetCurrentBackBufferIndex();
        move(back_[back].Get(), D3D12_RESOURCE_STATE_PRESENT, rt);
        if (gpu) move(gpu, rt, psr);
        if (heatNeedsClear_) {
            const float zero[4] = {0, 0, 0, 0};
            move(heat_[heatCur_].Get(), psr, rt);
            list_->ClearRenderTargetView(rtv(2 + heatCur_), zero, 0, nullptr);
            move(heat_[heatCur_].Get(), rt, psr);
            heatNeedsClear_ = false;
        }
        move(heat_[dst].Get(), psr, rt);
        ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
        list_->SetDescriptorHeaps(1, heaps);
        D3D12_VIEWPORT vp{0, 0, (float)width_, (float)height_, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)width_, (LONG)height_};
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &scissor);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->SetPipelineState(pso_.Get());
        list_->SetGraphicsRootSignature(root_.Get());
        list_->SetGraphicsRootConstantBufferView(0, cb_->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv(back), rtv(2 + dst)};
        list_->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
        list_->DrawInstanced(3, 1, 0, 0);
        if (gpu) move(gpu, psr, rt);
        move(heat_[dst].Get(), rt, psr);
        move(back_[back].Get(), rt, D3D12_RESOURCE_STATE_PRESENT);

        // Remember this state: the next frame glows wherever it differs.
        move(cur, psr, D3D12_RESOURCE_STATE_COPY_SOURCE);
        move(ref_.Get(), psr, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyResource(ref_.Get(), cur);
        move(ref_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, psr);
        move(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, psr);
        list_->Close();
        ID3D12CommandList* l[] = {list_.Get()};
        dx_->queue->ExecuteCommandLists(1, l);
        if (present) swap_->Present(0, 0);
        dx_->flush();
        heatCur_ = dst;
        refValid_ = true;
    }

    // Saves the back buffer as a BMP; call after render(..., false).
    bool capture(const std::string& path) {
        if (!hwnd_) return false;
        UINT back = swap_->GetCurrentBackBufferIndex();
        auto td = texDesc(width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dx_->dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        ComPtr<ID3D12Resource> rb;
        auto bd = bufferDesc(total);
        auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(dx_->dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&rb))))
            return false;
        OneShot os(*dx_);
        auto b0 = transition(back_[back].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        os.list->ResourceBarrier(1, &b0);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = back_[back].Get();
        os.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b1 = transition(back_[back].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        os.list->ResourceBarrier(1, &b1);
        os.run();
        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, (void**)&p))) return false;
        bool ok = memoryViewWriteBmp(path, width_, height_, p + fp.Offset, fp.Footprint.RowPitch);
        rb->Unmap(0, nullptr);
        return ok;
    }

    void setText(const MemoryViewText& columns) {
        std::vector<uint8_t> bits;
        if (!hwnd_ || !memoryViewText(columns, bits, textW_, textH_)) return;
        for (UINT y = 0; y < kMemoryViewTextH; ++y)
            memcpy(textMapped_ + textFp_.Offset + (size_t)y * textFp_.Footprint.RowPitch, bits.data() + (size_t)y * kMemoryViewTextW * 4,
                   (size_t)kMemoryViewTextW * 4);
        textDirty_ = true;
    }
    void setTitle(const std::string& title) {
        if (hwnd_) SetWindowTextW(hwnd_, widen(title).c_str());
    }
    bool open() const { return hwnd_ != nullptr; }
    void close() {
        if (!hwnd_) return;
        if (dx_) dx_->flush();
        for (auto& b : back_) b.Reset();
        swap_.Reset();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * rtvStep_;
        return h;
    }

    // Back buffers and heat textures at the window's current client size.
    bool createTargets() {
        dx_->flush();
        for (auto& b : back_) b.Reset();
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        width_ = (UINT)(rc.right > 1 ? rc.right : 1);
        height_ = (UINT)(rc.bottom > 1 ? rc.bottom : 1);
        HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
        for (UINT i = 0; i < 2 && SUCCEEDED(hr); ++i) {
            hr = swap_->GetBuffer(i, IID_PPV_ARGS(&back_[i]));
            if (SUCCEEDED(hr)) dx_->dev->CreateRenderTargetView(back_[i].Get(), nullptr, rtv(i));
        }
        auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        auto hd = texDesc(width_, height_, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        for (UINT i = 0; i < 2 && SUCCEEDED(hr); ++i) {
            heat_[i].Reset();
            hr = dx_->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &hd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   nullptr, IID_PPV_ARGS(&heat_[i]));
            if (SUCCEEDED(hr)) dx_->dev->CreateRenderTargetView(heat_[i].Get(), nullptr, rtv(2 + i));
        }
        heatNeedsClear_ = true;
        return SUCCEEDED(hr);
    }

    Dx* dx_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> back_[2], heat_[2], ref_, text_, textUpload_, cb_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, srvHeap_;
    ComPtr<ID3D12CommandAllocator> alloc_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT textFp_{};
    uint8_t* cbMapped_ = nullptr;
    uint8_t* textMapped_ = nullptr;
    UINT rtvStep_ = 0, srvStep_ = 0;
    UINT width_ = 0, height_ = 0, textW_ = 0, textH_ = 0;
    UINT texWidth_ = 0, texHeight_ = 0, stateRows_ = 0, frame_ = 0;
    int heatCur_ = 0;
    bool heatNeedsClear_ = true, refValid_ = false, textDirty_ = false;
};

}  // namespace
