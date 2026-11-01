// The fresh tick on real code under D3D12, for Nsight only (GPU Trace does not attach to D3D11): the bench's bindings, its DXBC converted to DXIL.
//   fresh12 SHADER COMMIT SNAP DIRTY --passes N [--code FILE] [--seconds S] [-D X=V]
// A frame is one go of the stretch: core 0 and the texels of DIRTY (u32s, what the go changes) put back, then N passes of tick and commit, then Present.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;
static void check(HRESULT h, const char* w) { if (FAILED(h)) { printf("%s failed: %08x\n", w, (unsigned)h); exit(1); } }
// ---- DXBC container hash (the skill nsight-gpu-trace, "DXBC vs DXIL")
static void hashBlock(uint32_t st[4], const uint8_t* b) {
    static const uint8_t R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    static uint32_t K[64]; if (!K[0]) for (int i = 0; i < 64; i++) K[i] = (uint32_t)(fabs(sin((double)i + 1)) * 4294967296.0);
    uint32_t x[16]; memcpy(x, b, 64); uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (B & C) | (~B & D); g = i; } else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) & 15; } else { f = C ^ (B | ~D); g = (7 * i) & 15; }
        uint32_t t = A + f + K[i] + x[g]; int r = R[(i / 16) * 4 + (i & 3)];
        A = D; D = C; C = B; B = B + ((t << r) | (t >> (32 - r)));
    }
    st[0] += A; st[1] += B; st[2] += C; st[3] += D;
}
static void sign(std::vector<uint8_t>& c) {
    const uint8_t* d = c.data() + 20; size_t n = c.size() - 20, at = 0; uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (; n - at >= 64; at += 64) hashBlock(st, d + at);
    size_t tail = n - at; uint32_t bits = (uint32_t)n * 8, last = (bits >> 2) | 1; uint8_t b[64] = {};
    if (tail < 56) { memcpy(b, &bits, 4); memcpy(b + 4, d + at, tail); b[4 + tail] = 0x80; memcpy(b + 60, &last, 4); hashBlock(st, b); }
    else { memcpy(b, d + at, tail); b[tail] = 0x80; hashBlock(st, b); memset(b, 0, 64); memcpy(b, &bits, 4); memcpy(b + 60, &last, 4); hashBlock(st, b); }
    memcpy(c.data() + 4, st, 16);
}
struct __declspec(uuid("5F956ED5-78D1-4B15-8247-F7187614A041")) IConv : public IUnknown { virtual HRESULT STDMETHODCALLTYPE Convert(LPCVOID, UINT32, LPCWSTR, LPVOID*, UINT32*, LPWSTR*) = 0; };
static std::vector<uint8_t> toDxil(ID3DBlob* in) {
    HMODULE m = LoadLibraryA("C:\\Windows\\System32\\dxilconv.dll"); typedef HRESULT(WINAPI * Create)(REFCLSID, REFIID, LPVOID*);
    Create create = (Create)GetProcAddress(m, "DxcCreateInstance");
    static const GUID clsid = {0x4900391E, 0xB752, 0x4EDD, {0xA8, 0x85, 0x6F, 0xB7, 0x6E, 0x25, 0xAD, 0xDB}};
    ComPtr<IConv> conv; check(create(clsid, __uuidof(IConv), (LPVOID*)conv.GetAddressOf()), "dxilconv");
    void* out = nullptr; UINT32 n = 0; LPWSTR diag = nullptr;
    check(conv->Convert(in->GetBufferPointer(), (UINT32)in->GetBufferSize(), nullptr, &out, &n, &diag), "convert");
    std::vector<uint8_t> r((uint8_t*)out, (uint8_t*)out + n); CoTaskMemFree(out); sign(r); return r;
}
static std::vector<uint8_t> compile(const std::string& file, const char* entry, const char* profile, const std::vector<std::string>& defs) {
    std::vector<D3D_SHADER_MACRO> m; std::vector<std::string> names, vals; names.reserve(defs.size()); vals.reserve(defs.size());
    for (auto& d : defs) { auto e = d.find('='); names.push_back(d.substr(0, e)); vals.push_back(e == std::string::npos ? "1" : d.substr(e + 1)); m.push_back({names.back().c_str(), vals.back().c_str()}); }
    m.push_back({nullptr, nullptr});
    std::wstring w(file.begin(), file.end()); ID3DBlob* b = nullptr, *err = nullptr;
    HRESULT h = D3DCompileFromFile(w.c_str(), m.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
    if (err) printf("%s\n", (const char*)err->GetBufferPointer());
    check(h, "compile");
    return toDxil(b);
}
static ComPtr<ID3D12Device> dev; static ComPtr<ID3D12CommandQueue> q; static ComPtr<ID3D12Fence> fence; static UINT64 fv = 0; static HANDLE ev;
static void waitIdle() { fv++; q->Signal(fence.Get(), fv); if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); } }
static D3D12_RESOURCE_BARRIER bar(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) { D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b; return x; }
static const D3D12_RESOURCE_STATES SR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static ComPtr<ID3D12Resource> tex(int w, int h, const void* data, ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* al, bool rt) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC d = {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R32G32B32A32_UINT; d.SampleDesc.Count = 1;
    d.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> t; check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)), "texture");
    D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD; D3D12_RESOURCE_DESC b = {}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = (UINT64)w * h * 16; b.Height = 1; b.DepthOrArraySize = 1; b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> ub; check(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub)), "upload");
    void* p; ub->Map(0, nullptr, &p); UINT pitch = w * 16; UINT al_p = (pitch + 255) & ~255u; if (al_p != pitch) { printf("pitch\n"); exit(1); } memcpy(p, data, (size_t)w * h * 16); ub->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {}; dst.pResource = t.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = ub.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_UINT, (UINT)w, (UINT)h, 1, pitch};
    check(al->Reset(), "reset"); check(cl->Reset(al, nullptr), "reset"); cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER br = bar(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, SR); cl->ResourceBarrier(1, &br); cl->Close();
    ID3D12CommandList* ls[] = {cl}; q->ExecuteCommandLists(1, ls); waitIdle();
    return t;
}
int main(int argc, char** argv) {
    std::string shader = argv[1], commit = argv[2], snap = argv[3], dirtyf = argv[4], codef; int passes = 4; double seconds = 8; std::vector<std::string> defs; int nent = 368;
    for (int i = 5; i < argc; i++) { std::string a = argv[i]; if (a == "--passes") passes = atoi(argv[++i]); else if (a == "--code") codef = argv[++i]; else if (a == "--seconds") seconds = atof(argv[++i]); else if (a == "-D") defs.push_back(argv[++i]); }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IDXGIFactory4> fac; check(CreateDXGIFactory1(IID_PPV_ARGS(&fac)), "factory");
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "device");
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "queue");
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence"); ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"fresh12"; RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"fresh12", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; check(fac->CreateSwapChainForHwnd(q.Get(), wnd, &sd, nullptr, nullptr, &swap), "swap");
    ComPtr<ID3D12CommandAllocator> al; check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "alloc");
    ComPtr<ID3D12GraphicsCommandList> cl; check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)), "list"); cl->Close();
    const int RW = 2048, RH = 9216, SW = 64, SH = 16;
    std::vector<unsigned> ram((size_t)RW * RH * 4); unsigned st[64][4] = {};
    { FILE* g = fopen(snap.c_str(), "rb"); if (!g) { printf("no snap\n"); return 1; } _fseeki64(g, 32, SEEK_SET); fread(st, 16, 64, g); _fseeki64(g, 32 + (long long)64 * 2048 * 16, SEEK_SET); fread(ram.data(), 4, ram.size(), g); fclose(g); }
    std::vector<unsigned> dirty; { FILE* g = fopen(dirtyf.c_str(), "rb"); if (g) { unsigned t; while (fread(&t, 4, 1, g) == 1) dirty.push_back(t); fclose(g); } }
    auto R = tex(RW, RH, ram.data(), cl.Get(), al.Get(), true), P = tex(RW, RH, ram.data(), cl.Get(), al.Get(), false);
    std::vector<unsigned> code((size_t)RW * RH * 4); if (!codef.empty()) { FILE* g = fopen(codef.c_str(), "rb"); fread(code.data(), 4, code.size(), g); fclose(g); }
    auto C = tex(RW, RH, code.data(), cl.Get(), al.Get(), false);
    code.clear(); code.shrink_to_fit(); ram.clear(); ram.shrink_to_fit();
    unsigned xr[32] = {}, pc = 0; for (int l = 29; l < 37; l++) { int k = 4 * (l - 29); xr[k + 1] = st[l][0]; xr[k + 2] = st[l][1]; xr[k + 3] = st[l][2]; if (l == 36) pc = st[l][3]; else xr[k + 4] = st[l][3]; }
    ComPtr<ID3D12Resource> S[2][8], I[8]; std::vector<unsigned> z(SW * SH * 4);
    for (int k = 0; k < 8; k++) { memset(z.data(), 0, z.size() * 4); memcpy(z.data(), xr + 4 * k, 16); if (k == 0) { z[4] = pc; } I[k] = tex(SW, SH, z.data(), cl.Get(), al.Get(), false); S[0][k] = tex(SW, SH, z.data(), cl.Get(), al.Get(), true); S[1][k] = tex(SW, SH, z.data(), cl.Get(), al.Get(), true); }
    // root signature: an SRV table t0-t9, b0 constants
    D3D12_DESCRIPTOR_RANGE rg = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0, 0, 0}; D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = {1, &rg}; rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC rs = {2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE}; ComPtr<ID3DBlob> rb, re; ComPtr<ID3D12RootSignature> root;
    check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rb, &re), "serialize"); check(dev->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    auto vs = compile(shader, "vert", "vs_5_0", defs), ps = compile(shader, "frag", "ps_5_0", defs), cv = compile(commit, "cvert", "vs_5_0", defs), cp = compile(commit, "cfrag", "ps_5_0", defs);
    FILE* kp = fopen("logs\\fresh12_ps.dxil", "wb"); if (kp) { fwrite(ps.data(), 1, ps.size(), kp); fclose(kp); }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = root.Get(); pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()}; pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 8;
    for (int k = 0; k < 8; k++) { pd.RTVFormats[k] = DXGI_FORMAT_R32G32B32A32_UINT; pd.BlendState.RenderTarget[k].RenderTargetWriteMask = 15; } pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> tick, com; check(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&tick)), "tick pso");
    pd.VS = {cv.data(), cv.size()}; pd.PS = {cp.data(), cp.size()}; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; pd.NumRenderTargets = 1; for (int k = 1; k < 8; k++) pd.RTVFormats[k] = DXGI_FORMAT_UNKNOWN;
    check(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&com)), "commit pso");
    ComPtr<ID3D12DescriptorHeap> rh, sh; D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 17, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0}; check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rh)), "rtv heap");
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 20, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0}; check(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sh)), "srv heap");
    UINT rs_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV), ss = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto rtv = [&](int i) { auto h = rh->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * rs_; return h; };
    for (int c = 0; c < 2; c++) for (int k = 0; k < 8; k++) dev->CreateRenderTargetView(S[c][k].Get(), nullptr, rtv(c * 8 + k));
    dev->CreateRenderTargetView(R.Get(), nullptr, rtv(16));
    for (int c = 0; c < 2; c++) for (int s = 0; s < 10; s++) { auto h = sh->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)(c * 10 + s) * ss; dev->CreateShaderResourceView(s == 0 ? R.Get() : s == 9 ? C.Get() : S[c][s - 1].Get(), nullptr, h); }
    D3D12_RESOURCE_BARRIER Pb = bar(P.Get(), SR, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int k = 0; k < 8; k++) { }
    check(al->Reset(), "r"); check(cl->Reset(al.Get(), nullptr), "r"); cl->ResourceBarrier(1, &Pb); { std::vector<D3D12_RESOURCE_BARRIER> b; for (auto& t : I) b.push_back(bar(t.Get(), SR, D3D12_RESOURCE_STATE_COPY_SOURCE)); cl->ResourceBarrier((UINT)b.size(), b.data()); } cl->Close(); { ID3D12CommandList* ls[] = {cl.Get()}; q->ExecuteCommandLists(1, ls); waitIdle(); }
    auto t0 = std::chrono::steady_clock::now(); unsigned long long goes = 0;
    int cur = 0; bool first = true;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        check(al->Reset(), "r"); check(cl->Reset(al.Get(), nullptr), "r");
        ID3D12DescriptorHeap* hs[] = {sh.Get()}; cl->SetDescriptorHeaps(1, hs); cl->SetGraphicsRootSignature(root.Get()); unsigned budget[4] = {8192, 0, 0, 0}; cl->SetGraphicsRoot32BitConstants(1, 4, budget, 0);
        // back to the start
        { std::vector<D3D12_RESOURCE_BARRIER> b; for (int k = 0; k < 8; k++) b.push_back(bar(S[0][k].Get(), SR, D3D12_RESOURCE_STATE_COPY_DEST)); b.push_back(bar(R.Get(), SR, D3D12_RESOURCE_STATE_COPY_DEST)); cl->ResourceBarrier((UINT)b.size(), b.data());
          for (int k = 0; k < 8; k++) cl->CopyResource(S[0][k].Get(), I[k].Get());
          if (!first) for (unsigned t : dirty) { D3D12_TEXTURE_COPY_LOCATION d = {}, s = {}; d.pResource = R.Get(); s.pResource = P.Get(); D3D12_BOX bx = {t & 2047, t >> 11, 0, (t & 2047) + 1, (t >> 11) + 1, 1}; cl->CopyTextureRegion(&d, t & 2047, t >> 11, 0, &s, &bx); }
          for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter); cl->ResourceBarrier((UINT)b.size(), b.data()); }
        first = false; cur = 0;
        for (int p = 0; p < passes; p++) {
            { std::vector<D3D12_RESOURCE_BARRIER> b; for (int k = 0; k < 8; k++) b.push_back(bar(S[1 - cur][k].Get(), SR, D3D12_RESOURCE_STATE_RENDER_TARGET)); cl->ResourceBarrier(8, b.data());
              D3D12_CPU_DESCRIPTOR_HANDLE r8[8]; for (int k = 0; k < 8; k++) r8[k] = rtv((1 - cur) * 8 + k); cl->OMSetRenderTargets(8, r8, FALSE, nullptr);
              auto g = sh->GetGPUDescriptorHandleForHeapStart(); g.ptr += (UINT64)cur * 10 * ss; cl->SetGraphicsRootDescriptorTable(0, g); cl->SetPipelineState(tick.Get()); cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
              D3D12_VIEWPORT v = {0, 0, (float)SW, (float)SH, 0, 1}; D3D12_RECT sc = {0, 0, 8, 8}; cl->RSSetViewports(1, &v); cl->RSSetScissorRects(1, &sc); cl->DrawInstanced(3, 1, 0, 0);
              for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter); cl->ResourceBarrier(8, b.data()); }
            cur = 1 - cur;
            { D3D12_RESOURCE_BARRIER b = bar(R.Get(), SR, D3D12_RESOURCE_STATE_RENDER_TARGET); cl->ResourceBarrier(1, &b); auto h = rtv(16); cl->OMSetRenderTargets(1, &h, FALSE, nullptr);
              auto g = sh->GetGPUDescriptorHandleForHeapStart(); g.ptr += (UINT64)cur * 10 * ss; cl->SetGraphicsRootDescriptorTable(0, g); cl->SetPipelineState(com.Get()); cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
              D3D12_VIEWPORT v = {0, 0, (float)RW, (float)RH, 0, 1}; D3D12_RECT sc = {0, 0, RW, RH}; cl->RSSetViewports(1, &v); cl->RSSetScissorRects(1, &sc); cl->DrawInstanced(nent, 1, 0, 0);
              std::swap(b.Transition.StateBefore, b.Transition.StateAfter); cl->ResourceBarrier(1, &b); }
        }
        // S[0] must be the side the next go resets: copy works from either; if the last pass left cur == 1, S[0] is still SR
        cl->Close(); ID3D12CommandList* ls[] = {cl.Get()}; q->ExecuteCommandLists(1, ls); swap->Present(0, 0); waitIdle(); goes++;
    }
    printf("fresh12 goes=%llu\n", goes);
    return 0;
}
