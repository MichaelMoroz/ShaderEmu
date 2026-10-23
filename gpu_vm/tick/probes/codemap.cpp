// codemap: ns a fetch of one core (an 8 x 4 block) walking N texels of code (straight, i -> i + 1) or of data (a random cycle)
// in a loop, with where texel i lies in the texture as the variable (codemap.hlsl's MAPs): today's row-major 8192-wide
// texture, narrower rows, tiles of T x T, Morton, column-major, and a StructuredBuffer. fxc2 as the tick; GPU timestamps,
// the best of rounds, the difference of two lengths (both after a warm trip), each draw well under a second.
//   codemap [--warp] [--rounds 5] [--only NAME] [--pat 0|1] [--f2 0|1]      (bin\codemap.exe, probes\build_floor.bat)
//   codemap --short N [--pf 1|2 --pfn TEXELS --pfe STEPS]   draws of N steps in order; --pf: the lanes warm the cache first
//                                                            (1) or every PFE steps ahead of the chain (2), codemap.hlsl
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
static const wchar_t* PATH = L"gpu_vm\\tick\\probes\\codemap.hlsl";
static const uint32_t TW = 8192, TH = 1024;
struct Map { const char* name; int map, w, t, h; };
static const Map MAPS[] = {
    {"row 8192 (today)", 0, 8192, 0, 0}, {"row 4096", 0, 4096, 0, 0}, {"row 2048", 0, 2048, 0, 0}, {"row 1024", 0, 1024, 0, 0}, {"row 512", 0, 512, 0, 0}, {"row 64", 0, 64, 0, 0},
    {"tiles 2x2", 1, 0, 2, 0}, {"tiles 4x4", 1, 0, 4, 0}, {"tiles 8x8", 1, 0, 8, 0}, {"Morton", 2, 0, 0, 0},
    {"column 1024", 3, 0, 0, 1024}, {"strips 512x1024", 5, 512, 0, 1024}, {"strips 256x1024", 5, 256, 0, 1024}, {"strips 512x64", 5, 512, 0, 64}, {"StructuredBuffer", 4, 0, 0, 0}, {"pairs 8192", 6, 8192, 0, 0},
};
static uint32_t even(uint32_t v) {
    v &= 0x55555555u; v = (v | (v >> 1)) & 0x33333333u; v = (v | (v >> 2)) & 0x0f0f0f0fu; v = (v | (v >> 4)) & 0x00ff00ffu; return (v | (v >> 8)) & 0x0000ffffu;
}
static void place(const Map& m, uint32_t i, uint32_t& x, uint32_t& y) {
    switch (m.map) {
    case 0: x = i & (m.w - 1); y = i / m.w; break;
    case 1: { uint32_t tile = i / (m.t * m.t), in = i & (m.t * m.t - 1); x = (tile & (8192 / m.t - 1)) * m.t + (in & (m.t - 1)); y = (tile / (8192 / m.t)) * m.t + in / m.t; break; }
    case 2: { uint32_t lo = i & 0xfffff; x = even(lo) | ((i >> 20) << 10); y = even(lo >> 1); break; }
    case 3: x = i / m.h; y = i & (m.h - 1); break;
    case 5: x = (i & (m.w - 1)) + (i / (m.w * m.h)) * m.w; y = (i / m.w) & (m.h - 1); break;
    case 6: x = (i >> 1) & (m.w - 1); y = ((i >> 1) / m.w) * 2 + (i & 1); break;
    default: x = i % TW; y = i / TW;   // (the buffer: element i)
    }
}
static int g_ser = 0, g_pf = 0, g_pfn = 1024, g_pfe = 256, g_pfs = 1;
static ID3DBlob* comp(const char* entry, const char* prof, const Map& m, int f2) {
    char a[16], b[16], c[16], d[16], e[16], g[16], p1[16], p2[16], p3[16], p4[16];
    snprintf(a, 16, "%d", m.map); snprintf(b, 16, "%d", m.w ? m.w : 8192); snprintf(c, 16, "%d", m.t ? m.t : 4); snprintf(d, 16, "%d", m.h ? m.h : 1024); snprintf(e, 16, "%d", f2); snprintf(g, 16, "%d", g_ser);
    snprintf(p1, 16, "%d", g_pf); snprintf(p2, 16, "%d", g_pfn); snprintf(p3, 16, "%d", g_pfe); snprintf(p4, 16, "%d", g_pfs);
    D3D_SHADER_MACRO mac[] = {{"MAP", a}, {"W", b}, {"T", c}, {"H", d}, {"SW", b}, {"SH", d}, {"F2", e}, {"SER", g}, {"PF", p1}, {"PFN", p2}, {"PFE", p3}, {"PFS", p4}, {nullptr, nullptr}};
    ID3DBlob *bl, *err = nullptr;
    if (FAILED(D3DCompileFromFile(PATH, mac, nullptr, entry, prof, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bl, &err))) { printf("%s: %s", entry, err ? (char*)err->GetBufferPointer() : "?"); exit(1); }
    return bl;
}
int main(int argc, char** argv) {
    bool warp = false; int rounds = 5, pat = -1, f2s = -1, shortN = 0, lineN = 0; std::string only;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true; else if (a == "--rounds" && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i]; else if (a == "--pat" && i + 1 < argc) pat = atoi(argv[++i]);
        else if (a == "--f2" && i + 1 < argc) f2s = atoi(argv[++i]);
        else if (a == "--short" && i + 1 < argc) shortN = atoi(argv[++i]);
        else if (a == "--pf" && i + 1 < argc) g_pf = atoi(argv[++i]);
        else if (a == "--pfn" && i + 1 < argc) g_pfn = atoi(argv[++i]);
        else if (a == "--pfe" && i + 1 < argc) g_pfe = atoi(argv[++i]);
        else if (a == "--pfs" && i + 1 < argc) g_pfs = atoi(argv[++i]);
        else if (a == "--line" && i + 1 < argc) lineN = atoi(argv[++i]);
    }
    if (g_pf && !shortN) { printf("--pf needs --short\n"); return 2; }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ck(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device");
    ID3D11Texture2D* tx; ID3D11ShaderResourceView* tsrv;
    { D3D11_TEXTURE2D_DESC d = {TW, TH, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
      ck(dev->CreateTexture2D(&d, nullptr, &tx), "tex"); ck(dev->CreateShaderResourceView(tx, nullptr, &tsrv), "srv"); }
    ID3D11RenderTargetView* trtv; ck(dev->CreateRenderTargetView(tx, nullptr, &trtv), "tex rtv");
    const uint32_t BN = 16384;
    ID3D11Buffer* sb; ID3D11ShaderResourceView* bsrv;
    { D3D11_BUFFER_DESC bd = {BN * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
      ck(dev->CreateBuffer(&bd, nullptr, &sb), "sb"); ck(dev->CreateShaderResourceView(sb, nullptr, &bsrv), "sb srv"); }
    ID3D11Texture2D* ot; ID3D11RenderTargetView* ortv;
    { D3D11_TEXTURE2D_DESC d = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
      ck(dev->CreateTexture2D(&d, nullptr, &ot), "out"); ck(dev->CreateRenderTargetView(ot, nullptr, &ortv), "rtv"); }
    ID3D11Texture2D* st; { D3D11_TEXTURE2D_DESC d = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0}; ck(dev->CreateTexture2D(&d, nullptr, &st), "staging"); }
    ID3D11Buffer* cb; { D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0}; ck(dev->CreateBuffer(&bd, nullptr, &cb), "cb"); }
    Map m0 = MAPS[0];
    ID3DBlob* vb = comp("vert", "vs_5_0", m0, 0);
    ID3D11VertexShader* vs; ck(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "vs");
    ctx->VSSetShader(vs, nullptr, 0); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp = {0, 0, 8, 4, 0, 1}; ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &ortv, nullptr);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qdis, *q0, *q1; dev->CreateQuery(&qd, &qdis); qd.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
    std::vector<uint32_t> img((size_t)TW * TH * 4, 0), buf(BN * 4, 0);
    const uint32_t SIZES[] = {128, 512, 1024, 2048, 8192};   // texels: 2, 8, 16, 32, 128 KB
    printf("%-18s %3s %2s", "map", "pat", "f2"); for (uint32_t n : SIZES) printf(" %8uK", n * 16 / 1024); printf("   (ns a step)\n");
    uint32_t seed = 12345; auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    bool bad = false;
    if (lineN) {   // --line N: ns a step (a base texel, then the one dx, dy from it), D draws in order, each cold
        g_ser = 1;
        ID3D11Texture2D* pt[2]; ID3D11RenderTargetView* prtv[2]; ID3D11ShaderResourceView* psrv[2];
        for (int k = 0; k < 2; k++) { D3D11_TEXTURE2D_DESC d = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
          ck(dev->CreateTexture2D(&d, nullptr, &pt[k]), "prev"); ck(dev->CreateRenderTargetView(pt[k], nullptr, &prtv[k]), "prev rtv"); ck(dev->CreateShaderResourceView(pt[k], nullptr, &psrv[k]), "prev srv"); }
        ID3DBlob* b = comp("linefrag", "ps_5_0", m0, 0); ID3D11PixelShader* ps; ck(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps), "ps");
        std::fill(img.begin(), img.end(), 0); ctx->UpdateSubresource(tx, 0, nullptr, img.data(), TW * 16, 0);
        const int D = warp ? 3 : 64;
        printf("--line %d: ns a step (a cold base texel, then the texel dx, dy from it), %d draws in order\n  dy\\dx", lineN, D);
        for (int dx = 0; dx < 16; dx++) printf(" %6d", dx); printf("\n");
        bool warm = false;
        for (int dy = 0; dy < 8; dy++) {
            printf("  %5d", dy); fflush(stdout);
            for (int dx = 0; dx < 16; dx++) {
                uint32_t c[4] = {(uint32_t)lineN, 1, (uint32_t)dx, (uint32_t)dy}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
                auto run = [&]() {
                    const float z[4] = {0, 0, 0, 0}; for (int k = 0; k < 2; k++) ctx->ClearRenderTargetView(prtv[k], z);
                    ctx->Begin(qdis); ctx->End(q0);
                    for (int k = 0; k < D; k++) {
                        ID3D11ShaderResourceView* nil[3] = {}; ctx->PSSetShaderResources(0, 3, nil);
                        ctx->OMSetRenderTargets(1, &prtv[k & 1], nullptr); ctx->RSSetViewports(1, &vp);
                        ID3D11ShaderResourceView* sv[3] = {tsrv, bsrv, psrv[(k & 1) ^ 1]};
                        ctx->PSSetShaderResources(0, 3, sv); ctx->PSSetConstantBuffers(0, 1, &cb); ctx->PSSetShader(ps, nullptr, 0);
                        ctx->Draw(3, 0);
                    }
                    ctx->End(q1); ctx->End(qdis);
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, bb;
                    while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {} while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {} while (ctx->GetData(q1, &bb, sizeof bb, 0) != S_OK) {}
                    return dj.Disjoint ? 1e9 : double(bb - a) / dj.Frequency;
                };
                if (!warm && !warp) { warm = true; LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); do { run(); QueryPerformanceCounter(&t1); } while (double(t1.QuadPart - t0.QuadPart) / fq.QuadPart < 1.5); }
                double best = 1e9; for (int r = 0; r < rounds; r++) best = std::min(best, run());
                if (best > 0.5) { printf("\na batch took %.3f s: stopped\n", best); return 1; }
                ctx->CopyResource(st, pt[(D - 1) & 1]); D3D11_MAPPED_SUBRESOURCE ms; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms), "map");
                uint32_t got = *(uint32_t*)ms.pData; ctx->Unmap(st, 0);
                if (got != 0) { printf(" WRONG(%u)", got); bad = true; continue; }
                printf(" %6.1f", warp ? 0.0 : best / ((double)D * lineN) * 1e9); fflush(stdout);
            }
            printf("\n");
        }
        printf(bad ? "codemap: WRONG\n" : "codemap: all chains ok\n");
        return bad ? 1 : 0;
    }
    if (shortN) {   // --short N: draws of N steps from the chain's start, D of them back to back, as passes; between them nothing,
                    // a texel drawn into another target, or a texel drawn into the chain's own texture (as the commit draws RAM)
        ID3D11Texture2D* ot2; ID3D11RenderTargetView* o2rtv;
        { D3D11_TEXTURE2D_DESC d = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
          ck(dev->CreateTexture2D(&d, nullptr, &ot2), "out2"); ck(dev->CreateRenderTargetView(ot2, nullptr, &o2rtv), "rtv2"); }
        // the draws in order: each reads the word the last one wrote (two targets in turn), as a pass reads the state the last drew
        g_ser = 1;
        ID3D11Texture2D* pt[2]; ID3D11RenderTargetView* prtv[2]; ID3D11ShaderResourceView* psrv[2];
        for (int k = 0; k < 2; k++) { D3D11_TEXTURE2D_DESC d = {8, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
          ck(dev->CreateTexture2D(&d, nullptr, &pt[k]), "prev"); ck(dev->CreateRenderTargetView(pt[k], nullptr, &prtv[k]), "prev rtv"); ck(dev->CreateShaderResourceView(pt[k], nullptr, &psrv[k]), "prev srv"); }
        ID3DBlob* fb = comp("fill", "ps_5_0", m0, 0); ID3D11PixelShader* fps; ck(dev->CreatePixelShader(fb->GetBufferPointer(), fb->GetBufferSize(), nullptr, &fps), "fill");
        const char* conds[] = {"nothing between", "a texel of another target", "a texel of the chain's texture"};
        const uint32_t SZ[] = {512, 1024, 2048, 8192};
        const int D = warp ? 3 : 64;
        printf("--short %d --pf %d (pfn %d, pfe %d, pfs %d): ns a step, %d draws of %d steps back to back\n%-18s %-30s", shortN, g_pf, g_pfn, g_pfe, g_pfs, D, shortN, "map", "between draws");
        for (uint32_t n : SZ) printf(" %7uK", n * 16 / 1024); printf("\n");
        for (int mi : {0, 13, 15}) {
            const Map* mp = nullptr; for (const Map& m : MAPS) if ((mi == 0 && m.map == 0 && m.w == 8192) || (mi == 13 && m.map == 5 && m.w == 512 && m.h == 1024) || (mi == 15 && m.map == 6)) mp = &m;
            const Map& m = *mp;
            ID3DBlob* b = comp("frag", "ps_5_0", m, 0); ID3D11PixelShader* ps; ck(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps), "ps");
            for (int cnd = 0; cnd < 3; cnd++) {
                printf("%-18s %-30s", m.name, conds[cnd]); fflush(stdout);
                for (uint32_t N : SZ) {
                    std::fill(img.begin(), img.end(), 0);
                    for (uint32_t i = 0; i < N; i++) { uint32_t x, y; place(m, i, x, y); img[((size_t)y * TW + x) * 4] = (i + 1) % N; }
                    ctx->UpdateSubresource(tx, 0, nullptr, img.data(), TW * 16, 0);
                    uint32_t c[4] = {(uint32_t)shortN, N, 0, 0}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
                    { const float z[4] = {0, 0, 0, 0}; for (int k = 0; k < 2; k++) ctx->ClearRenderTargetView(prtv[k], z); }
                    auto run = [&]() {
                        { const float z[4] = {0, 0, 0, 0}; for (int k = 0; k < 2; k++) ctx->ClearRenderTargetView(prtv[k], z); }
                        ctx->Begin(qdis); ctx->End(q0);
                        for (int k = 0; k < D; k++) {
                            ID3D11ShaderResourceView* nil[3] = {}; ctx->PSSetShaderResources(0, 3, nil);
                            ctx->OMSetRenderTargets(1, &prtv[k & 1], nullptr); ctx->RSSetViewports(1, &vp);
                            ID3D11ShaderResourceView* sv[3] = {tsrv, bsrv, psrv[(k & 1) ^ 1]};
                            ctx->PSSetShaderResources(0, 3, sv); ctx->PSSetConstantBuffers(0, 1, &cb); ctx->PSSetShader(ps, nullptr, 0);
                            ctx->Draw(3, 0);
                            if (cnd == 0) continue;
                            ID3D11ShaderResourceView* none[3] = {}; ctx->PSSetShaderResources(0, 3, none);
                            D3D11_VIEWPORT v1 = {(float)(TW - 1), (float)(TH - 1), 1, 1, 0, 1};
                            if (cnd == 1) { ctx->OMSetRenderTargets(1, &o2rtv, nullptr); v1.TopLeftX = 0; v1.TopLeftY = 0; } else ctx->OMSetRenderTargets(1, &trtv, nullptr);
                            ctx->RSSetViewports(1, &v1); ctx->PSSetShader(fps, nullptr, 0); ctx->Draw(3, 0);
                        }
                        ctx->End(q1); ctx->End(qdis);
                        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, bb;
                        while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {} while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {} while (ctx->GetData(q1, &bb, sizeof bb, 0) != S_OK) {}
                        return dj.Disjoint ? 1e9 : double(bb - a) / dj.Frequency;
                    };
                    static bool warm = false;
                    if (!warm && !warp) { warm = true; LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); do { run(); QueryPerformanceCounter(&t1); } while (double(t1.QuadPart - t0.QuadPart) / fq.QuadPart < 1.5); }
                    double best = 1e9; for (int r = 0; r < rounds; r++) best = std::min(best, run());
                    if (best > 0.5) { printf("\na batch took %.3f s: stopped\n", best); return 1; }
                    ctx->OMSetRenderTargets(1, &ortv, nullptr); ctx->RSSetViewports(1, &vp);
                    ctx->CopyResource(st, pt[(D - 1) & 1]); D3D11_MAPPED_SUBRESOURCE ms; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms), "map");
                    uint32_t got = *(uint32_t*)ms.pData; ctx->Unmap(st, 0);
                    if (got != (uint32_t)((uint64_t)D * shortN % N)) { printf(" WRONG(%u)", got); bad = true; continue; }
                    printf(" %8.1f", warp ? 0.0 : best / ((double)D * shortN) * 1e9); fflush(stdout);
                }
                printf("\n");
            }
        }
        FILE* pp = warp ? nullptr : _popen("nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits", "r");
        int mhz = 0; if (pp) { if (fscanf(pp, "%d", &mhz) != 1) mhz = 0; _pclose(pp); printf("clock %d MHz\n", mhz); }
        printf(bad ? "codemap: WRONG\n" : "codemap: all chains ok\n");
        return bad ? 1 : 0;
    }
    for (int p = 0; p <= 1; p++) for (int f2 = 0; f2 <= 1; f2++) {
        if ((pat >= 0 && p != pat) || (f2s >= 0 && f2 != f2s)) continue;
        for (const Map& m : MAPS) {
            if (!only.empty() && only != m.name) continue;
            ID3DBlob* b = comp("frag", "ps_5_0", m, f2); ID3D11PixelShader* ps; ck(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps), "ps");
            printf("%-18s %3d %2d", m.name, p, f2); fflush(stdout);
            for (uint32_t N : SIZES) {
                std::vector<uint32_t> nx(N);   // the chain
                if (p == 0) for (uint32_t i = 0; i < N; i++) nx[i] = (i + 1) % N;
                else { std::vector<uint32_t> o(N); for (uint32_t i = 0; i < N; i++) o[i] = i; for (uint32_t i = N - 1; i > 0; i--) std::swap(o[i], o[rnd() % (i + 1)]);
                       for (uint32_t i = 0; i < N; i++) nx[o[i]] = o[(i + 1) % N]; }
                std::fill(img.begin(), img.end(), 0); std::fill(buf.begin(), buf.end(), 0);
                for (uint32_t i = 0; i < N; i++) {
                    uint32_t x, y; place(m, i, x, y);
                    if (m.map == 4) buf[4 * i] = nx[i]; else img[((size_t)y * TW + x) * 4] = nx[i];
                }
                if (m.map == 4) ctx->UpdateSubresource(sb, 0, nullptr, buf.data(), 0, 0); else ctx->UpdateSubresource(tx, 0, nullptr, img.data(), TW * 16, 0);
                ID3D11ShaderResourceView* sv[2] = {tsrv, bsrv}; ctx->PSSetShaderResources(0, 2, sv); ctx->PSSetConstantBuffers(0, 1, &cb); ctx->PSSetShader(ps, nullptr, 0);
                auto timed = [&](uint32_t n) {
                    uint32_t c[4] = {n, 0, 0, 0}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
                    ctx->Begin(qdis); ctx->End(q0); ctx->Draw(3, 0); ctx->End(q1); ctx->End(qdis);
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, bb;
                    while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {} while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {} while (ctx->GetData(q1, &bb, sizeof bb, 0) != S_OK) {}
                    return dj.Disjoint ? 1e9 : double(bb - a) / dj.Frequency;
                };
                uint32_t n1 = warp ? 300 : std::max(2 * N, 4000u), n2 = warp ? 900 : n1 + std::max(8 * N, 40000u);
                timed(n1);
                ctx->CopyResource(st, ot); D3D11_MAPPED_SUBRESOURCE ms; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms), "map");
                uint32_t got = *(uint32_t*)ms.pData; ctx->Unmap(st, 0);
                uint32_t want = 0; for (uint32_t k = 0; k < n1; k++) want = nx[want];
                if (got != want) { printf(" WRONG(%u/%u)", got, want); bad = true; continue; }
                double ns = 0;
                if (!warp) {
                    static bool warm = false;   // (the clock up, once)
                    if (!warm) { warm = true; LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0); do { timed(n2); QueryPerformanceCounter(&t1); } while (double(t1.QuadPart - t0.QuadPart) / fq.QuadPart < 1.5); }
                    double b1 = 1e9, b2 = 1e9;
                    for (int r = 0; r < rounds; r++) { b1 = std::min(b1, timed(n1)); b2 = std::min(b2, timed(n2)); }
                    if (b2 > 0.25) { printf("\na draw took %.3f s: stopped\n", b2); return 1; }
                    ns = (b2 - b1) / (n2 - n1) * 1e9;
                }
                printf(" %9.1f", ns); fflush(stdout);
            }
            printf("\n"); ps->Release();
        }
    }
    FILE* pp = warp ? nullptr : _popen("nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits", "r");
    int mhz = 0; if (pp) { if (fscanf(pp, "%d", &mhz) != 1) mhz = 0; _pclose(pp); printf("clock %d MHz\n", mhz); }
    printf(bad ? "codemap: WRONG\n" : "codemap: all chains ok\n");
    return bad ? 1 : 0;
}
