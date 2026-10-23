// xlane: cross-lane exchange inside a warp from SM5 (xlane.hlsl): an unbound UAV's atomics and the quad's derivatives;
// what each lane gets back, exactness, and ns a step of a dependent chain. One core (8 x 4 pixels).
//   xlane [--warp] [--debug]          (bin\xlane.exe, probes\build_floor.bat)
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d11_1.h>
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
#pragma comment(lib, "dxguid.lib")
static ID3D11Device* dev; static ID3D11DeviceContext* ctx;
static void ck(HRESULT h, const char* w) { if (FAILED(h)) { printf("%s failed: %08lx\n", w, (unsigned long)h); exit(1); } }
static ID3DBlob* comp(const char* entry, const char* prof, int mode) {
    char ms[16]; snprintf(ms, sizeof ms, "%d", mode);
    D3D_SHADER_MACRO m[] = {{"MODE", ms}, {nullptr, nullptr}};
    ID3DBlob *b, *err = nullptr;
    if (FAILED(D3DCompileFromFile(L"gpu_vm\\tick\\probes\\xlane.hlsl", m, nullptr, entry, prof, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err))) { printf("%s %d: %s", entry, mode, err ? (char*)err->GetBufferPointer() : "?"); exit(1); }
    if (err) printf("(mode %d: %s)\n", mode, (char*)err->GetBufferPointer());
    return b;
}
int main(int argc, char** argv) {
    bool warp = false, debug = false;
    for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--warp") warp = true; if (a == "--debug") debug = true; }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    HRESULT h = D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(h) && debug) { printf("no debug layer (%08lx): without it\n", (unsigned long)h); debug = false; ck(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device"); }
    else ck(h, "device (feature level 11_1: UAV slot 8)");
    ID3D11InfoQueue* iq = nullptr;
    if (debug) dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq);
    auto msgs = [&](const char* when) {
        if (!iq) return;
        UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; i++) { SIZE_T len = 0; iq->GetMessage(i, nullptr, &len); std::vector<char> b(len); auto* m = (D3D11_MESSAGE*)b.data(); iq->GetMessage(i, m, &len); printf("  debug layer (%s): %.*s\n", when, (int)m->DescriptionByteLength, m->pDescription); }
        iq->ClearStoredMessages();
    };
    D3D11_TEXTURE2D_DESC od = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* out; ID3D11RenderTargetView* rtv; ck(dev->CreateTexture2D(&od, nullptr, &out), "out"); ck(dev->CreateRenderTargetView(out, nullptr, &rtv), "rtv");
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st; ck(dev->CreateTexture2D(&od, nullptr, &st), "staging");
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer* cb; ck(dev->CreateBuffer(&bd, nullptr, &cb), "cb");
    ID3DBlob* vb = comp("vert", "vs_5_0", 0); ID3D11VertexShader* vs; ck(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "vs");
    D3D11_VIEWPORT vp = {0, 0, 8, 4, 0, 1};
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);   // (nothing at u8)
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetConstantBuffers(0, 1, &cb); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto ps_of = [&](int mode) { ID3DBlob* b = comp("frag", "ps_5_0", mode); ID3D11PixelShader* p; ck(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p), "ps"); return p; };
    auto run = [&](ID3D11PixelShader* ps, uint32_t n, uint32_t kind) {
        ctx->PSSetShader(ps, nullptr, 0); uint32_t c[4] = {n, 0x5bd1e995, kind, 0}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0); ctx->Draw(3, 0);
        ctx->CopyResource(st, out); D3D11_MAPPED_SUBRESOURCE m; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
        std::vector<uint32_t> v(32 * 4); for (int y = 0; y < 4; y++) memcpy(&v[y * 32], (char*)m.pData + y * m.RowPitch, 128);
        ctx->Unmap(st, 0); return v;
    };
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qdis, *q0, *q1; dev->CreateQuery(&qd, &qdis); qd.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
    auto timed = [&](ID3D11PixelShader* ps, uint32_t n) {
        ctx->PSSetShader(ps, nullptr, 0); uint32_t c[4] = {n, 0x5bd1e995, 1, 0}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
        ctx->Begin(qdis); ctx->End(q0); ctx->Draw(3, 0); ctx->End(q1); ctx->End(qdis);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
        while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {}
        while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {}
        while (ctx->GetData(q1, &b, sizeof b, 0) != S_OK) {}
        return dj.Disjoint ? 1e9 : double(b - a) / dj.Frequency;
    };
    auto ns_of = [&](ID3D11PixelShader* ps) {
        if (warp) return 0.0;
        uint32_t n1 = 2000, n2 = 22000;
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        do { timed(ps, n2); QueryPerformanceCounter(&b); } while (double(b.QuadPart - a.QuadPart) / f.QuadPart < 1.5);
        double b1 = 1e9, b2 = 1e9;
        for (int r = 0; r < 7; r++) { b1 = std::min(b1, timed(ps, n1)); b2 = std::min(b2, timed(ps, n2)); }
        if (b2 > 0.25) { printf("a draw took %.3f s: stopped\n", b2); exit(1); }
        return (b2 - b1) / (n2 - n1) * 1e9;
    };
    // (j) the unbound UAV's atomic: what each lane gets back
    ID3D11PixelShader* p0 = ps_of(0); msgs("compile");
    for (uint32_t kind = 0; kind < 2; kind++) {
        std::vector<uint32_t> v = run(p0, 1, kind); msgs("draw");
        printf("(j) InterlockedAdd on an unbound UAV, lane k adds %s: orig by pixel (rows y 0-3, x 0-7):\n", kind ? "k + 1" : "1 << k");
        for (int y = 0; y < 4; y++) { printf("   "); for (int x = 0; x < 8; x++) printf(kind ? " %4u" : " %08x", v[(y * 8 + x) * 4]); printf("\n"); }
    }
    ID3D11PixelShader* p1 = ps_of(1);
    std::vector<uint32_t> c1 = run(p1, 1000, 1);
    printf("(j) a dependent chain of them: %.2f ns a step (lane 0's x %08x)\n", ns_of(p1), c1[0]); msgs("chain");
    // (k) the quad's derivatives: each pixel's neighbours' words, exact?
    ID3D11PixelShader* p2 = ps_of(2);
    std::vector<uint32_t> v = run(p2, 1, 0);
    int bad = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) {
        uint32_t* q = &v[(y * 8 + x) * 4];
        uint32_t hx = v[(y * 8 + (x ^ 1)) * 4], hy = v[((y ^ 1) * 8 + x) * 4];
        if (q[1] != hx || q[2] != hy) { if (bad < 4) printf("   pixel (%d, %d): neighbours %08x %08x, got %08x %08x\n", x, y, hx, hy, q[1], q[2]); bad++; }
    }
    printf("(k) ddx_fine / ddy_fine quad exchange of 32-bit words (two 16-bit halves): %d of 32 pixels wrong\n", bad);
    ID3D11PixelShader* p3 = ps_of(3); ID3D11PixelShader* p4 = ps_of(4);
    double e = ns_of(p3), r = ns_of(p4);
    printf("(k) a dependent chain of horizontal exchanges: %.2f ns a step (the same loop without the exchange: %.2f)\n", e, r);
    msgs("end");
    FILE* p = warp ? nullptr : _popen("nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits", "r");
    int mhz = 0; if (p) { if (fscanf(p, "%d", &mhz) != 1) mhz = 0; _pclose(p); printf("clock %d MHz\n", mhz); }
    return 0;
}
