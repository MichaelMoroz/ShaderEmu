#include "material.h"

#include "gpu.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>

bool StageLayout::reflect(ID3D11Device* dev, ID3DBlob* bytecode, std::string& err) {
    present = true;
    ComPtr<ID3D11ShaderReflection> refl;
    HRESULT hr = D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_PPV_ARGS(&refl));
    if (FAILED(hr)) {
        err = "D3DReflect failed: " + hrToString(hr);
        return false;
    }
    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        refl->GetResourceBindingDesc(i, &bd);
        switch (bd.Type) {
            case D3D_SIT_CBUFFER: {
                if (std::string(bd.Name) != "$Globals") {
                    fprintf(stderr, "[harness] warning: cbuffer '%s' is not supported (only $Globals); it stays zero\n",
                            bd.Name);
                    break;
                }
                ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("$Globals");
                D3D11_SHADER_BUFFER_DESC cbd{};
                cb->GetDesc(&cbd);
                hasGlobals = true;
                globalsSlot = bd.BindPoint;
                globalsSize = cbd.Size;
                for (UINT v = 0; v < cbd.Variables; ++v) {
                    ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                    D3D11_SHADER_VARIABLE_DESC vd{};
                    var->GetDesc(&vd);
                    D3D11_SHADER_TYPE_DESC td{};
                    var->GetType()->GetDesc(&td);
                    ShaderVar sv;
                    sv.name = vd.Name;
                    sv.offset = vd.StartOffset;
                    sv.size = vd.Size;
                    sv.cls = td.Class;
                    sv.type = td.Type;
                    sv.rows = td.Rows;
                    sv.cols = td.Columns;
                    sv.elements = td.Elements;
                    vars.push_back(sv);
                }
                D3D11_BUFFER_DESC bdesc{};
                bdesc.ByteWidth = (globalsSize + 15) & ~15u;
                bdesc.Usage = D3D11_USAGE_DEFAULT;
                bdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                hr = dev->CreateBuffer(&bdesc, nullptr, &globalsBuffer);
                if (FAILED(hr)) {
                    err = "CreateBuffer($Globals) failed: " + hrToString(hr);
                    return false;
                }
                scratch.assign(bdesc.ByteWidth, 0);
                break;
            }
            case D3D_SIT_TEXTURE:
                textures.push_back({bd.Name, bd.BindPoint});
                break;
            case D3D_SIT_SAMPLER:
                samplers.push_back({bd.Name, bd.BindPoint});
                break;
            default:
                fprintf(stderr, "[harness] warning: unsupported resource '%s' (type %d)\n", bd.Name, (int)bd.Type);
                break;
        }
    }
    return true;
}

bool buildPasses(Gpu& gpu, const SLShader& shader, const std::vector<std::string>& passNames,
                 const ShaderBuildOptions& opt, std::vector<GpuPass>& out, std::string& err) {
    std::vector<const SLPass*> selected;
    if (passNames.empty()) {
        for (auto& p : shader.passes) selected.push_back(&p);
    } else {
        for (auto& n : passNames) {
            const SLPass* p = shader.findPass(n);
            if (!p) {
                err = "pass '" + n + "' not found in " + shader.path;
                return false;
            }
            selected.push_back(p);
        }
    }

    std::string rootDir = std::filesystem::u8path(shader.path).parent_path().u8string();
    struct Job {
        size_t passIdx;
        char stage;  // 'v', 'g', 'p'
        std::string entry;
        std::future<StageResult> fut;
    };
    std::vector<Job> jobs;
    for (size_t i = 0; i < selected.size(); ++i) {
        const SLPass& p = *selected[i];
        std::string sm = shaderModelSuffix(p.target);
        auto launch = [&](char stage, const std::string& entry, const char* prefix, const char* stageDefine) {
            std::string profile = std::string(prefix) + sm;
            Defines extra = {{stageDefine, "1"}};
            jobs.push_back({i, stage, entry,
                            std::async(std::launch::async, compileStage, p.code, shader.path, rootDir, entry, profile,
                                       extra, opt.settings)});
        };
        if (p.vertexEntry.empty() || p.fragmentEntry.empty()) {
            err = "pass '" + p.name + "' is missing #pragma vertex/fragment";
            return false;
        }
        launch('v', p.vertexEntry, "vs_", "SHADER_STAGE_VERTEX");
        if (!p.geometryEntry.empty()) launch('g', p.geometryEntry, "gs_", "SHADER_STAGE_GEOMETRY");
        launch('p', p.fragmentEntry, "ps_", "SHADER_STAGE_FRAGMENT");
    }
    fprintf(stderr, "[harness] compiling %zu shader stage(s) of '%s' (uncached FXC builds of large shaders can take minutes)...\n",
            jobs.size(), shader.name.c_str());

    out.clear();
    out.resize(selected.size());
    for (size_t i = 0; i < selected.size(); ++i) out[i].name = selected[i]->name;
    bool ok = true;
    for (auto& j : jobs) {
        StageResult r = j.fut.get();
        const char* stageName = j.stage == 'v' ? "vertex" : j.stage == 'g' ? "geometry" : "fragment";
        GpuPass& gp = out[j.passIdx];
        if (!r.ok) {
            fprintf(stderr, "[harness] pass '%s' %s (%s) FAILED:\n%s\n", gp.name.c_str(), stageName, j.entry.c_str(),
                    r.log.c_str());
            ok = false;
            continue;
        }
        fprintf(stderr, "[harness] pass '%s' %s: %s in %.1fs (%zu bytes)\n", gp.name.c_str(), stageName,
                r.fromCache ? "cache hit" : "compiled", r.seconds, (size_t)r.bytecode->GetBufferSize());
        if (opt.verbose && !r.log.empty()) fprintf(stderr, "%s\n", r.log.c_str());

        const void* bc = r.bytecode->GetBufferPointer();
        SIZE_T bcSize = r.bytecode->GetBufferSize();
        HRESULT hr = S_OK;
        StageLayout* layout = nullptr;
        if (j.stage == 'v') { hr = gpu.device->CreateVertexShader(bc, bcSize, nullptr, &gp.vs); layout = &gp.vsLayout; }
        else if (j.stage == 'g') { hr = gpu.device->CreateGeometryShader(bc, bcSize, nullptr, &gp.gs); layout = &gp.gsLayout; }
        else { hr = gpu.device->CreatePixelShader(bc, bcSize, nullptr, &gp.ps); layout = &gp.psLayout; }
        if (FAILED(hr)) {
            fprintf(stderr, "[harness] Create%sShader failed: %s\n", stageName, hrToString(hr).c_str());
            ok = false;
            continue;
        }
        std::string e;
        if (!layout->reflect(gpu.device.Get(), r.bytecode.Get(), e)) {
            fprintf(stderr, "[harness] %s\n", e.c_str());
            ok = false;
        }
    }
    if (!ok) err = "shader build failed";
    return ok;
}

namespace {

std::vector<double> parseNumberList(const std::string& s) {
    std::vector<double> v;
    const char* p = s.c_str();
    while (*p) {
        if (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.') {
            char* end = nullptr;
            double d = strtod(p, &end);
            if (end == p) { ++p; continue; }
            v.push_back(d);
            p = end;
        } else {
            ++p;
        }
    }
    return v;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void writeScalar(uint8_t* dst, D3D_SHADER_VARIABLE_TYPE type, double v) {
    switch (type) {
        case D3D_SVT_FLOAT: { float f = (float)v; memcpy(dst, &f, 4); break; }
        case D3D_SVT_INT: { int32_t i = (int32_t)(int64_t)v; memcpy(dst, &i, 4); break; }
        case D3D_SVT_UINT: { uint32_t u = (uint32_t)(int64_t)v; memcpy(dst, &u, 4); break; }
        case D3D_SVT_BOOL: { uint32_t b = v != 0 ? 1u : 0u; memcpy(dst, &b, 4); break; }
        default: break;
    }
}

}  // namespace

void Material::applyDefaults(const SLShader& shader) {
    for (auto& p : shader.properties) {
        std::string t = lower(p.type);
        if (t == "2d" || t == "3d" || t == "cube" || t == "2darray" || t == "any") continue;  // texture: default bound at draw
        std::vector<double> v = parseNumberList(p.defaultValue);
        if (!v.empty()) values_[p.name] = v;
    }
}

double Material::getFloat(const std::string& name, double fallback) const {
    auto it = values_.find(name);
    return it == values_.end() || it->second.empty() ? fallback : it->second[0];
}

void Material::setTexture(const std::string& name, ID3D11ShaderResourceView* srv, UINT width, UINT height) {
    textures_[name] = srv;
    setVector(name + "_TexelSize", 1.0 / width, 1.0 / height, width, height);
    if (!values_.count(name + "_ST")) setVector(name + "_ST", 1, 1, 0, 0);
}

void Material::bindStage(ID3D11DeviceContext* ctx, StageLayout& L, int stage, const Gpu& gpu) {
    if (!L.present) return;
    if (L.hasGlobals) {
        std::fill(L.scratch.begin(), L.scratch.end(), 0);
        for (auto& v : L.vars) {
            auto it = values_.find(v.name);
            if (it == values_.end()) continue;
            const std::vector<double>& vals = it->second;
            if (v.type == D3D_SVT_DOUBLE) continue;  // not used by Unity materials
            UINT comps = v.rows * v.cols;
            UINT elems = (std::max)(1u, v.elements);
            bool matrix = v.cls == D3D_SVC_MATRIX_COLUMNS || v.cls == D3D_SVC_MATRIX_ROWS;
            for (UINT e = 0; e < elems; ++e) {
                for (UINT c = 0; c < comps; ++c) {
                    size_t idx = (size_t)e * comps + c;
                    if (idx >= vals.size()) goto next_var;
                    UINT off;
                    if (matrix) {
                        // Values are given row-major (m[r][c] at r*cols+c).
                        UINT r = c / v.cols, col = c % v.cols;
                        if (v.cls == D3D_SVC_MATRIX_COLUMNS) off = v.offset + e * 16 * v.cols + col * 16 + r * 4;
                        else off = v.offset + e * 16 * v.rows + r * 16 + col * 4;
                    } else {
                        off = v.offset + e * 16 + c * 4;
                    }
                    if (off + 4 > v.offset + v.size || off + 4 > L.scratch.size()) goto next_var;
                    writeScalar(L.scratch.data() + off, v.type, vals[idx]);
                }
            }
        next_var:;
        }
        ctx->UpdateSubresource(L.globalsBuffer.Get(), 0, nullptr, L.scratch.data(), 0, 0);
        ID3D11Buffer* cb = L.globalsBuffer.Get();
        if (stage == 0) ctx->VSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else if (stage == 1) ctx->GSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else ctx->PSSetConstantBuffers(L.globalsSlot, 1, &cb);
    }
    for (auto& t : L.textures) {
        auto it = textures_.find(t.name);
        ID3D11ShaderResourceView* srv = it != textures_.end() && it->second ? it->second.Get() : gpu.blackTexture.Get();
        if (stage == 0) ctx->VSSetShaderResources(t.slot, 1, &srv);
        else if (stage == 1) ctx->GSSetShaderResources(t.slot, 1, &srv);
        else ctx->PSSetShaderResources(t.slot, 1, &srv);
    }
    for (auto& s : L.samplers) {
        auto it = linearSamplers_.find(s.name);
        bool linear = it != linearSamplers_.end() ? it->second : lower(s.name).find("linear") != std::string::npos;
        ID3D11SamplerState* ss = linear ? gpu.samplerLinear.Get() : gpu.samplerPoint.Get();
        if (stage == 0) ctx->VSSetSamplers(s.slot, 1, &ss);
        else if (stage == 1) ctx->GSSetSamplers(s.slot, 1, &ss);
        else ctx->PSSetSamplers(s.slot, 1, &ss);
    }
}

void Material::bind(ID3D11DeviceContext* ctx, GpuPass& pass, const Gpu& gpu) {
    ctx->VSSetShader(pass.vs.Get(), nullptr, 0);
    ctx->GSSetShader(pass.gs.Get(), nullptr, 0);
    ctx->PSSetShader(pass.ps.Get(), nullptr, 0);
    bindStage(ctx, pass.vsLayout, 0, gpu);
    bindStage(ctx, pass.gsLayout, 1, gpu);
    bindStage(ctx, pass.psLayout, 2, gpu);
}
