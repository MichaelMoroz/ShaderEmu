// The wave probe (gpu_vm/tick/WAVES.md): for each way of drawing the tick's cores (one triangle over all, a triangle a
// core as c8, a quad a core), each block shape and each way of treating padding, which pixels share a wave (the map
// shader appends a record an invocation: its wave, pixel, core, lane) and what a draw costs (the timing shader: every
// core the same work, then odd cores long and even cores short, which costs the long time only where waves mix cores).
//   waves [--warp] [--cores N] [--work N] [--rounds N]          (on the card through tools\gpu_queue.ps1)
// Shader model 6.0 through the Windows SDK's dxcompiler.dll; D3D12. The UAV is the probe's own: the tick has none.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("waves: %s failed: %08lx\n", what, (unsigned long)hr); exit(1); }
}
static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string slurp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("waves: no %s\n", path.c_str()); exit(1); }
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
static std::vector<uint8_t> compile(const std::string& path, const char* entry, const char* profile) {
    static ComPtr<IDxcUtils> utils;
    static ComPtr<IDxcCompiler3> dxc;
    if (!dxc) {   // the newest Windows SDK's dxcompiler.dll
        std::string best;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator("C:/Program Files (x86)/Windows Kits/10/bin", ec)) {
            auto cand = e.path() / "x64";
            if (std::filesystem::exists(cand / "dxcompiler.dll", ec) && cand.string() > best) best = cand.string();
        }
        HMODULE h = LoadLibraryExW(widen(best + "\\dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto create = h ? (DxcCreateInstanceProc)GetProcAddress(h, "DxcCreateInstance") : nullptr;
        if (!create) { printf("waves: no dxcompiler.dll in the Windows SDK\n"); exit(1); }
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc)), "DxcCompiler");
    }
    std::string src = slurp(path);
    std::wstring wpath = widen(path), wentry = widen(entry), wprof = widen(profile);
    std::vector<LPCWSTR> args = {wpath.c_str(), L"-T", wprof.c_str(), L"-E", wentry.c_str(), L"-O3"};
    DxcBuffer buf{src.data(), src.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> res;
    check(dxc->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&res)), "DXC");
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
static ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_STATES st, bool uav) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC d{D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                          D3D12_TEXTURE_LAYOUT_ROW_MAJOR, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
    ComPtr<ID3D12Resource> r;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}
static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    list->ResourceBarrier(1, &x);
}

// w: the target's width (0: 1024); busyN and stride: the busy cores (waves.hlsl's busy())
struct Case { const char* name; uint32_t geo, bw, bh, cw, ch, pad, w = 0, busyN = 0, stride = 1; };
struct Params { uint32_t geo, bw, bh, cw, ch, perRow, cores, work, pad, alt, w, h, busyN, stride; };

int main(int argc, char** argv) {
    bool warp = false;
    uint32_t ncores = 256, work = 20000, rounds = 7;
    std::string only;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--cores" && i + 1 < argc) ncores = atoi(argv[++i]);
        else if (a == "--work" && i + 1 < argc) work = atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else { printf("waves [--warp] [--cores N] [--work N] [--rounds N] [--only TEXT]\n"); return 1; }
    }
    ComPtr<IDXGIFactory4> fac;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&fac)), "factory");
    ComPtr<IDXGIAdapter1> ad;
    if (warp) check(fac->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "WARP");
    else check(fac->EnumAdapters1(0, &ad), "adapter");
    DXGI_ADAPTER_DESC1 desc;
    ad->GetDesc1(&desc);
    check(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
    printf("waves: %ls, waves of %u to %u lanes, %u cores, work %u, %u rounds\n", desc.Description, o1.WaveLaneCountMin,
           o1.WaveLaneCountMax, ncores, work, rounds);
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");

    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = {0, 0, 14};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[1].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rsb, rse;
    check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse), "root signature");
    ComPtr<ID3D12RootSignature> root;
    check(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&root)), "CreateRootSignature");
    std::string hlsl = (std::filesystem::path(argv[0]).parent_path() / "../gpu_vm/tick/probes/waves.hlsl").string();
    if (!std::filesystem::exists(hlsl)) hlsl = "gpu_vm/tick/probes/waves.hlsl";
    std::vector<uint8_t> vs = compile(hlsl, "vs", "vs_6_0"), psm = compile(hlsl, "ps_map", "ps_6_0"), pst = compile(hlsl, "ps_time", "ps_6_0");
    auto pso = [&](const std::vector<uint8_t>& ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = root.Get();
        d.VS = {vs.data(), vs.size()};
        d.PS = {ps.data(), ps.size()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = ~0u;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_UINT;
        d.SampleDesc = {1, 0};
        ComPtr<ID3D12PipelineState> p;
        check(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p)), "pipeline");
        return p;
    };
    ComPtr<ID3D12PipelineState> pMap = pso(psm), pTime = pso(pst);

    // the cases: c8's layout (8 x 6 blocks in cells 8 x 16) three ways, then blocks with no padding
    const Case cases[] = {
        {"c8: 8x6 in 8x16, one triangle, padding discarded", 0, 8, 6, 8, 16, 0},
        {"c8: 8x6 in 8x16, one triangle, padding returns", 0, 8, 6, 8, 16, 1},
        {"c8: 8x6 in 8x16, a triangle a core (today)", 1, 8, 6, 8, 16, 0},
        {"c8: 8x6 in 8x16, a quad a core", 2, 8, 6, 8, 16, 0},
        {"8x6, a quad a core, no padding", 2, 8, 6, 8, 6, 0},
        {"8x4, one triangle", 0, 8, 4, 8, 4, 0},
        {"8x4, a quad a core", 2, 8, 4, 8, 4, 0},
        {"4x8, a quad a core", 2, 4, 8, 4, 8, 0},
        {"16x2, one triangle", 0, 16, 2, 16, 2, 0},
        {"16x2, a quad a core", 2, 16, 2, 16, 2, 0},
        {"32x1, a quad a core", 2, 32, 1, 32, 1, 0},
        {"8x8, one triangle", 0, 8, 8, 8, 8, 0},
        {"8x8, a quad a core", 2, 8, 8, 8, 8, 0},
        {"16x4, a quad a core", 2, 16, 4, 16, 4, 0},
        {"32x2, one triangle", 0, 32, 2, 32, 2, 0},
        {"32x2, a quad a core", 2, 32, 2, 32, 2, 0},
        {"64x1, a quad a core", 2, 64, 1, 64, 1, 0},
        {"16x16, a quad a core", 2, 16, 16, 16, 16, 0},
        // far apart: a core's quad alone in a large empty cell that nothing rasterizes
        {"8x6 in 64x64, a quad a core", 2, 8, 6, 64, 64, 0},
        {"8x4 in 32x32, a quad a core", 2, 8, 4, 32, 32, 0},
        {"8x4 in 64x64, a quad a core", 2, 8, 4, 64, 64, 0},
        {"16x2 in 64x16, a quad a core", 2, 16, 2, 64, 16, 0},
        {"32x1 in 64x8, a quad a core", 2, 32, 1, 64, 8, 0},
        {"8x8 in 64x64, a quad a core", 2, 8, 8, 64, 64, 0},
        {"32x2 in 64x16, a quad a core", 2, 32, 2, 64, 16, 0},
        {"4x8 in 64x64, a quad a core", 2, 4, 8, 64, 64, 0},
        {"4x8 in 64x64, a clipped triangle a core", 3, 4, 8, 64, 64, 0},
        {"8x4 in 64x64, a clipped triangle a core", 3, 8, 4, 64, 64, 0},
        {"16x2 in 64x16, a clipped triangle a core", 3, 16, 2, 64, 16, 0},
        {"8x6 in 64x64, a clipped triangle a core", 3, 8, 6, 64, 64, 0},
        {"8x8 in 64x64, a clipped triangle a core", 3, 8, 8, 64, 64, 0},
        {"4x16 in 64x64, a clipped triangle a core", 3, 4, 16, 64, 64, 0},
        {"c8: 8x6 in 8x16, a clipped triangle a core", 3, 8, 6, 8, 16, 0},
        {"4x8 in 4x8, a clipped triangle a core", 3, 4, 8, 4, 8, 0},
        {"4x8 in 4x8, a quad a core", 2, 4, 8, 4, 8, 0},
        {"4x8 in 8x8, a clipped triangle a core", 3, 4, 8, 8, 8, 0},
        // blocks on NVIDIA's 4 x 8 tile, under large primitives (geo 4: a quad over a run of cores; pad = runs a row)
        {"4x8, one triangle", 0, 4, 8, 4, 8, 0},
        {"4x16, one triangle", 0, 4, 16, 4, 16, 0},
        {"8x16, one triangle", 0, 8, 16, 8, 16, 0},
        {"8x6, one triangle", 0, 8, 6, 8, 6, 0},
        {"4x8, a quad a row of 256 cores", 4, 4, 8, 4, 8, 0},
        {"4x8, a quad a run of 8 cores", 4, 4, 8, 4, 8, 32},
        {"4x8, a quad a run of 2 cores", 4, 4, 8, 4, 8, 128},
        {"8x8, a quad a row of 128 cores", 4, 8, 8, 8, 8, 0},
        {"8x8, a quad a run of 4 cores", 4, 8, 8, 8, 8, 32},
        {"8x8, a quad a run of 2 cores", 4, 8, 8, 8, 8, 64},
        // one triangle over all, 4 x 8 cores on the warp's tiles, padding between them launched and left at the top
        {"4x8 in 8x8, one triangle, padding discarded", 0, 4, 8, 8, 8, 0},
        {"4x8 in 8x8, one triangle, padding returns", 0, 4, 8, 8, 8, 1},
        {"4x8 in 8x16, one triangle, padding discarded", 0, 4, 8, 8, 16, 0},
        {"4x8 in 16x16, one triangle, padding discarded", 0, 4, 8, 16, 16, 0},
        {"4x8 in 32x32, one triangle, padding discarded", 0, 4, 8, 32, 32, 0},
        {"4x8 in 64x64, one triangle, padding discarded", 0, 4, 8, 64, 64, 0},
        {"4x8 in 64x64, one triangle, padding returns", 0, 4, 8, 64, 64, 1},
        {"4x16 in 16x32, one triangle, padding discarded", 0, 4, 16, 16, 32, 0},
        // wider shapes, a quad a core, far apart
        {"64x2 in 128x16, a quad a core", 2, 64, 2, 128, 16, 0},
        {"32x4 in 64x16, a quad a core", 2, 32, 4, 64, 16, 0},
        {"16x16 in 64x64, a quad a core", 2, 16, 16, 64, 64, 0},
        {"32x8 in 64x64, a quad a core", 2, 32, 8, 64, 64, 0},
        // the new tick's geometry (8 x 8 cores in one row, 2,048 x 8) with idle cores: one triangle over all of them
        // (an idle core leaves at the top), a quad a busy core, or one quad over the busy run
        {"idle: all 256 busy, one triangle", 0, 8, 8, 8, 8, 0, 2048},
        {"idle: 128 of 256 busy (a run), one triangle", 0, 8, 8, 8, 8, 0, 2048, 128, 1},
        {"idle: 128 of 256 busy (a run), one quad over them", 6, 8, 8, 8, 8, 0, 2048, 128, 1},
        {"idle: 128 of 256 busy (a run), a quad a busy core", 5, 8, 8, 8, 8, 0, 2048, 128, 1},
        {"idle: 128 of 256 busy (every 2nd), one triangle", 0, 8, 8, 8, 8, 0, 2048, 128, 2},
        {"idle: 128 of 256 busy (every 2nd), a quad a busy core", 5, 8, 8, 8, 8, 0, 2048, 128, 2},
        {"idle: 32 of 256 busy (a run), one triangle", 0, 8, 8, 8, 8, 0, 2048, 32, 1},
        {"idle: 32 of 256 busy (a run), one quad over them", 6, 8, 8, 8, 8, 0, 2048, 32, 1},
        {"idle: 32 of 256 busy (every 8th), one triangle", 0, 8, 8, 8, 8, 0, 2048, 32, 8},
        {"idle: 32 of 256 busy (every 8th), a quad a busy core", 5, 8, 8, 8, 8, 0, 2048, 32, 8},
        {"idle: 5 of 256 busy (a run), one triangle", 0, 8, 8, 8, 8, 0, 2048, 5, 1},
        {"idle: 5 of 256 busy (a run), one quad over them", 6, 8, 8, 8, 8, 0, 2048, 5, 1},
        {"idle: 1 of 256 busy, one triangle", 0, 8, 8, 8, 8, 0, 2048, 1, 1},
        {"idle: 1 of 256 busy, one quad over it", 6, 8, 8, 8, 8, 0, 2048, 1, 1},
        {"idle: 1 of 256 busy, one triangle, idle copy", 0, 8, 8, 8, 8, 2, 2048, 1, 1},
        {"idle: 32 of 256 busy (a run), one triangle, idle copy", 0, 8, 8, 8, 8, 2, 2048, 32, 1},
        {"idle: 32 of 256 busy (every 8th), one triangle, idle copy", 0, 8, 8, 8, 8, 2, 2048, 32, 8},
        {"idle: 128 of 256 busy (a run), one triangle, idle copy", 0, 8, 8, 8, 8, 2, 2048, 128, 1},
    };
    ComPtr<ID3D12QueryHeap> qh;
    D3D12_QUERY_HEAP_DESC qhd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * 64};
    check(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "query heap");
    ComPtr<ID3D12Resource> qrb = buffer(D3D12_HEAP_TYPE_READBACK, 8 * 2 * 64, D3D12_RESOURCE_STATE_COPY_DEST, false);
    UINT64 freq = 1;
    queue->GetTimestampFrequency(&freq);
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "rtv heap");

    printf("\n%-52s %7s %9s %9s %9s %9s %9s %10s %10s %6s\n", "case", "waves", "mixed %", "cores/w", "lanes/w", "pad w %",
           "inv/px", "same ms", "alt ms", "alt/s");
    for (const Case& cs : cases) {
        if (!only.empty() && std::string(cs.name).find(only) == std::string::npos) continue;
        const uint32_t W = cs.w ? cs.w : 1024;
        uint32_t perRow = W / cs.cw, rows = (ncores + perRow - 1) / perRow, H = rows * cs.ch;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        ComPtr<ID3D12Resource> rt;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)), "target");
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);
        UINT64 bytes = 16 + (UINT64)W * H * 3 * 16;
        ComPtr<ID3D12Resource> ub = buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COMMON, true);
        ComPtr<ID3D12Resource> zero = buffer(D3D12_HEAP_TYPE_UPLOAD, 16, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        ComPtr<ID3D12Resource> rb = buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, false);
        { void* p; check(zero->Map(0, nullptr, &p), "Map"); memset(p, 0, 16); zero->Unmap(0, nullptr); }
        uint32_t verts = cs.geo == 0 ? 3 : cs.geo == 2 ? 6 * ncores : cs.geo == 4 ? 6 * rows * (cs.pad ? cs.pad : 1)
                       : cs.geo == 5 ? 6 * cs.busyN : cs.geo == 6 ? 6 : 3 * ncores;
        auto setup = [&](ID3D12PipelineState* p, const Params& pr) {
            list->SetPipelineState(p);
            list->SetGraphicsRootSignature(root.Get());
            list->SetGraphicsRoot32BitConstants(0, 14, &pr, 0);
            list->SetGraphicsRootUnorderedAccessView(1, ub->GetGPUVirtualAddress());
            D3D12_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1};
            D3D12_RECT sc{0, 0, (LONG)W, (LONG)H};
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        };
        Params pr{cs.geo, cs.bw, cs.bh, cs.cw, cs.ch, perRow, ncores, 0, cs.pad, 0, W, H, cs.busyN, cs.stride};
        // the map
        barrier(ub.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(ub.Get(), 0, zero.Get(), 0, 16);
        barrier(ub.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setup(pMap.Get(), pr);
        list->DrawInstanced(verts, 1, 0, 0);
        barrier(ub.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(rb.Get(), ub.Get());
        barrier(ub.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flush();
        const uint32_t* m;
        check(rb->Map(0, nullptr, (void**)&m), "Map readback");
        uint32_t nw = m[0], nrec = std::min<uint32_t>(m[1], W * H * 3);
        std::vector<std::set<uint32_t>> cores(nw);
        std::vector<uint32_t> lanes(nw), padl(nw);
        for (uint32_t r = 0; r < nrec; r++) {
            const uint32_t* x = m + 4 + 4 * r;
            if (x[0] >= nw) continue;
            lanes[x[0]]++;
            if (x[2] == 0xffffffffu) padl[x[0]]++; else cores[x[0]].insert(x[2]);
        }
        rb->Unmap(0, nullptr);
        uint32_t used = 0, mixed = 0, padw = 0;
        double sumc = 0, suml = 0;
        for (uint32_t k = 0; k < nw; k++) {
            if (!lanes[k]) continue;
            used++;
            mixed += cores[k].size() > 1;
            padw += padl[k] > 0;
            sumc += cores[k].size();
            suml += lanes[k];
        }
        // the timing: the same work in every core, then alternating; the median of `rounds` draws each
        double ms[2];
        for (int alt = 0; alt < 2; alt++) {
            Params tp = pr;
            tp.work = work; tp.alt = alt;
            // (a round a submission, waited for: back to back the draws overlapped and a small one's timestamps did
            // not wait for its work; the tick's passes each wait for the last)
            for (uint32_t r = 0; r < rounds; r++) {
                setup(pTime.Get(), tp);
                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * r);
                list->DrawInstanced(verts, 1, 0, 0);
                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * r + 1);
                flush();
            }
            list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * rounds, qrb.Get(), 0);
            flush();
            const UINT64* t;
            check(qrb->Map(0, nullptr, (void**)&t), "Map queries");
            std::vector<double> v;
            for (uint32_t r = 0; r < rounds; r++) v.push_back(1000.0 * (double)(t[2 * r + 1] - t[2 * r]) / freq);
            qrb->Unmap(0, nullptr);
            std::sort(v.begin(), v.end());
            ms[alt] = v[v.size() / 2];
        }
        printf("%-52s %7u %9.1f %9.2f %9.1f %9.1f %9.2f %10.3f %10.3f %6.2f\n", cs.name, used, used ? 100.0 * mixed / used : 0,
               used ? sumc / used : 0, used ? suml / used : 0, used ? 100.0 * padw / used : 0,
               (double)nrec / ((double)ncores * cs.bw * cs.bh), ms[0], ms[1], ms[0] > 0 ? ms[1] / ms[0] : 0);
        fflush(stdout);
    }
    return 0;
}
