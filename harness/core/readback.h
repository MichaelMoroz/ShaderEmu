// Pipelined GPU->CPU readback of a small texture region through a ring of staging
// textures, so reading results every frame does not stall the GPU.
#pragma once

#include "common.h"

#include <d3d11.h>

#include <deque>

class RegionReadback {
public:
    bool init(ID3D11Device* dev, DXGI_FORMAT format, UINT bytesPerPixel, UINT width, UINT height, int ringSize,
              std::string& err);
    // Caller must pop() first if full().
    void request(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT x, UINT y, uint64_t tag);
    bool full() const { return pending_.size() >= ring_.size(); }
    size_t pending() const { return pending_.size(); }
    // Blocks until the oldest request is available. Output is tightly packed rows.
    bool pop(ID3D11DeviceContext* ctx, std::vector<uint8_t>& out, uint64_t& tag);

private:
    std::vector<ComPtr<ID3D11Texture2D>> ring_;
    std::deque<std::pair<int, uint64_t>> pending_;
    int next_ = 0;
    UINT width_ = 0, height_ = 0, bpp_ = 0;
};
