// rvc_trace12: runs rvc's shader on Direct3D 12 from a snapshot, presenting once per frame.
//
// It exists for profilers that only attach to D3D12 (Nsight GPU Trace). The shader bytecode
// is the same FXC output rvc_harness uses (shared cache), and the frame is the same two
// draws: CPUTick on the 64x64 state area, then Commit on the whole 2048x4096 texture.
// There is no console I/O here; use rvc_harness to interact with the guest. At exit it
// prints the same BENCH line as rvc_harness --bench, so the state hash can be compared.

#include "common.h"
#include "image.h"
#include "material.h"
#include "rvc_time.h"
#include "shaderlab.h"

#include <d3d12.h>
#include <d3d12shader.h>
#include <dxcapi.h>
#include <dxgi1_4.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef SHADERX86_UNITY_INCLUDE_DIR
#define SHADERX86_UNITY_INCLUDE_DIR "harness/unity_include"
#endif

namespace {

const UINT W = 2048, H = 4096;
const DXGI_FORMAT kStateFormat = DXGI_FORMAT_R32G32B32A32_UINT;
const UINT kSlots = 3;         // frames in flight
const UINT kTableSize = 16;    // SRV slots t0..t15 per pass
const UINT kCbBytes = 4096;    // per stage $Globals upload space

struct SnapshotHeader {  // must match rvc_harness
    char magic[8];
    uint32_t width, height;
    double time;
    uint32_t sentTag, sentChar;
};

struct Options {
    std::string rvcDir = "rvc/_Nix/rvc";
    std::string payloadDir;
    std::string snapshot = "build/snapshots/rvc_bench.snap";
    std::string cacheDir = "build/shadercache";
    int ticks = 2048;
    uint64_t frames = 2000;
    uint64_t warmup = 30;
    double fixedDt = 0.004;
    double seconds = 0;  // > 0: keep running until this much wall time has passed
    std::string dumpState;
    bool noDoubles = false;  // NVIDIA's D3D12 path miscomputes rvc's double math; see README
    std::vector<std::string> defines;
    bool dxc = false;        // compile with DXC to DXIL: seconds instead of minutes, for prototyping
    std::string dxcOpt = "-O3";  // DXC optimisation flags, space separated
    std::string dxcSm = "6_6";   // DXC shader model; 6.6 matches FXC speed, 6.0 is ~10% slower
    std::string dxcDir;      // folder with dxcompiler.dll; default: newest Windows SDK bin
    bool warp = false;   // software adapter, as a reference for driver differences
    bool debug = false;  // D3D12 debug layer; its messages are printed at exit
};

[[noreturn]] void die(const char* what, HRESULT hr = S_OK) {
    fprintf(stderr, "[trace12] %s%s%s\n", what, FAILED(hr) ? ": " : "", FAILED(hr) ? hrToString(hr).c_str() : "");
    exit(1);
}
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) die(what, hr);
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = t;
    return h;
}
D3D12_RESOURCE_DESC bufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
D3D12_RESOURCE_DESC texDesc(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

struct Dx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;

    UINT64 signal() {
        check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
        return fenceValue;
    }
    void waitFor(UINT64 v) {
        if (fence->GetCompletedValue() >= v) return;
        check(fence->SetEventOnCompletion(v, fenceEvent), "SetEventOnCompletion");
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    void flush() { waitFor(signal()); }
};

// Runs one-off commands (uploads, readbacks) and blocks until the GPU has finished them.
struct OneShot {
    Dx& dx;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit OneShot(Dx& d) : dx(d) {
        check(dx.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
        check(dx.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    }
    void run() {
        check(list->Close(), "Close");
        ID3D12CommandList* l[] = {list.Get()};
        dx.queue->ExecuteCommandLists(1, l);
        dx.flush();
    }
};

// Creates a texture in COPY_DEST, fills it from tightly packed rows, leaves it in `after`.
ComPtr<ID3D12Resource> uploadTexture(Dx& dx, UINT w, UINT h, DXGI_FORMAT fmt, UINT bytesPerPixel, const uint8_t* rows,
                                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES after) {
    ComPtr<ID3D12Resource> tex, up;
    auto td = texDesc(w, h, fmt, flags);
    auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    check(dx.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&tex)), "texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dx.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto bd = bufferDesc(total);
    auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    check(dx.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&up)), "upload buffer");
    uint8_t* p = nullptr;
    check(up->Map(0, nullptr, (void**)&p), "Map upload");
    for (UINT y = 0; y < h; ++y)
        memcpy(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, rows + (size_t)y * w * bytesPerPixel, (size_t)w * bytesPerPixel);
    up->Unmap(0, nullptr);

    OneShot os(dx);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    os.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b = transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, after);
    os.list->ResourceBarrier(1, &b);
    os.run();
    return tex;
}

// Reads back the top-left w x h texels of a state texture that is in `state`.
std::vector<uint8_t> readRegion(Dx& dx, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, UINT w, UINT h) {
    auto td = texDesc(w, h, kStateFormat, D3D12_RESOURCE_FLAG_NONE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dx.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> rb;
    auto bd = bufferDesc(total);
    auto hr = heapProps(D3D12_HEAP_TYPE_READBACK);
    check(dx.dev->CreateCommittedResource(&hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&rb)), "readback buffer");
    OneShot os(dx);
    auto b0 = transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    os.list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{0, 0, 0, w, h, 1};
    os.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    auto b1 = transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    os.list->ResourceBarrier(1, &b1);
    os.run();
    std::vector<uint8_t> out((size_t)w * h * 16);
    uint8_t* p = nullptr;
    check(rb->Map(0, nullptr, (void**)&p), "Map readback");
    for (UINT y = 0; y < h; ++y) memcpy(out.data() + (size_t)y * w * 16, p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)w * 16);
    rb->Unmap(0, nullptr);
    return out;
}

// DXC (dxcompiler.dll from the Windows SDK), loaded on demand for --dxc.
struct Dxc {
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    void init(std::string dir) {
        if (dir.empty()) {  // newest x64 SDK bin folder that ships the compiler
            std::error_code ec;
            for (auto& e : fs::directory_iterator("C:/Program Files (x86)/Windows Kits/10/bin", ec)) {
                fs::path cand = e.path() / "x64";
                if (fs::exists(cand / "dxcompiler.dll", ec) && cand.u8string() > dir) dir = cand.u8string();
            }
        }
        std::wstring path = widen((fs::u8path(dir) / "dxcompiler.dll").u8string());
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);  // finds dxil.dll beside it
        auto create = h ? (DxcCreateInstanceProc)GetProcAddress(h, "DxcCreateInstance") : nullptr;
        if (!create) die(("cannot load " + narrow(path) + " (use --dxc-dir)").c_str());
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCompiler");
    }
};

// The DXIL counterpart of StageLayout::reflect.
void reflectDxil(StageLayout& L, ID3D12ShaderReflection* refl) {
    L.present = true;
    D3D12_SHADER_DESC sd{};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        refl->GetResourceBindingDesc(i, &bd);
        if (bd.Type == D3D_SIT_TEXTURE) L.textures.push_back({bd.Name, bd.BindPoint});
        else if (bd.Type == D3D_SIT_SAMPLER) L.samplers.push_back({bd.Name, bd.BindPoint});
        else if (bd.Type == D3D_SIT_UAV_RWSTRUCTURED) L.uavs.push_back({bd.Name, bd.BindPoint});
        else if (bd.Type == D3D_SIT_CBUFFER && std::string(bd.Name) == "$Globals") {
            ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("$Globals");
            D3D12_SHADER_BUFFER_DESC cbd{};
            cb->GetDesc(&cbd);
            L.hasGlobals = true;
            L.globalsSlot = bd.BindPoint;
            L.globalsSize = cbd.Size;
            for (UINT v = 0; v < cbd.Variables; ++v) {
                D3D12_SHADER_VARIABLE_DESC vd{};
                D3D12_SHADER_TYPE_DESC td{};
                ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                var->GetDesc(&vd);
                var->GetType()->GetDesc(&td);
                ShaderVar sv;
                sv.name = vd.Name;
                sv.offset = vd.StartOffset;
                sv.size = vd.Size;
                sv.cls = td.Class;
                sv.type = td.Type;
                sv.rows = td.Rows;
                sv.cols = td.Columns;
                sv.elements = td.Elements;
                L.vars.push_back(sv);
            }
            L.scratch.assign((cbd.Size + 15) & ~15u, 0);
        }
    }
}

// Compiles preprocessed HLSL with DXC in FXC-compatibility mode and reflects it.
std::vector<uint8_t> dxcCompile(Dxc& dxc, const std::string& text, const std::string& entry, const std::string& profile,
                                const std::string& optFlags, StageLayout& layout) {
    std::wstring wentry = widen(entry);
    // -Gec / -HV 2016: assignment to uniforms and other FXC-era syntax the shader relies on.
    // The two defines map DX9 sampler types that DXC no longer has (declared but unused).
    std::wstring wprofile = widen(profile);
    std::vector<std::wstring> extra;
    for (size_t i = 0; i < optFlags.size();) {
        size_t e = optFlags.find(' ', i);
        if (e == std::string::npos) e = optFlags.size();
        if (e > i) extra.push_back(widen(optFlags.substr(i, e - i)));
        i = e + 1;
    }
    std::vector<LPCWSTR> args = {L"-T", wprofile.c_str(), L"-E", wentry.c_str(), L"-Gec", L"-HV", L"2016",
                                 L"-D", L"samplerCUBE=TextureCube", L"-D", L"sampler3D=Texture3D"};
    for (auto& x : extra) args.push_back(x.c_str());
    DxcBuffer src{text.data(), text.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> res;
    check(dxc.compiler->Compile(&src, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&res)), "DXC Compile");
    HRESULT status = E_FAIL;
    res->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> errs;
        res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr);
        std::string log = errs && errs->GetStringLength() ? std::string(errs->GetStringPointer(), errs->GetStringLength()) : "";
        size_t firstError = log.find("error");
        die(("DXC failed for " + entry + ":\n" + log.substr(firstError == std::string::npos ? 0 : log.rfind('\n', firstError) + 1, 1500)).c_str());
    }
    ComPtr<IDxcBlob> obj, reflBlob;
    check(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr), "DXC object");
    check(res->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflBlob), nullptr), "DXC reflection");
    DxcBuffer rb{reflBlob->GetBufferPointer(), reflBlob->GetBufferSize(), 0};
    ComPtr<ID3D12ShaderReflection> refl;
    check(dxc.utils->CreateReflection(&rb, IID_PPV_ARGS(&refl)), "DXC CreateReflection");
    reflectDxil(layout, refl.Get());
    const uint8_t* p = (const uint8_t*)obj->GetBufferPointer();
    return std::vector<uint8_t>(p, p + obj->GetBufferSize());
}

struct Pass12 {
    std::string name;
    StageLayout vs, ps;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die(("missing value for " + a).c_str());
            return argv[++i];
        };
        if (a == "--rvc") opt.rvcDir = next();
        else if (a == "--payload") opt.payloadDir = next();
        else if (a == "--load-state") opt.snapshot = next();
        else if (a == "--cache") opt.cacheDir = next();
        else if (a == "--ticks") opt.ticks = atoi(next().c_str());
        else if (a == "--frames") opt.frames = strtoull(next().c_str(), nullptr, 10);
        else if (a == "--bench") opt.warmup = strtoull(next().c_str(), nullptr, 10);
        else if (a == "--fixed-dt") opt.fixedDt = atof(next().c_str());
        else if (a == "--seconds") opt.seconds = atof(next().c_str());
        else if (a == "--dump-state") opt.dumpState = next();
        else if (a == "--debug") opt.debug = true;
        else if (a == "--warp") opt.warp = true;
        else if (a == "--dxc") opt.dxc = true;
        else if (a == "--dxc-dir") opt.dxcDir = next();
        else if (a == "--dxc-opt") opt.dxcOpt = next();
        else if (a == "--dxc-sm") opt.dxcSm = next();
        else if (a == "--no-doubles") opt.noDoubles = true;
        else if (a == "--define") opt.defines.push_back(next());
        else {
            fprintf(stderr, "usage: rvc_trace12 [--rvc DIR] [--payload DIR] [--load-state FILE] [--ticks N]\n"
                            "                   [--frames N | --seconds S] [--bench WARMUP] [--fixed-dt S] [--cache DIR] [--dump-state FILE]\n"
                            "                   [--dxc [--dxc-dir DIR] [--dxc-opt \"FLAGS\"] [--dxc-sm 6_x]] [--no-doubles] [--define NAME[=V]] [--warp] [--debug]\n");
            return a == "--help" ? 0 : 1;
        }
    }
    if (opt.payloadDir.empty()) opt.payloadDir = (fs::u8path(opt.rvcDir) / "data-net").u8string();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // --- Device, queue, hidden window and swapchain (Present marks the frames for profilers) ---
    Dx dx;
    if (opt.debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        else fprintf(stderr, "[trace12] debug layer unavailable\n");
    }
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    if (opt.warp) check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx.dev)), "D3D12CreateDevice");
    D3D12_FEATURE_DATA_D3D12_OPTIONS fo{};
    dx.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &fo, sizeof fo);
    fprintf(stderr, "[trace12] %s, doubles: %s\n", opt.warp ? "WARP" : "hardware adapter", fo.DoublePrecisionFloatShaderOps ? "yes" : "NO");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(dx.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&dx.queue)), "queue");
    check(dx.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx.fence)), "fence");
    dx.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"rvc_trace12";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"rvc_trace12", WS_POPUP, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);  // never shown
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    check(factory->CreateSwapChainForHwnd(dx.queue.Get(), wnd, &sd, nullptr, nullptr, &swap), "swapchain");
    factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_ALT_ENTER);

    // --- Shaders: same sources, defines and flags as rvc_harness, so the bytecode cache is shared ---
    std::string shaderPath = (fs::u8path(opt.rvcDir) / "main.shader").u8string();
    SLShader shader;
    std::string err;
    if (!loadShaderLab(shaderPath, shader, err)) die(err.c_str());
    CompileSettings cs;
    cs.cacheDir = opt.cacheDir;
    cs.includeDirs = {SHADERX86_UNITY_INCLUDE_DIR};
    cs.defines = {{"SHADER_API_D3D11", "1"}, {"SHADER_TARGET", "50"}, {"UNITY_COMPILER_HLSL", "1"}, {"UNITY_VERSION", "202235"}};
    if (opt.noDoubles) cs.defines.push_back({"NO_DOUBLES", "1"});
    for (auto& d : opt.defines) {  // NAME or NAME=VALUE
        size_t eq = d.find('=');
        cs.defines.push_back({d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1)});
    }
    std::string rootDir = fs::u8path(shader.path).parent_path().u8string();

    Dxc dxc;
    if (opt.dxc) dxc.init(opt.dxcDir);
    Pass12 passes[2];
    const char* passNames[2] = {"CPUTick", "Commit"};
    for (int p = 0; p < 2; ++p) {
        const SLPass* sp = shader.findPass(passNames[p]);
        if (!sp) die("pass not found in shader");
        Pass12& P = passes[p];
        P.name = passNames[p];
        std::string sm = shaderModelSuffix(sp->target);
        std::vector<uint8_t> vsBytes, psBytes;
        if (opt.dxc) {
            auto tc = std::chrono::steady_clock::now();
            std::string vsText, psText;
            if (!preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_VERTEX", "1"}}, cs, vsText, err) ||
                !preprocessStage(sp->code, shader.path, rootDir, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs, psText, err))
                die(err.c_str());
            vsBytes = dxcCompile(dxc, vsText, sp->vertexEntry, "vs_" + opt.dxcSm, opt.dxcOpt, P.vs);
            psBytes = dxcCompile(dxc, psText, sp->fragmentEntry, "ps_" + opt.dxcSm, opt.dxcOpt, P.ps);
            fprintf(stderr, "[trace12] pass '%s': DXC in %.1fs (ps %zu bytes)\n", P.name.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count(), psBytes.size());
        } else {
        StageResult vs = compileStage(sp->code, shader.path, rootDir, sp->vertexEntry, "vs_" + sm, {{"SHADER_STAGE_VERTEX", "1"}}, cs);
        StageResult ps = compileStage(sp->code, shader.path, rootDir, sp->fragmentEntry, "ps_" + sm, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs);
        if (!vs.ok || !ps.ok) die((P.name + " failed to compile:\n" + vs.log + ps.log).c_str());
        fprintf(stderr, "[trace12] pass '%s': vs %s, ps %s in %.1fs (%zu bytes)\n", P.name.c_str(), vs.fromCache ? "cached" : "compiled",
                ps.fromCache ? "cached" : "compiled", ps.seconds, (size_t)ps.bytecode->GetBufferSize());
        if (!P.vs.reflect(nullptr, vs.bytecode.Get(), err) || !P.ps.reflect(nullptr, ps.bytecode.Get(), err)) die(err.c_str());
        vsBytes.assign((const uint8_t*)vs.bytecode->GetBufferPointer(), (const uint8_t*)vs.bytecode->GetBufferPointer() + vs.bytecode->GetBufferSize());
        psBytes.assign((const uint8_t*)ps.bytecode->GetBufferPointer(), (const uint8_t*)ps.bytecode->GetBufferPointer() + ps.bytecode->GetBufferSize());
        }
        if (!P.vs.textures.empty() || !P.vs.samplers.empty() || !P.ps.samplers.empty() || !P.ps.uavs.empty())
            die("shader uses vertex textures, samplers or UAVs, which this runner does not bind");
        if (P.vs.globalsSize > kCbBytes || P.ps.globalsSize > kCbBytes) die("$Globals larger than the upload slot");
        for (auto& t : P.ps.textures)
            if (t.slot >= kTableSize) die("texture slot beyond the SRV table");

        // Root: [0] VS $Globals, [1] PS $Globals, [2] PS texture table t0..t15.
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kTableSize;
        D3D12_ROOT_PARAMETER rp[3]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = P.vs.globalsSlot;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[1].Descriptor.ShaderRegister = P.ps.globalsSlot;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1;
        rp[2].DescriptorTable.pDescriptorRanges = &range;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> rsBlob, rsErr;
        check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr), "root signature");
        check(dx.dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&P.root)), "root signature");

        // Cull Off, ZTest Off, Blend One Zero, as in the ShaderLab pass.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = P.root.Get();
        pd.VS = {vsBytes.data(), vsBytes.size()};
        pd.PS = {psBytes.data(), psBytes.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xffffffff;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kStateFormat;
        pd.SampleDesc.Count = 1;
        auto t0 = std::chrono::steady_clock::now();
        check(dx.dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&P.pso)), "pipeline state");
        fprintf(stderr, "[trace12] pass '%s': pipeline state in %.1fs\n", P.name.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    // --- State textures from the snapshot ---
    std::string snap;
    SnapshotHeader hdr{};
    if (!readFileBinary(opt.snapshot, snap) || snap.size() != sizeof hdr + (size_t)W * H * 16) die("cannot read snapshot");
    memcpy(&hdr, snap.data(), sizeof hdr);
    if (memcmp(hdr.magic, "SX86SNAP", 8) != 0 || hdr.width != W || hdr.height != H) die("not a snapshot");
    ComPtr<ID3D12Resource> state[2];
    for (int i = 0; i < 2; ++i)
        state[i] = uploadTexture(dx, W, H, kStateFormat, 16, (const uint8_t*)snap.data() + sizeof hdr,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 i == 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET);
    snap.clear();
    snap.shrink_to_fit();

    // --- Payload textures by shader property name (the RAM image is only read at _Init) ---
    std::map<std::string, ComPtr<ID3D12Resource>> textures;
    const uint8_t black[4] = {0, 0, 0, 0};
    ComPtr<ID3D12Resource> blackTex = uploadTexture(dx, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4, black, D3D12_RESOURCE_FLAG_NONE,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const char* lanes[4] = {"r", "g", "b", "a"};
    const char* props[4] = {"_R", "_G", "_B", "_A"};
    for (auto& pr : {std::pair<const char*, const char*>{"rootfs", "_Data_MTD"}, {"dts", "_Data_DTB"}}) {
        for (int i = 0; i < 4; ++i) {
            ImageRGBA8 img;
            std::string path = (fs::u8path(opt.payloadDir) / (std::string(pr.first) + "." + lanes[i] + ".png")).u8string();
            if (!loadImageRGBA8(path, img, err)) die(err.c_str());
            std::vector<uint8_t> flipped(img.pixels.size());  // Unity layout: row 0 = bottom of the image
            size_t pitch = (size_t)img.width * 4;
            for (UINT y = 0; y < img.height; ++y)
                memcpy(flipped.data() + y * pitch, img.pixels.data() + (img.height - 1 - y) * pitch, pitch);
            textures[std::string(pr.second) + props[i]] =
                uploadTexture(dx, img.width, img.height, DXGI_FORMAT_R8G8B8A8_UNORM, 4, flipped.data(), D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // --- Descriptors: RTV per state texture; one SRV table per (pass, which texture is current) ---
    ComPtr<ID3D12DescriptorHeap> rtvHeap, srvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    check(dx.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 * kTableSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(dx.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)), "SRV heap");
    UINT rtvStep = dx.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    UINT srvStep = dx.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2];
    for (int i = 0; i < 2; ++i) {
        rtv[i] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += (SIZE_T)i * rtvStep;
        dx.dev->CreateRenderTargetView(state[i].Get(), nullptr, rtv[i]);
    }
    auto tableIndex = [](int pass, int cur) { return (UINT)(pass * 2 + cur) * kTableSize; };
    for (int p = 0; p < 2; ++p) {
        for (int cur = 0; cur < 2; ++cur) {
            for (UINT s = 0; s < kTableSize; ++s) {
                ID3D12Resource* res = blackTex.Get();
                for (auto& t : passes[p].ps.textures) {
                    if (t.slot != s) continue;
                    if (t.name == "_SelfTexture2D") res = state[cur].Get();
                    else if (textures.count(t.name)) res = textures[t.name].Get();
                }
                D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)(tableIndex(p, cur) + s) * srvStep;
                dx.dev->CreateShaderResourceView(res, nullptr, h);
            }
        }
    }

    // --- Per-frame command storage and constant upload space ---
    ComPtr<ID3D12CommandAllocator> allocs[kSlots];
    UINT64 slotFence[kSlots] = {};
    for (auto& a : allocs) check(dx.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> cl;
    check(dx.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].Get(), nullptr, IID_PPV_ARGS(&cl)), "command list");
    cl->Close();
    ComPtr<ID3D12Resource> cbuf;
    auto cbd = bufferDesc((UINT64)kSlots * 4 * kCbBytes);
    auto hu = heapProps(D3D12_HEAP_TYPE_UPLOAD);
    check(dx.dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &cbd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&cbuf)), "constant buffer");
    uint8_t* cbMapped = nullptr;
    check(cbuf->Map(0, nullptr, (void**)&cbMapped), "Map constants");

    // --- Material, as rvc_harness sets it up ---
    Material mat;
    mat.applyDefaults(shader);
    mat.setInt("_Ticks", opt.ticks);
    mat.setInt("_TicksDivisor", 1);
    mat.setInt("_DoTick", 0);
    mat.setInt("_Init", 0);
    mat.setInt("_InitRaw", 0);
    mat.setInt("_UdonUARTInChar", (int)hdr.sentChar);
    mat.setInt("_UdonUARTInTag", (int)hdr.sentTag);
    mat.setVector("unity_OrthoParams", 1, 1, 0, 1);
    mat.setVector("CustomRenderTextureParameters", 1, 0, 0, 0);
    mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
    mat.setVector("_CustomRenderTextureInfo", W, H, 1, 0);

    auto clockOf = [&](int cur) {
        std::vector<uint8_t> row = readRegion(dx, state[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 64, 1);
        return ((const uint32_t*)row.data())[28 * 4 + 1];
    };

    int cur = 0;
    uint64_t frame = 0;
    uint32_t clock0 = 0;
    auto tStart = std::chrono::steady_clock::now();
    auto benchT0 = tStart;
    for (;;) {
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        if (opt.seconds > 0 ? wall >= opt.seconds : frame >= opt.warmup + opt.frames) break;
        if (frame == opt.warmup) {
            dx.flush();
            clock0 = clockOf(cur);
            benchT0 = std::chrono::steady_clock::now();
        }
        UINT slot = (UINT)(frame % kSlots);
        dx.waitFor(slotFence[slot]);
        check(allocs[slot]->Reset(), "allocator reset");
        check(cl->Reset(allocs[slot].Get(), nullptr), "list reset");

        double t = hdr.time + (double)frame * opt.fixedDt;
        mat.setVector("_Time", t / 20, t, t * 2, t * 3);
        uint32_t mtimeLo, mtimeHi;
        rvcMtime(t, mtimeLo, mtimeHi);
        mat.setInt("_HostMtimeLo", mtimeLo);
        mat.setInt("_HostMtimeHi", mtimeHi);
        mat.setVector("_SinTime", sin(t / 8), sin(t / 4), sin(t / 2), sin(t));
        mat.setVector("_CosTime", cos(t / 8), cos(t / 4), cos(t / 2), cos(t));

        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        D3D12_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1};
        D3D12_RECT scissor{0, 0, (LONG)W, (LONG)H};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &scissor);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto draw = [&](int p, double cx, double cy, double zw, double zh) {
            Pass12& P = passes[p];
            int dst = 1 - cur;
            mat.setVector("CustomRenderTextureCenters", cx, cy, 0.5, 0);
            mat.setVector("CustomRenderTextureSizesAndRotations", zw, zh, 1, 0);
            mat.fillGlobals(P.vs);
            mat.fillGlobals(P.ps);
            UINT64 base = ((UINT64)slot * 4 + (UINT64)p * 2) * kCbBytes;
            if (P.vs.hasGlobals) memcpy(cbMapped + base, P.vs.scratch.data(), P.vs.scratch.size());
            if (P.ps.hasGlobals) memcpy(cbMapped + base + kCbBytes, P.ps.scratch.data(), P.ps.scratch.size());
            cl->BeginEvent(1, P.name.c_str(), (UINT)P.name.size() + 1);  // 1 = ANSI string marker
            cl->SetPipelineState(P.pso.Get());
            cl->SetGraphicsRootSignature(P.root.Get());
            cl->SetGraphicsRootConstantBufferView(0, cbuf->GetGPUVirtualAddress() + base);
            cl->SetGraphicsRootConstantBufferView(1, cbuf->GetGPUVirtualAddress() + base + kCbBytes);
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += (UINT64)tableIndex(p, cur) * srvStep;
            cl->SetGraphicsRootDescriptorTable(2, table);
            cl->OMSetRenderTargets(1, &rtv[dst], FALSE, nullptr);
            cl->DrawInstanced(6, 1, 0, 0);
            cl->EndEvent();
        };

        // Zone 1: CPUTick on the 64x64 state area, then copy that area back so `cur` stays complete.
        draw(0, 32, 4064, 64, 64);
        int dst = 1 - cur;
        D3D12_RESOURCE_BARRIER toCopy[2] = {
            transition(state[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(state[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        cl->ResourceBarrier(2, toCopy);
        D3D12_TEXTURE_COPY_LOCATION cdst{}, csrc{};
        cdst.pResource = state[cur].Get();
        csrc.pResource = state[dst].Get();
        D3D12_BOX box{0, 0, 0, 64, 64, 1};
        cl->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, &box);
        D3D12_RESOURCE_BARRIER fromCopy[2] = {
            transition(state[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(state[cur].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        cl->ResourceBarrier(2, fromCopy);

        // Zone 2: Commit on the whole texture, then the buffers swap roles.
        draw(1, 1024, 2048, 2048, 4096);
        D3D12_RESOURCE_BARRIER swapStates[2] = {
            transition(state[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(state[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        cl->ResourceBarrier(2, swapStates);
        check(cl->Close(), "Close");
        ID3D12CommandList* lists[] = {cl.Get()};
        dx.queue->ExecuteCommandLists(1, lists);
        check(swap->Present(0, 0), "Present");
        slotFence[slot] = dx.signal();
        cur = dst;
        ++frame;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        HRESULT removed = dx.dev->GetDeviceRemovedReason();
        if (FAILED(removed)) die("device removed", removed);
    }

    dx.flush();
    if (opt.debug) {
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(dx.dev.As(&iq))) {
            UINT64 n = iq->GetNumStoredMessages();
            fprintf(stderr, "[trace12] %llu debug layer message(s)\n", (unsigned long long)n);
            for (UINT64 i = 0; i < n && i < 12; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                std::vector<char> buf(len);
                auto* m = (D3D12_MESSAGE*)buf.data();
                if (SUCCEEDED(iq->GetMessage(i, m, &len))) fprintf(stderr, "  %s\n", m->pDescription);
            }
        }
    }
    double secs =std::chrono::duration<double>(std::chrono::steady_clock::now() - benchT0).count();
    if (frame > opt.warmup) {
        uint64_t frames = frame - opt.warmup;
        uint32_t instr = clockOf(cur) - clock0;
        std::vector<uint8_t> area = readRegion(dx, state[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 64, 64);
        if (!opt.dumpState.empty()) writeFileBinary(opt.dumpState, area.data(), area.size());
        fprintf(stderr, "\nBENCH frames=%llu seconds=%.3f instructions=%u ips=%.0f fps=%.1f per_frame=%.1f state=%016llx\n",
                (unsigned long long)frames, secs, instr, instr / secs, frames / secs, (double)instr / frames,
                (unsigned long long)fnv1a64(area.data(), area.size()));
    }
    return 0;
}
