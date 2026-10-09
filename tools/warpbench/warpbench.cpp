// warpbench: what long, different work in the pixels of one pass costs (docs/multicore.md).
//
// One pixel shader with two unrelated loops: an interpreter of sorts on integers (a hashed
// "program", sixteen registers, a local array, a branch every step) and an iteration on floats.
// Rectangles of pixels run one or the other, with a seed: pixels with the same kind and seed do
// exactly the same thing, a different seed is the same code on other data (another program to
// the interpreter), the other kind is other code. The sweep draws arrangements of rectangles
// into a 4096 x 4096 target and prints the GPU's time for each.
//
//   warpbench [--arr N] [--ms T] [--only PREFIX]
//     --arr N    words in the interpreter's local array (64; the emulator's write cache is 2048)
//     --live N   N more values of four words that the interpreter keeps in registers (0; a power of two)
//     --ms T     the work is sized so that one 64 x 64 rectangle takes about T ms (3)
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static ID3D11Query *qDisjoint, *qT0, *qT1;
static ID3D11Buffer *vb, *cb;
static const UINT kTarget = 4096, kMaxVerts = 6 * 8192;

struct Vertex { float x, y; UINT kind, seed; };
struct Rect { int x, y, w, h; UINT kind, seed; };   // kind 1: integers, 2: floats; | 0x100: a seed a pixel

static const char* kShader = R"(
cbuffer C : register(b0) { uint g_iters; uint g_fiters; uint g_pad0; uint g_pad1; };
struct V { float4 pos : SV_Position; nointerpolation uint2 p : P; };
V vs(float2 pos : POS, uint2 p : P) {
    V o;
    o.pos = float4(pos.x / 2048.0 - 1.0, 1.0 - pos.y / 2048.0, 0, 1);
    o.p = p;
    return o;
}
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
uint4 ps(V v) : SV_Target {
    uint kind = v.p.x & 0xff, seed = v.p.y;
    if (v.p.x & 0x100) seed ^= hash((uint)v.pos.x * 4099u + (uint)v.pos.y);
    uint acc = 0;
    if (kind == 1) {
        uint reg[16];
        uint mem[ARR];
        uint i;
        for (i = 0; i < 16; i++) reg[i] = hash(seed + i);
        for (i = 0; i < ARR; i += 16) mem[i] = seed + i;
        uint pc = seed;
        LIVE_DECL
        [loop]
        for (i = 0; i < g_iters; i++) {
            uint op = hash(pc);
            uint a = reg[op & 15], b = reg[(op >> 4) & 15], r;
            [branch]
            switch ((op >> 8) & 7) {
                case 0: r = a + b; break;
                case 1: r = a - b; break;
                case 2: r = a ^ (b << 3); break;
                case 3: r = (a >> (b & 31)) | 1; break;
                case 4: r = a * (b | 1); break;
                case 5: r = mem[(a ^ b) & (ARR - 1)]; break;
                case 6: mem[a & (ARR - 1)] = b; r = b; break;
                default:
                    r = a;
                    [branch]
                    if (a < b) pc += (op >> 12) & 63;
                    break;
            }
            reg[(op >> 16) & 15] = r;
            LIVE_STEP
            pc++;
        }
        for (i = 0; i < 16; i++) acc += reg[i];
        LIVE_SUM
    } else if (kind == 2) {
        float2 c = float2(frac(seed * 0.6180339) * 3.0 - 2.0, frac(seed * 0.3819660) * 2.0 - 1.0);
        float2 z = 0;
        [loop]
        for (uint i = 0; i < g_fiters; i++) {
            z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
            [branch]
            if (dot(z, z) > 4.0) {
                z = sin(z * 1.7) * 0.5;
                acc++;
            }
        }
        acc += asuint(z.x);
    }
    return uint4(acc, kind, seed, 1);
}
)";

static double gpuMs(ID3D11Query* a, ID3D11Query* b) {
    UINT64 t0 = 0, t1 = 0;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    while (ctx->GetData(qDisjoint, &dj, sizeof dj, 0) != S_OK) Sleep(0);
    while (ctx->GetData(a, &t0, sizeof t0, 0) != S_OK) Sleep(0);
    while (ctx->GetData(b, &t1, sizeof t1, 0) != S_OK) Sleep(0);
    if (dj.Disjoint) return -1;
    return (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
}

// The GPU's time for the rectangles, in one draw or a draw each: the median of `runs`.
static double measure(const std::vector<Rect>& rects, bool separate = false, int runs = 7) {
    std::vector<Vertex> verts;
    for (auto& r : rects) {
        float x0 = (float)r.x, y0 = (float)r.y, x1 = (float)(r.x + r.w), y1 = (float)(r.y + r.h);
        Vertex q[6] = {{x0, y0, r.kind, r.seed}, {x1, y0, r.kind, r.seed}, {x0, y1, r.kind, r.seed},
                       {x1, y0, r.kind, r.seed}, {x1, y1, r.kind, r.seed}, {x0, y1, r.kind, r.seed}};
        verts.insert(verts.end(), q, q + 6);
    }
    if (verts.size() > kMaxVerts) return -1;
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, verts.data(), verts.size() * sizeof(Vertex));
    ctx->Unmap(vb, 0);
    std::vector<double> times;
    for (int run = -2; run < runs; run++) {
        ctx->Begin(qDisjoint);
        ctx->End(qT0);
        if (separate) for (UINT i = 0; i < verts.size(); i += 6) ctx->Draw(6, i);
        else ctx->Draw((UINT)verts.size(), 0);
        ctx->End(qT1);
        ctx->End(qDisjoint);
        ctx->Flush();
        double t = gpuMs(qT0, qT1);
        if (run >= 0 && t >= 0) times.push_back(t);
    }
    if (times.empty()) return -1;
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

static void setIters(UINT ints, UINT floats) {
    UINT v[4] = {ints, floats, 0, 0};
    ctx->UpdateSubresource(cb, 0, nullptr, v, 0, 0);
}

static std::string only;
static void row(const char* group, const std::string& what, double ms, double reference) {
    if (!only.empty() && strncmp(group, only.c_str(), only.size()) != 0) return;
    printf("%-10s %-58s %8.3f ms  %5.2fx\n", group, what.c_str(), ms, reference > 0 ? ms / reference : 0);
    fflush(stdout);
}
static bool want(const char* group) { return only.empty() || strncmp(group, only.c_str(), only.size()) == 0; }

int main(int argc, char** argv) {
    int arr = 64, live = 0;
    double targetMs = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--arr") && i + 1 < argc) arr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--live") && i + 1 < argc) live = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) targetMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    }
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, &got, &ctx))) {
        printf("no D3D11 device\n");
        return 1;
    }
    std::string arrText = std::to_string(arr);
    // --live N: N more values (four words each) that every step of the interpreter may change,
    // which the GPU has to keep in registers for the whole loop
    std::string liveDecl = " ", liveStep = " ", liveSum = " ";
    for (int j = 0; j < live; j++) {
        liveDecl += "uint4 v" + std::to_string(j) + " = seed + " + std::to_string(j) + ";";
        liveStep += "v" + std::to_string(j) + " += (((op >> 20) & " + std::to_string(live - 1) + ") == " + std::to_string(j) + ") ? uint4(r, a, b, op) : (uint4)0;";
        liveSum += "acc += v" + std::to_string(j) + ".x ^ v" + std::to_string(j) + ".y ^ v" + std::to_string(j) + ".z ^ v" + std::to_string(j) + ".w;";
    }
    D3D_SHADER_MACRO macros[] = {{"ARR", arrText.c_str()}, {"LIVE_DECL", liveDecl.c_str()}, {"LIVE_STEP", liveStep.c_str()},
                                 {"LIVE_SUM", liveSum.c_str()}, {nullptr, nullptr}};
    ID3DBlob *vsCode = nullptr, *psCode = nullptr, *errors = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "warpbench", macros, nullptr, "vs", "vs_5_0", 0, 0, &vsCode, &errors)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "warpbench", macros, nullptr, "ps", "ps_5_0", 0, 0, &psCode, &errors))) {
        printf("compile: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
        return 1;
    }
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {{"POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                     {"P", 0, DXGI_FORMAT_R32G32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ID3D11InputLayout* layout;
    dev->CreateInputLayout(il, 2, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout);

    D3D11_TEXTURE2D_DESC td = {kTarget, kTarget, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* tex;
    ID3D11RenderTargetView* rtv;
    dev->CreateTexture2D(&td, nullptr, &tex);
    dev->CreateRenderTargetView(tex, nullptr, &rtv);
    D3D11_BUFFER_DESC bd = {kMaxVerts * sizeof(Vertex), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    dev->CreateBuffer(&bd, nullptr, &vb);
    D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&cd, nullptr, &cb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs;
    dev->CreateRasterizerState(&rd, &rs);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&qd, &qDisjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &qT0);
    dev->CreateQuery(&qd, &qT1);

    D3D11_VIEWPORT vp = {0, 0, (float)kTarget, (float)kTarget, 0, 1};
    UINT stride = sizeof(Vertex), offset = 0;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);

    // Size each loop so that a 64 x 64 rectangle of it takes about the target time.
    const UINT A = 1, B = 2, PIXEL = 0x100;
    UINT ints = 1000, floats = 1000;
    for (int pass = 0; pass < 12; pass++) {
        setIters(ints, floats);
        double a = measure({{0, 0, 64, 64, A, 1}}, false, 3), b = measure({{0, 0, 64, 64, B, 1}}, false, 3);
        if (a > 0) ints = (UINT)std::min(4e6, std::max(100.0, ints * std::min(8.0, targetMs / std::max(a, 0.01))));
        if (b > 0) floats = (UINT)std::min(4e6, std::max(100.0, floats * std::min(8.0, targetMs / std::max(b, 0.01))));
    }
    setIters(ints, floats);
    double tA = measure({{0, 0, 64, 64, A, 1}}), tB = measure({{0, 0, 64, 64, B, 1}});
    printf("warpbench: %d live values, array of %d words; %u interpreter steps and %u float steps a pixel\n", live, arr, ints, floats);
    printf("one 64 x 64 rectangle: integers %.3f ms, floats %.3f ms. The last column is the time against that of one\n", tA, tB);
    printf("such rectangle of the first kind in the line (1.00x: the rest came free; N.00x: N of them, one after another)\n\n");
    char text[200];

    // ---- how many pixels do the same thing ----
    if (want("size")) for (int s : {4, 8, 16, 32, 64, 128, 256, 512, 1024}) {
        snprintf(text, sizeof text, "%d x %d, integers, one seed (%d pixels)", s, s, s * s);
        row("size", text, measure({{0, 0, s, s, A, 1}}), tA);
    }
    if (want("size")) for (int s : {8, 64, 256, 1024}) {
        snprintf(text, sizeof text, "%d x %d, floats, one seed", s, s);
        row("size", text, measure({{0, 0, s, s, B, 1}}), tB);
    }
    if (want("size")) for (int s : {8, 16, 32, 64, 128}) {
        snprintf(text, sizeof text, "%d x %d, integers, a seed a pixel", s, s);
        row("size", text, measure({{0, 0, s, s, A | PIXEL, 1}}), tA);
    }

    // ---- two rectangles: what the second does, how far away, which way ----
    struct Second { const char* name; UINT kind, seed; double own; };
    Second seconds[] = {{"the same (integers, same seed)", A, 1, tA}, {"integers, another seed", A, 2, tA}, {"floats", B, 1, tB}};
    if (want("pair")) for (int s : {8, 32, 64}) {
        double one = measure({{0, 0, s, s, A, 1}});
        for (auto& sec : seconds) {
            double oneB = measure({{0, 0, s, s, sec.kind, sec.seed}});
            for (int gap : {0, 8, 64, 512, 2048}) {
                for (int dir = 0; dir < 3; dir++) {
                    if (gap != 64 && dir != 0) continue;
                    int dx = dir != 1 ? s + gap : 0, dy = dir != 0 ? s + gap : 0;
                    snprintf(text, sizeof text, "%d: + %s, %d %s", s, sec.name, gap, dir == 0 ? "right" : dir == 1 ? "down" : "diagonal");
                    row("pair", text, measure({{0, 0, s, s, A, 1}, {dx, dy, s, s, sec.kind, sec.seed}}), one);
                }
            }
            snprintf(text, sizeof text, "%d: + %s, 64 right, a draw each", s, sec.name);
            row("pair", text, measure({{0, 0, s, s, A, 1}, {s + 64, 0, s, s, sec.kind, sec.seed}}, true), one);
            snprintf(text, sizeof text, "%d:   (%s alone)", s, sec.name);
            row("pair", text, oneB, one);
        }
    }

    // ---- N rectangles of the interpreter: one seed, a seed each, alternating kinds ----
    if (want("count")) for (int s : {8, 32, 64}) {
        double one = measure({{0, 0, s, s, A, 1}});
        for (int n : {1, 2, 4, 8, 16, 32, 64, 256, 1024}) {
            int pitch = s * 2, per_row = 4096 / pitch;
            if (n > per_row * per_row) continue;
            for (int variant = 0; variant < 3; variant++) {
                std::vector<Rect> rects;
                for (int k = 0; k < n; k++)
                    rects.push_back({(k % per_row) * pitch, (k / per_row) * pitch, s, s, variant == 2 && (k & 1) ? B : A,
                                     variant == 0 ? 1u : (UINT)(k + 1)});
                snprintf(text, sizeof text, "%d rectangles of %d x %d, %s", n, s, s,
                         variant == 0 ? "one seed" : variant == 1 ? "a seed each" : "integers and floats in turn, a seed each");
                row("count", text, measure(rects), one);
            }
        }
    }

    // ---- the same number of pixels (4096) and of seeds (4), arranged differently ----
    if (want("shape")) {
        auto four = [&](int w, int h, int dx, int dy, const char* name) {
            std::vector<Rect> same, apart;
            for (int k = 0; k < 4; k++) {
                same.push_back({k * dx, k * dy, w, h, A, 1});
                apart.push_back({k * dx, k * dy, w, h, A, (UINT)(k + 1)});
            }
            double one = measure({{0, 0, w, h, A, 1}});
            snprintf(text, sizeof text, "4 of %d x %d %s: one seed", w, h, name);
            row("shape", text, measure(same), one);
            snprintf(text, sizeof text, "4 of %d x %d %s: a seed each", w, h, name);
            row("shape", text, measure(apart), one);
        };
        four(32, 32, 32, 0, "side by side");
        four(32, 32, 256, 0, "256 apart");
        four(32, 32, 0, 256, "256 apart, down");
        four(32, 32, 1024, 1024, "on the diagonal, 1024 apart");
        four(128, 8, 0, 8, "rows stacked");
        four(128, 8, 0, 64, "rows 64 apart");
        four(8, 128, 8, 0, "columns side by side");
        four(8, 128, 64, 0, "columns 64 apart");
        four(1024, 1, 0, 1, "single lines stacked");
        four(1024, 1, 0, 16, "single lines 16 apart");
    }
    return 0;
}
