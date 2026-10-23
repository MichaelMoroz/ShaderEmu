// gvmbench: gpu_vm's tick (gpu_vm/tick/tick.hlsl) on D3D11, every case checked against the reference machine
// (gpu_vm/ref/gvm_ref.h), and the loops timed by the GPU's clock.
//
//   gvmbench [--warp] [--case NAME[,NAME]] [--ticks N] [--passes N] [--seed N] [-D NAME=VALUE]...
//
// A case is a program for each of its cores, assembled with gpu_vm/ref/gvm_asm.h at GVM_RAM, and data. Both
// machines run it in passes of --ticks bundles (a check runs it in short passes too, so that passes end
// inside bundles and on a full write cache) until every core has halted (break), and then registers, pc
// and every word of RAM must agree. A case marked timed is timed first: --passes passes of 8,192 bundles.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <algorithm>
#include <vector>
#define GVM_REF_IMPL
#include "../ref/gvm_ref.h"
#include "../ref/gvm_asm.h"

#include "cases.h"
static int g_romrows = 0;   // (probe: the ROM in RAM's texture)
static uint32_t g_wcn1 = 0;    // a step's WCN1 (x19g): a worker's entries, the commit's points for it (0: WCN)
static uint32_t g_ntex = 0;    // a step's NTEX (x22): its state texels (0: T_WC + WCN / 2)
static uint32_t g_geo = 0;     // a step's GEO_BLOCKS (x21): pixel p of core c at ((c & 127) * 8 + p / 8, (c >> 7) * 16 + p % 8)
static uint32_t g_wcn0 = 0;    // a step's WCN0 (x25): core 0's own write cache, its tick a compile of its own (CORE0=1)
static uint32_t g_spix0 = 0;   // and its state pixels (NTEX0): 0-63 in its block, 64 on at ((p >> 3) - 8, 8 + (p & 7))
struct Geo { uint32_t bw = 0, bh = 0, band = 0, per = 0, jrow = 0, perm = 0, mul = 0, inv = 0; uint32_t cpts = 0, xr = 0, npair = 0, xbind = 0, xfast = 0, t_xf = 0, t_xt = 0, t_pn = 23, nc = 0, thin = 0, thin8 = 0, wide3 = 0, late = 0, fx1 = 0, wsh = 0, wad = 0, vec = 0, t_vr = 0; };
static Geo g_gg;               // a built step's layout (builder/gen.py: GEO_BW...): core c's pixel j at (slot * bw + jx, band * BAND + jy)
static uint32_t g_lrows = 1;   // a core's state pixels on this many rows (a step's LROWS, x19): pixel p at (p % spx, core * LROWS + p / spx)

// ---- D3D11
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("%s failed: %08lx\n", what, (unsigned long)hr); exit(1); }
}
struct Tex { ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11UnorderedAccessView* uav = nullptr; UINT w = 0, h = 0; };
static bool g_uav_tex = false;   // (x19 probe: the state textures also UAVs, a tick whose one lane writes a core's state)
static Tex make_tex(UINT w, UINT h, const void* init) {
    Tex t; t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (g_uav_tex ? D3D11_BIND_UNORDERED_ACCESS : 0u), 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {init, w * 16, 0};
    check(dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.t), "CreateTexture2D");
    check(dev->CreateShaderResourceView(t.t, nullptr, &t.srv), "SRV");
    check(dev->CreateRenderTargetView(t.t, nullptr, &t.rtv), "RTV");
    if (g_uav_tex) check(dev->CreateUnorderedAccessView(t.t, nullptr, &t.uav), "UAV");
    return t;
}
static void free_tex(Tex& t) { if (t.t) { t.srv->Release(); t.rtv->Release(); if (t.uav) t.uav->Release(); t.t->Release(); t = Tex(); } }
static std::vector<uint32_t> read_tex(const Tex& t) {
    D3D11_TEXTURE2D_DESC d = {t.w, t.h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D* st;
    check(dev->CreateTexture2D(&d, nullptr, &st), "staging");
    ctx->CopyResource(st, t.t);
    D3D11_MAPPED_SUBRESOURCE m;
    check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint32_t> out((size_t)t.w * t.h * 4);
    for (UINT y = 0; y < t.h; y++) memcpy(&out[(size_t)y * t.w * 4], (char*)m.pData + (size_t)y * m.RowPitch, t.w * 16);
    ctx->Unmap(st, 0);
    st->Release();
    return out;
}
static std::string slurp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
// fxc2's flags: D3DCOMPILE_OPTIMIZATION_LEVEL3, or a variant's -D FXC=N (a probe of the compiler's other forms)
static UINT fxc_flags(const std::vector<D3D_SHADER_MACRO>& defs) {
    for (auto& d : defs) if (d.Name && !strcmp(d.Name, "FXC")) return (UINT)strtoul(d.Definition, nullptr, 0);
    return D3DCOMPILE_OPTIMIZATION_LEVEL3;
}
// compiled shaders are kept in bin\gvmcache by a hash of the tick, isa.h, the defines and the entry
static ID3DBlob* compile(const char* path, const char* entry, const char* profile, const std::vector<D3D_SHADER_MACRO>& defs) {
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::string key = slurp(path) + slurp(dir + "/../isa.h") + entry + profile;
    char dll[MAX_PATH] = "";   // the compiler that makes it (fxc2 or Microsoft's FXC)
    GetModuleFileNameA(GetModuleHandleA("d3dcompiler_47.dll"), dll, MAX_PATH);
    key += std::to_string(slurp(dll).size());
    for (auto& d : defs) if (d.Name) key += std::string(d.Name) + "=" + d.Definition + ";";
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    char name[64];
    snprintf(name, sizeof name, "%016llx.cso", (unsigned long long)h);
    std::string cdir = dir + "/../../bin/gvmcache";
    CreateDirectoryA(cdir.c_str(), nullptr);
    std::string cfile = cdir + "/" + name, got = slurp(cfile);
    ID3DBlob* code = nullptr;
    if (!got.empty()) {
        check(D3DCreateBlob(got.size(), &code), "D3DCreateBlob");
        memcpy(code->GetBufferPointer(), got.data(), got.size());
        return code;
    }
    std::vector<wchar_t> wpath(strlen(path) + 1);
    mbstowcs(wpath.data(), path, wpath.size());
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompileFromFile(wpath.data(), defs.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                                    fxc_flags(defs), 0, &code, &err);
    if (err && (FAILED(hr) || getenv("GVM_WARNINGS"))) printf("%s", (const char*)err->GetBufferPointer());
    check(hr, entry);
    if (FILE* f = fopen(cfile.c_str(), "wb")) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
    return code;
}
struct Prog { ID3D11VertexShader* vs; ID3D11PixelShader* ps; };
static Prog program(const char* path, const char* v, const char* p, const std::vector<D3D_SHADER_MACRO>& defs) {
    Prog r;
    ID3DBlob *vb = compile(path, v, "vs_5_0", defs), *pb = compile(path, p, "ps_5_0", defs);
    check(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &r.vs), v);
    check(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &r.ps), p);
    if (!strcmp(p, "frag")) {
        printf("gvmbench: the tick's fragment shader is %zu bytes\n", pb->GetBufferSize());
        {   // D3D11 allows 4,096 temporary registers, an indexable array's element taking one: past it the card removes the
            // device (x25's first core 0 tick: 5,375). Counted from the disassembly (Windows' own compiler's).
            ID3DBlob* text = nullptr;
            HMODULE sys = LoadLibraryA("C:\\Windows\\System32\\d3dcompiler_47.dll");
            auto dis = sys ? (decltype(&D3DDisassemble))GetProcAddress(sys, "D3DDisassemble") : nullptr;
            if (dis && SUCCEEDED(dis(pb->GetBufferPointer(), pb->GetBufferSize(), 0, nullptr, &text))) {
                std::string t((const char*)text->GetBufferPointer(), text->GetBufferSize());
                size_t regs = 0, at = 0;
                while ((at = t.find("dcl_temps ", at)) != std::string::npos) regs += strtoul(t.c_str() + (at += 10), nullptr, 10);
                at = 0;
                while ((at = t.find("dcl_indexableTemp x", at)) != std::string::npos) { at = t.find('[', at) + 1; regs += strtoul(t.c_str() + at, nullptr, 10); }
                text->Release();
                printf("gvmbench: %zu temporary registers\n", regs);
                if (regs > 4096) { printf("gvmbench: more than D3D11's 4,096 temporary registers: not run\n"); exit(1); }
            }
        }
        if (const char* out = getenv("GVM_DISASM")) {   // its DXBC, for counting the loop's instructions
            ID3DBlob* text = nullptr;   // (fxc2 has no disassembler: Windows' own compiler's)
            HMODULE sys = LoadLibraryA("C:\\Windows\\System32\\d3dcompiler_47.dll");
            auto dis = sys ? (decltype(&D3DDisassemble))GetProcAddress(sys, "D3DDisassemble") : nullptr;
            if (dis && SUCCEEDED(dis(pb->GetBufferPointer(), pb->GetBufferSize(), 0, nullptr, &text))) {
                if (FILE* f = fopen(out, "wb")) { fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, f); fclose(f); }
                text->Release();
            } else printf("gvmbench: no disassembly\n");
        }
    }
    vb->Release(); pb->Release();
    return r;
}

// ---- The GPU's machine
struct Gpu {
    uint32_t cores, statePix, spx;   // (spx: the state textures' width, statePix / LROWS rounded up)
    Tex ram, rom, stage, ops[2], set[2][8];
    ID3D11Buffer* code = nullptr;   // t12: RAM's first image as a StructuredBuffer<uint4> (a probe's code fetched from a buffer)
    ID3D11ShaderResourceView* codeSrv = nullptr;
    int cur = 0, oc = 0;
    Prog tick, commit, stg, wrt, prog, any, tickW, tick0;   // (tickW: the workers' rows' tick, a step's WORKER=1, x19; tick0: core 0's, CORE0=1, x25)
    bool hasCommit = true, hasOps = true, hasAny = false, uavTick = false, hasW = false;   // (uavTick: x19's probe, fragu)   // (hasAny: the step's op_any draw, x19: the rounds predicated on it)
    ID3D11Predicate* pred = nullptr;   // an occlusion predicate: some core has an operation queued
    Tex dummy;                         // op_any's 1 x 1 target (nothing is written: its blend state masks every component)
    ID3D11BlendState* none = nullptr;
    ID3D11Buffer *cb, *ccb[4];
    ID3D11BlendState* mask[4];
    uint32_t opSlots = 32, opSpan = 272, opRounds = 2;   // the operations' stage (a step's OP_SLOTS, OP_SPAN) and rounds a pass
    struct { uint32_t ticks, ramEnd, timeLo, timeHi, player, flags, ramRows, ext0, ncores, pad[3], ext[8]; } pass;   // (x17's PassParams)
    size_t pix(uint32_t c, uint32_t p) const {   // the place of core c's state pixel p in a state texture
        if (g_gg.bw) {
            uint32_t i = c % g_gg.per, s = i;
            if (g_gg.perm == 1) { uint32_t lg = 0; while ((1u << lg) < g_gg.per) lg++; s = 0; for (uint32_t k = 0; k < lg; k++) s |= ((i >> k) & 1) << (lg - 1 - k); }
            else if (g_gg.perm == 2) s = (i * g_gg.mul) & (g_gg.per - 1);
            uint32_t jx = g_gg.jrow ? p % g_gg.bw : p / g_gg.bh, jy = g_gg.jrow ? p / g_gg.bw : p % g_gg.bh;
            return (size_t)((c / g_gg.per) * g_gg.band + jy) * texW() + s * g_gg.bw + jx;
        }
        if (g_geo && c == 0 && p >= 64) return (size_t)(8 + p % 8) * texW() + p / 8 - 8;
        if (g_geo) return (size_t)((c >> 7) * 16 + p % 8) * texW() + (c & 127) * 8 + p / 8;
        return (size_t)(c * g_lrows + p / spx) * spx + p % spx;
    }
    UINT texW() const { if (g_gg.bw) return g_gg.per * g_gg.bw; return g_geo ? std::max((cores < 128 ? cores : 128) * 8, g_spix0 > 64 ? (g_spix0 - 64 + 7) / 8 : 0u) : spx; }
    UINT texH() const { if (g_gg.bw) return ((cores - 1) / g_gg.per) * g_gg.band + g_gg.bh; return g_geo ? ((cores - 1) / 128) * 16 + (g_spix0 > 64 ? 16 : 8) : cores * g_lrows; }
    void make(uint32_t ncores, const std::vector<uint32_t>& ramImg, const std::vector<uint32_t>& romImg, const std::vector<std::vector<uint32_t>>& state) {
        cores = ncores;
        statePix = ((g_ntex ? g_ntex : g_twc + g_wcn / 2) + 7) / 8;
        spx = (statePix + g_lrows - 1) / g_lrows;
        if (g_romrows) {   // (probe: the ROM in RAM's texture, rows from RamRows on)
            std::vector<uint32_t> both(ramImg);
            both.insert(both.end(), romImg.begin(), romImg.end());
            ram = make_tex(g_texw, ram_rows() + rom_rows(), both.data());
        } else ram = make_tex(g_texw, ram_rows(), ramImg.data());
        {
            D3D11_BUFFER_DESC bd = {(UINT)(ramImg.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
            D3D11_SUBRESOURCE_DATA sd = {ramImg.data(), 0, 0};
            check(dev->CreateBuffer(&bd, &sd, &code), "code buffer");
            check(dev->CreateShaderResourceView(code, nullptr, &codeSrv), "code SRV");
        }
        rom = make_tex(g_texw, rom_rows(), romImg.data());
        std::vector<uint32_t> z((size_t)opSpan * cores * opSlots * 4, 0);
        stage = make_tex(opSpan, cores * opSlots, z.data());
        std::vector<uint32_t> oz((size_t)cores * 4, 0);
        for (int k = 0; k < 2; k++) ops[k] = make_tex(cores, 1, oz.data());
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) {
                g_uav_tex = uavTick;
                std::vector<uint32_t> img((size_t)texW() * texH() * 4, 0);
                for (uint32_t c = 0; c < cores; c++)
                    for (uint32_t tx = t; tx < statePix * 8 && tx < state[c].size() / 4; tx += 8)
                        memcpy(&img[pix(c, tx / 8) * 4], &state[c][tx * 4], 16);
                set[k][t] = make_tex(texW(), texH(), img.data());
                g_uav_tex = false;
            }
        cur = 0; oc = 0;
    }
    void release() {
        free_tex(ram); free_tex(rom); free_tex(stage);
        if (code) { codeSrv->Release(); code->Release(); code = nullptr; codeSrv = nullptr; }
        for (auto& t : ops) free_tex(t);
        for (auto& s : set) for (auto& t : s) free_tex(t);
    }
    void clear_srv() {
        ID3D11ShaderResourceView* none[13] = {};
        ctx->PSSetShaderResources(0, 13, none);
        ctx->VSSetShaderResources(0, 13, none);
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    }
    void bind(const Tex& state0, int which, bool withRam, bool withStage) {   // t0 RAM, t1 ROM, t2-t9 a state set, t10 ops, t11 stage
        ID3D11ShaderResourceView* s[13] = {};
        if (withRam) s[0] = ram.srv;
        s[1] = rom.srv;
        for (int t = 0; t < 8; t++) s[2 + t] = set[which][t].srv;
        s[10] = ops[oc].srv;
        if (withStage) s[11] = stage.srv;
        s[12] = codeSrv;
        ctx->PSSetShaderResources(0, 13, s);
        ctx->VSSetShaderResources(0, 13, s);
        (void)state0;
    }
    void viewport(UINT w, UINT h, UINT y = 0, UINT x = 0) { D3D11_VIEWPORT v = {(float)x, (float)y, (float)w, (float)h, 0, 1}; ctx->RSSetViewports(1, &v); }
    void run_pass() {
        ctx->UpdateSubresource(cb, 0, nullptr, &pass, 0, 0);
        ID3D11Buffer* cbs[2] = {cb, ccb[0]};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->IASetInputLayout(nullptr);
        // the tick: set cur from set cur ^ 1
        clear_srv();
        ID3D11RenderTargetView* rt[8];
        for (int t = 0; t < 8; t++) rt[t] = set[cur][t].rtv;
        if (uavTick) {   // (one lane a core writes its state as UAVs)
            ID3D11UnorderedAccessView* u[8];
            for (int t = 0; t < 8; t++) u[t] = set[cur][t].uav;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, u, nullptr);
        } else ctx->OMSetRenderTargets(8, rt, nullptr);
        bind(set[cur ^ 1][0], cur ^ 1, true, false);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(tick.vs, nullptr, 0); ctx->PSSetShader(tick.ps, nullptr, 0);
        static const int drawMode = env_int("GVM_DRAW");   // (a measure: 1 a draw per core, as the harness's runs between frozen
                                                           // workers; 2 core 0 apart first, 3 last, the rest band by band; perm id)
        if (g_gg.bw && drawMode == 1) {
            for (uint32_t c = 0; c < cores; c++) {
                viewport(g_gg.bw, g_gg.bh, (c / g_gg.per) * g_gg.band, (uint32_t)(pix(c, 0) % texW()) / g_gg.bw * g_gg.bw);
                ctx->Draw(3, 0);
            }
        } else if (g_gg.bw && drawMode >= 2 && !g_gg.perm) {
            if (drawMode == 2) { viewport(g_gg.bw, g_gg.bh); ctx->Draw(3, 0); }
            for (uint32_t b = 0; b * g_gg.per < cores; b++) {
                uint32_t c0 = b ? b * g_gg.per : 1, n = std::min(g_gg.per, cores - b * g_gg.per) - (b ? 0 : 1);
                if (!n) continue;
                viewport(n * g_gg.bw, g_gg.bh, b * g_gg.band, (c0 % g_gg.per) * g_gg.bw);
                ctx->Draw(3, 0);
            }
            if (drawMode == 3) { viewport(g_gg.bw, g_gg.bh); ctx->Draw(3, 0); }
        } else if (g_gg.bw) {   // a built step: band by band (the whole band when its blocks are permuted)
            for (uint32_t b = 0; b * g_gg.per < cores; b++) {
                uint32_t n = g_gg.perm ? g_gg.per : std::min(g_gg.per, cores - b * g_gg.per);
                viewport(n * g_gg.bw, g_gg.bh, b * g_gg.band);
                ctx->Draw(3, 0);
            }
        } else if (g_wcn0) {   // x25: core 0 by its own tick (its block, then its pixels in rows 8-15), the workers band by band
            ctx->PSSetShader(tick0.ps, nullptr, 0);
            viewport(8, 8);
            ctx->Draw(3, 0);
            if (g_spix0 > 64) {
                viewport((g_spix0 - 64 + 7) / 8, 8, 8);
                ctx->Draw(3, 0);
            }
            ctx->PSSetShader(tick.ps, nullptr, 0);
            for (uint32_t b = 0; b * 128 < cores; b++) {
                uint32_t c0 = b == 0 ? 1 : b * 128, c1 = std::min(cores, (b + 1) * 128);
                if (c1 <= c0) continue;
                viewport((c1 - c0) * 8, 8, b * 16, (c0 % 128) * 8);
                ctx->Draw(3, 0);
            }
        } else if (hasW && cores > 1) {   // x19: core 0's row by the tick, the workers' rows by the workers' tick
            viewport(spx, g_lrows);
            ctx->Draw(3, 0);
            viewport(spx, (cores - 1) * g_lrows, g_lrows);
            ctx->PSSetShader(tickW.ps, nullptr, 0);
            ctx->Draw(3, 0);
        } else if (g_geo) {   // a band of 128 blocks at a time, its 8 rows
            for (uint32_t b = 0; b * 128 < cores; b++) {
                viewport((cores - b * 128 < 128 ? cores - b * 128 : 128) * 8, 8, b * 16);
                ctx->Draw(3, 0);
            }
        } else {
            viewport(uavTick ? 1 : spx, cores * g_lrows);
            ctx->Draw(3, 0);
        }
        if (!hasCommit) { clear_srv(); cur ^= 1; return; }
        // the commit: four draws, a component each
        clear_srv();
        ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
        bind(set[cur][0], cur, false, false);
        viewport(g_texw, ram_rows());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(commit.vs, nullptr, 0); ctx->PSSetShader(commit.ps, nullptr, 0);
        for (int c = 0; c < 4; c++) {
            ID3D11Buffer* b2[2] = {cb, ccb[c]};
            ctx->VSSetConstantBuffers(0, 2, b2);
            ctx->OMSetBlendState(mask[c], nullptr, 0xffffffff);
            ctx->Draw(g_gg.cpts ? cores * g_gg.cpts : g_wcn0 ? g_wcn0 + (cores - 1) * g_wcn : g_wcn1 ? g_wcn + (cores - 1) * g_wcn1 : cores * g_wcn, 0);   // (x19g: a worker's WCN1 entries; x25: core 0's WCN0)
        }
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        if (!hasOps) { clear_srv(); cur ^= 1; return; }
        if (hasAny) {   // x19: no operation queued anywhere, no rounds: the GPU skips them by the predicate (no readback)
            if (!pred) {
                D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION_PREDICATE, 0};
                check(dev->CreatePredicate(&qd, &pred), "predicate");
                dummy = make_tex(1, 1, nullptr);
                D3D11_BLEND_DESC b = {};
                b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
                b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                b.RenderTarget[0].RenderTargetWriteMask = 0;
                check(dev->CreateBlendState(&b, &none), "blend none");
            }
            clear_srv();
            ctx->OMSetRenderTargets(1, &dummy.rtv, nullptr);
            ctx->OMSetBlendState(none, nullptr, 0xffffffff);
            bind(set[cur][0], cur, false, false);
            viewport(1, 1);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ctx->VSSetShader(any.vs, nullptr, 0); ctx->PSSetShader(any.ps, nullptr, 0);
            ctx->Begin(pred);
            ctx->Draw(cores, 0);
            ctx->End(pred);
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->SetPredication(pred, FALSE);   // the rounds' draws are skipped when no sample passed
        }
        // the operations (gpu_vm/tick/STATE.md): rounds of stage, write, progress; _OpState ping-pongs a round
        for (uint32_t round = 0; round < opRounds; round++) {
            clear_srv();
            ctx->OMSetRenderTargets(1, &stage.rtv, nullptr);
            bind(set[cur][0], cur, true, false);
            viewport(opSpan, cores * opSlots);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(stg.vs, nullptr, 0); ctx->PSSetShader(stg.ps, nullptr, 0);
            ctx->DrawInstanced(6, cores * opSlots, 0, 0);
            clear_srv();
            ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
            bind(set[cur][0], cur, false, true);
            viewport(g_texw, ram_rows());
            ctx->VSSetShader(wrt.vs, nullptr, 0); ctx->PSSetShader(wrt.ps, nullptr, 0);
            ctx->DrawInstanced(12, cores * opSlots, 0, 0);
            clear_srv();
            ctx->OMSetRenderTargets(1, &ops[oc ^ 1].rtv, nullptr);
            bind(set[cur][0], cur, false, false);
            viewport(cores, 1);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ctx->VSSetShader(prog.vs, nullptr, 0); ctx->PSSetShader(prog.ps, nullptr, 0);
            ctx->Draw(cores, 0);
            oc ^= 1;
        }
        if (hasAny) ctx->SetPredication(nullptr, FALSE);
        clear_srv();
        cur ^= 1;
    }
    std::vector<uint32_t> core_state(uint32_t c) {   // the last pass's state of core c, texel by texel
        uint32_t np = c == 0 && g_spix0 ? g_spix0 : statePix;
        std::vector<uint32_t> out((size_t)np * 8 * 4);
        for (int t = 0; t < 8; t++) {
            std::vector<uint32_t> v = read_tex(set[cur ^ 1][t]);
            for (uint32_t p = 0; p < np; p++) memcpy(&out[((size_t)p * 8 + t) * 4], &v[pix(c, p) * 4], 16);
        }
        return out;
    }
};

static std::vector<uint32_t> g_rom;
static uint32_t g_op_rounds = 2;
static int g_show = env_int("GVM_SHOW") ? env_int("GVM_SHOW") : 4;   // differences a check prints
static void commit_constants(Gpu& gpu, uint32_t cores) {
    for (int c = 0; c < 4; c++) {
        uint32_t cdata[4] = {(uint32_t)c, cores, 0, 0};
        if (gpu.ccb[c]) gpu.ccb[c]->Release();
        D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA ci = {cdata, 0, 0};
        check(dev->CreateBuffer(&cd, &ci, &gpu.ccb[c]), "commit cbuffer");
    }
}
static bool runs_on(const Case& cs) {   // a phase 2 case needs the tick's file, bindings or pairs
    return g_gg.xr >= cs.xr_need && g_gg.npair >= cs.pair_need && (!cs.bind_need || g_gg.xbind) && !((g_gg.thin8 || g_gg.thin) && (cs.elf || cs.mc)) && (!cs.vec_need || g_gg.vec);   // (thin, thin8: compiled code is two-op bundles)
}
// Both machines to the halt in passes of 61 bundles; false (and the differences printed) when they disagree
static bool check_case(Gpu& gpu, const Case& cs, uint32_t seed, bool verbose) {
    RAM_END = cs.mc || cs.big ? RAM_MC : 2048 * 2048 * 16;
    Prepared pr = prepare(cs, seed);
    commit_constants(gpu, cs.cores);
    gvm_machine m = {};
    std::vector<uint32_t> refRam = pr.ramImg;
    std::vector<uint8_t> romBytes(ROM_SIZE);
    memcpy(romBytes.data(), g_rom.data(), ROM_SIZE);
    m.ram = refRam.data(); m.ram_end = RAM_END; m.rom = romBytes.data(); m.rom_size = ROM_SIZE;
    m.flags = GVM_REF_BREAK_HALTS; m.thin = g_gg.thin8; m.wide3 = g_gg.wide3 != 0; m.late = g_gg.late == 3 ? 2 : g_gg.late != 0; m.vec = g_gg.vec != 0;
    std::vector<gvm_core> rc(cs.cores);
    for (uint32_t c = 0; c < cs.cores; c++) {
        gvm_core_reset(&rc[c], c, pr.bases[c]); rc[c].sr[GVM_SR_TVEC] = HALT_STUB;
        rc[c].xr = g_gg.xr; rc[c].npair = g_gg.npair;   // (a built step's file and pairs, phase 2)
        for (int k = 1; k < 64; k++) rc[c].x[k] = pr.state[c][k];   // (a real program's sp)
    }
    m.cores = rc.data(); m.ncores = cs.cores;
    const uint32_t T = cs.check_ticks;   // short passes: they end inside bundles and on a full write cache
    gpu.make(cs.cores, pr.ramImg, g_rom, pr.state);
    gpu.pass = {T, RAM_END, 1000, 0, 0, 1, ram_rows(), 0, cs.cores, {0, 0, 0}, {0}};
    // TIME is 1000 + the pass (both machines), core 0's external line on in passes [ext_from, ext_to)
    auto ext_on = [&](int pass) { return pass >= cs.ext_from && pass < cs.ext_to; };
    int end0 = GVM_END_BUDGET; uint32_t bun0 = T, busy = 0, busyNext = 0;
    for (int refPasses = 0; refPasses < 400000; refPasses++) {
        bool any = false;
        m.time = 1000 + (uint64_t)refPasses;
        rc[0].ext = ext_on(refPasses);
        busy = busyNext; busyNext = 0; const int e0 = end0; const uint32_t n0 = bun0;   // (core 0's last pass, as the tick reads it)
        uint32_t wm[8] = {0};   // (WAD 5, 6: the kernel's REST mask at 0x00c01100, as the last commit left it)
        bool wgiven = false;
        if (g_gg.wad >= 5) for (int k = 0; k < 8; k++) { wm[k] = m.ram[0x00c01100 / 4 + k]; wgiven |= wm[k] != 0; }
        const bool declared = m.ram[0x00c01100 / 4 + 8] != 0;   // (WAD 7: the full cap only while a program has declared a mask)
        for (uint32_t c = 0; c < cs.cores; c++) if (!rc[c].halted) {   // (a worker's budget: the tick's WSH and WAD)
            uint32_t b = c == 0 ? T : T >> g_gg.wsh;
            bool cap5 = g_gg.wad >= 5 && e0 == GVM_END_SYNC && wgiven && !((wm[c >> 5] >> (c & 31)) & 1);
            if (c != 0 && g_gg.wad && ((((g_gg.wad == 1 || g_gg.wad == 4) && e0 == GVM_END_SYNC) || (e0 == GVM_END_FULL && g_gg.wad != 5 && (g_gg.wad != 7 || declared))) && (g_gg.wad != 4 || busy < 4) || cap5)) b = std::min(b, std::max(g_gg.wad == 3 ? b >> 1 : 512u, n0 + (n0 >> 2)));
            int e = gvm_run(&m, &rc[c], b); any = true;
            if (c == 0) { end0 = e; bun0 = b - rc[0].left; }
            if (c >= 1 && c <= 8) busyNext += e == GVM_END_BUDGET;
        }
        gvm_commit(&m);
        if (!any) break;
    }
    for (uint32_t c = 0; c < cs.cores && (cs.elf || cs.mc); c++)   // a real program's check ends at its first call to the host
        if (rc[c].sr[GVM_SR_CAUSE] != 0 && (rc[c].sr[GVM_SR_CAUSE] != GVM_CAUSE_SYSCALL || (rc[c].x[12] == 93 && rc[c].x[5] != 0)))
            printf("  %s: core %u ended by cause %x at %08x (r12 %u, r5 %u) on the reference\n", cs.name, c, rc[c].sr[GVM_SR_CAUSE], rc[c].sr[GVM_SR_EPC], rc[c].x[12], rc[c].x[5]);
    bool halted = false;
    int gpuPasses = 0;
    ULONGLONG checkStart = GetTickCount64();   // a check gives up after 60 s: every run stays short
    for (int round = 0; round < 4000 && !halted && GetTickCount64() - checkStart < (env_int("GVM_CHECK_SECONDS") ? env_int("GVM_CHECK_SECONDS") : 60) * 1000ull; round++) {
        for (int k = 0; k < 50; k++) {
            gpu.pass.timeLo = 1000 + gpuPasses;
            gpu.pass.ext0 = gpu.pass.ext[0] = ext_on(gpuPasses) ? 1 : 0;
            gpu.run_pass(); gpuPasses++;
        }
        halted = true;
        for (uint32_t c = 0; c < cs.cores; c++) halted = halted && (gpu.core_state(c)[18 * 4 + 2] & 4);
    }
    int bad = 0;
    for (uint32_t c = 0; c < cs.cores; c++)   // phase 2: a load of a bound word or of one with a pair is the program's error
        if (rc[c].stale && bad++ < g_show && verbose) printf("  %s: core %u made %u stale loads (the first at %08x) on the reference\n", cs.name, c, rc[c].stale, rc[c].stale_at);
    if (getenv("GVM_DEBUG")) {   // (each core's queue and progress texels)
        std::vector<uint32_t> os = read_tex(gpu.ops[gpu.oc]);
        for (uint32_t c = 0; c < cs.cores; c++) {
            std::vector<uint32_t> st = gpu.core_state(c);
            printf("  core %u: epc %08x cause %x tval %08x pc %08x flags %x end %u queue %u %u %u %u op0 %08x %08x %08x %08x op1 %08x %08x %08x %08x progress %u %u %u %u\n", c, st[64], st[74], st[79],
                   st[88], st[89], st[90], st[91], st[96], st[97], st[98], st[99], st[104], st[105], st[106], st[107], os[4 * c], os[4 * c + 1], os[4 * c + 2], os[4 * c + 3]);
        }
    }
    if (!halted) { if (verbose) printf("  %s: the tick did not halt in %d passes\n", cs.name, gpuPasses); bad++; }
    for (uint32_t c = 0; c < cs.cores; c++) {
        std::vector<uint32_t> st = gpu.core_state(c);
        for (int k = 1; k < 64; k++) {
            bool same = st[k] == rc[c].x[k] || ((cs.skip_regs >> k) & 1);   // (the game bench's time-dependent registers)
            if (!same && cs.tol > 0) {   // both numbers, and near
                float x, y; memcpy(&x, &st[k], 4); memcpy(&y, &rc[c].x[k], 4);
                same = fabsf(x - y) <= (float)cs.tol * fmaxf(1.0f, fabsf(y));
            }
            if (!same && bad++ < g_show && verbose) printf("  %s: core %u r%d %08x, the reference %08x\n", cs.name, c, k, st[k], rc[c].x[k]);
        }
        // phase 2: the core's file, its bindings (the dirty bit apart: a commit clears it) and the pairs not yet written
        for (uint32_t k = 0; k < g_gg.xr && g_gg.t_xf; k++)
            if (st[g_gg.t_xf * 4 + k] != rc[c].xf[k] && bad++ < g_show && verbose) printf("  %s: core %u file %u %08x, the reference %08x\n", cs.name, c, k, st[g_gg.t_xf * 4 + k], rc[c].xf[k]);
        for (uint32_t k = 0; k < g_gg.xr && g_gg.xbind && g_gg.t_xt; k++)
            if ((st[g_gg.t_xt * 4 + k] & ~2u) != (rc[c].xtag[k] & ~2u) && bad++ < g_show && verbose) printf("  %s: core %u binding %u %08x, the reference %08x\n", cs.name, c, k, st[g_gg.t_xt * 4 + k], rc[c].xtag[k]);
        for (uint32_t k = 1; k < 64 && g_gg.vec; k++)   // phase 3: lanes 1-3 of the 128-bit registers
            for (uint32_t l = 1; l < 4; l++)
                if (st[(g_gg.t_vr + k) * 4 + l] != rc[c].xv[k][l - 1] && bad++ < g_show && verbose) printf("  %s: core %u r%u lane %u %08x, the reference %08x\n", cs.name, c, k, l, st[(g_gg.t_vr + k) * 4 + l], rc[c].xv[k][l - 1]);
        if (g_gg.npair && st[g_gg.t_pn * 4] != rc[c].np && bad++ < g_show && verbose) printf("  %s: core %u %u pairs, the reference %u\n", cs.name, c, st[g_gg.t_pn * 4], rc[c].np);
        if (st[16 * 4] != rc[c].pc && bad++ < g_show && verbose) printf("  %s: core %u pc %08x, the reference %08x\n", cs.name, c, st[16 * 4], rc[c].pc);
        if (!cs.skip_ops && st[19 * 4 + 1] != (uint32_t)rc[c].ops_run && bad++ < g_show && verbose)
            printf("  %s: core %u ops %u, the reference %u\n", cs.name, c, st[19 * 4 + 1], (uint32_t)rc[c].ops_run);
    }
    std::vector<uint32_t> gram = read_tex(gpu.ram);
    size_t ramBad = 0;
    for (size_t k = 0; k < refRam.size(); k++)
        if (gram[k] != refRam[k] && ramBad++ < 3 && verbose) printf("  %s: word %08zx %08x, the reference %08x\n", cs.name, k * 4, gram[k], refRam[k]);
    gpu.release();
    return bad == 0 && ramBad == 0;
}
static ID3D11Query *g_qDis, *g_q0, *g_q1;
static double g_util;
static uint32_t g_max_passes = 0;   // --max-passes: a case's own passes (Case::passes) at most this many   // the last timing's budget used by core 0: its bundles over passes x ticks (1: every pass ran its whole budget)
// A timed case: millions of RISC-V instructions a second (its rv_per_iter), or gpu_vm ops when it has none
static double time_case(Gpu& gpu, const Case& cs, uint32_t ticks, uint32_t passes) {
    RAM_END = cs.mc || cs.big ? RAM_MC : 2048 * 2048 * 16;
    Prepared pr = prepare(cs, 1);
    commit_constants(gpu, cs.cores);
    gpu.make(cs.cores, pr.ramImg, g_rom, pr.state);
    gpu.pass = {ticks, RAM_END, 1000, 0, 0, 1, ram_rows(), 0, cs.cores, {0, 0, 0}, {0}};
    for (int k = 0; k < (env_int("GVM_ENDS") > 1 ? 0 : 20); k++) gpu.run_pass();
    int at = (g_fused && cs.bundles_fused > 0) || cs.ns ? 0 : 1;   // fused loops and real programs are counted in bundles (19.x), others in ops (19.y)
    if (cs.passes) passes = g_max_passes ? std::min(cs.passes, g_max_passes) : cs.passes;
    auto count = [&]() {   // core 0's bundles (or ops); many cores (Case::mc): every core's
        if (cs.units_reg) return gpu.core_state(0)[cs.units_reg];   // (the game bench: core 0's count of units)
        uint32_t n = 0;
        for (uint32_t c = 0; c < (cs.mc ? cs.cores : 1u); c++) n += gpu.core_state(c)[19 * 4 + at];
        return n;
    };
    uint32_t o0 = count(), u0 = gpu.core_state(0)[19 * 4];
    ctx->Begin(g_qDis);
    ctx->End(g_q0);
    if (env_int("GVM_ENDS")) {   // (not a timing: why core 0's passes end, and their bundles, from a readback after each pass)
        uint32_t n[8] = {}, b[8] = {};
        for (uint32_t k = 0; k < passes; k++) {
            gpu.run_pass();
            std::vector<uint32_t> st = gpu.core_state(0);
            n[st[19 * 4 + 3] & 7]++; b[st[19 * 4 + 3] & 7] += st[19 * 4 + 2];
            if (env_int("GVM_ENDS") > 1) {
                uint32_t used = 0;
                for (uint32_t j = 0; j < g_wcn; j++) used += st[(g_twc + j / 2) * 4 + (j & 1) * 2] != 0xffffffffu;
                printf(" %u/%u/%u/%u", st[19 * 4 + 2], used, st[23 * 4], st[23 * 4 + 1]);
            }
        }
        printf("  %s: passes by why they ended (bundles a pass):", cs.name);
        for (int e = 0; e < 8; e++) if (n[e]) printf(" %d: %u (%u)", e, n[e], b[e] / n[e]);
        printf("\n");
    }
    for (uint32_t k = 0; k < passes; k++) gpu.run_pass();
    ctx->End(g_q1);
    ctx->End(g_qDis);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    while (ctx->GetData(g_qDis, &dj, sizeof dj, 0) != S_OK) Sleep(1);
    UINT64 t0, t1;
    while (ctx->GetData(g_q0, &t0, sizeof t0, 0) != S_OK) Sleep(1);
    while (ctx->GetData(g_q1, &t1, sizeof t1, 0) != S_OK) Sleep(1);
    uint32_t o1 = count();
    g_util = (gpu.core_state(0)[19 * 4] - u0) / ((double)passes * ticks);
    gpu.release();
    if (cs.us_pass) return double(t1 - t0) / dj.Frequency * 1e6 / passes;   // (p_: us a pass)
    // (the game bench: frames a second; GVM_PASS_US=N adds N us a pass, the harness's own cost of a pass that the
    // card's passes here, queued back to back, do not pay: ~250 makes the scenes rank ticks as the games do, GAMEBENCH.md)
    static const double passUs = env_int("GVM_PASS_US");
    if (cs.units_reg) return (o1 - o0) / cs.unit_div / (double(t1 - t0) / dj.Frequency + passes * passUs * 1e-6);
    double mops = (o1 - o0) / (double(t1 - t0) / dj.Frequency) / 1e6;
    double per = at == 0 ? cs.bundles_fused : cs.ops_per_iter;
    if (env_int("GVM_BPP") && cs.ns) printf("  %s: %.0f bundles a pass\n", cs.name, (double)(o1 - o0) / passes);   // (passes end early on a full write cache)
    if (cs.ns) return 1000.0 / mops;   // a real program: ns a bundle
    return cs.rv_per_iter > 0 ? mops / per * cs.rv_per_iter : mops;
}
struct Variant { std::string name, shader; std::vector<std::string> defs; Prog tick, commit, stg, wrt, prog, any, tickW, tick0; uint32_t wcn0 = 0, spix0 = 0; bool hasCommit, hasOps, hasAny = false, uavTick = false, hasW = false; uint32_t wcn = 128, texw = 2048, twc = 24, slots = 32, span = 272, lrows = 1, wcn1 = 0, geo = 0, ntex = 0; Geo gg; };
static void use(Gpu& gpu, const Variant& v) {
    gpu.tick = v.tick; gpu.commit = v.commit; gpu.stg = v.stg; gpu.wrt = v.wrt; gpu.prog = v.prog;
    gpu.hasCommit = v.hasCommit; gpu.hasOps = v.hasOps; gpu.uavTick = v.uavTick; gpu.hasW = v.hasW; gpu.tickW = v.tickW; gpu.tick0 = v.tick0; g_wcn0 = v.wcn0; g_spix0 = v.spix0; gpu.hasAny = v.hasAny && !env_int("GVM_NOPRED"); gpu.any = v.any;
    g_wcn = v.wcn; g_texw = v.texw; g_twc = v.twc; g_lrows = v.lrows; g_wcn1 = v.wcn1; g_geo = v.geo; g_ntex = v.ntex; g_gg = v.gg; GvmAsm::file_paths = v.gg.xfast != 0; GvmAsm::nc_loads = v.gg.nc != 0; GvmAsm::max_ops = v.gg.thin || v.gg.thin8 ? 1 : v.gg.wide3 ? 3 : 2; GvmAsm::thin8 = v.gg.thin8 != 0; GvmAsm::late_br = v.gg.late != 0; GvmAsm::late_float = v.gg.late == 3; GvmAsm::fx1 = v.gg.fx1 != 0; GvmAsm::vec_ops = v.gg.vec != 0;   // (the state's layout, the commit's points and RAM's width are the variant's)
    gpu.opSlots = v.slots; gpu.opSpan = v.span; gpu.opRounds = g_op_rounds;
    g_romrows = 0; g_x17_paths = 0;   // (probe: the ROM in RAM's texture; x17 on the real programs' own paths)
    for (auto& d : v.defs) { if (d == "ROMROWS=1") g_romrows = 1; if (d == "X17PATHS=1") g_x17_paths = 1; }
}
// a step's write cache entries and RAM's width: a -D WCN= or RAM_W= of the variant, else the step's own
// (`#define WCN 130` or `static const uint WCN = 130;`), else 128 and 2048
static uint32_t step_size(const std::string& src, const std::vector<std::string>& defs, const char* name, uint32_t dflt) {
    std::string n = name;
    for (auto& d : defs) if (d.rfind(n + "=", 0) == 0) return (uint32_t)atoi(d.c_str() + n.size() + 1);
    for (const std::string& form : {"#define " + n + " ", "uint " + n + " = "}) {
        size_t at = src.find(form);
        if (at != std::string::npos) return (uint32_t)atoi(src.c_str() + at + form.size());
    }
    return dflt;
}
static void step_sizes(const std::string& path, const std::vector<std::string>& defs, uint32_t& wcn, uint32_t& texw, uint32_t& twc, uint32_t& slots, uint32_t& span, uint32_t& lrows, uint32_t& wcn1, uint32_t& geo, uint32_t& ntex) {
    std::string src = slurp(path);
    wcn = step_size(src, defs, "WCN", 128);
    texw = step_size(src, defs, "RAM_W", 2048);
    twc = step_size(src, defs, "T_WC", 24);
    slots = step_size(src, defs, "OP_SLOTS", 32);
    span = step_size(src, defs, "OP_SPAN", 272);
    lrows = step_size(src, defs, "LROWS", 1);
    wcn1 = step_size(src, defs, "WCN1", 0);
    geo = step_size(src, defs, "GEO_BLOCKS", 0);
    ntex = step_size(src, defs, "NTEX", 0);
}
static Geo step_geo(const std::string& path) {
    std::string src = slurp(path);
    Geo g;
    g.bw = step_size(src, {}, "GEO_BW", 0);
    if (!g.bw) return g;
    g.bh = step_size(src, {}, "GEO_BH", 0); g.band = step_size(src, {}, "GEO_BAND", 0); g.per = step_size(src, {}, "GEO_PER", 0);
    g.jrow = step_size(src, {}, "GEO_JROW", 0); g.perm = step_size(src, {}, "GEO_PERM", 0); g.mul = step_size(src, {}, "GEO_MUL", 0);
    g.inv = step_size(src, {}, "GEO_INV", 0);
    g.cpts = step_size(src, {}, "CPTS", 0); g.xr = step_size(src, {}, "XR", 0); g.npair = step_size(src, {}, "NPAIR", 0);
    g.xbind = step_size(src, {}, "XBIND", 0); g.xfast = step_size(src, {}, "XFAST", 0); g.nc = step_size(src, {}, "NCLOAD", 0); g.thin = step_size(src, {}, "THIN", 0); g.thin8 = step_size(src, {}, "THIN8", 0); g.wide3 = step_size(src, {}, "WIDE3", 0); g.late = step_size(src, {}, "LATEBR", 0); g.fx1 = step_size(src, {}, "FX1", 0); g.wsh = step_size(src, {}, "WSH", 0); g.wad = step_size(src, {}, "WAD", 0); g.vec = step_size(src, {}, "VEC", 0); g.t_vr = step_size(src, {}, "T_VR", 0); g.t_xf = step_size(src, {}, "T_XF", 0); g.t_xt = step_size(src, {}, "T_XT", 0);
    return g;
}
static void build_variant(Variant& v) {
    std::vector<std::string> names, values;
    for (auto& d : v.defs) {
        size_t e = d.find('=');
        names.push_back(d.substr(0, e));
        values.push_back(e == std::string::npos ? "1" : d.substr(e + 1));
    }
    std::vector<D3D_SHADER_MACRO> defs;
    for (size_t i = 0; i < names.size(); i++) defs.push_back({names[i].c_str(), values[i].c_str()});
    defs.push_back({nullptr, nullptr});
    const char* path = v.shader.c_str();
    v.uavTick = slurp(v.shader).find("fragu(") != std::string::npos;
    v.tick = program(path, "vert", v.uavTick ? "fragu" : "frag", defs);
    if (slurp(v.shader).find("#ifndef CORE0") != std::string::npos) {   // x25: core 0's tick, the same with CORE0=1
        std::vector<D3D_SHADER_MACRO> d0(defs.begin(), defs.end() - 1);
        d0.push_back({"CORE0", "1"});
        d0.push_back({nullptr, nullptr});
        v.tick0 = program(path, "vert", "frag", d0);
        std::string src = slurp(v.shader);
        v.wcn0 = step_size(src, {}, "WCN0", 0);
        v.spix0 = (step_size(src, {}, "NTEX0", 0) + 7) / 8;
    }
    v.hasW = slurp(v.shader).find("#ifndef WORKER") != std::string::npos;
    if (v.hasW) {   // the workers' tick: the same with WORKER=1
        std::vector<D3D_SHADER_MACRO> dw(defs.begin(), defs.end() - 1);
        dw.push_back({"WORKER", "1"});
        dw.push_back({nullptr, nullptr});
        v.tickW = program(path, "vert", "frag", dw);
    }
    std::string src = slurp(v.shader);   // a step without stores has no commit, one without the region no operations
    v.hasCommit = src.find("commit_vert") != std::string::npos;
    v.hasOps = src.find("op_stage_vert") != std::string::npos;
    if (v.hasCommit) v.commit = program(path, "commit_vert", "commit_frag", defs);
    v.hasAny = src.find("op_any_vert") != std::string::npos;
    if (v.hasAny) v.any = program(path, "op_any_vert", "op_any_frag", defs);
    if (v.hasOps) {
        v.stg = program(path, "op_stage_vert", "op_stage_frag", defs);
        v.wrt = program(path, "op_write_vert", "op_write_frag", defs);
        v.prog = program(path, "op_prog_vert", "op_prog_frag", defs);
    }
}

// A built variant's stamp: its source and defines hashed. The card runs a builder-made tick only when SHADER.warp_ok holds
// the stamp (written by a WARP run with --mark-ok that passed every check): the card never sees an unchecked tick.
static std::string stamp_of(const Variant& v) {
    std::string key = slurp(v.shader);
    for (auto& d : v.defs) key += ";" + d;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}
static bool built(const Variant& v) { return slurp(v.shader).find("made by gpu_vm/tick/builder") != std::string::npos; }
int main(int argc, char** argv) {
    bool warp = false, nocheck = false;
    std::string jsonOut;
    bool markOk = false;
    double deadline = 0;   // --json FILE: every variant's checks and every round's number, for builder/search.py   // --nocheck: time only (a batch's checks run apart: every run stays under 5 minutes)
    std::string only, shader, varfile, setName;
    uint32_t ticks = 8192, seed = 1, passes = 300, rounds = 2;
    std::vector<std::string> defStore;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--nocheck") nocheck = true;
        else if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--mark-ok") markOk = true;
        else if (a == "--max-passes" && i + 1 < argc) g_max_passes = (uint32_t)atoi(argv[++i]);
        else if (a == "--deadline" && i + 1 < argc) deadline = atof(argv[++i]);   // seconds: no timing round starts that would end after it   // (WARP) a variant that passes every check gets SHADER.warp_ok, its source's stamp
        else if (a == "--fused" && i + 1 < argc) g_fused = atoi(argv[++i]);   // the timed loops fused (cases.h; or GVM_FUSED=N)
        else if (a == "--shader" && i + 1 < argc) shader = argv[++i];
        else if (a == "--variants" && i + 1 < argc) varfile = argv[++i];
        else if (a == "--case" && i + 1 < argc) only = argv[++i];
        else if (a == "--set" && i + 1 < argc) setName = argv[++i];   // a named set of cases (Case::set: `game`, gamebench.h)
        else if (a == "--ticks" && i + 1 < argc) ticks = (uint32_t)atoi(argv[++i]);
        else if (a == "--passes" && i + 1 < argc) passes = (uint32_t)atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = (uint32_t)atoi(argv[++i]);
        else if (a == "--op-rounds" && i + 1 < argc) g_op_rounds = (uint32_t)atoi(argv[++i]);   // the operations' rounds a pass
        else if (a == "--seed" && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (a == "--cores" && i + 1 < argc) g_mc_cores = (uint32_t)atoi(argv[++i]);   // the many-core cases' cores
        else if (a == "-D" && i + 1 < argc) defStore.push_back(argv[++i]);
        else {
            printf("gvmbench [--warp] [--fused 1|2] [--shader F [-D NAME=V]... | --variants FILE] [--case A,B] [--ticks N] [--passes N] [--rounds N] [--seed N]\n"
                   "  FILE: a variant a line, NAME SHADER [-D NAME=V]...; # starts a comment\n");
            return 1;
        }
    }
    std::vector<Variant> vars;
    if (!varfile.empty()) {
        FILE* f = fopen(varfile.c_str(), "r");
        if (!f) { printf("gvmbench: no %s\n", varfile.c_str()); return 1; }
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            std::vector<std::string> tok;
            for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.push_back(t);
            if (tok.empty() || tok[0][0] == '#') continue;
            Variant v;
            v.name = tok[0];
            v.shader = tok.size() > 1 ? tok[1] : "";
            for (size_t k = 2; k + 1 < tok.size(); k += 2) if (tok[k] == "-D") v.defs.push_back(tok[k + 1]);
            vars.push_back(v);
        }
        fclose(f);
    } else {
        Variant v;
        v.name = "tick";
        v.shader = shader;
        v.defs = defStore;
        vars.push_back(v);
    }
    if (!warp)   // the safety rule (gpu_vm/tick/builder/README.md): a built tick reaches the card only after WARP passed it
        for (Variant& v : vars)
            if (!v.shader.empty() && built(v)) {
                std::string ok = slurp(v.shader + ".warp_ok");
                while (!ok.empty() && (ok.back() == '\n' || ok.back() == '\r')) ok.pop_back();
                if (ok != stamp_of(v)) { printf("gvmbench: %s (%s) has not passed WARP's checks as it is: not run on the card\n", v.name.c_str(), v.shader.c_str()); return 3; }
            }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dflt(exe);
    dflt = dflt.substr(0, dflt.find_last_of("\\/")) + "\\..\\gpu_vm\\tick\\tick.hlsl";
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceCounter(&f0);
    for (Variant& v : vars) {
        if (v.shader.empty()) v.shader = dflt;
        step_sizes(v.shader, v.defs, v.wcn, v.texw, v.twc, v.slots, v.span, v.lrows, v.wcn1, v.geo, v.ntex);
        v.gg = step_geo(v.shader);
        build_variant(v);
    }
    QueryPerformanceCounter(&f1);
    QueryPerformanceFrequency(&fq);
    printf("gvmbench: %s%s, %zu variant%s compiled in %.1f s\n", warp ? "WARP" : "the card", g_fused == 1 ? " (loops fused: addbnz)" : g_fused >= 2 ? " (loops fused: addbnz, the control word's add)" : "", vars.size(), vars.size() == 1 ? "" : "s",
           double(f1.QuadPart - f0.QuadPart) / fq.QuadPart);
    Gpu gpu;
    {
        D3D11_BUFFER_DESC bd = {80, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(dev->CreateBuffer(&bd, nullptr, &gpu.cb), "cbuffer");
        for (int c = 0; c < 4; c++) {
            D3D11_BLEND_DESC b = {};
            b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            b.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1 << c);
            check(dev->CreateBlendState(&b, &gpu.mask[c]), "blend");
            gpu.ccb[c] = nullptr;
        }
    }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&qd, &g_qDis);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &g_q0);
    dev->CreateQuery(&qd, &g_q1);
    g_rom.resize((size_t)ROM_SIZE / 4);
    for (size_t k = 0; k < g_rom.size(); k++) g_rom[k] = (uint32_t)(k * 0x01000193u) ^ 0x5bd1e995u;

    std::vector<Case> all = cases(), picked;
    if (!setName.empty() && only.empty()) only = "-";   // (--set alone: that set's cases only)
    for (const Case& cs : all)
        if (((only.empty() || ("," + only + ",").find(",default,") != std::string::npos) && !cs.named) || ("," + only + ",").find("," + std::string(cs.name) + ",") != std::string::npos || (cs.set && ("," + setName + ",").find("," + std::string(cs.set) + ",") != std::string::npos)) picked.push_back(cs);
    // checks: every picked case on every variant, the timed loops at a few thousand iterations
    std::vector<std::string> wrong(vars.size());
    g_loop_n = 3000;
    for (size_t vi = 0; vi < vars.size() && !nocheck; vi++) {
        use(gpu, vars[vi]);
        for (const Case& cs : picked)
            if (runs_on(cs) && !check_case(gpu, cs, seed, vars.size() == 1)) wrong[vi] += std::string(wrong[vi].empty() ? "" : ",") + cs.name;
    }
    // timing: rounds over every variant, forward then backward (ABBA...); the mean of the rounds and their spread
    g_loop_n = 3000000;
    std::vector<const Case*> timed;
    for (const Case& cs : picked) if (cs.timed) timed.push_back(&cs);
    std::vector<std::vector<std::vector<double>>> got(vars.size(), std::vector<std::vector<double>>(timed.size())), util = got;
    if (!timed.empty() && rounds > 0 && !warp) {   // (the card's clocks up first, 4 s of the first case: not kept; one timing was not enough)
        use(gpu, vars[0]);
        ULONGLONG w0 = GetTickCount64();
        while (GetTickCount64() - w0 < 4000) time_case(gpu, *timed[0], ticks, passes);
    }
    ULONGLONG tStart = GetTickCount64();
    for (uint32_t r = 0; r < rounds && !timed.empty(); r++) {
        if (r > 0 && deadline > 0 && (GetTickCount64() - tStart) / 1000.0 * (r + 1) / r > deadline) {   // (the next round would end past it)
            fprintf(stderr, "gvmbench: %u rounds: the next would pass the deadline\n", r);
            break;
        }
        for (size_t i = 0; i < vars.size(); i++) {
            size_t vi = (r & 1) ? vars.size() - 1 - i : i;
            use(gpu, vars[vi]);
            for (size_t ci = 0; ci < timed.size(); ci++) {
                if (!runs_on(*timed[ci])) continue;
                got[vi][ci].push_back(time_case(gpu, *timed[ci], ticks, passes));
                util[vi][ci].push_back(g_util);
            }
            if (env_int("GVM_PROGRESS")) { fprintf(stderr, "gvmbench: round %u, %s timed\n", r, vars[vi].name.c_str()); fflush(stderr); }
        }
    }
    // each variant's mean over the rounds and its spread ((max - min) / mean, in %): the card's noise is visible
    printf("%-16s", "variant");
    for (auto* cs : timed) printf(" %13s", cs->ns ? (std::string(cs->name).substr(0, 8) + " ns").c_str() : cs->us_pass ? (std::string(cs->name) + " us").c_str() : cs->name);
    printf("  x lean  checks\n");
    for (size_t vi = 0; vi < vars.size(); vi++) {
        printf("%-16s", vars[vi].name.c_str());
        double lg = 0;
        int nl = 0;
        for (size_t ci = 0; ci < timed.size(); ci++) {
            const std::vector<double>& x = got[vi][ci];
            double mean = 0, lo = 1e300, hi = -1e300;
            for (double y : x) { mean += y; lo = std::min(lo, y); hi = std::max(hi, y); }
            mean = x.empty() ? 0 : mean / x.size();
            printf(" %8.2f %3.0f%%", mean, x.size() > 1 && mean > 0 ? (hi - lo) / mean * 100 : 0.0);
            double l = lean_of(timed[ci]->name);
            if (l > 0 && mean > 0) { lg += log(mean / l); nl++; }
        }
        printf("  %6.2f  %s\n", nl ? exp(lg / nl) : 0.0, nocheck ? "(not checked)" : wrong[vi].empty() ? "same as the reference" : ("WRONG: " + wrong[vi]).c_str());
    }
    if (!jsonOut.empty()) {
        if (FILE* f = fopen(jsonOut.c_str(), "w")) {
            fprintf(f, "{\"device\": \"%s\", \"variants\": [\n", warp ? "warp" : "card");
            for (size_t vi = 0; vi < vars.size(); vi++) {
                fprintf(f, " {\"name\": \"%s\", \"shader\": \"", vars[vi].name.c_str());
                for (char ch : vars[vi].shader) fputs(ch == '\\' ? "/" : std::string(1, ch).c_str(), f);
                fprintf(f, "\", \"checked\": %s, \"wrong\": \"%s\", \"cases\": {", nocheck ? "false" : "true", wrong[vi].c_str());
                for (size_t ci = 0; ci < timed.size(); ci++) {
                    fprintf(f, "%s\"%s\": [", ci ? ", " : "", timed[ci]->name);
                    for (size_t k = 0; k < got[vi][ci].size(); k++) fprintf(f, "%s%.4f", k ? ", " : "", std::isfinite(got[vi][ci][k]) ? got[vi][ci][k] : 0.0);   // (0: no number)
                    fprintf(f, "]");
                }
                fprintf(f, "}, \"util\": {");
                for (size_t ci = 0; ci < timed.size(); ci++) {
                    fprintf(f, "%s\"%s\": [", ci ? ", " : "", timed[ci]->name);
                    for (size_t k = 0; k < util[vi][ci].size(); k++) fprintf(f, "%s%.4f", k ? ", " : "", util[vi][ci][k]);
                    fprintf(f, "]");
                }
                fprintf(f, "}}%s\n", vi + 1 < vars.size() ? "," : "");
            }
            fprintf(f, "]}\n");
            fclose(f);
        }
    }
    if (warp && markOk && !nocheck)
        for (size_t vi = 0; vi < vars.size(); vi++)
            if (wrong[vi].empty())
                if (FILE* f = fopen((vars[vi].shader + ".warp_ok").c_str(), "w")) { fprintf(f, "%s\n", stamp_of(vars[vi]).c_str()); fclose(f); }
    bool ok = true;
    for (auto& w : wrong) ok = ok && w.empty();
    return ok ? 0 : 2;
}
