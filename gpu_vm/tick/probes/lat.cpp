// lat: ns a step of a dependent chain of each kind (lat.hlsl), one pixel, fxc2 like the tick
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <windows.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
static ID3D11Device* dev; static ID3D11DeviceContext* ctx;
static void ck(HRESULT h, const char* w) { if (FAILED(h)) { printf("%s %08lx\n", w, (unsigned long)h); exit(1); } }
int main(int argc, char** argv) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ck(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "dev");
    ID3DBlob *vb, *pb, *err = nullptr;
    const wchar_t* path = L"build\\x19\\lat.hlsl";
    if (FAILED(D3DCompileFromFile(path, nullptr, nullptr, "vert", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vb, &err))) { printf("%s", (char*)err->GetBufferPointer()); return 1; }
    if (FAILED(D3DCompileFromFile(path, nullptr, nullptr, "frag", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err))) { printf("%s", (char*)err->GetBufferPointer()); return 1; }
    ID3D11VertexShader* vs; ID3D11PixelShader* ps;
    ck(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs), "vs");
    ck(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps), "ps");
    // the chase texture: texel x of row r holds (x * 2654435761 + r) mod 8192 + (r + 1) % 8 << 13
    std::vector<uint32_t> img(8192 * 8 * 4);
    for (uint32_t r = 0; r < 8; r++) for (uint32_t x = 0; x < 8192; x++) img[(r * 8192 + x) * 4] = ((x * 2654435761u + 977) & 8191) | ((r + 1) & 7) << 13;
    D3D11_TEXTURE2D_DESC d = {8192, 8, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {img.data(), 8192 * 16, 0};
    ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv;
    ck(dev->CreateTexture2D(&d, &sd, &tex), "tex"); ck(dev->CreateShaderResourceView(tex, nullptr, &srv), "srv");
    D3D11_TEXTURE2D_DESC od = {1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D* out; ID3D11RenderTargetView* rtv;
    ck(dev->CreateTexture2D(&od, nullptr, &out), "out"); ck(dev->CreateRenderTargetView(out, nullptr, &rtv), "rtv");
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer* cb; ck(dev->CreateBuffer(&bd, nullptr, &cb), "cb");
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qdis, *q0, *q1; dev->CreateQuery(&qd, &qdis); qd.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&qd, &q0); dev->CreateQuery(&qd, &q1);
    D3D11_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0); ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetConstantBuffers(0, 1, &cb); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const char* names[] = {"imad", "xor-shift-add (3 ops)", "local load chase", "local store then load", "texel chase (one row)", "movc on a bit + add", "local uint2[515] chase", "texel chase (8 rows)", "iadd", "16 chains of xor+add (32 ops)", "8 chains of and+movc+add (32 ops)"};
    for (uint32_t kind = 0; kind < 11; kind++) {
        double t[2];
        for (int pass = 0; pass < 2; pass++) {
            uint32_t n = pass ? 2000000 : 200000;
            uint32_t c[4] = {n, kind, 12345, 0};
            ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
            ctx->Draw(3, 0);   // (warm)
            auto sync = [&]() {   // wait for the GPU: a copy of the output read back
                D3D11_TEXTURE2D_DESC sdsc = od; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D* st; ck(dev->CreateTexture2D(&sdsc, nullptr, &st), "staging");
                ctx->CopyResource(st, out);
                D3D11_MAPPED_SUBRESOURCE m; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
                ctx->Unmap(st, 0); st->Release();
            };
            sync();
            LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            for (int r = 0; r < 3; r++) ctx->Draw(3, 0);
            sync();
            QueryPerformanceCounter(&b);
            t[pass] = double(b.QuadPart - a.QuadPart) / f.QuadPart / 3;
        }
        D3D11_TEXTURE2D_DESC sdsc = od; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st; ck(dev->CreateTexture2D(&sdsc, nullptr, &st), "staging");
        ctx->CopyResource(st, out);
        D3D11_MAPPED_SUBRESOURCE m; ck(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "map");
        uint32_t got = *(uint32_t*)m.pData; ctx->Unmap(st, 0); st->Release();
        printf("%-26s %7.2f ns a step (%.4f, %.4f ms; %08x)\n", names[kind], (t[1] - t[0]) / 1800000 * 1e9, t[0] * 1e3, t[1] * 1e3, got);
    }
    return 0;
}
