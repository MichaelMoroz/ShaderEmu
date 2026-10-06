// The machine's window. Left two thirds: the guest's display (docs/display.md), decoded from
// its framebuffer in RAM or taken from the GPU device. Right third: memory live, the RAM part of the state texture
// drawn as colour (the largest byte of each texel's words), with texels written since
// the view's previous frame glowing and fading. Beside it, the guest's display (docs/display.md),
// decoded from the framebuffer it keeps in RAM. A bar underneath, clear of the memory image,
// holds the speed counters and the 64x64 CPU state area magnified. It keeps its own copy of
// the texture to compare against.
// It only reads the state textures. Closing the window closes the view; rvc_harness then quits.
#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <string>
#include <vector>

// Pieces shared with the D3D12 view (rvc_memview12.h): shader source (entry points vs/ps,
// textures t0..t3 = current, previous, heat, text; cbuffer b0), window, text and BMP output.
const UINT kMemoryViewBar = 176;  // info bar: eight text lines, and the CPU state area at 160 px
const UINT kMemoryViewTextW = 1536, kMemoryViewTextH = 160;
// Columns of text in the bar under the picture: speed, console, input. The CPU state sits right of them.
const UINT kMemoryViewColumnX[3] = {0, 408, 1016};   // 48 and 72 characters wide
typedef std::vector<std::vector<std::string>> MemoryViewText;
const UINT kMemoryViewDispW = 1280, kMemoryViewMemW = 640, kMemoryViewHeight = 720;  // initial layout above the bar
struct MemoryViewConstants {
    float winSize[2];
    uint32_t frame, strips, texWidth, ramRows, stateRows;
    float inset;
    float textSize[2];
    float bar;
    float split;   // x where the display ends and the memory image begins
};
const char* memoryViewShader();
// What was typed into the window since the last call, as terminal bytes (arrows as escape
// sequences). Keys arrive when the window's messages are pumped, which render() does.
std::string memoryViewTakeKeys();
// WM_CLOSE sets the window property L"closed" instead of destroying the window.
HWND memoryViewCreateWindow();
// Rasterises the lines into a kMemoryViewTextW x kMemoryViewTextH BGRA bitmap, white on black.
bool memoryViewText(const MemoryViewText& columns, std::vector<uint8_t>& out, UINT& usedW, UINT& usedH);
bool memoryViewWriteBmp(const std::string& path, UINT width, UINT height, const uint8_t* rgba, size_t pitch);

class MemoryView {
public:
    // stateRows: rows at the top of the texture that are CPU state rather than RAM.
    bool init(ID3D11Device* dev, UINT texWidth, UINT texHeight, UINT stateRows, std::string& err);
    // Draws one frame from the current state texture and presents it.
    // Returns false once the window has been closed.
    // present = false leaves the frame in the back buffer for capture().
    // gpu: the GPU device's colour target, shown when the guest selects display mode 3.
    bool render(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* cur, bool present = true,
                ID3D11ShaderResourceView* gpu = nullptr);
    // Saves the back buffer as a 32-bit BMP; call after render(..., false).
    bool capture(ID3D11DeviceContext* ctx, const std::string& path);
    void setTitle(const std::string& title);
    // Lines of text shown in the info bar under the memory image (speed counters).
    void setText(ID3D11DeviceContext* ctx, const MemoryViewText& columns);
    bool open() const { return hwnd_ != nullptr; }
    void close();

private:
    bool createTargets(std::string& err);

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> dev_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    ComPtr<ID3D11Texture2D> backTex_;
    ComPtr<ID3D11Texture2D> refTex_;                 // the state as of the previous view frame
    ComPtr<ID3D11ShaderResourceView> refSrv_;
    bool refValid_ = false;
    ComPtr<ID3D11Texture2D> heatTex_[2];
    ComPtr<ID3D11ShaderResourceView> heatSrv_[2];
    ComPtr<ID3D11RenderTargetView> heatRtv_[2];
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> textTex_;                // text box, drawn with GDI
    ComPtr<ID3D11ShaderResourceView> textSrv_;
    UINT textW_ = 0, textH_ = 0;                     // used part of the text texture
    ComPtr<ID3D11RasterizerState> raster_;
    UINT width_ = 0, height_ = 0;
    UINT texWidth_ = 0, texHeight_ = 0, stateRows_ = 0;
    UINT frame_ = 0;
    int heatCur_ = 0;
    bool heatNeedsClear_ = true;
};
