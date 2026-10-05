// PNG/other image loading through WIC (built into Windows), returning raw 8-bit RGBA with no
// colour management, premultiplication or gamma, which is what binary-data textures need.
#pragma once

#include "common.h"

#include <d3d11.h>

struct ImageRGBA8 {
    UINT width = 0, height = 0;
    std::vector<uint8_t> pixels;  // row 0 = top row of the file
};

bool loadImageRGBA8(const std::string& path, ImageRGBA8& out, std::string& err);

// flipY = true reproduces Unity's Texture2D layout (row 0 = bottom row of the image),
// which shaders written for Unity (rvc included) assume when they index texels directly.
ComPtr<ID3D11ShaderResourceView> createTextureRGBA8(ID3D11Device* dev, const ImageRGBA8& img, bool flipY,
                                                    std::string& err);
