#include "crt.h"

#include "gpu.h"

#include <algorithm>
#include <cmath>

bool CustomRenderTexture::init(ID3D11Device* dev, UINT width, UINT height, DXGI_FORMAT format, std::string& err) {
    width_ = width;
    height_ = height;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i) {
        HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex_[i]);
        if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(tex_[i].Get(), nullptr, &srv_[i]);
        if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(tex_[i].Get(), nullptr, &rtv_[i]);
        if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(tex_[i].Get(), nullptr, &uav_[i]);
        if (FAILED(hr)) {
            err = "creating CRT buffer failed: " + hrToString(hr);
            return false;
        }
    }
    cur_ = 0;
    return true;
}

void CustomRenderTexture::clear(ID3D11DeviceContext* ctx) {
    const float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) ctx->ClearRenderTargetView(rtv_[i].Get(), zero);
}

void CustomRenderTexture::load(ID3D11DeviceContext* ctx, const void* data, UINT rowPitch) {
    for (int i = 0; i < 2; ++i) ctx->UpdateSubresource(tex_[i].Get(), 0, nullptr, data, rowPitch, 0);
}

void CustomRenderTexture::copyZone(Gpu& gpu, const UpdateZone& z) {
    LONG x0 = (LONG)std::lround(z.centerX - z.width * 0.5f);
    LONG y0 = (LONG)std::lround((float)height_ - (z.centerY + z.height * 0.5f));
    LONG x1 = x0 + (LONG)std::lround(z.width);
    LONG y1 = y0 + (LONG)std::lround(z.height);
    x0 = (std::max)(x0, 0L); y0 = (std::max)(y0, 0L);
    x1 = (std::min)(x1, (LONG)width_); y1 = (std::min)(y1, (LONG)height_);
    if (x1 <= x0 || y1 <= y0) return;
    D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
    gpu.ctx->CopySubresourceRegion(tex_[cur_].Get(), 0, (UINT)x0, (UINT)y0, 0, tex_[1 - cur_].Get(), 0, &box);
}

void CustomRenderTexture::copyIn(Gpu& gpu, const UpdateZone& z) {
    LONG x0 = (LONG)std::lround(z.centerX - z.width * 0.5f);
    LONG y0 = (LONG)std::lround((float)height_ - (z.centerY + z.height * 0.5f));
    LONG x1 = x0 + (LONG)std::lround(z.width);
    LONG y1 = y0 + (LONG)std::lround(z.height);
    x0 = (std::max)(x0, 0L); y0 = (std::max)(y0, 0L);
    x1 = (std::min)(x1, (LONG)width_); y1 = (std::min)(y1, (LONG)height_);
    if (x1 <= x0 || y1 <= y0) return;
    D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
    ID3D11ShaderResourceView* nulls[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    gpu.ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
    gpu.ctx->GSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
    gpu.ctx->CopySubresourceRegion(tex_[1 - cur_].Get(), 0, (UINT)x0, (UINT)y0, 0, tex_[cur_].Get(), 0, &box);
}

void CustomRenderTexture::runZone(Gpu& gpu, GpuPass& pass, Material& mat, const UpdateZone& z, UINT vertices, bool copyBack,
                                  D3D11_PRIMITIVE_TOPOLOGY topology) {
    ID3D11DeviceContext* ctx = gpu.ctx.Get();

    // Region in memory coordinates (row 0 = top), for the copy-back of partial zones.
    LONG x0 = (LONG)std::lround(z.centerX - z.width * 0.5f);
    LONG y0 = (LONG)std::lround((float)height_ - (z.centerY + z.height * 0.5f));
    LONG x1 = x0 + (LONG)std::lround(z.width);
    LONG y1 = y0 + (LONG)std::lround(z.height);
    x0 = (std::max)(x0, 0L); y0 = (std::max)(y0, 0L);
    x1 = (std::min)(x1, (LONG)width_); y1 = (std::min)(y1, (LONG)height_);
    bool full = x0 == 0 && y0 == 0 && x1 == (LONG)width_ && y1 == (LONG)height_;

    // Uniforms consumed by Unity's CustomRenderTextureVertexShader (zone 0 of a batch).
    mat.setVector("CustomRenderTextureCenters", z.centerX, z.centerY, 0.5, 0);
    mat.setVector("CustomRenderTextureSizesAndRotations", z.width, z.height, 1, 0);
    mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
    mat.setVector("CustomRenderTextureParameters", 1 /* pixel space */, 0, 0, 0);
    mat.setVector("_CustomRenderTextureInfo", width_, height_, 1, 0);
    mat.setTexture("_SelfTexture2D", srv_[cur_].Get(), width_, height_);

    // Unbind everything first: the back buffer may still be bound as a shader resource.
    ID3D11ShaderResourceView* nulls[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ctx->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
    ctx->GSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
    ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);

    int dst = 1 - cur_;
    // A pixel shader may declare one UAV (profiling counters); its slot must follow the RTV.
    ID3D11UnorderedAccessView* uav = pass.psLayout.uavs.empty() ? nullptr : mat.uav(pass.psLayout.uavs[0].name);
    if (uav)
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, rtv_[dst].GetAddressOf(), nullptr,
                                                       pass.psLayout.uavs[0].slot, 1, &uav, nullptr);
    else
        ctx->OMSetRenderTargets(1, rtv_[dst].GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)width_, (float)height_, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(gpu.rasterNoCull.Get());
    ctx->OMSetBlendState(gpu.blendOpaque.Get(), nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(gpu.depthOff.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(topology);

    mat.bind(ctx, pass, gpu);
    ctx->Draw(vertices, 0);

    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (!copyBack) {
        // the caller copies (copyZone)
    } else if (full) {
        cur_ = dst;
    } else if (x1 > x0 && y1 > y0) {
        D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
        ctx->CopySubresourceRegion(tex_[cur_].Get(), 0, (UINT)x0, (UINT)y0, 0, tex_[dst].Get(), 0, &box);
    }
}
