// D3D11 device creation (hardware adapter or WARP) plus fixed pipeline state
// matching a Unity Custom Render Texture pass: Cull Off, ZTest Off, Blend One Zero.
#pragma once

#include "common.h"

#include <d3d11.h>

struct GpuOptions {
    bool warp = false;       // Microsoft's software rasterizer: slow, but no GPU and no TDR
    bool debug = false;      // D3D11 debug layer (needs Graphics Tools installed)
    int adapterIndex = -1;   // -1 = default adapter
};

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    std::string adapterName;
    bool doubles = false;           // DoublePrecisionFloatShaderOps
    bool extendedDoubles = false;   // ExtendedDoublesShaderInstructions (ddiv, dfma, conversions)

    ComPtr<ID3D11RasterizerState> rasterNoCull;
    ComPtr<ID3D11BlendState> blendOpaque;
    ComPtr<ID3D11DepthStencilState> depthOff;
    ComPtr<ID3D11SamplerState> samplerPoint;
    ComPtr<ID3D11SamplerState> samplerLinear;
    ComPtr<ID3D11ShaderResourceView> blackTexture;  // Unity's "black" default: (0,0,0,0)

    bool init(const GpuOptions& opt, std::string& err);
    // Empty string if the device is fine, otherwise a description of why it was removed (e.g. TDR).
    std::string deviceRemovedReason() const;
};

void listAdapters();
