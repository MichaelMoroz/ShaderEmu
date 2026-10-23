// gvmtrace12: gpu_vm's tick (a step of gpu_vm/tick/steps) on Direct3D 12, for Nsight Graphics' GPU Trace and its shader
// profiler, which attach to D3D12 alone (gpu_vm/tick/README.md, "Nsight"). It runs one case of cases.h for a time,
// the tick's draw and the commit's four a pass, and prints ops a second as gvmbench does.
//
//   gvmtrace12 --shader F [-D NAME=V]... [--case NAME] [--seconds S] [--ticks N] [--dxbc]
//
// The shaders are DXC's (shader model 6.6, -O3) with their debug information in them (-Zi -Qembed_debug), so the
// profiler maps the card's code to the source lines; --dxbc takes fxc2's bytecode instead (d3dcompiler_47.dll
// beside the exe), the bytecode the D3D11 bench times.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#define GVM_REF_IMPL
#include "cases.h"
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("gvmtrace12: %s failed: %08lx\n", what, (unsigned long)hr); exit(1); }
}
static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string slurp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("gvmtrace12: no %s\n", path.c_str()); exit(1); }
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// ---- Shaders: DXC with debug information, or fxc2's DXBC
static std::vector<uint8_t> compile(const std::string& path, const char* entry, const char* stage, const std::vector<std::string>& defs, bool dxbc) {
    std::string src = slurp(path), dir = std::filesystem::path(path).parent_path().string();
    if (dxbc) {
        std::vector<std::string> n, v;
        for (auto& d : defs) { size_t e = d.find('='); n.push_back(d.substr(0, e)); v.push_back(e == std::string::npos ? "1" : d.substr(e + 1)); }
        std::vector<D3D_SHADER_MACRO> m;
        for (size_t i = 0; i < n.size(); i++) m.push_back({n[i].c_str(), v[i].c_str()});
        m.push_back({nullptr, nullptr});
        ComPtr<ID3DBlob> code, err;
        HRESULT hr = D3DCompileFromFile(widen(path).c_str(), m.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                                        stage[0] == 'p' ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) { printf("%s\n", err ? (const char*)err->GetBufferPointer() : "fxc2 failed"); exit(1); }
        const uint8_t* p = (const uint8_t*)code->GetBufferPointer();
        return std::vector<uint8_t>(p, p + code->GetBufferSize());
    }
    static ComPtr<IDxcUtils> utils;
    static ComPtr<IDxcCompiler3> dxc;
    static ComPtr<IDxcIncludeHandler> inc;
    if (!dxc) {   // the newest Windows SDK's dxcompiler.dll
        std::string best;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator("C:/Program Files (x86)/Windows Kits/10/bin", ec)) {
            auto cand = e.path() / "x64";
            if (std::filesystem::exists(cand / "dxcompiler.dll", ec) && cand.string() > best) best = cand.string();
        }
        HMODULE h = LoadLibraryExW(widen(best + "\\dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto create = h ? (DxcCreateInstanceProc)GetProcAddress(h, "DxcCreateInstance") : nullptr;
        if (!create) { printf("gvmtrace12: no dxcompiler.dll in the Windows SDK\n"); exit(1); }
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)), "DxcCompiler");
        check(utils->CreateDefaultIncludeHandler(&inc), "include handler");
    }
    std::wstring wpath = widen(path), wdir = widen(dir), wentry = widen(entry), wprof = widen(std::string(stage) + "_6_6");
    std::vector<std::wstring> wdefs;
    for (auto& d : defs) wdefs.push_back(widen(d));
    // -HV 2016: the tick's ?: and && as fxc2 reads them; -Zi -Qembed_debug: the source lines in the shader
    std::vector<LPCWSTR> args = {wpath.c_str(), L"-T", wprof.c_str(), L"-E", wentry.c_str(), L"-O3", L"-HV", L"2016",
                                 L"-Zi", L"-Qembed_debug", L"-I", wdir.c_str()};
    for (auto& d : wdefs) { args.push_back(L"-D"); args.push_back(d.c_str()); }
    DxcBuffer buf{src.data(), src.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> res;
    check(dxc->Compile(&buf, args.data(), (UINT32)args.size(), inc.Get(), IID_PPV_ARGS(&res)), "DXC");
    HRESULT st;
    res->GetStatus(&st);
    if (FAILED(st)) {
        ComPtr<IDxcBlobUtf8> e;
        res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr);
        printf("%s\n", e ? e->GetStringPointer() : "DXC failed");
        exit(1);
    }
    ComPtr<IDxcBlob> obj;
    check(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr), "DXC object");
    const uint8_t* p = (const uint8_t*)obj->GetBufferPointer();
    return std::vector<uint8_t>(p, p + obj->GetBufferSize());
}

// ---- Direct3D 12
static ComPtr<ID3D12Device> dev;
static ComPtr<ID3D12CommandQueue> queue;
static ComPtr<ID3D12CommandAllocator> alloc;
static ComPtr<ID3D12GraphicsCommandList> list;
static ComPtr<ID3D12Fence> fence;
static UINT64 fenceValue = 0;
static void flush() {
    check(list->Close(), "Close");
    ID3D12CommandList* l[] = {list.Get()};
    queue->ExecuteCommandLists(1, l);
    check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(fenceValue, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    check(alloc->Reset(), "allocator Reset");
    check(list->Reset(alloc.Get(), nullptr), "list Reset");
}
struct Tex { ComPtr<ID3D12Resource> r; UINT w = 0, h = 0; D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST; };
static Tex make_tex(UINT w, UINT h, const void* init) {
    Tex t; t.w = w; t.h = h;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0},
                          D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.r)), "texture");
    // its first contents through an upload buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> ub;
    check(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub)), "upload");
    uint8_t* p;
    check(ub->Map(0, nullptr, (void**)&p), "Map upload");
    for (UINT y = 0; y < h; y++) {
        if (init) memcpy(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const uint8_t*)init + (size_t)y * w * 16, (size_t)w * 16);
        else memset(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, 0, (size_t)w * 16);
    }
    ub->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{t.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src{ub.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    flush();
    return t;
}
static void to(Tex& t, D3D12_RESOURCE_STATES s) {
    if (t.st == s) return;
    D3D12_RESOURCE_BARRIER b{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {t.r.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.st, s};
    list->ResourceBarrier(1, &b);
    t.st = s;
}
static std::vector<uint32_t> read_tex(Tex& t) {
    to(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_DESC d = t.r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rh{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> rb;
    check(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "readback");
    D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src{t.r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    flush();
    std::vector<uint32_t> out((size_t)t.w * t.h * 4);
    uint8_t* p;
    check(rb->Map(0, nullptr, (void**)&p), "Map readback");
    for (UINT y = 0; y < t.h; y++) memcpy(&out[(size_t)y * t.w * 4], p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)t.w * 16);
    rb->Unmap(0, nullptr);
    return out;
}

static uint32_t g_bw = 0, g_bh = 1, g_jrow = 0, g_cpts = 0;   // a built step's block (builder/gen.py's GEO_BW, GEO_BH, GEO_JROW): core 0's pixel j at (jx, jy)
int main(int argc, char** argv) {
    std::string shader, name = "arith";
    std::vector<std::string> defs;
    double seconds = 10;
    uint32_t ticks = 8192;
    bool dxbc = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shader" && i + 1 < argc) shader = argv[++i];
        else if (a == "--case" && i + 1 < argc) name = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc) ticks = (uint32_t)atoi(argv[++i]);
        else if (a == "--dxbc") dxbc = true;
        else if (a == "-D" && i + 1 < argc) defs.push_back(argv[++i]);
        else { printf("gvmtrace12 --shader F [-D NAME=V]... [--case NAME] [--seconds S] [--ticks N] [--dxbc]\n"); return 1; }
    }
    g_loop_n = 100000000;   // (a loop that outlasts any trace: the case's program halts at its end)
    {   // the step's write cache entries and RAM's width (as gvmbench reads them)
        std::string src = slurp(shader);
        auto size = [&](const std::string& n, uint32_t dflt) {
            for (auto& d : defs) if (d.rfind(n + "=", 0) == 0) return (uint32_t)atoi(d.c_str() + n.size() + 1);
            for (const std::string& form : {"#define " + n + " ", "uint " + n + " = "}) {
                size_t at = src.find(form);
                if (at != std::string::npos) return (uint32_t)atoi(src.c_str() + at + form.size());
            }
            return dflt;
        };
        g_wcn = size("WCN", 128); g_texw = size("RAM_W", 2048); g_twc = size("T_WC", 24);
        g_bw = size("GEO_BW", 0); g_bh = size("GEO_BH", 1); g_jrow = size("GEO_JROW", 0); g_cpts = size("CPTS", g_wcn);
    }
    const Case* cs = nullptr;
    static std::vector<Case> all = cases();
    for (auto& c : all) if (name == c.name) cs = &c;
    if (!cs || cs->cores != 1) { printf("gvmtrace12: no case %s of one core\n", name.c_str()); return 1; }

    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");

    // the root: b0 the pass's constants (20 words: x17's external lines at 12-19), b1 the commit's (4), t0-t11 a table
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 12, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = {0, 0, 20};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {1, 0, 4};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable = {1, &range};
    D3D12_ROOT_SIGNATURE_DESC rsd{3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rsb, rse;
    check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse), "root signature");
    ComPtr<ID3D12RootSignature> root;
    check(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");

    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> tvs = compile(shader, "vert", "vs", defs, dxbc), tps = compile(shader, "frag", "ps", defs, dxbc);
    bool hasCommit = slurp(shader).find("commit_vert") != std::string::npos;
    std::vector<uint8_t> cvs, cps;
    if (hasCommit) { cvs = compile(shader, "commit_vert", "vs", defs, dxbc); cps = compile(shader, "commit_frag", "ps", defs, dxbc); }
    printf("gvmtrace12: %s (%s) compiled in %.1f s, the tick's fragment shader %zu bytes\n", shader.c_str(), dxbc ? "fxc2" : "DXC with debug information",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), tps.size());
    auto pso = [&](const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, int targets, int mask, bool points) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = root.Get();
        d.VS = {vs.data(), vs.size()};
        d.PS = {ps.data(), ps.size()};
        for (int k = 0; k < 8; k++) d.BlendState.RenderTarget[k].RenderTargetWriteMask = (UINT8)(mask >= 0 ? (1 << mask) : 15);
        d.SampleMask = ~0u;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets;
        for (int k = 0; k < targets; k++) d.RTVFormats[k] = DXGI_FORMAT_R32G32B32A32_UINT;
        d.SampleDesc = {1, 0};
        ComPtr<ID3D12PipelineState> p;
        check(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p)), "pipeline state");
        return p;
    };
    ComPtr<ID3D12PipelineState> tick = pso(tvs, tps, 8, -1, false), commit[4];
    if (hasCommit) for (int c = 0; c < 4; c++) commit[c] = pso(cvs, cps, 1, c, true);

    // the machine: RAM, the ROM, two sets of eight state targets
    Prepared pr = prepare(*cs, 1);
    uint32_t statePix = (g_twc + g_wcn / 2 + 7) / 8;
    uint32_t sw = g_bw ? g_bw : statePix, shh = g_bw ? g_bh : 1;   // the state textures' size: a built step's block, else a row
    auto at_of = [&](uint32_t j) { return g_bw == 0 ? j : g_jrow ? (j / g_bw) * sw + j % g_bw : (j % g_bh) * sw + j / g_bh; };
    std::vector<uint32_t> rom((size_t)ROM_SIZE / 4);
    for (size_t k = 0; k < rom.size(); k++) rom[k] = (uint32_t)(k * 0x01000193u) ^ 0x5bd1e995u;
    Tex ram = make_tex(g_texw, ram_rows(), pr.ramImg.data()), romt = make_tex(g_texw, rom_rows(), rom.data()), set[2][8];
    for (int k = 0; k < 2; k++)
        for (int t = 0; t < 8; t++) {
            std::vector<uint32_t> img((size_t)sw * shh * 4, 0);
            for (uint32_t tx = t; tx < sw * shh * 8 && tx < pr.state[0].size() / 4; tx += 8) memcpy(&img[at_of(tx / 8) * 4], &pr.state[0][tx * 4], 16);
            set[k][t] = make_tex(sw, shh, img.data());
        }
    // descriptors: an RTV each; two SRV tables, the state read from set 0 or from set 1
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 17}, sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 24, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> rtvs, srvs;
    check(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvs)), "RTV heap");
    check(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvs)), "SRV heap");
    UINT rinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto rtv = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart(); h.ptr += (size_t)i * rinc; return h; };
    for (int k = 0; k < 2; k++) for (int t = 0; t < 8; t++) dev->CreateRenderTargetView(set[k][t].r.Get(), nullptr, rtv(k * 8 + t));
    dev->CreateRenderTargetView(ram.r.Get(), nullptr, rtv(16));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 12; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvs->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (size_t)(k * 12 + i) * sinc;
            ID3D12Resource* r = i == 0 ? ram.r.Get() : i == 1 ? romt.r.Get() : i < 10 ? set[k][i - 2].r.Get() : romt.r.Get();
            dev->CreateShaderResourceView(r, &sv, h);
        }
    auto table = [&](int k) { D3D12_GPU_DESCRIPTOR_HANDLE h = srvs->GetGPUDescriptorHandleForHeapStart(); h.ptr += (size_t)k * 12 * sinc; return h; };
    uint32_t pass[20] = {ticks, RAM_END, 1000, 0, 0, 1, ram_rows(), 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    int cur = 0;
    auto record_pass = [&]() {
        ID3D12DescriptorHeap* hs[] = {srvs.Get()};
        list->SetDescriptorHeaps(1, hs);
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRoot32BitConstants(0, 20, pass, 0);
        // the tick: set cur from set cur ^ 1
        for (int t = 0; t < 8; t++) { to(set[cur][t], D3D12_RESOURCE_STATE_RENDER_TARGET); to(set[cur ^ 1][t], SRV); }
        to(ram, SRV); to(romt, SRV);
        D3D12_CPU_DESCRIPTOR_HANDLE rt[8];
        for (int t = 0; t < 8; t++) rt[t] = rtv(cur * 8 + t);
        list->OMSetRenderTargets(8, rt, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, (float)sw, (float)shh, 0, 1};
        D3D12_RECT sc{0, 0, (LONG)sw, (LONG)shh + 1};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetGraphicsRootDescriptorTable(2, table(cur ^ 1));
        list->SetPipelineState(tick.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        if (hasCommit) {   // the commit: the pass's word pairs into RAM, a component a draw
            for (int t = 0; t < 8; t++) to(set[cur][t], SRV);
            to(ram, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_CPU_DESCRIPTOR_HANDLE r = rtv(16);
            list->OMSetRenderTargets(1, &r, FALSE, nullptr);
            D3D12_VIEWPORT rv{0, 0, (float)g_texw, (float)ram_rows(), 0, 1};
            D3D12_RECT rs{0, 0, (LONG)g_texw, (LONG)ram_rows()};
            list->RSSetViewports(1, &rv);
            list->RSSetScissorRects(1, &rs);
            list->SetGraphicsRootDescriptorTable(2, table(cur));
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            for (uint32_t c = 0; c < 4; c++) {
                uint32_t cc[4] = {c, 1, 0, 0};
                list->SetGraphicsRoot32BitConstants(1, 4, cc, 0);
                list->SetPipelineState(commit[c].Get());
                list->DrawInstanced(g_bw ? g_cpts : g_wcn, 1, 0, 0);
            }
        }
        cur ^= 1;
    };
    auto ops = [&]() { std::vector<uint32_t> v = read_tex(set[cur ^ 1][19 & 7]); return v[at_of(19 >> 3) * 4 + 1]; };
    // passes in batches of 50 a submit, for as long as asked
    for (int k = 0; k < 20; k++) record_pass();
    flush();
    uint32_t o0 = ops();
    auto w0 = std::chrono::steady_clock::now();
    uint64_t passes = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count() < seconds) {
        for (int k = 0; k < 50; k++) record_pass();
        flush();
        passes += 50;
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    uint32_t o1 = ops();
    double mops = (double)(o1 - o0) / sec / 1e6;
    printf("%s: %llu passes in %.1f s, %.3f ms a pass (wall), %.2f M ops a second", cs->name, (unsigned long long)passes, sec,
           sec * 1e3 / passes, mops);
    if (cs->rv_per_iter > 0 && !g_fused) printf(", as RISC-V's work %.2f M", mops / cs->ops_per_iter * cs->rv_per_iter);
    printf("\n");
    return 0;
}
