// A compiled ShaderLab pass (VS/GS/PS) plus a Unity-style Material: named uniforms and
// textures, bound by name through shader reflection. Values are converted to the type the
// shader declares (float/int/uint/bool), as Unity does, so the same property can be `float`
// in one pass and `uint` in another (rvc does exactly this with _Init).
#pragma once

#include "common.h"
#include "shader_compiler.h"
#include "shaderlab.h"

#include <d3d11.h>
#include <d3d11shader.h>

#include <map>
#include <string>
#include <vector>

struct Gpu;

struct ShaderVar {
    std::string name;
    UINT offset = 0, size = 0;
    D3D_SHADER_VARIABLE_CLASS cls{};
    D3D_SHADER_VARIABLE_TYPE type{};
    UINT rows = 1, cols = 1, elements = 0;
};

struct ShaderBinding {
    std::string name;
    UINT slot = 0;
};

struct StageLayout {
    bool present = false;
    bool hasGlobals = false;
    UINT globalsSlot = 0;
    UINT globalsSize = 0;
    std::vector<ShaderVar> vars;
    std::vector<ShaderBinding> textures;
    std::vector<ShaderBinding> samplers;
    std::vector<ShaderBinding> uavs;
    ComPtr<ID3D11Buffer> globalsBuffer;
    std::vector<uint8_t> scratch;

    bool reflect(ID3D11Device* dev, ID3DBlob* bytecode, std::string& err);
};

struct GpuPass {
    std::string name;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11PixelShader> ps;
    StageLayout vsLayout, gsLayout, psLayout;
};

struct ShaderBuildOptions {
    CompileSettings settings;
    bool verbose = false;
};

// Compiles all requested passes of a ShaderLab shader (stages in parallel).
// passNames empty = all passes.
bool buildPasses(Gpu& gpu, const SLShader& shader, const std::vector<std::string>& passNames,
                 const ShaderBuildOptions& opt, std::vector<GpuPass>& out, std::string& err);

class Material {
public:
    // Initialise values from the shader's Properties block defaults.
    void applyDefaults(const SLShader& shader);

    void setFloat(const std::string& name, double v) { values_[name] = {v}; }
    void setInt(const std::string& name, int64_t v) { values_[name] = {(double)v}; }
    void setVector(const std::string& name, double x, double y, double z, double w) { values_[name] = {x, y, z, w}; }
    void setValues(const std::string& name, std::vector<double> v) { values_[name] = std::move(v); }
    double getFloat(const std::string& name, double fallback = 0) const;

    // Also sets <name>_TexelSize and <name>_ST like Unity.
    void setTexture(const std::string& name, ID3D11ShaderResourceView* srv, UINT width, UINT height);
    void setUav(const std::string& name, ID3D11UnorderedAccessView* uav) { uavs_[name] = uav; }
    ID3D11UnorderedAccessView* uav(const std::string& name) const;
    void setLinearSampler(const std::string& samplerName, bool linear) { linearSamplers_[samplerName] = linear; }

    void bind(ID3D11DeviceContext* ctx, GpuPass& pass, const Gpu& gpu);
    // The same for a compute shader's layout (the caller sets the shader and its UAVs).
    void bindCompute(ID3D11DeviceContext* ctx, StageLayout& L, const Gpu& gpu) { bindStage(ctx, L, 3, gpu); }
    // Writes the current values into L.scratch in the stage's $Globals layout (no GPU calls).
    void fillGlobals(StageLayout& L) const;

private:
    void bindStage(ID3D11DeviceContext* ctx, StageLayout& L, int stage, const Gpu& gpu);
    std::map<std::string, std::vector<double>> values_;
    std::map<std::string, ComPtr<ID3D11ShaderResourceView>> textures_;
    std::map<std::string, bool> linearSamplers_;
    std::map<std::string, ComPtr<ID3D11UnorderedAccessView>> uavs_;
};
