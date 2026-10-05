#include "readback.h"

#include <cstring>

bool RegionReadback::init(ID3D11Device* dev, DXGI_FORMAT format, UINT bytesPerPixel, UINT width, UINT height,
                          int ringSize, std::string& err) {
    width_ = width;
    height_ = height;
    bpp_ = bytesPerPixel;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ring_.resize(ringSize);
    for (auto& t : ring_) {
        HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
        if (FAILED(hr)) {
            err = "creating staging texture failed: " + hrToString(hr);
            return false;
        }
    }
    return true;
}

void RegionReadback::request(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT x, UINT y, uint64_t tag) {
    D3D11_BOX box{x, y, 0, x + width_, y + height_, 1};
    ctx->CopySubresourceRegion(ring_[next_].Get(), 0, 0, 0, 0, src, 0, &box);
    pending_.push_back({next_, tag});
    next_ = (next_ + 1) % (int)ring_.size();
}

bool RegionReadback::pop(ID3D11DeviceContext* ctx, std::vector<uint8_t>& out, uint64_t& tag) {
    if (pending_.empty()) return false;
    auto [idx, t] = pending_.front();
    pending_.pop_front();
    tag = t;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(ring_[idx].Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize((size_t)width_ * height_ * bpp_);
    for (UINT row = 0; row < height_; ++row)
        memcpy(out.data() + (size_t)row * width_ * bpp_, (const uint8_t*)m.pData + (size_t)row * m.RowPitch,
               (size_t)width_ * bpp_);
    ctx->Unmap(ring_[idx].Get(), 0);
    return true;
}
