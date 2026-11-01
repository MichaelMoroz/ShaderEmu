// A bench for a tick grown from an empty shader: RAM a texture, the state eight targets, a block of pixels drawn by one
// triangle; it times the draw on the card and prints ns an instruction. Every run through tools\gpu_queue.ps1.
//   bench SHADER RAM.bin [--core WxH] [--budget N] [--passes N] [--warp] [-D NAME=V]... [--dump N]
// Word 0 of pixel (1,0)'s first texel is the pc, word 1 the instructions so far; registers in pixel (0,0)'s eight texels.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
static void check(HRESULT h, const char* w) { if (FAILED(h)) { printf("%s failed: %08x\n", w, (unsigned)h); exit(1); } }
static ID3D11Device* dev; static ID3D11DeviceContext* ctx;
struct Tex { ID3D11Texture2D* t; ID3D11ShaderResourceView* s; ID3D11RenderTargetView* r; };
static Tex tex(int w, int h, const void* init, UINT pitch) {
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_UINT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA sd = {init, pitch, 0}; Tex x = {};
    check(dev->CreateTexture2D(&d, init ? &sd : nullptr, &x.t), "texture");
    check(dev->CreateShaderResourceView(x.t, nullptr, &x.s), "srv"); check(dev->CreateRenderTargetView(x.t, nullptr, &x.r), "rtv");
    return x;
}
static ID3DBlob* compile(const std::string& file, const char* entry, const char* profile, const std::vector<std::string>& defs) {
    std::vector<D3D_SHADER_MACRO> m; std::vector<std::string> names, vals;
    names.reserve(defs.size()); vals.reserve(defs.size());
    for (auto& d : defs) { auto e = d.find('='); names.push_back(d.substr(0, e)); vals.push_back(e == std::string::npos ? "1" : d.substr(e + 1)); m.push_back({names.back().c_str(), vals.back().c_str()}); }
    m.push_back({nullptr, nullptr});
    std::wstring w(file.begin(), file.end()); ID3DBlob* b = nullptr, *err = nullptr;
    HRESULT h = D3DCompileFromFile(w.c_str(), m.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
    if (err) printf("%s\n", (const char*)err->GetBufferPointer());
    check(h, "compile");
    return b;
}
int main(int argc, char** argv) {
    if (argc < 3) { printf("bench SHADER RAM.bin [--core WxH] [--budget N] [--passes N] [--warp] [-D X=V] [--dump N] [--out BLOB]\n"); return 1; }
    std::string shader = argv[1], ramf = argv[2], out; int cw = 4, ch = 8, budget = 8192, passes = 400, dump = 0; bool warp = false; std::vector<std::string> defs;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--core") sscanf(argv[++i], "%dx%d", &cw, &ch);
        else if (a == "--budget") budget = atoi(argv[++i]);
        else if (a == "--passes") passes = atoi(argv[++i]);
        else if (a == "--warp") warp = true;
        else if (a == "-D") defs.push_back(argv[++i]);
        else if (a == "--dump") dump = atoi(argv[++i]);
        else if (a == "--out") out = argv[++i];
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device");
    FILE* f = fopen(ramf.c_str(), "rb"); if (!f) { printf("no %s\n", ramf.c_str()); return 1; }
    const int RW = 2048, RH = 2048; std::vector<unsigned> ram(RW * RH * 4);
    size_t nw = fread(ram.data(), 4, ram.size(), f); fclose(f);
    Tex R = tex(RW, RH, ram.data(), RW * 16);
    const int SW = 64, SH = 16; Tex S[2][8];
    std::vector<unsigned> zero(SW * SH * 4);
    for (int p = 0; p < 2; p++) for (int k = 0; k < 8; k++) S[p][k] = tex(SW, SH, zero.data(), SW * 16);
    ID3DBlob* vs = compile(shader, "vert", "vs_5_0", defs), *ps = compile(shader, "frag", "ps_5_0", defs);
    if (!out.empty()) { FILE* o = fopen(out.c_str(), "wb"); fwrite(ps->GetBufferPointer(), 1, ps->GetBufferSize(), o); fclose(o); }
    ID3D11VertexShader* v; ID3D11PixelShader* p;
    check(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &v), "vs"); check(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p), "ps");
    ctx->VSSetShader(v, nullptr, 0); ctx->PSSetShader(p, nullptr, 0); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; unsigned cbd[4] = {(unsigned)budget, 0, 0, 0}; D3D11_SUBRESOURCE_DATA cbi = {cbd}; ID3D11Buffer* cb;
    check(dev->CreateBuffer(&bd, &cbi, &cb), "cb"); ctx->PSSetConstantBuffers(0, 1, &cb);
    D3D11_VIEWPORT vp = {0, 0, (float)SW, (float)SH, 0, 1}; ctx->RSSetViewports(1, &vp);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; ID3D11RasterizerState* rs; check(dev->CreateRasterizerState(&rd, &rs), "rs"); ctx->RSSetState(rs);
    D3D11_RECT sc = {0, 0, cw, ch}; ctx->RSSetScissorRects(1, &sc);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}; ID3D11Query* dis; check(dev->CreateQuery(&qd, &dis), "q"); qd.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Query* q0, *q1; check(dev->CreateQuery(&qd, &q0), "q"); check(dev->CreateQuery(&qd, &q1), "q");
    D3D11_TEXTURE2D_DESC sd = {}; S[0][0].t->GetDesc(&sd); sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; ID3D11Texture2D* st; check(dev->CreateTexture2D(&sd, nullptr, &st), "staging");
    std::vector<double> times; int cur = 0; LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    const int BATCH = 32;
    unsigned count0 = 0;
    auto readback = [&](int side, int k, int x, int y, unsigned* to) {
        ctx->CopyResource(st, S[side][k].t); D3D11_MAPPED_SUBRESOURCE m; check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
        memcpy(to, (char*)m.pData + y * m.RowPitch + x * 16, 16); ctx->Unmap(st, 0);
    };
    unsigned first[4] = {}; int warm = 16 * BATCH / BATCH; int measured = 0;
    for (int b = 0; b * BATCH < passes; b++) {
        if (b == warm) { readback(cur, 0, 1, 0, first); QueryPerformanceCounter(&t0); }
        ctx->Begin(dis); ctx->End(q0);
        for (int i = 0; i < BATCH; i++) {
            ID3D11ShaderResourceView* n[9] = {}; ctx->PSSetShaderResources(0, 9, n);
            ID3D11RenderTargetView* r[8]; ID3D11ShaderResourceView* s[9];
            s[0] = R.s; for (int k = 0; k < 8; k++) { s[k + 1] = S[cur][k].s; r[k] = S[1 - cur][k].r; }
            ctx->OMSetRenderTargets(8, r, nullptr); ctx->PSSetShaderResources(0, 9, s);
            ctx->Draw(3, 0); cur = 1 - cur;
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        ctx->End(q1); ctx->End(dis); ctx->Flush();
        UINT64 a, c; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        while (ctx->GetData(dis, &dj, sizeof dj, 0) != S_OK) {}
        ctx->GetData(q0, &a, 8, 0); ctx->GetData(q1, &c, 8, 0);
        if (b >= warm && !dj.Disjoint) { times.push_back((double)(c - a) / dj.Frequency / BATCH); measured += BATCH; }
    }
    QueryPerformanceCounter(&t1);
    unsigned last[4]; readback(cur, 0, 1, 0, last);
    std::sort(times.begin(), times.end());
    double med = times.empty() ? 0 : times[times.size() / 2];
    double per = (double)(last[1] - first[1]) / max(1, measured + 0);
    double wall = (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart / max(1, measured);
    printf("RESULT core=%dx%d budget=%d pass_us=%.2f wall_us=%.2f instr_a_pass=%.1f ns_a_instr=%.2f pc=%08x count=%u words=%zu\n", cw, ch, budget, med * 1e6, wall * 1e6, per, per > 0 ? med * 1e9 / per : 0.0, last[0], last[1], nw);
    if (dump) { for (int k = 0; k < dump && k < 8; k++) { unsigned w[4]; readback(cur, k, 0, 0, w); printf("texel %d: %08x %08x %08x %08x\n", k, w[0], w[1], w[2], w[3]); } }
    return 0;
}
