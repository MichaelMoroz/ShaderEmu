// fetchlat: ns a fetch of a dependent chain for each way a pixel shader can fetch a word (fetchlat.hlsl's MODEs), one core
// (an 8 x 4 block), the chain's words L1's (512 of them); and whether a float texture's point sample gives its bits back
// exactly (NaNs, denormals). fxc2 as the tick; GPU timestamps, the best of rounds, two lengths' difference.
//   fetchlat [--warp] [--rounds 7]          (bin\fetchlat.exe, probes\build_floor.bat)
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
static ID3D11Device* dev; static ID3D11DeviceContext* ctx;
static void ck(HRESULT h, const char* w) { if (FAILED(h)) { printf("%s failed: %08lx\n", w, (unsigned long)h); exit(1); } }
static uint32_t nxt(uint32_t i) { return (i * 37u + 11u) & 511u; }
static const wchar_t* PATH = L"gpu_vm\\tick\\probes\\fetchlat.hlsl";
static ID3DBlob* comp(const char* entry, const char* prof, int mode) {
    char ms[16]; snprintf(ms, sizeof ms, "%d", mode);
    D3D_SHADER_MACRO m[] = {{"MODE", ms}, {nullptr, nullptr}};
    ID3DBlob *b, *err = nullptr;
    if (FAILED(D3DCompileFromFile(PATH, m, nullptr, entry, prof, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err))) { printf("%s %d: %s", entry, mode, err ? (char*)err->GetBufferPointer() : "?"); exit(1); }
    return b;
}
struct T { ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11RenderTargetView* rtv = nullptr; };
static T tex(UINT w, UINT h, DXGI_FORMAT f, const void* data, UINT pitch, bool rt) {
    T r; D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0u), 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
    ck(dev->CreateTexture2D(&d, data ? &sd : nullptr, &r.t), "tex"); ck(dev->CreateShaderResourceView(r.t, nullptr, &r.srv), "srv");
    if (rt) ck(dev->CreateRenderTargetView(r.t, nullptr, &r.rtv), "rtv");
    return r;
}
int main(int argc, char** argv) {
    bool warp = false; int rounds = 7;
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--warp") warp = true; else if (a == "--rounds" && i + 1 < argc) rounds = atoi(argv[++i]); }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ck(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device");
    uint32_t seed = 12345; auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    // the textures: texel i of row 0 holds nxt(i) (x, and y 64 apart for MODE 12); the float texture's rows 1-7 random bits
    std::vector<uint32_t> t4(8192 * 8 * 4, 0), t1(8192 * 8), t2(8192 * 8 * 2), tf(8192 * 8 * 4), ts(256 * 256 * 4, 0);
    for (uint32_t i = 0; i < 8192 * 8; i++) { t1[i] = rnd(); t2[2 * i] = rnd(); t2[2 * i + 1] = rnd(); for (int c = 0; c < 4; c++) { t4[4 * i + c] = rnd(); tf[4 * i + c] = rnd(); } }
    // special bit patterns in the float texture: NaNs (quiet, signalling, with payloads), infinities, denormals, -0
    const uint32_t sp[] = {0x7fc00000, 0x7f800001, 0xffc12345, 0x7fbfffff, 0xff800000, 0x7f800000, 0x00000001, 0x007fffff, 0x80000001, 0x80000000, 0x00400000, 0x807fffff};
    for (uint32_t k = 0; k < 8192 * 8 * 4; k += 7) tf[k] = sp[(k / 7) % 12];
    for (uint32_t i = 0; i < 512; i++) { t4[4 * i] = nxt(i); t1[i] = nxt(i); t2[2 * i] = nxt(i); tf[4 * i] = nxt(i); ts[4 * ((i >> 8) * 256 + (i & 255))] = nxt(i); }
    for (uint32_t i = 0; i < 512; i++) { uint32_t at = (i >> 7 & 7) * 8192 + (i * 64 & 8191); t4[4 * at + 1] = nxt(i); }
    T T4 = tex(8192, 8, DXGI_FORMAT_R32G32B32A32_UINT, t4.data(), 8192 * 16, false);
    T T1 = tex(8192, 8, DXGI_FORMAT_R32_UINT, t1.data(), 8192 * 4, false);
    T T2 = tex(8192, 8, DXGI_FORMAT_R32G32_UINT, t2.data(), 8192 * 8, false);
    T TF = tex(8192, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, tf.data(), 8192 * 16, false);
    T TS = tex(256, 256, DXGI_FORMAT_R32G32B32A32_UINT, ts.data(), 256 * 16, false);
    T TR = tex(8192, 8, DXGI_FORMAT_R32G32B32A32_UINT, nullptr, 0, true);
    ID3D11Buffer *ba, *sb; ID3D11ShaderResourceView *basrv, *sbsrv;
    {
        std::vector<uint32_t> w(8192); for (uint32_t i = 0; i < 8192; i++) w[i] = i < 512 ? nxt(i) : rnd();
        D3D11_BUFFER_DESC bd = {8192 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_SUBRESOURCE_DATA sd = {w.data(), 0, 0};
        ck(dev->CreateBuffer(&bd, &sd, &ba), "ba");
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {}; v.Format = DXGI_FORMAT_R32_TYPELESS; v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; v.BufferEx.NumElements = 8192; v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ck(dev->CreateShaderResourceView(ba, &v, &basrv), "ba srv");
        D3D11_BUFFER_DESC sbd = {8192 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
        D3D11_SUBRESOURCE_DATA ssd = {t4.data(), 0, 0};
        ck(dev->CreateBuffer(&sbd, &ssd, &sb), "sb");
        ck(dev->CreateShaderResourceView(sb, nullptr, &sbsrv), "sb srv");
    }
    ID3D11Buffer *cb, *cc;
    { D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0}; ck(dev->CreateBuffer(&bd, nullptr, &cb), "cb");
      std::vector<uint32_t> c(512 * 4, 0); for (uint32_t i = 0; i < 512; i++) c[4 * i] = nxt(i);
      D3D11_BUFFER_DESC cd = {512 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0}; D3D11_SUBRESOURCE_DATA sd = {c.data(), 0, 0}; ck(dev->CreateBuffer(&cd, &sd, &cc), "cc"); }
    D3D11_SAMPLER_DESC sdsc = {}; sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sdsc.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* pt; ck(dev->CreateSamplerState(&sdsc, &pt), "sampler");
    T out = tex(8, 4, DXGI_FORMAT_R32G32B32A32_UINT, nullptr, 0, true);
    ID3DBlob* vb = comp("vert", "vs_5_0", 0);
    ID3D11VertexShader* vs; ck(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "vs");
    ctx->VSSetShader(vs, nullptr, 0); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto ps_of = [&](const char* e, int mode) { ID3DBlob* b = comp(e, "ps_5_0", mode); ID3D11PixelShader* p; ck(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p), e); return p; };
    auto target = [&](ID3D11RenderTargetView* r, UINT w, UINT h) { ID3D11ShaderResourceView* none[8] = {}; ctx->PSSetShaderResources(0, 8, none); D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1}; ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &r, nullptr); };
    auto bind = [&]() { ID3D11ShaderResourceView* s[8] = {T4.srv, T1.srv, T2.srv, TF.srv, TS.srv, TR.srv, basrv, sbsrv}; ctx->PSSetShaderResources(0, 8, s); ctx->PSSetSamplers(0, 1, &pt); ID3D11Buffer* b[2] = {cb, cc}; ctx->PSSetConstantBuffers(0, 2, b); };
    auto readback = [&](ID3D11Texture2D* t, UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        ID3D11Texture2D* st; ck(dev->CreateTexture2D(&d, nullptr, &st), "staging"); ctx->CopyResource(st, t);
        D3D11_MAPPED_SUBRESOURCE m; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
        std::vector<uint32_t> v((size_t)w * h * 4); for (UINT y = 0; y < h; y++) memcpy(&v[(size_t)y * w * 4], (char*)m.pData + y * m.RowPitch, w * 16);
        ctx->Unmap(st, 0); st->Release(); return v;
    };
    // the render target's texture: drawn by fill_frag (row 0's chain), as the state set is drawn by the tick
    ID3D11PixelShader* fill = ps_of("fill_frag", 0);
    auto refill = [&]() { target(TR.rtv, 8192, 8); ctx->PSSetShader(fill, nullptr, 0); ctx->Draw(3, 0); };
    refill();
    {   // the float texture's bits through SampleLevel, every texel
        T chk = tex(8192, 8, DXGI_FORMAT_R32G32B32A32_UINT, nullptr, 0, true);
        target(chk.rtv, 8192, 8); bind(); ctx->PSSetShader(ps_of("check_frag", 0), nullptr, 0); ctx->Draw(3, 0);
        target(out.rtv, 8, 4);
        std::vector<uint32_t> got = readback(chk.t, 8192, 8);
        size_t bad = 0, badsp = 0, first = SIZE_MAX;
        for (size_t k = 0; k < got.size(); k++) if (got[k] != tf[k]) { bad++; if (k % 7 == 0) badsp++; if (first == SIZE_MAX) first = k; }
        printf("float texture, SampleLevel with a point sampler: %zu of %zu words differ (%zu of them the special patterns)", bad, got.size(), badsp);
        if (first != SIZE_MAX) printf("; first: word %zu, %08x read as %08x", first, tf[first], got[first]);
        printf("\n");
    }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qdis, *q0, *q1; dev->CreateQuery(&qd, &qdis); qd.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
    const char* names[] = {"Texture2D<uint4> Load (RAM, 8192 wide)", "Texture2D<uint> R32_UINT Load", "Texture2D<uint2> R32G32 Load", "Load with a constant offset",
                           "GatherRed of R32_UINT", "SampleLevel, point, RGBA32F", "Load of a 256 x 256 texture", "Load of a texture drawn last draw",
                           "ByteAddressBuffer Load", "StructuredBuffer<uint4>", "cbuffer uint4[512], uniform index", "Sample (implicit LOD), point", "Load, a cache line a word"};
    printf("%-42s %8s %10s\n", "fetch", "ns", "x");
    for (int mode = 0; mode <= 12; mode++) {
        ID3D11PixelShader* ps = ps_of("frag", mode);
        auto timed = [&](uint32_t n) {
            if (mode == 7) refill();   // (the texture drawn by the draw before each one timed)
            target(out.rtv, 8, 4); bind(); ctx->PSSetShader(ps, nullptr, 0);
            uint32_t c[4] = {n, 77, 0, 0}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
            ctx->Begin(qdis); ctx->End(q0); ctx->Draw(3, 0); ctx->End(q1); ctx->End(qdis);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
            while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {} while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {} while (ctx->GetData(q1, &b, sizeof b, 0) != S_OK) {}
            return dj.Disjoint ? 1e9 : double(b - a) / dj.Frequency;
        };
        uint32_t n1 = warp ? 100 : 2000, n2 = warp ? 300 : 22000;
        timed(n1);
        uint32_t x = readback(out.t, 8, 4)[0];
        uint32_t want = 77 & 511; for (uint32_t k = 0; k < n1; k++) want = nxt(want);
        if (mode == 3) { want = 77 & 511; for (uint32_t k = 0; k < n1; k++) want = nxt(want + 1) & 511; }
        double ns = 0;
        if (!warp) {
            LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
            do { timed(n2); QueryPerformanceCounter(&b); } while (double(b.QuadPart - a.QuadPart) / f.QuadPart < 1.5);   // (the clock up)
            double b1 = 1e9, b2 = 1e9;
            for (int r = 0; r < rounds; r++) { b1 = std::min(b1, timed(n1)); b2 = std::min(b2, timed(n2)); }
            if (b2 > 0.25) { printf("a draw took %.3f s: stopped\n", b2); return 1; }
            ns = (b2 - b1) / (n2 - n1) * 1e9;
        }
        printf("%-42s %8.2f %10s\n", names[mode], ns, x == want ? "ok" : "WRONG");
        fflush(stdout);
    }
    FILE* p = warp ? nullptr : _popen("nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits", "r");
    int mhz = 0; if (p) { if (fscanf(p, "%d", &mhz) != 1) mhz = 0; _pclose(p); printf("clock %d MHz\n", mhz); }
    return 0;
}
