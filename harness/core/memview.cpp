#include "memview.h"

#include <d3dcompiler.h>

#include <cstring>
#include <vector>

namespace {

const char* kShader = R"(
Texture2D<uint4> Cur : register(t0);
Texture2D<uint4> Prev : register(t1);
Texture2D<float> Heat : register(t2);
Texture2D<float4> Text : register(t3);
cbuffer C : register(b0) {
    float2 WinSize;
    uint Frame;
    uint Strips;       // RAM is cut into this many vertical strips shown side by side
    uint TexWidth;
    uint RamRows;
    uint StateRows;
    float Inset;       // side of the CPU state inset in window pixels
    float2 TextSize;   // used size of the text texture; 0 = no text
    float Bar;         // height of the info bar under the memory image
};

float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0, 1);
}

uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

float3 bytes(uint4 t) {
    return float3(t.r & 0xff, (t.r >> 8) & 0xff, t.g & 0xff) / 255.0;
}

struct Out {
    float4 color : SV_Target0;
    float heat : SV_Target1;
};

Out ps(float4 pos : SV_Position) {
    Out o;
    uint2 px = (uint2)pos.xy;
    float memHeight = WinSize.y - Bar;     // memory occupies everything above the info bar

    // Info bar along the bottom: counters on the left, CPU state area on the right.
    if (pos.y >= memHeight) {
        o.heat = 0;
        float3 c = float3(0.055, 0.062, 0.075);
        if (pos.y < memHeight + 1) c = float3(0.22, 0.24, 0.28);   // separator line
        float2 t = pos.xy - float2(12, memHeight + 10);
        if (TextSize.x > 0 && t.x >= 0 && t.y >= 0 && t.x < TextSize.x && t.y < TextSize.y) {
            c = lerp(c, float3(0.93, 0.95, 0.98), Text.Load(int3(t, 0)).g);
        }
        float2 inset = pos.xy - float2(WinSize.x - Inset - 12, memHeight + (Bar - Inset) * 0.5);
        if (inset.x >= -1 && inset.y >= -1 && inset.x <= Inset && inset.y <= Inset) {
            if (inset.x < 0 || inset.y < 0 || inset.x >= Inset || inset.y >= Inset) {
                c = float3(0.75, 0.78, 0.85);                         // frame
            } else {
                int2 st = int2(inset / Inset * StateRows);
                uint4 a = Cur.Load(int3(st, 0)), b = Prev.Load(int3(st, 0));
                c = bytes(a) * 0.8 + 0.06;
                if (any(a != b)) c = lerp(c, float3(0.35, 0.85, 1.0), 0.6);
            }
        }
        o.color = float4(c, 1);
        return o;
    }

    // Heat fades in place and bleeds a little into neighbours, so a single written texel
    // shows up as a small dot rather than one pixel.
    float prevHeat = Heat.Load(int3(px, 0)) * 0.955;
    float around = max(max(Heat.Load(int3(px + int2(1, 0), 0)), Heat.Load(int3(px - int2(1, 0), 0))),
                       max(Heat.Load(int3(px + int2(0, 1), 0)), Heat.Load(int3(px - int2(0, 1), 0))));
    prevHeat = max(prevHeat, around * 0.5);

    // RAM: strip s covers rows [s, s+1) * rowsPerStrip
    float rowsPerStrip = (float)RamRows / Strips;
    float sx = pos.x / WinSize.x * Strips;
    uint strip = min((uint)sx, Strips - 1);
    float2 texel = float2(frac(sx) * TexWidth, StateRows + (strip + pos.y / memHeight) * rowsPerStrip);
    float2 footprint = float2(TexWidth * Strips / WinSize.x, rowsPerStrip / memHeight);

    // A window pixel covers several texels: a 4x4 grid of taps over its footprint, nudged per
    // frame so larger footprints are covered over time.
    bool changed = false;
    uint4 first = 0;
    uint h = hash(px.x + px.y * 4099u + Frame * 7919u);
    float2 nudge = float2(h & 0xffff, h >> 16) / 65536.0 * 0.25;
    for (uint k = 0; k < 16; k++) {
        float2 cell = (float2(k & 3, k >> 2) + nudge * 4.0) / 4.0;
        int2 t = clamp(int2(texel + (cell - 0.5) * footprint), int2(0, StateRows), int2(TexWidth - 1, StateRows + RamRows - 1));
        uint4 a = Cur.Load(int3(t, 0)), b = Prev.Load(int3(t, 0));
        if (any(a != b)) changed = true;
        if (k == 5) first = a;
    }
    float heat = max(changed ? 1.0 : 0.0, prevHeat);
    float3 base = any(first != 0) ? bytes(first) * 0.5 + 0.05 : float3(0.02, 0.025, 0.04);
    float3 glow = lerp(float3(1.0, 0.45, 0.05), float3(1.0, 0.95, 0.8), saturate(heat * heat));
    o.color = float4(lerp(base, glow, saturate(heat * 1.2)), 1);
    o.heat = heat;
    return o;
}
)";

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        // Flag it for render(); the emulator keeps running without the view.
        SetPropW(h, L"closed", (HANDLE)1);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

const UINT kBarHeight = kMemoryViewBar;  // info bar: five text lines, and the CPU state area at 100 px

using Constants = MemoryViewConstants;

}  // namespace

const char* memoryViewShader() { return kShader; }

HWND memoryViewCreateWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"ShaderEmuMemoryView";
    RegisterClassW(&wc);
    RECT r{0, 0, 1024, 504 + (LONG)kBarHeight};  // two 2048-texel strips at quarter scale, plus the info bar
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"memory", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
}

bool memoryViewWriteBmp(const std::string& path, UINT width, UINT height, const uint8_t* rgba, size_t pitch) {
    std::vector<uint8_t> file(54 + (size_t)width * height * 4, 0);
    uint8_t* h = file.data();
    auto put32 = [&](size_t off, uint32_t v) { memcpy(h + off, &v, 4); };
    h[0] = 'B'; h[1] = 'M';
    put32(2, (uint32_t)file.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, (uint32_t)(-(int32_t)height));  // top-down
    h[26] = 1; h[28] = 32;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* src = rgba + (size_t)y * pitch;
        uint8_t* dst = h + 54 + (size_t)y * width * 4;
        for (UINT x = 0; x < width; ++x) {  // RGBA -> BGRA
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    return writeFileBinary(path, file.data(), file.size());
}

bool memoryViewText(const std::vector<std::string>& lines, std::vector<uint8_t>& out, UINT& usedW, UINT& usedH) {
    const UINT kW = kMemoryViewTextW, kH = kMemoryViewTextH, kLine = 19;
    bool ok = false;
    // White text on black in a memory bitmap; the shader uses one channel as coverage.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)kW;
    bi.bmiHeader.biHeight = -(LONG)kH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (dc && bmp && font && bits) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, font);
        memset(bits, 0, (size_t)kW * kH * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        LONG widest = 0;
        UINT n = 0;
        for (auto& line : lines) {
            if ((n + 1) * kLine > kH) break;
            std::wstring w = widen(line);
            TextOutW(dc, 0, (int)(n * kLine), w.c_str(), (int)w.size());
            SIZE sz{};
            GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
            if (sz.cx > widest) widest = sz.cx;
            ++n;
        }
        GdiFlush();
        out.assign((const uint8_t*)bits, (const uint8_t*)bits + (size_t)kW * kH * 4);
        ok = true;
        usedW = (UINT)(widest < (LONG)kW ? widest : (LONG)kW);
        usedH = n * kLine;
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
    }
    if (font) DeleteObject(font);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return ok;
}

bool MemoryView::init(ID3D11Device* dev, UINT texWidth, UINT texHeight, UINT stateRows, std::string& err) {
    dev_ = dev;
    texWidth_ = texWidth;
    texHeight_ = texHeight;
    stateRows_ = stateRows;

    hwnd_ = memoryViewCreateWindow();
    if (!hwnd_) {
        err = "cannot create the memory view window";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd_;
    sd.Windowed = TRUE;
    HRESULT hr = dev_.As(&dxgiDev);
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(dev_.Get(), &sd, &swap_);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) {
        err = "memory view swapchain: " + hrToString(hr);
        return false;
    }

    ComPtr<ID3DBlob> vsb, psb, errors;
    hr = D3DCompile(kShader, strlen(kShader), "memview", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &errors);
    if (SUCCEEDED(hr)) hr = D3DCompile(kShader, strlen(kShader), "memview", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &errors);
    if (FAILED(hr)) {
        err = "memory view shader: " + (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()) : hrToString(hr));
        return false;
    }
    dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (sizeof(Constants) + 15) & ~15u;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev_->CreateRasterizerState(&rd, &raster_);

    D3D11_TEXTURE2D_DESC rd2{};
    rd2.Width = texWidth;
    rd2.Height = texHeight;
    rd2.MipLevels = rd2.ArraySize = 1;
    rd2.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    rd2.SampleDesc.Count = 1;
    rd2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = dev_->CreateTexture2D(&rd2, nullptr, &refTex_);
    if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(refTex_.Get(), nullptr, &refSrv_);
    if (FAILED(hr)) {
        err = "memory view reference texture: " + hrToString(hr);
        return false;
    }

    if (!createTargets(err)) return false;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);  // never takes the keyboard from the console
    return true;
}

bool MemoryView::createTargets(std::string& err) {
    backRtv_.Reset();
    backTex_.Reset();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    width_ = (UINT)(rc.right > 1 ? rc.right : 1);
    height_ = (UINT)(rc.bottom > 1 ? rc.bottom : 1);
    HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) hr = swap_->GetBuffer(0, IID_PPV_ARGS(&backTex_));
    if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(backTex_.Get(), nullptr, &backRtv_);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        heatTex_[i].Reset(); heatSrv_[i].Reset(); heatRtv_[i].Reset();
        hr = dev_->CreateTexture2D(&td, nullptr, &heatTex_[i]);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(heatTex_[i].Get(), nullptr, &heatSrv_[i]);
        if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(heatTex_[i].Get(), nullptr, &heatRtv_[i]);
    }
    if (FAILED(hr)) {
        err = "memory view targets: " + hrToString(hr);
        return false;
    }
    heatNeedsClear_ = true;
    return true;
}

bool MemoryView::render(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur, bool present) {
    if (!hwnd_) return false;
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetPropW(hwnd_, L"closed")) {
        close();
        return false;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return true;  // minimised
    std::string err;
    if ((UINT)rc.right != width_ || (UINT)rc.bottom != height_) {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        if (!createTargets(err)) return true;
    }
    if (heatNeedsClear_) {
        const float zero[4] = {0, 0, 0, 0};
        for (auto& r : heatRtv_) ctx->ClearRenderTargetView(r.Get(), zero);
        heatNeedsClear_ = false;
    }

    Constants c{};
    c.winSize[0] = (float)width_;
    c.winSize[1] = (float)height_;
    c.frame = frame_++;
    c.strips = 2;
    c.texWidth = texWidth_;
    c.ramRows = texHeight_ - stateRows_;
    c.stateRows = stateRows_;
    c.bar = (float)kBarHeight;
    c.inset = (float)(kBarHeight - 16);
    c.textSize[0] = (float)textW_;
    c.textSize[1] = (float)textH_;
    ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);

    int dst = 1 - heatCur_;
    ID3D11ShaderResourceView* nulls[4] = {};
    ctx->PSSetShaderResources(0, 4, nulls);
    ID3D11RenderTargetView* rtvs[2] = {backRtv_.Get(), heatRtv_[dst].Get()};
    ctx->OMSetRenderTargets(2, rtvs, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)width_, (float)height_, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    // The first frame has nothing to compare with, so it compares the state with itself.
    ID3D11ShaderResourceView* srvs[4] = {cur, refValid_ ? refSrv_.Get() : cur, heatSrv_[heatCur_].Get(), textSrv_.Get()};
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->PSSetShaderResources(0, 4, nulls);
    heatCur_ = dst;
    if (present) swap_->Present(0, 0);

    // Remember this state: the next frame glows wherever it differs.
    ComPtr<ID3D11Resource> curRes;
    cur->GetResource(&curRes);
    ctx->CopyResource(refTex_.Get(), curRes.Get());
    refValid_ = true;
    return true;
}

bool MemoryView::capture(ID3D11DeviceContext* ctx, const std::string& path) {
    if (!backTex_) return false;
    D3D11_TEXTURE2D_DESC td{};
    backTex_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), backTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bool ok = memoryViewWriteBmp(path, td.Width, td.Height, (const uint8_t*)m.pData, m.RowPitch);
    ctx->Unmap(staging.Get(), 0);
    return ok;
}

void MemoryView::setText(ID3D11DeviceContext* ctx, const std::vector<std::string>& lines) {
    if (!hwnd_) return;
    const UINT kW = kMemoryViewTextW, kH = kMemoryViewTextH;
    if (!textTex_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW;
        td.Height = kH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &textTex_)) ||
            FAILED(dev_->CreateShaderResourceView(textTex_.Get(), nullptr, &textSrv_)))
            return;
    }
    std::vector<uint8_t> bits;
    if (memoryViewText(lines, bits, textW_, textH_)) ctx->UpdateSubresource(textTex_.Get(), 0, nullptr, bits.data(), kW * 4, 0);
}

void MemoryView::setTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(hwnd_, widen(title).c_str());
}

void MemoryView::close() {
    if (!hwnd_) return;
    backRtv_.Reset();
    backTex_.Reset();
    swap_.Reset();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}
