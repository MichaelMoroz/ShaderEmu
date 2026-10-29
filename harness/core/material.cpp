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
                hr = dev ? dev->CreateBuffer(&bdesc, nullptr, &globalsBuffer) : S_OK;  // null: layout only
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
            case D3D_SIT_UAV_RWSTRUCTURED:
                uavs.push_back({bd.Name, bd.BindPoint});
                break;
            default:
                fprintf(stderr, "[harness] warning: unsupported resource '%s' (type %d)\n", bd.Name, (int)bd.Type);
                break;
        }
    }
    return true;
}

// DXBC's checksum: MD5's rounds over the container after its first 20 bytes, with DXBC's own last block.
static void md5Block(uint32_t s[4], const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    memcpy(m, p, 64);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d; d = c; c = b;
        uint32_t x = a + f + K[i] + m[g];
        b = b + ((x << R[i]) | (x >> (32 - R[i])));
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}
static void dxbcChecksum(const uint8_t* data, size_t size, uint32_t out[4]) {
    const uint8_t* p = data + 20;
    uint32_t n = (uint32_t)(size - 20), bits = n * 8, bits2 = (bits >> 2) | 1;
    uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    size_t full = n / 64 * 64, left = n % 64;
    for (size_t i = 0; i < full; i += 64) md5Block(s, p + i);
    uint8_t blk[64] = {};
    if (left >= 56) {
        memcpy(blk, p + full, left);
        blk[left] = 0x80;
        md5Block(s, blk);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        memcpy(blk + 60, &bits2, 4);
        md5Block(s, blk);
    } else {
        memcpy(blk, &bits, 4);
        memcpy(blk + 4, p + full, left);
        blk[4 + left] = 0x80;
        memcpy(blk + 60, &bits2, 4);
        md5Block(s, blk);
    }
    memcpy(out, s, 16);
}
// Sets D3D11_SB_GLOBALLY_COHERENT_ACCESS on every dcl_uav_typed of the shader's code chunk and signs the container
// again: how many it set, or -1 when the checksum computed is not the one the compiler wrote (then nothing is made).
static int dxbcGloballyCoherent(const uint8_t* bc, size_t size, std::vector<uint8_t>& out) {
    uint32_t sum[4];
    dxbcChecksum(bc, size, sum);
    if (size < 32 || memcmp(bc, "DXBC", 4) != 0 || memcmp(sum, bc + 4, 16) != 0) return -1;
    out.assign(bc, bc + size);
    uint32_t chunks;
    memcpy(&chunks, bc + 28, 4);
    int set = 0;
    for (uint32_t c = 0; c < chunks; c++) {
        uint32_t off;
        memcpy(&off, bc + 32 + 4 * c, 4);
        if (memcmp(bc + off, "SHEX", 4) != 0 && memcmp(bc + off, "SHDR", 4) != 0) continue;
        uint32_t* tok = (uint32_t*)(out.data() + off + 8);
        uint32_t len = tok[1];
        for (uint32_t i = 2; i < len;) {
            uint32_t op = tok[i] & 0x7ff, n = (tok[i] >> 24) & 0x7f;
            if (op == 0x35) n = tok[i + 1];       // customdata: its length in the next token
            if (op == 0x9c) { tok[i] |= 0x00010000u; set++; }   // dcl_uav_typed
            if (n == 0) break;
            i += n;
        }
    }
    dxbcChecksum(out.data(), out.size(), sum);
    memcpy(out.data() + 4, sum, 16);
    return set;
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
        char stage;  // 'v', 'g', 'p', 'h', 'd'
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
        if (!p.hullEntry.empty()) launch('h', p.hullEntry, "hs_", "SHADER_STAGE_HULL");
        if (!p.domainEntry.empty()) launch('d', p.domainEntry, "ds_", "SHADER_STAGE_DOMAIN");
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
        const char* stageName = j.stage == 'v' ? "vertex" : j.stage == 'g' ? "geometry" : j.stage == 'h' ? "hull" : j.stage == 'd' ? "domain" : "fragment";
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
        std::vector<uint8_t> glc;   // RVC_UAV_GLC=1 (rvc_harness --uav-coherent): typed UAVs made globallycoherent
        if (j.stage == 'p' && getenv("RVC_UAV_GLC") && atoi(getenv("RVC_UAV_GLC")) != 0) {
            int n = dxbcGloballyCoherent((const uint8_t*)bc, bcSize, glc);
            if (n < 0) fprintf(stderr, "[harness] RVC_UAV_GLC: pass '%s': the bytecode's checksum is not the one computed: left as it is\n", gp.name.c_str());
            else if (n > 0) {
                fprintf(stderr, "[harness] RVC_UAV_GLC: pass '%s': %d typed UAV(s) made globallycoherent\n", gp.name.c_str(), n);
                bc = glc.data();
                bcSize = glc.size();
            }
        }
        HRESULT hr = S_OK;
        StageLayout* layout = nullptr;
        if (j.stage == 'v') { hr = gpu.device->CreateVertexShader(bc, bcSize, nullptr, &gp.vs); layout = &gp.vsLayout; }
        else if (j.stage == 'g') { hr = gpu.device->CreateGeometryShader(bc, bcSize, nullptr, &gp.gs); layout = &gp.gsLayout; }
        else if (j.stage == 'h') { hr = gpu.device->CreateHullShader(bc, bcSize, nullptr, &gp.hs); layout = &gp.hsLayout; }
        else if (j.stage == 'd') { hr = gpu.device->CreateDomainShader(bc, bcSize, nullptr, &gp.ds); layout = &gp.dsLayout; }
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

ID3D11UnorderedAccessView* Material::uav(const std::string& name) const {
    auto it = uavs_.find(name);
    return it == uavs_.end() ? nullptr : it->second.Get();
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

void Material::fillGlobals(StageLayout& L) const {
    if (!L.present || !L.hasGlobals) return;
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
}

// RVC_DUMP_CB=FILE: the pixel stage's constants of the pass bound while dumpConstants is set,
// written each time (the last one stays): what tools/dxbc_run.py runs the tick's bytecode with.
bool Material::dumpConstants = false;

void Material::bindStage(ID3D11DeviceContext* ctx, StageLayout& L, int stage, const Gpu& gpu) {
    if (!L.present) return;
    if (L.hasGlobals) {
        fillGlobals(L);
        if (dumpConstants && stage == 2) {
            if (const char* path = getenv("RVC_DUMP_CB")) {
                if (FILE* f = fopen(path, "wb")) { fwrite(L.scratch.data(), 1, L.scratch.size(), f); fclose(f); }
            }
        }
        ctx->UpdateSubresource(L.globalsBuffer.Get(), 0, nullptr, L.scratch.data(), 0, 0);
        ID3D11Buffer* cb = L.globalsBuffer.Get();
        if (stage == 0) ctx->VSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else if (stage == 1) ctx->GSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else if (stage == 3) ctx->CSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else if (stage == 4) ctx->HSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else if (stage == 5) ctx->DSSetConstantBuffers(L.globalsSlot, 1, &cb);
        else ctx->PSSetConstantBuffers(L.globalsSlot, 1, &cb);
    }
    for (auto& t : L.textures) {
        auto it = textures_.find(t.name);
        ID3D11ShaderResourceView* srv = it != textures_.end() && it->second ? it->second.Get() : gpu.blackTexture.Get();
        if (stage == 0) ctx->VSSetShaderResources(t.slot, 1, &srv);
        else if (stage == 1) ctx->GSSetShaderResources(t.slot, 1, &srv);
        else if (stage == 3) ctx->CSSetShaderResources(t.slot, 1, &srv);
        else if (stage == 4) ctx->HSSetShaderResources(t.slot, 1, &srv);
        else if (stage == 5) ctx->DSSetShaderResources(t.slot, 1, &srv);
        else ctx->PSSetShaderResources(t.slot, 1, &srv);
    }
    for (auto& s : L.samplers) {
        auto it = linearSamplers_.find(s.name);
        bool linear = it != linearSamplers_.end() ? it->second : lower(s.name).find("linear") != std::string::npos;
        ID3D11SamplerState* ss = linear ? gpu.samplerLinear.Get() : gpu.samplerPoint.Get();
        if (stage == 0) ctx->VSSetSamplers(s.slot, 1, &ss);
        else if (stage == 1) ctx->GSSetSamplers(s.slot, 1, &ss);
        else if (stage == 3) ctx->CSSetSamplers(s.slot, 1, &ss);
        else if (stage == 4) ctx->HSSetSamplers(s.slot, 1, &ss);
        else if (stage == 5) ctx->DSSetSamplers(s.slot, 1, &ss);
        else ctx->PSSetSamplers(s.slot, 1, &ss);
    }
}

void Material::bind(ID3D11DeviceContext* ctx, GpuPass& pass, const Gpu& gpu) {
    ctx->VSSetShader(pass.vs.Get(), nullptr, 0);
    ctx->GSSetShader(pass.gs.Get(), nullptr, 0);
    ctx->PSSetShader(pass.ps.Get(), nullptr, 0);
    // (null for every pass that has none: a pass of patches leaves them bound otherwise)
    ctx->HSSetShader(pass.hs.Get(), nullptr, 0);
    ctx->DSSetShader(pass.ds.Get(), nullptr, 0);
    bindStage(ctx, pass.vsLayout, 0, gpu);
    bindStage(ctx, pass.gsLayout, 1, gpu);
    bindStage(ctx, pass.psLayout, 2, gpu);
    bindStage(ctx, pass.hsLayout, 4, gpu);
    bindStage(ctx, pass.dsLayout, 5, gpu);
}
