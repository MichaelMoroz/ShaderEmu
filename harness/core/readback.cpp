#include "readback.h"

#include <cstring>

bool RegionReadback::init(ID3D11Device* dev, DXGI_FORMAT format, UINT bytesPerPixel, UINT width, UINT height,
                          int ringSize, std::string& err, int batch) {
    width_ = width;
    height_ = height;
    bpp_ = bytesPerPixel;
    batch_ = batch < 1 ? 1 : batch;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height * batch_;
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
    ctx->CopySubresourceRegion(ring_[open_].Get(), 0, 0, (UINT)filled_ * height_, 0, src, 0, &box);
    pending_.push_back({open_, filled_, tag, serial_++});
    if (++filled_ == batch_) {
        filled_ = 0;
        open_ = (open_ + 1) % (int)ring_.size();
    }
}

bool RegionReadback::pop(ID3D11DeviceContext* ctx, std::vector<uint8_t>& out, uint64_t& tag) {
    if (pending_.empty()) return false;
    Request r = pending_.front();
    pending_.pop_front();
    tag = r.tag;
    size_t bytes = (size_t)width_ * height_ * bpp_;
    if (r.serial < cachedFrom_ || r.serial >= cachedFrom_ + cachedCount_) {
        // Map its texture once for every request there from this one on. A texture still being
        // filled has only some of them: the rest are mapped when their turn comes.
        UINT slots = r.texture == open_ && r.slot < filled_ ? (UINT)filled_ : (UINT)batch_;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(ring_[r.texture].Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        cache_.resize(bytes * batch_);
        for (UINT row = (UINT)r.slot * height_; row < slots * height_; ++row)
            memcpy(cache_.data() + (size_t)row * width_ * bpp_, (const uint8_t*)m.pData + (size_t)row * m.RowPitch,
                   (size_t)width_ * bpp_);
        ctx->Unmap(ring_[r.texture].Get(), 0);
        cachedFrom_ = r.serial - (uint64_t)r.slot;
        cachedCount_ = slots;
    }
    out.assign(cache_.begin() + (size_t)r.slot * bytes, cache_.begin() + (size_t)(r.slot + 1) * bytes);
    return true;
}
