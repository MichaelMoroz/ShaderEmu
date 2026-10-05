#include "image.h"

#include <wincodec.h>

#include <cstdio>
#include <cstring>

bool loadImageRGBA8(const std::string& path, ImageRGBA8& out, std::string& err) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { err = "WIC factory: " + hrToString(hr); return false; }
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { err = "cannot open image " + path + ": " + hrToString(hr); return false; }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { err = "cannot decode " + path + ": " + hrToString(hr); return false; }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    WICPixelFormatGUID fmt{};
    frame->GetPixelFormat(&fmt);
    ComPtr<IWICBitmapSource> src = frame;
    if (fmt != GUID_WICPixelFormat32bppRGBA) {
        ComPtr<IWICFormatConverter> conv;
        hr = factory->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr))
            hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) { err = "cannot convert " + path + " to RGBA8: " + hrToString(hr); return false; }
        // WIC hands 8-bit PNGs back as BGR(A); reordering channels is lossless.
        bool exact = fmt == GUID_WICPixelFormat32bppBGRA || fmt == GUID_WICPixelFormat24bppBGR ||
                     fmt == GUID_WICPixelFormat24bppRGB;
        if (!exact)
            fprintf(stderr, "[harness] note: %s is not 8-bit RGB(A); converted (data may not be bit-exact)\n", path.c_str());
        src = conv;
    }
    out.width = w;
    out.height = h;
    out.pixels.resize((size_t)w * h * 4);
    hr = src->CopyPixels(nullptr, w * 4, (UINT)out.pixels.size(), out.pixels.data());
    if (FAILED(hr)) { err = "CopyPixels failed for " + path + ": " + hrToString(hr); return false; }
    return true;
}

ComPtr<ID3D11ShaderResourceView> createTextureRGBA8(ID3D11Device* dev, const ImageRGBA8& img, bool flipY,
                                                    std::string& err) {
    std::vector<uint8_t> data;
    const uint8_t* p = img.pixels.data();
    size_t pitch = (size_t)img.width * 4;
    if (flipY) {
        data.resize(img.pixels.size());
        for (UINT y = 0; y < img.height; ++y)
            memcpy(data.data() + y * pitch, img.pixels.data() + (img.height - 1 - y) * pitch, pitch);
        p = data.data();
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.width;
    td.Height = img.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{p, (UINT)pitch, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) {
        err = "creating texture failed: " + hrToString(hr);
        return nullptr;
    }
    return srv;
}
