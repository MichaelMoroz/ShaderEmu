// Pipelined GPU->CPU readback of a small texture region through a ring of staging
// textures, so reading results every frame does not stall the GPU.
#pragma once

#include "common.h"

#include <d3d11.h>

#include <deque>

class RegionReadback {
public:
    // With batch > 1 a staging texture holds that many requests, one under the other, and is
    // mapped once for all of them: fewer Maps, and results that many requests later.
    bool init(ID3D11Device* dev, DXGI_FORMAT format, UINT bytesPerPixel, UINT width, UINT height, int ringSize,
              std::string& err, int batch = 1);
    // Caller must pop() first if full().
    void request(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT x, UINT y, uint64_t tag);
    // The next request would go where a result not yet popped is.
    bool full() const { return !pending_.empty() && pending_.front().texture == open_ && pending_.front().slot >= filled_; }
    size_t pending() const { return pending_.size(); }
    // Blocks until the oldest request is available. Output is tightly packed rows.
    bool pop(ID3D11DeviceContext* ctx, std::vector<uint8_t>& out, uint64_t& tag);

private:
    struct Request { int texture, slot; uint64_t tag, serial; };
    std::vector<ComPtr<ID3D11Texture2D>> ring_;
    std::deque<Request> pending_;
    int open_ = 0, filled_ = 0, batch_ = 1;   // the texture being filled, and how many of its slots are
    uint64_t serial_ = 0;
    // the requests of one texture as they were when it was mapped last: serials from cachedFrom_
    std::vector<uint8_t> cache_;
    uint64_t cachedFrom_ = 0, cachedCount_ = 0;
    UINT width_ = 0, height_ = 0, bpp_ = 0;
};
