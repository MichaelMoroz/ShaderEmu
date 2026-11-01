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
    std::string shader = argv[1], ramf = argv[2], out, commitf; int commitN = 0; bool twice = false; std::string snapf, dumpram, codef; int reps = 3, maxpasses = 400; int cw = 4, ch = 8, budget = 8192, passes = 400, dump = 0; bool warp = false; std::vector<std::string> defs;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--core") sscanf(argv[++i], "%dx%d", &cw, &ch);
        else if (a == "--budget") budget = atoi(argv[++i]);
        else if (a == "--passes") passes = atoi(argv[++i]);
        else if (a == "--warp") warp = true;
        else if (a == "-D") defs.push_back(argv[++i]);
        else if (a == "--dump") dump = atoi(argv[++i]);
        else if (a == "--out") out = argv[++i];
        else if (a == "--snap") snapf = argv[++i];
        else if (a == "--code") codef = argv[++i];
        else if (a == "--twice") twice = true;
        else if (a == "--dump-ram") dumpram = argv[++i];
        else if (a == "--reps") reps = atoi(argv[++i]);
        else if (a == "--max-passes") maxpasses = atoi(argv[++i]);
        else if (a == "--commit") { commitf = argv[++i]; commitN = atoi(argv[++i]); }
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device");
    FILE* f = fopen(ramf.c_str(), "rb"); if (!f) { printf("no %s\n", ramf.c_str()); return 1; }
    const int RW = 2048, RH = 9216; std::vector<unsigned> ram((size_t)RW * RH * 4);
    size_t nw = fread(ram.data(), 4, ram.size(), f); fclose(f);
    unsigned snapState[64][4] = {};
    if (!snapf.empty()) {   // a harness snapshot: RAM and the devices from row 64, core 0's texels in rows 0 on
        FILE* g = fopen(snapf.c_str(), "rb"); if (!g) { printf("no %s\n", snapf.c_str()); return 1; }
        fseek(g, 32, SEEK_SET); fread(snapState, 16, 64, g);
        _fseeki64(g, 32 + (long long)64 * 2048 * 16, SEEK_SET); nw = fread(ram.data(), 4, ram.size(), g); fclose(g);
    }
    Tex R = tex(RW, RH, ram.data(), RW * 16);
    const int SW = 64, SH = 16; Tex S[2][8];
    std::vector<unsigned> zero(SW * SH * 4);
    for (int p = 0; p < 2; p++) for (int k = 0; k < 8; k++) S[p][k] = tex(SW, SH, zero.data(), SW * 16);
    ID3DBlob* vs = compile(shader, "vert", "vs_5_0", defs), *ps = compile(shader, "frag", "ps_5_0", defs);
    if (!out.empty()) { FILE* o = fopen(out.c_str(), "wb"); fwrite(ps->GetBufferPointer(), 1, ps->GetBufferSize(), o); fclose(o); }
    if (!codef.empty()) {   // the predecoded form (xlate.py), slot 9: stays bound
        FILE* g = fopen(codef.c_str(), "rb"); if (!g) { printf("no %s\n", codef.c_str()); return 1; }
        std::vector<unsigned> code((size_t)RW * RH * 4); fread(code.data(), 4, code.size(), g); fclose(g);
        Tex C = tex(RW, RH, code.data(), RW * 16); ctx->PSSetShaderResources(9, 1, &C.s);
    }
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
    ID3D11VertexShader* cv = nullptr; ID3D11PixelShader* cp = nullptr;
    if (!commitf.empty()) {
        ID3DBlob* a = compile(commitf, "cvert", "vs_5_0", defs), *c = compile(commitf, "cfrag", "ps_5_0", defs);
        check(dev->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &cv), "cvs"); check(dev->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &cp), "cps");
    }
    ID3D11Query* qa, *qb, *qc; qd.Query = D3D11_QUERY_TIMESTAMP; check(dev->CreateQuery(&qd, &qa), "q"); check(dev->CreateQuery(&qd, &qb), "q"); check(dev->CreateQuery(&qd, &qc), "q");
    double tickSum = 0, commitSum = 0; int sampled = 0;
    if (!snapf.empty()) {
        // x1 to x31 and the pc in core 0's texels 29 to 36 (experiments/rvc_lean/tick.hlsl, state_out)
        unsigned xr[32] = {}, pc = 0;
        for (int l = 29; l < 37; l++) { int k = 4 * (l - 29); xr[k + 1] = snapState[l][0]; xr[k + 2] = snapState[l][1]; xr[k + 3] = snapState[l][2]; if (l == 36) pc = snapState[l][3]; else xr[k + 4] = snapState[l][3]; }
        D3D11_TEXTURE2D_DESC pd = {}; R.t->GetDesc(&pd); pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = 0; ID3D11Texture2D* pristine; D3D11_SUBRESOURCE_DATA pi = {ram.data(), RW * 16, 0};
        pd.BindFlags = D3D11_BIND_SHADER_RESOURCE; check(dev->CreateTexture2D(&pd, &pi, &pristine), "pristine");
        int lastSide = 0; std::vector<unsigned> dirty;
        double tick = 0, commit = 0; unsigned long long ins = 0, outs = 0, full = 0, passes = 0; unsigned lastc[4] = {}, lastx[4] = {};
        for (int rep = 0; rep < reps; rep++) {
            // only what the last go changed goes back: a copy of the whole of RAM would empty the card's caches, which a machine's passes do not
            if (rep == 0) ctx->CopyResource(R.t, pristine);
            else {
                for (unsigned t : dirty) { D3D11_BOX b = {t & 2047, t >> 11, 0, (t & 2047) + 1, (t >> 11) + 1, 1}; ctx->CopySubresourceRegion(R.t, 0, t & 2047, t >> 11, 0, pristine, 0, &b); }
                dirty.clear();
            }
            int c0 = 0;
            for (int k = 0; k < 8; k++) { D3D11_BOX b = {0, 0, 0, 1, 1, 1}; ctx->UpdateSubresource(S[0][k].t, 0, &b, xr + 4 * k, 16, 0); }
            { unsigned w[4] = {pc, 0, 0, 0}; D3D11_BOX b = {1, 0, 0, 2, 1, 1}; ctx->UpdateSubresource(S[0][0].t, 0, &b, w, 16, 0); }
            unsigned prevN = 0, prevO = 0;
            for (int q = 0; q < maxpasses; q++) {
                ctx->Begin(dis); ctx->End(qa);
                ID3D11ShaderResourceView* n[9] = {}; ctx->PSSetShaderResources(0, 9, n);
                ID3D11RenderTargetView* r[8]; ID3D11ShaderResourceView* sv[9];
                sv[0] = R.s; for (int k = 0; k < 8; k++) { sv[k + 1] = S[c0][k].s; r[k] = S[1 - c0][k].r; }
                ctx->OMSetRenderTargets(8, r, nullptr); ctx->PSSetShaderResources(0, 9, sv);
                D3D11_VIEWPORT vs0 = {0, 0, (float)SW, (float)SH, 0, 1}; ctx->RSSetViewports(1, &vs0); ctx->RSSetScissorRects(1, &sc);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->VSSetShader(v, nullptr, 0); ctx->PSSetShader(p, nullptr, 0);
                if (twice) { ctx->Draw(3, 0); ctx->End(qa); }   // the same pass again, the texture's caches as the first left them: the second is the one timed
                ctx->Draw(3, 0); c0 = 1 - c0; ctx->End(qb);
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                ID3D11ShaderResourceView* n2[9] = {}; ctx->PSSetShaderResources(0, 9, n2);
                ID3D11ShaderResourceView* s2[8]; for (int k = 0; k < 8; k++) s2[k] = S[c0][k].s;
                ctx->VSSetShaderResources(1, 8, s2); ctx->PSSetShaderResources(1, 8, s2);
                ctx->OMSetRenderTargets(1, &R.r, nullptr);
                D3D11_VIEWPORT vr = {0, 0, (float)RW, (float)RH, 0, 1}; ctx->RSSetViewports(1, &vr); D3D11_RECT rr = {0, 0, RW, RH}; ctx->RSSetScissorRects(1, &rr);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); ctx->VSSetShader(cv, nullptr, 0); ctx->PSSetShader(cp, nullptr, 0);
                ctx->Draw(commitN, 0); ctx->OMSetRenderTargets(0, nullptr, nullptr);
                ID3D11ShaderResourceView* n3[8] = {}; ctx->VSSetShaderResources(1, 8, n3); ctx->PSSetShaderResources(1, 8, n3);
                ctx->End(qc); ctx->End(dis);
                unsigned cw[4], xw[4];
                ctx->CopyResource(st, S[c0][0].t); D3D11_MAPPED_SUBRESOURCE m; check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
                memcpy(cw, (char*)m.pData + 16, 16); ctx->Unmap(st, 0);
                ctx->CopyResource(st, S[c0][1].t); check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map"); memcpy(xw, (char*)m.pData + 16, 16); ctx->Unmap(st, 0);
                UINT64 ta, tb, tc; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; while (ctx->GetData(dis, &dj, sizeof dj, 0) != S_OK) {}
                ctx->GetData(qa, &ta, 8, 0); ctx->GetData(qb, &tb, 8, 0); ctx->GetData(qc, &tc, 8, 0);
                unsigned did = cw[1] - prevN; prevN = cw[1];
                if (rep > 0 && did > 0) { tick += (double)(tb - ta) / dj.Frequency; commit += (double)(tc - tb) / dj.Frequency; ins += did; passes++; outs += xw[0]; full += xw[1]; }
                memcpy(lastc, cw, 16); memcpy(lastx, xw, 16);
                lastSide = c0;
                // the entries' tags, texels 16 to 107 of the core (mem.hlsl), to know what the commit changed
                for (int k = 0; k < 8; k++) {
                    ctx->CopyResource(st, S[c0][k].t); D3D11_MAPPED_SUBRESOURCE mt; check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mt), "map");
                    for (int lin = 16 + ((k - 16 % 8 + 8) % 8); lin < 108; lin += 8) {
                        unsigned tg[4]; memcpy(tg, (char*)mt.pData + (lin >> 6) * mt.RowPitch + ((lin >> 3) & 7) * 16, 16);
                        for (int e = 0; e < 4; e++) if (tg[e] != 0xffffffffu) dirty.push_back(tg[e]);
                    }
                    ctx->Unmap(st, 0);
                }
                if (cw[2] != 0) break;
            }
        }
        printf("SNAP instructions=%llu passes=%llu a_pass=%.0f tick_ns=%.2f commit_ns=%.2f whole_ns=%.2f ways_out_a_100=%.1f full_passes=%llu stop=%u word=%08x pc=%08x count=%u\n", ins, passes, (double)ins / max(1ull, passes),
               tick * 1e9 / max(1ull, ins), commit * 1e9 / max(1ull, ins), (tick + commit) * 1e9 / max(1ull, ins), 100.0 * outs / max(1ull, ins), full, lastc[2], lastc[3], lastc[0], lastc[1]);
        for (int k = 0; k < 8; k++) { unsigned w[4]; ctx->CopyResource(st, S[lastSide][k].t); D3D11_MAPPED_SUBRESOURCE m; check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map"); memcpy(w, m.pData, 16); ctx->Unmap(st, 0); printf("texel %d: %08x %08x %08x %08x\n", k, w[0], w[1], w[2], w[3]); }
        if (!dumpram.empty()) {
            D3D11_TEXTURE2D_DESC rd2 = {}; R.t->GetDesc(&rd2); rd2.Usage = D3D11_USAGE_STAGING; rd2.BindFlags = 0; rd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ; ID3D11Texture2D* rs2; check(dev->CreateTexture2D(&rd2, nullptr, &rs2), "ram staging");
            ctx->CopyResource(rs2, R.t); D3D11_MAPPED_SUBRESOURCE m; check(ctx->Map(rs2, 0, D3D11_MAP_READ, 0, &m), "map ram");
            FILE* o = fopen(dumpram.c_str(), "wb");
            for (int y = 0; y < RH; y++) fwrite((char*)m.pData + (size_t)y * m.RowPitch, 16, RW, o);
            fclose(o); ctx->Unmap(rs2, 0);
        }
        return 0;
    }
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
            D3D11_VIEWPORT vs0 = {0, 0, (float)SW, (float)SH, 0, 1}; ctx->RSSetViewports(1, &vs0); ctx->RSSetScissorRects(1, &sc);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->VSSetShader(v, nullptr, 0); ctx->PSSetShader(p, nullptr, 0);
            if (i == BATCH - 1) ctx->End(qa);
            ctx->Draw(3, 0); cur = 1 - cur;
            if (i == BATCH - 1) ctx->End(qb);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (cv) {
                ID3D11ShaderResourceView* n2[9] = {}; ctx->PSSetShaderResources(0, 9, n2);
                ID3D11ShaderResourceView* s2[8]; for (int k = 0; k < 8; k++) s2[k] = S[cur][k].s;
                ctx->VSSetShaderResources(1, 8, s2); ctx->PSSetShaderResources(1, 8, s2);
                ctx->OMSetRenderTargets(1, &R.r, nullptr);
                D3D11_VIEWPORT vr = {0, 0, (float)RW, (float)RH, 0, 1}; ctx->RSSetViewports(1, &vr); D3D11_RECT rr = {0, 0, RW, RH}; ctx->RSSetScissorRects(1, &rr);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST); ctx->VSSetShader(cv, nullptr, 0); ctx->PSSetShader(cp, nullptr, 0);
                ctx->Draw(commitN, 0);
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                ID3D11ShaderResourceView* n3[8] = {}; ctx->VSSetShaderResources(1, 8, n3); ctx->PSSetShaderResources(1, 8, n3);
            }
            if (i == BATCH - 1) ctx->End(qc);
        }
        ctx->End(q1); ctx->End(dis); ctx->Flush();
        UINT64 a, c; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        while (ctx->GetData(dis, &dj, sizeof dj, 0) != S_OK) {}
        ctx->GetData(q0, &a, 8, 0); ctx->GetData(q1, &c, 8, 0);
        if (b >= warm && !dj.Disjoint) { times.push_back((double)(c - a) / dj.Frequency / BATCH); measured += BATCH;
            UINT64 ta, tb, tc; ctx->GetData(qa, &ta, 8, 0); ctx->GetData(qb, &tb, 8, 0); ctx->GetData(qc, &tc, 8, 0);
            tickSum += (double)(tb - ta) / dj.Frequency; commitSum += (double)(tc - tb) / dj.Frequency; sampled++; }
    }
    QueryPerformanceCounter(&t1);
    unsigned last[4]; readback(cur, 0, 1, 0, last);
    std::sort(times.begin(), times.end());
    double med = times.empty() ? 0 : times[times.size() / 2];
    double per = (double)(last[1] - first[1]) / max(1, measured + 0);
    double wall = (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart / max(1, measured);
    printf("RESULT core=%dx%d budget=%d pass_us=%.2f wall_us=%.2f instr_a_pass=%.1f ns_a_instr=%.2f pc=%08x count=%u words=%zu tick_us=%.2f commit_us=%.2f tick_ns=%.2f\n", cw, ch, budget, med * 1e6, wall * 1e6, per, per > 0 ? med * 1e9 / per : 0.0, last[0], last[1], nw, tickSum / max(1, sampled) * 1e6, commitSum / max(1, sampled) * 1e6, per > 0 ? tickSum / max(1, sampled) * 1e9 / per : 0.0);
    if (dump) { for (int k = 0; k < dump && k < 8; k++) { unsigned w[4]; readback(cur, k, 0, 0, w); printf("texel %d: %08x %08x %08x %08x\n", k, w[0], w[1], w[2], w[3]); } }
    return 0;
}
