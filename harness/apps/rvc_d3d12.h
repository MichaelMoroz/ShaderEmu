// D3D12 plumbing shared by rvc_trace12 and the D3D12 backend of rvc_harness: device and fence,
// texture upload and readback, DXC, and building the two rvc passes into pipeline states.
// Include from one translation unit per executable.
#pragma once

#include "common.h"
#include "material.h"
#include "shaderlab.h"

#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dxgi1_4.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const UINT W = 2048, H = 9280;   // the state texture (rvc_backend.h, kStateHeight)
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

[[noreturn]] void die(const char* what, HRESULT hr = S_OK) {
    fprintf(stderr, "[d3d12] %s%s%s\n", what, FAILED(hr) ? ": " : "", FAILED(hr) ? hrToString(hr).c_str() : "");
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
            for (auto& e : std::filesystem::directory_iterator("C:/Program Files (x86)/Windows Kits/10/bin", ec)) {
                std::filesystem::path cand = e.path() / "x64";
                if (std::filesystem::exists(cand / "dxcompiler.dll", ec) && cand.u8string() > dir) dir = cand.u8string();
            }
        }
        std::wstring path = widen((std::filesystem::u8path(dir) / "dxcompiler.dll").u8string());
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
    // RVC12_DXBC=1: shaders as DXBC from d3dcompiler_47.dll (fxc2 when its DLL is beside the
    // harness), to tell the bytecode's cost from the API's
    if (getenv("RVC12_DXBC")) {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(text.data(), text.size(), entry.c_str(), nullptr, nullptr, entry.c_str(),
                                profile.compare(0, 3, "ps_") == 0 ? "ps_5_0" : "vs_5_0",
                                D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0, &code, &errors);
        if (FAILED(hr))
            die(("D3DCompile failed for " + entry + ":\n" +
                 (errors ? std::string((const char*)errors->GetBufferPointer(), errors->GetBufferSize()).substr(0, 1500) : "")).c_str());
        ComPtr<ID3D12ShaderReflection> refl;
        check(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&refl)), "D3DReflect (D3D12)");
        reflectDxil(layout, refl.Get());
        const uint8_t* p = (const uint8_t*)code->GetBufferPointer();
        return std::vector<uint8_t>(p, p + code->GetBufferSize());
    }
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
    int vsSelf = -1;   // register of _SelfTexture2D in the vertex shader, when it reads the state
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};


struct Build12 {
    bool dxc = false;
    std::string dxcOpt = "-O3";  // DXC optimisation flags, space separated
    std::string dxcSm = "6_6";   // DXC shader model; 6.6 matches FXC speed, 6.0 is ~10% slower
    std::string dxcDump;         // folder to write the DXIL of each pixel shader to (<pass>.dxil)
    std::string dxcDir;          // folder with dxcompiler.dll; default: newest Windows SDK bin
};

// RVC_DXIL=1: what Nsight can read. DXBC to DXIL by dxilconv, the container signed by hand (the skill nsight-gpu-trace, "DXBC vs DXIL").
inline void dxbcHashBlock(uint32_t st[4], const uint8_t* b) {
    static const uint8_t R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t x[16];
    memcpy(x, b, 64);
    uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (B & C) | (~B & D); g = i; }
        else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) & 15; }
        else { f = C ^ (B | ~D); g = (7 * i) & 15; }
        uint32_t k = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
        uint32_t t = A + f + k + x[g];
        int r = R[(i / 16) * 4 + i % 4];
        A = D; D = C; C = B;
        B = B + ((t << r) | (t >> (32 - r)));
    }
    st[0] += A; st[1] += B; st[2] += C; st[3] += D;
}
inline void dxbcSign(std::vector<uint8_t>& c) {
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t* d = c.data() + 20;
    size_t n = c.size() - 20, at = 0;
    for (; n - at >= 64; at += 64) dxbcHashBlock(st, d + at);
    size_t tail = n - at;
    uint32_t bits = (uint32_t)n * 8, last = (bits >> 2) | 1;
    uint8_t b[64] = {};
    if (tail < 56) {
        memcpy(b, &bits, 4);
        memcpy(b + 4, d + at, tail);
        b[4 + tail] = 0x80;
        memcpy(b + 60, &last, 4);
        dxbcHashBlock(st, b);
    } else {
        memcpy(b, d + at, tail);
        b[tail] = 0x80;
        dxbcHashBlock(st, b);
        memset(b, 0, 64);
        memcpy(b, &bits, 4);
        memcpy(b + 60, &last, 4);
        dxbcHashBlock(st, b);
    }
    memcpy(c.data() + 4, st, 16);
}
struct __declspec(uuid("5F956ED5-78D1-4B15-8247-F7187614A041")) IDxbcConverterLocal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Convert(LPCVOID, UINT32, LPCWSTR, LPVOID*, UINT32*, LPWSTR*) = 0;
};
inline std::vector<uint8_t> dxbcToDxil(const std::vector<uint8_t>& in) {
    static HMODULE m = LoadLibraryA("C:\\Windows\\System32\\dxilconv.dll");
    if (!m) die("dxilconv.dll");
    typedef HRESULT(WINAPI * Create)(REFCLSID, REFIID, LPVOID*);
    Create create = (Create)GetProcAddress(m, "DxcCreateInstance");
    static const GUID clsid = {0x4900391E, 0xB752, 0x4EDD, {0xA8, 0x85, 0x6F, 0xB7, 0x6E, 0x25, 0xAD, 0xDB}};
    ComPtr<IDxbcConverterLocal> conv;
    check(create(clsid, __uuidof(IDxbcConverterLocal), (LPVOID*)conv.GetAddressOf()), "dxilconv");
    void* out = nullptr;
    UINT32 n = 0;
    LPWSTR diag = nullptr;
    check(conv->Convert(in.data(), (UINT32)in.size(), nullptr, &out, &n, &diag), "dxbc to dxil");
    std::vector<uint8_t> r((uint8_t*)out, (uint8_t*)out + n);
    CoTaskMemFree(out);
    dxbcSign(r);
    return r;
}

// Compiles `count` (1 or 2) passes of a shader (FXC bytecode from the shared cache, or DXC) into passes[].
void buildPasses12(Dx& dx, const SLShader& shader, const CompileSettings& cs, const Build12& opt, Pass12 passes[2],
                   const char* first = "CPUTick", const char* second = "Commit", int count = 2) {
    std::string err;
    std::string rootDir = std::filesystem::u8path(shader.path).parent_path().u8string();
    Dxc dxc;
    if (opt.dxc) dxc.init(opt.dxcDir);
    const char* passNames[2] = {first, second};
    for (int p = 0; p < count; ++p) {
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
            if (!opt.dxcDump.empty())
                writeFileBinary((std::filesystem::u8path(opt.dxcDump) / (P.name + ".dxil")).u8string(), psBytes.data(), psBytes.size());
            fprintf(stderr, "[d3d12] pass '%s': DXC in %.1fs (ps %zu bytes)\n", P.name.c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count(), psBytes.size());
        } else {
        StageResult vs = compileStage(sp->code, shader.path, rootDir, sp->vertexEntry, "vs_" + sm, {{"SHADER_STAGE_VERTEX", "1"}}, cs);
        StageResult ps = compileStage(sp->code, shader.path, rootDir, sp->fragmentEntry, "ps_" + sm, {{"SHADER_STAGE_FRAGMENT", "1"}}, cs);
        if (!vs.ok || !ps.ok) die((P.name + " failed to compile:\n" + vs.log + ps.log).c_str());
        fprintf(stderr, "[d3d12] pass '%s': vs %s, ps %s in %.1fs (%zu bytes)\n", P.name.c_str(), vs.fromCache ? "cached" : "compiled",
                ps.fromCache ? "cached" : "compiled", ps.seconds, (size_t)ps.bytecode->GetBufferSize());
        if (!P.vs.reflect(nullptr, vs.bytecode.Get(), err) || !P.ps.reflect(nullptr, ps.bytecode.Get(), err)) die(err.c_str());
        vsBytes.assign((const uint8_t*)vs.bytecode->GetBufferPointer(), (const uint8_t*)vs.bytecode->GetBufferPointer() + vs.bytecode->GetBufferSize());
        psBytes.assign((const uint8_t*)ps.bytecode->GetBufferPointer(), (const uint8_t*)ps.bytecode->GetBufferPointer() + ps.bytecode->GetBufferSize());
        // RVC_PS_CSO=FILE: the first pass's pixel shader is this blob of build\\shadercache (D3D11's tick, as it is, for its code)
        if (getenv("RVC_PS_CSO") && p == 0) {
            std::string blob;
            if (!readFileBinary(getenv("RVC_PS_CSO"), blob)) die("RVC_PS_CSO");
            psBytes.assign(blob.begin(), blob.end());
            ComPtr<ID3DBlob> pb;
            D3DCreateBlob(blob.size(), &pb);
            memcpy(pb->GetBufferPointer(), blob.data(), blob.size());
            P.ps = decltype(P.ps){};
            if (!P.ps.reflect(nullptr, pb.Get(), err)) die(err.c_str());
        }
        if (getenv("RVC_DXIL")) {
            vsBytes = dxbcToDxil(vsBytes);
            psBytes = dxbcToDxil(psBytes);
            fprintf(stderr, "[d3d12] pass '%s': DXIL %zu bytes\n", P.name.c_str(), psBytes.size());
        }
        }
        if (P.vs.textures.size() == 1 && P.vs.textures[0].name == "_SelfTexture2D") P.vsSelf = (int)P.vs.textures[0].slot;
        else if (!P.vs.textures.empty()) die("a vertex shader may read no texture but _SelfTexture2D");
        if (!P.vs.samplers.empty() || !P.ps.samplers.empty() || !P.ps.uavs.empty())
            die("shader uses samplers or UAVs, which this runner does not bind");
        if (P.vs.globalsSize > kCbBytes || P.ps.globalsSize > kCbBytes) die("$Globals larger than the upload slot");
        for (auto& t : P.ps.textures)
            if (t.slot >= kTableSize) die("texture slot beyond the SRV table");

        // Root: [0] VS $Globals, [1] PS $Globals, [2] PS texture table t0..t15.
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kTableSize;
        D3D12_DESCRIPTOR_RANGE vsRange{};
        vsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        vsRange.NumDescriptors = 1;
        vsRange.BaseShaderRegister = (UINT)(P.vsSelf < 0 ? 0 : P.vsSelf);
        D3D12_ROOT_PARAMETER rp[4]{};
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;   // [3] the state, for the vertex shader
        rp[3].DescriptorTable.NumDescriptorRanges = 1;
        rp[3].DescriptorTable.pDescriptorRanges = &vsRange;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
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
        rsd.NumParameters = P.vsSelf < 0 ? 3 : 4;
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
        fprintf(stderr, "[d3d12] pass '%s': pipeline state in %.1fs\n", P.name.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

}

}  // namespace
