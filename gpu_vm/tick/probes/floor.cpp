// floor: what the pieces of an interpreter step cost on the card, one warp a core (floor.hlsl), against the tick's
// fast loop. Each probe is floor.hlsl compiled with its defines (fxc2, as the tick); it runs N bundles of a synthetic
// program on every core, and ns a bundle is the difference of two N over their difference (GPU timestamps, the best of
// several rounds). A check: the output's sum at a short N, which WARP and the card must give alike.
//
//   floor --probes FILE [--warp] [--cores 255] [--n1 2000] [--n2 12000] [--rounds 5] [--code 128] [--fpct 10]
//         [--only NAME[,NAME]]
// FILE: a probe a line, "name DEFINE=V DEFINE=V ..." (# comments). Build: gpu_vm\tick\probes\build_floor.bat.
// --cores 4608 (128 an SM) fills every SM's issue: then ns a bundle times the clock times 144 (36 SMs x 4 schedulers)
// over the cores is the issue slots a bundle takes ("slots", printed when --slots is given).
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
static uint32_t rng = 12345;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

struct Probe { std::string name; std::vector<std::string> defs; };

static std::string disasm(ID3DBlob* b) {
    HMODULE sys = LoadLibraryA("C:\\Windows\\System32\\d3dcompiler_47.dll");
    auto dis = sys ? (decltype(&D3DDisassemble))GetProcAddress(sys, "D3DDisassemble") : nullptr;
    ID3DBlob* text = nullptr;
    if (!dis || FAILED(dis(b->GetBufferPointer(), b->GetBufferSize(), 0, nullptr, &text))) return "";
    std::string t((const char*)text->GetBufferPointer(), text->GetBufferSize());
    text->Release();
    return t;
}
// temporary registers (D3D11's limit is 4,096) and the instructions of the biggest loop (the timed one)
static void count(const std::string& t, size_t& regs, int& loopn) {
    regs = 0; size_t at = 0;
    while ((at = t.find("dcl_temps ", at)) != std::string::npos) regs += strtoul(t.c_str() + (at += 10), nullptr, 10);
    at = 0;
    while ((at = t.find("dcl_indexableTemp x", at)) != std::string::npos) { at = t.find('[', at) + 1; regs += strtoul(t.c_str() + at, nullptr, 10); }
    std::vector<std::string> lines; size_t s = 0;
    while (s < t.size()) { size_t e = t.find('\n', s); if (e == std::string::npos) e = t.size(); lines.push_back(t.substr(s, e - s)); s = e + 1; }
    std::vector<int> stack; std::vector<int> body;   // instructions inside each open loop
    loopn = 0;
    for (auto& l : lines) {
        size_t p = l.find_first_not_of(" \t0123456789:");
        if (p == std::string::npos || l[p] == '/' || l.compare(p, 4, "dcl_") == 0 || l.compare(p, 2, "ps") == 0 || l.compare(p, 2, "vs") == 0) continue;
        if (l.compare(p, 4, "loop") == 0) { stack.push_back((int)body.size()); body.push_back(0); continue; }
        if (l.compare(p, 7, "endloop") == 0) { if (!stack.empty()) { loopn = std::max(loopn, body[stack.back()]); stack.pop_back(); } continue; }
        for (int i : stack) body[i]++;
    }
}

int main(int argc, char** argv) {
    bool warp = false, slots = false;
    const char* pfile = nullptr; std::string only;
    uint32_t hot = 0, nfast = 8, cores = 255, n1 = 2000, n2 = 12000, rounds = 5, codeN = 128, fpct = 10, nchk = 300;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--slots") slots = true;
        else if (a == "--probes" && i + 1 < argc) pfile = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--cores" && i + 1 < argc) cores = atoi(argv[++i]);
        else if (a == "--n1" && i + 1 < argc) n1 = atoi(argv[++i]);
        else if (a == "--n2" && i + 1 < argc) n2 = atoi(argv[++i]);
        else if (a == "--rounds" && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (a == "--code" && i + 1 < argc) codeN = atoi(argv[++i]);
        else if (a == "--fpct" && i + 1 < argc) fpct = atoi(argv[++i]);
        else if (a == "--hot" && i + 1 < argc) hot = atoi(argv[++i]);     // a register field is one of r1..r(nf-1) this percent of the time
        else if (a == "--nf" && i + 1 < argc) nfast = atoi(argv[++i]);
        else { printf("floor --probes FILE [--warp] [--slots] [--only A,B] [--cores N] [--n1 N] [--n2 N] [--rounds N] [--code N] [--fpct P]\n"); return 1; }
    }
    if (!pfile) { printf("--probes FILE\n"); return 1; }
    if (cores > 8192) cores = 8192;
    std::vector<Probe> probes;
    {
        FILE* f = fopen(pfile, "r"); if (!f) { printf("no %s\n", pfile); return 1; }
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string l(line); size_t h = l.find('#'); if (h != std::string::npos) l = l.substr(0, h);
            std::vector<std::string> w; size_t s = 0;
            while (true) { s = l.find_first_not_of(" \t\r\n", s); if (s == std::string::npos) break; size_t e = l.find_first_of(" \t\r\n", s); if (e == std::string::npos) e = l.size(); w.push_back(l.substr(s, e - s)); s = e; }
            if (w.empty()) continue;
            if (!only.empty() && ("," + only + ",").find("," + w[0] + ",") == std::string::npos) continue;
            probes.push_back({w[0], std::vector<std::string>(w.begin() + 1, w.end())});
        }
        fclose(f);
    }
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ck(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "device");
    if (codeN & (codeN - 1)) { printf("--code: a power of two\n"); return 1; }
    // the program: random fields (floor.hlsl's encoding); a branch taken 15% of the time (BR 0's bit)
    std::vector<uint32_t> img(8192 * 4 * 4, 0);
    auto reg = [&](uint32_t r) { return rnd() % 100 < hot ? 1 + rnd() % (nfast - 1) : r; };
    auto op = [&](bool fl) {
        uint32_t ra = reg(rnd() % 64), rb = reg(rnd() % 64), rd = reg(1 + rnd() % 63), f = rnd() & 0x3f;
        if (rnd() % 32 == 0) rd = 64;   // r0's sink
        if (rnd() % 2) f |= 0x80;       // the I form
        if (fl) f |= 0x40;
        return ra | rb << 8 | rd << 16 | f << 24;
    };
    for (uint32_t b = 0; b < 8192 * 4; b++) {
        uint32_t* q = &img[b * 4];
        q[0] = op(rnd() % 100 < fpct); q[1] = op(false);
        q[2] = rnd() & 0x7ffc;
        int off = (int)(rnd() % 41) - 20; if (off == 0) off = 3;
        q[3] = (rnd() % 100 < 15 ? 1u : 0u) | (rnd() & 15) << 1 | reg(rnd() % 64) << 8 | (rnd() % 64) << 16 | (uint32_t)off << 24;
    }
    D3D11_TEXTURE2D_DESC d = {8192, 4, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {img.data(), 8192 * 16, 0};
    ID3D11Texture2D *ctex, *dtex; ID3D11ShaderResourceView *csrv, *dsrv;
    ck(dev->CreateTexture2D(&d, &sd, &ctex), "code"); ck(dev->CreateShaderResourceView(ctex, nullptr, &csrv), "code srv");
    std::vector<uint32_t> dat(8192 * 8 * 4);
    for (auto& x : dat) x = rnd();
    d.Height = 8; sd.pSysMem = dat.data();
    ck(dev->CreateTexture2D(&d, &sd, &dtex), "data"); ck(dev->CreateShaderResourceView(dtex, nullptr, &dsrv), "data srv");
    uint32_t H = (cores + 127) / 128 * 4;
    D3D11_TEXTURE2D_DESC od = {1024, H, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* out; ID3D11RenderTargetView* rtv;
    ck(dev->CreateTexture2D(&od, nullptr, &out), "out"); ck(dev->CreateRenderTargetView(out, nullptr, &rtv), "rtv");
    D3D11_TEXTURE2D_DESC sdsc = od; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st; ck(dev->CreateTexture2D(&sdsc, nullptr, &st), "staging");
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer* cb; ck(dev->CreateBuffer(&bd, nullptr, &cb), "cb");
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qdis, *q0, *q1; dev->CreateQuery(&qd, &qdis); qd.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
    D3D11_VIEWPORT vp = {0, 0, 1024, (float)H, 0, 1};
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ID3D11ShaderResourceView* srvs[2] = {csrv, dsrv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetConstantBuffers(0, 1, &cb); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto setn = [&](uint32_t n) { uint32_t c[4] = {n, cores, 777, codeN - 1}; ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0); };
    auto sum = [&]() {
        ctx->CopyResource(st, out);
        D3D11_MAPPED_SUBRESOURCE m; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
        uint64_t h = 1469598103934665603ull;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < 1024; x++) { uint32_t v = ((uint32_t*)((char*)m.pData + y * m.RowPitch))[x * 4]; h = (h ^ v) * 1099511628211ull; }
        ctx->Unmap(st, 0);
        return h;
    };
    auto timed = [&](uint32_t n) {   // seconds of one draw by the GPU's clock
        setn(n);
        ctx->Begin(qdis); ctx->End(q0); ctx->Draw(3, 0); ctx->End(q1); ctx->End(qdis);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
        while (ctx->GetData(qdis, &dj, sizeof dj, 0) != S_OK) {}
        while (ctx->GetData(q0, &a, sizeof a, 0) != S_OK) {}
        while (ctx->GetData(q1, &b, sizeof b, 0) != S_OK) {}
        return dj.Disjoint ? 1e9 : double(b - a) / dj.Frequency;
    };
    auto clock = [&]() {
        if (warp) return 0;
        FILE* p = _popen("nvidia-smi --query-gpu=clocks.gr --format=csv,noheader,nounits", "r");
        int mhz = 0; if (p) { if (fscanf(p, "%d", &mhz) != 1) mhz = 0; _pclose(p); }
        return mhz;
    };
    printf("floor: %s, %u cores, code %u bundles, N %u and %u, %u rounds\n", warp ? "WARP" : "the card", cores, codeN, n1, n2, rounds);
    printf("%-14s %8s %6s %6s %18s %6s%s  defines\n", "probe", "ns", "dxbc", "temps", "sum", "MHz", slots ? "  slots" : "");
    bool warmed = warp;
    for (auto& pr : probes) {
        std::vector<D3D_SHADER_MACRO> m;
        std::vector<std::string> nm, vl;
        for (auto& dd : pr.defs) { size_t e = dd.find('='); nm.push_back(dd.substr(0, e)); vl.push_back(e == std::string::npos ? "1" : dd.substr(e + 1)); }
        for (size_t i = 0; i < nm.size(); i++) m.push_back({nm[i].c_str(), vl[i].c_str()});
        m.push_back({nullptr, nullptr});
        ID3DBlob *vb, *pb, *err = nullptr;
        const wchar_t* path = L"gpu_vm\\tick\\probes\\floor.hlsl";
        if (FAILED(D3DCompileFromFile(path, m.data(), nullptr, "vert", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err))) { printf("%s", (char*)err->GetBufferPointer()); return 1; }
        if (FAILED(D3DCompileFromFile(path, m.data(), nullptr, "frag", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err))) { printf("%s: %s", pr.name.c_str(), (char*)err->GetBufferPointer()); return 1; }
        std::string text = disasm(pb);
        size_t regs; int loopn; count(text, regs, loopn);
        if (getenv("FLOOR_DISASM")) { std::string fn = std::string(getenv("FLOOR_DISASM")) + "_" + pr.name + ".asm"; if (FILE* f = fopen(fn.c_str(), "wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); } }
        if (regs > 4096) { printf("%-14s more than 4,096 temporary registers (%zu): not run\n", pr.name.c_str(), regs); continue; }
        ID3D11VertexShader* vs; ID3D11PixelShader* ps;
        ck(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "vs");
        ck(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps), "ps");
        ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
        setn(nchk); ctx->Draw(3, 0);
        uint64_t h = sum();
        double ns = 0; int mhz = 0;
        if (!warp) {
            {   // the card's clock up: 4 s before the first probe, 1.5 s before each (a compile's pause lets it fall)
                LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
                do { timed(n2); QueryPerformanceCounter(&b); } while (double(b.QuadPart - a.QuadPart) / f.QuadPart < (warmed ? 1.5 : 4.0));
                warmed = true;
            }
            double b1 = 1e9, b2 = 1e9;
            for (uint32_t r = 0; r < rounds; r++) { b1 = std::min(b1, timed(n1)); b2 = std::min(b2, timed(n2)); }
            if (b2 > 0.25) { printf("%s: a draw took %.3f s: stopped (a draw must stay well under a second)\n", pr.name.c_str(), b2); return 1; }
            ns = (b2 - b1) / (n2 - n1) * 1e9;
            for (size_t i = 0; i < nm.size(); i++) if (nm[i] == "BLOCK") ns /= atoi(vl[i].c_str());
            for (size_t i = 0; i < nm.size(); i++) if (nm[i] == "VBLOCK") {   // (a turn is a block of 1-8: the bundles core 0 ran, from its pixel)
                auto bundles = [&](uint32_t n) { setn(n); ctx->Draw(3, 0); ctx->CopyResource(st, out); D3D11_MAPPED_SUBRESOURCE mm; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm), "map"); uint32_t z = ((uint32_t*)mm.pData)[2]; ctx->Unmap(st, 0); return z; };
                uint32_t B1 = bundles(n1), B2 = bundles(n2);
                ns = (b2 - b1) / double(B2 - B1) * 1e9;
                printf("(%s: %.2f bundles a block) ", pr.name.c_str(), double(B2 - B1) / (n2 - n1));
            }   // (a turn is a block: ns a bundle)
            mhz = clock();
        }
        std::string ds; for (auto& dd : pr.defs) ds += dd + " ";
        if (slots) printf("%-14s %8.2f %6d %6zu %018llx %6d %6.1f  %s\n", pr.name.c_str(), ns, loopn, regs, (unsigned long long)h, mhz, ns * mhz * 1e-3 * 144.0 / cores, ds.c_str());
        else printf("%-14s %8.2f %6d %6zu %018llx %6d  %s\n", pr.name.c_str(), ns, loopn, regs, (unsigned long long)h, mhz, ds.c_str());
        fflush(stdout);
        vs->Release(); ps->Release(); vb->Release(); pb->Release();
    }
    return 0;
}
