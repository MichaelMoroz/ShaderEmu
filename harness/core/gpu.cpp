#include "gpu.h"

#include <dxgi1_2.h>

#include <cstdio>

void listAdapters() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        fprintf(stderr, "  [%u] %s (%llu MB VRAM)%s\n", i, narrow(d.Description).c_str(),
                (unsigned long long)(d.DedicatedVideoMemory >> 20),
                (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
        a.Reset();
    }
}

bool Gpu::init(const GpuOptions& opt, std::string& err) {
    UINT flags = 0;
    if (opt.debug) flags |= D3D11_CREATE_DEVICE_DEBUG;

    ComPtr<IDXGIAdapter1> adapter;
    D3D_DRIVER_TYPE type = opt.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    if (!opt.warp && opt.adapterIndex >= 0) {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || FAILED(factory->EnumAdapters1((UINT)opt.adapterIndex, &adapter))) {
            err = "adapter index " + std::to_string(opt.adapterIndex) + " not found";
            return false;
        }
        type = D3D_DRIVER_TYPE_UNKNOWN;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    auto create = [&](UINT f, const D3D_FEATURE_LEVEL* lv, UINT n) {
        device.Reset();
        ctx.Reset();
        return D3D11CreateDevice(adapter.Get(), type, nullptr, f, lv, n, D3D11_SDK_VERSION, &device, &got, &ctx);
    };
    HRESULT hr = create(flags, levels, 2);
    if (hr == E_INVALIDARG) hr = create(flags, levels + 1, 1);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        fprintf(stderr, "[harness] debug layer unavailable (%s), continuing without it\n", hrToString(hr).c_str());
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = create(flags, levels, 2);
        if (hr == E_INVALIDARG) hr = create(flags, levels + 1, 1);
    }
    if (FAILED(hr)) {
        err = "D3D11CreateDevice failed: " + hrToString(hr);
        return false;
    }

    {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(device.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&a)) && SUCCEEDED(a->GetDesc(&d)))
            adapterName = narrow(d.Description);
    }

    D3D11_FEATURE_DATA_DOUBLES dd{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dd, sizeof dd)))
        doubles = dd.DoublePrecisionFloatShaderOps != FALSE;
    D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)))
        extendedDoubles = o.ExtendedDoublesShaderInstructions != FALSE;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &rasterNoCull);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &blendOpaque);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dsd, &depthOff);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    device->CreateSamplerState(&sd, &samplerPoint);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&sd, &samplerLinear);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint32_t zero = 0;
    D3D11_SUBRESOURCE_DATA init{&zero, 4, 0};
    ComPtr<ID3D11Texture2D> black;
    device->CreateTexture2D(&td, &init, &black);
    device->CreateShaderResourceView(black.Get(), nullptr, &blackTexture);
    return true;
}

std::string Gpu::deviceRemovedReason() const {
    if (!device) return "no device";
    HRESULT hr = device->GetDeviceRemovedReason();
    if (hr == S_OK) return {};
    switch (hr) {
        case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG (" + hrToString(hr) + ")";
        case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED (" + hrToString(hr) + ")";
        case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET (" + hrToString(hr) + ")";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR (" + hrToString(hr) + ")";
        default: return hrToString(hr);
    }
}
