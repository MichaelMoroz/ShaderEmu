// Emulates a double-buffered Unity Custom Render Texture with update zones.
//
// Zones use Unity's pixel-space convention (y measured from the bottom), so values can be
// copied straight out of a .asset file. Each zone is drawn with the pass's own vertex shader
// (Unity's CustomRenderTextureVertexShader reads the CustomRenderTexture* uniforms we set).
//
// Semantics: every zone reads the complete previous state through _SelfTexture2D and writes
// its region. A full-texture zone renders into the back buffer and swaps; a partial zone
// renders into the back buffer and its region is copied back, so the current buffer is
// always complete.
#pragma once

#include "common.h"
#include "material.h"

#include <d3d11.h>

struct Gpu;

struct UpdateZone {
    float centerX, centerY;  // Unity pixel space, y up
    float width, height;
    int pass;                // index into the pass list given to run()
};

class CustomRenderTexture {
public:
    bool init(ID3D11Device* dev, UINT width, UINT height, DXGI_FORMAT format, std::string& err);
    void clear(ID3D11DeviceContext* ctx);
    void runZone(Gpu& gpu, GpuPass& pass, Material& mat, const UpdateZone& zone);

    ID3D11Texture2D* current() const { return tex_[cur_].Get(); }
    ID3D11ShaderResourceView* currentSRV() const { return srv_[cur_].Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }

private:
    ComPtr<ID3D11Texture2D> tex_[2];
    ComPtr<ID3D11ShaderResourceView> srv_[2];
    ComPtr<ID3D11RenderTargetView> rtv_[2];
    int cur_ = 0;
    UINT width_ = 0, height_ = 0;
};
