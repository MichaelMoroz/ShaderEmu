// gvm_gpu: gpu_vm machines on the card. The tick (gpu_vm/tick/steps/x17.hlsl, its interface gpu_vm/tick/STATE.md) runs
// the cores in passes on D3D11; this host loads the program, gives each pass its TIME, reads the console and the cores'
// state back, makes the semihosted calls, and with --check runs the reference machine (gpu_vm/ref/gvm_ref.h) beside
// the card, bundle for bundle. gvm_cpu's GPU twin: the same command line where it makes sense (gpu_vm/gpu/README.md).
//
//   gvm_gpu [options] PROGRAM.elf [ARGS...]
//   gvm_gpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]
//   gvm_gpu [options] --dtb gvm.dtb vmlinux
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define GVM_REF_IMPL
#include "../ref/gvm_ref.h"

// exit codes of gvm_gpu itself (a program's exit is its own code), gvm_cpu's and two more
#define EXIT_USAGE 1   // bad options, a file that cannot be loaded
#define EXIT_TRAP 2    // an unhandled trap (break too), every core halted, or every core waiting for nothing
#define EXIT_LIMIT 3   // --max-bundles reached
#define EXIT_CHECK 4   // --check: the card and the reference differ
#define EXIT_GPU 5     // D3D11 failed (no device, a shader that does not compile, a lost device)

#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_INVAL 22
#define E_NOTTY 25
#define E_NOSYS 38

// The tick's state (STATE.md): flags (18.z) and why a pass ended (19.w)
enum { F_WAIT = 1, F_SOFT = 2, F_HALT = 4, F_SYNC = 8, F_AGAIN = 16 };
enum { E_BUDGET = 0, E_SYNC = 1, E_WAIT = 2, E_FULL = 3, E_HALT = 5 };
// words of a core's state as read back (texel T word j is 4T + j)
enum { V_PC = 64, V_ST, V_EPC, V_CAUSE, V_TVAL, V_TVEC, V_SCRATCH, V_IEN, V_TCMPLO, V_TCMPHI, V_FLAGS, V_RND,
       V_CYCLE, V_OPS, V_LAST, V_END, V_QN = 88 };
static const char* end_name[6] = {"budget", "sync", "wait", "full", "?", "halt"};

// the console device (gvm_cpu.c, gpu_vm/linux/README.md)
#define CON_BASE 0x016b8280u
#define CON_TEXELS 18u   // delivered/last, taken/written, the ring's 16
#define CON_DELIVERED 0
#define CON_LAST 1
#define CON_TAKEN 4
#define CON_WRITTEN 5
#define CON_RING 8       // (words from CON_BASE)

// The host-call stub at address 0: a trap while TVEC is 0 lands there, and the core spins in it (`sync` ends each pass)
// until the host has made the call (or reported the trap): { sync ; j 0 }
static const uint32_t STUB[4] = {GVM_X_SYNC << 12, 0, 0, 1u << 3 | GVM_CTRL_J};

// ---- Options and the machine as loaded
static struct {
    uint32_t ncores = 1, budget = 8192, pass_time = 20, ram_end = 0, batch = 32, rounds = 2, check_ram = 4096;
    uint64_t max_bundles = 0;
    bool stats = false, strace = false, console = true, console_in = true, check = false, trace_pass = false, warp = false;
    std::string shader;
} O;
static std::vector<uint32_t> g_ram;   // the image (address 0 up), RAM's words
static std::vector<uint8_t> g_rom;
static uint32_t g_rom_size = 0, g_heap_start, g_brk, g_mmap_top, g_stack_limit;
static uint64_t g_time = 0;           // TIME of the next pass

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void out_flush() { fflush(stdout); fflush(stderr); }

// ---- A disassembler (gvm_cpu.c's), for the check's report
static const char* int_name[31] = {"add", "and", "or", "xor", "sub", "andn", "orn", "xnor", "slt", "sltu", "min", "minu",
    "sge", "sgeu", "max", "maxu", "sll", "srl", "sra", "mul", "ror", "rol", "seq", "sne", "shadd", "sel", "addcy", "carry",
    "subby", "borrow", "bfe"};
static const char* flt_name[32] = {"fadd", "fmul", "fmad", "fmsb", "fsub", "fnmsb", "fmin", "fmax", "feq", "flt", "fle",
    "funord", "fsgnj", "fsgnjn", "fsgnjx", "ffrac", "itof", "utof", "ftoi", "ftou", "ffloor", "fceil", "ftrunc", "fround",
    "fdiv", "fsqrt", "frsqrt", "frcp", "fsin", "fcos", "fexp2", "flog2"};
static const char* width_name[8] = {"w", "d", "h", "b", "q", "?", "hu", "bu"};
static const char* amo_name[8] = {"swap", "add", "and", "or", "xor", "min", "max", "cas"};
static const char* ext_name(uint32_t opc) {
    static const char* n[0x1d] = {"ill", "mulh", "mulhu", "mulhsu", "div", "divu", "rem", "remu", "clz", "ctz", "popc",
        "bswap", "brev", "bfi", "lpc", 0, "syscall", "break", "wait", "sync", "fence", 0, 0, 0, "mfsr", "mtsr", "xsr",
        "srset", "srclr"};
    return opc < 0x1d && n[opc] ? n[opc] : 0;
}
static void dis_op(char* o, size_t sz, uint32_t w, int is_long, uint32_t limm) {
    uint32_t opc = GVM_OPC(w), rd = GVM_RD(w), ra = GVM_RA(w), rb = GVM_RB(w), rc = GVM_RC(w), f = opc & 31;
    int32_t imm = is_long ? (int32_t)limm : GVM_IMM12(w);
    char is[24];
    if (is_long) snprintf(is, sizeof is, "0x%08x", limm); else snprintf(is, sizeof is, "%d", imm);
    switch (GVM_CLASS(opc)) {
    case 1:
        if (f > 30) break;
        if (!GVM_IFORM(opc)) {
            if (f == GVM_I_SHADD) { snprintf(o, sz, "shadd r%u, r%u, r%u << %u", rd, ra, rb, rc & 31); return; }
            if (f >= GVM_I_SEL && f <= GVM_I_BORROW) { snprintf(o, sz, "%s r%u, r%u, r%u, r%u", int_name[f], rd, ra, rb, rc); return; }
            if (f > GVM_I_COMMON_LAST) break;
            snprintf(o, sz, "%s r%u, r%u, r%u", int_name[f], rd, ra, rb);
            return;
        }
        if (f == GVM_I_BFE) { snprintf(o, sz, "bfe r%u, r%u, pos %u, len %u%s", rd, ra, imm & 31, ((imm >> 5) & 31) + 1, imm & 0x400 ? ", signed" : ""); return; }
        if (f > GVM_I_COMMON_LAST) break;
        if (f == GVM_I_ADD && ra == 0) { snprintf(o, sz, "li r%u, %s", rd, is); return; }
        snprintf(o, sz, "%si r%u, r%u, %s", int_name[f], rd, ra, is);
        return;
    case 2: {
        uint32_t mode = (opc >> 3) & 7, wd = opc & 7;
        const char* wn = width_name[wd];
        switch (mode) {
        case GVM_M_LOAD: snprintf(o, sz, "l%s r%u, [r%u + %s]", wn, rd, ra, is); return;
        case GVM_M_STORE: snprintf(o, sz, "s%s r%u, [r%u + %s]", wn, rd, ra, is); return;
        case GVM_M_LOADX: snprintf(o, sz, "l%sx r%u, [r%u + r%u << %u]", wn, rd, ra, rb, rc & 31); return;
        case GVM_M_STOREX: snprintf(o, sz, "s%sx r%u, [r%u + r%u << %u]", wn, rd, ra, rb, rc & 31); return;
        case GVM_M_AMO: snprintf(o, sz, "amo%s r%u, [r%u], r%u", amo_name[wd], rd, ra, rb); return;
        case GVM_M_AMO2: if (wd <= 1) { snprintf(o, sz, "amo%s r%u, [r%u], r%u", wd ? "maxu" : "minu", rd, ra, rb); return; } break;
        }
        break;
    }
    case 3:
        if (GVM_IFORM(opc)) {
            if (!is_long) snprintf(is, sizeof is, "0x%08x", (uint32_t)imm << 20);
            snprintf(o, sz, "%si r%u, r%u, %s", flt_name[f], rd, ra, is);
            return;
        }
        if (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB) { snprintf(o, sz, "%s r%u, r%u, r%u, r%u", flt_name[f], rd, ra, rb, rc); return; }
        snprintf(o, sz, "%s r%u, r%u, r%u", flt_name[f], rd, ra, rb);
        return;
    default: {
        const char* n = ext_name(opc);
        if (!n) break;
        switch (opc) {
        case GVM_X_SYSCALL: case GVM_X_BREAK: case GVM_X_WAIT: case GVM_X_SYNC: case GVM_X_FENCE: case GVM_X_ILL:
            snprintf(o, sz, "%s", n); return;
        case GVM_X_MFSR: snprintf(o, sz, "mfsr r%u, sr%d", rd, imm & 0xfff); return;
        case GVM_X_MTSR: snprintf(o, sz, "mtsr sr%d, r%u", imm & 0xfff, ra); return;
        case GVM_X_XSR: case GVM_X_SRSET: case GVM_X_SRCLR: snprintf(o, sz, "%s r%u, sr%d, r%u", n, rd, imm & 0xfff, ra); return;
        case GVM_X_BFI: snprintf(o, sz, "bfi r%u, r%u, pos %u, len %u", rd, ra, imm & 31, ((imm >> 5) & 31) + 1); return;
        case GVM_X_LPC: snprintf(o, sz, "lpc r%u, %s", rd, is); return;
        }
        if (opc >= GVM_X_CLZ && opc <= GVM_X_BREV) { snprintf(o, sz, "%s r%u, r%u", n, rd, ra); return; }
        snprintf(o, sz, "%s r%u, r%u, r%u", n, rd, ra, rb);
        return;
    }
    }
    snprintf(o, sz, ".word 0x%08x", w);
}
static void dis_ctl(char* o, size_t sz, uint32_t base, uint32_t ctl) {
    static const char* cc_name[16] = {0, "addbnz", "bltu", "bgeu", "beq", "bne", "blt", "bge", "bbs", "bbc", "bltui",
        "bgeui", "beqi", "bnei", "blti", "bgei"};
    uint32_t cc = GVM_CC(ctl), to = base + (uint32_t)GVM_OFF9(ctl) * 16;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), far_to = base + (uint32_t)GVM_OFF18(ctl) * 16;
        switch (sub) {
        case GVM_SUB_NONE: snprintf(o, sz, "-"); return;
        case GVM_SUB_JR: snprintf(o, sz, rs == 1 ? "ret" : "jr r%u", rs); return;
        case GVM_SUB_CALLR: snprintf(o, sz, "callr r%u", rs); return;
        case GVM_SUB_ERET: snprintf(o, sz, "eret"); return;
        case GVM_SUB_J: snprintf(o, sz, "j %08x", far_to); return;
        case GVM_SUB_CALL: snprintf(o, sz, "call %08x", far_to); return;
        case GVM_SUB_ADD: snprintf(o, sz, "add r%u += %d", rs, GVM_CIMM12(ctl)); return;
        }
        snprintf(o, sz, "illegal");
        return;
    }
    if (cc == GVM_CC_BNE && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) { snprintf(o, sz, "-"); return; }
    if (cc == GVM_CC_BEQ && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) { snprintf(o, sz, "j %08x", to); return; }
    if (cc == GVM_CC_ADDBNZ || cc >= GVM_CC_BLTUI || cc == GVM_CC_BBS || cc == GVM_CC_BBC)
        snprintf(o, sz, "%s r%u, %d -> %08x", cc_name[cc], GVM_CRA(ctl), cc == GVM_CC_BBS || cc == GVM_CC_BBC ? (int)(GVM_CRB(ctl) & 31) : GVM_CIMM6(ctl), to);
    else
        snprintf(o, sz, "%s r%u, r%u -> %08x", cc_name[cc], GVM_CRA(ctl), GVM_CRB(ctl), to);
}
static std::string dis_bundle(uint32_t pc, const uint32_t t[4]) {
    uint32_t n = GVM_N(t[3]), from = (pc & 15) >> 2, base = pc & ~15u;
    std::string s;
    char buf[96];
    for (uint32_t k = from; k < n && k < 2; k++) {
        dis_op(buf, sizeof buf, t[k], k == 0 ? GVM_LONG0(t[3]) : GVM_LONG1(t[3]), t[2]);
        s += (k > from ? " ; " : "") + std::string(buf);
    }
    if (n == 3) s += "(n 3: illegal)";
    dis_ctl(buf, sizeof buf, base, t[3]);
    return s + (s.empty() ? "" : " ") + "| " + buf;
}

// ---- D3D11
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        fprintf(stderr, "gvm_gpu: %s failed: %08lx\n", what, (unsigned long)hr);
        if (dev && FAILED(dev->GetDeviceRemovedReason())) fprintf(stderr, "gvm_gpu: the device was removed: %08lx\n", (unsigned long)dev->GetDeviceRemovedReason());
        exit(EXIT_GPU);
    }
}
struct Tex { ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11RenderTargetView* rtv = nullptr; UINT w = 0, h = 0; };
static Tex make_tex(UINT w, UINT h, const void* init) {
    Tex t; t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {init, w * 16, 0};
    check(dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.t), "CreateTexture2D");
    check(dev->CreateShaderResourceView(t.t, nullptr, &t.srv), "SRV");
    check(dev->CreateRenderTargetView(t.t, nullptr, &t.rtv), "RTV");
    return t;
}
static ID3D11Texture2D* make_staging(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D* st;
    check(dev->CreateTexture2D(&d, nullptr, &st), "staging texture");
    return st;
}
static std::string slurp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
static std::string g_exe_dir;
// compiled shaders are kept in bin\gvmcache by a hash of the tick, isa.h, the entry and the compiler
static ID3DBlob* compile(const std::string& path, const char* entry, const char* profile) {
    std::string dir = path.substr(0, path.find_last_of("\\/"));
    std::string key = std::string("gvm_gpu;") + slurp(path) + slurp(dir + "/../../isa.h") + entry + profile;
    char dll[MAX_PATH] = "";
    GetModuleFileNameA(GetModuleHandleA("d3dcompiler_47.dll"), dll, MAX_PATH);
    key += std::to_string(slurp(dll).size());
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    char name[64];
    snprintf(name, sizeof name, "%016llx.cso", (unsigned long long)h);
    std::string cdir = g_exe_dir + "\\gvmcache";
    CreateDirectoryA(cdir.c_str(), nullptr);
    std::string cfile = cdir + "\\" + name, got = slurp(cfile);
    ID3DBlob* code = nullptr;
    if (!got.empty()) {
        check(D3DCreateBlob(got.size(), &code), "D3DCreateBlob");
        memcpy(code->GetBufferPointer(), got.data(), got.size());
        return code;
    }
    std::vector<wchar_t> wpath(path.size() + 1);
    mbstowcs(wpath.data(), path.c_str(), wpath.size());
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompileFromFile(wpath.data(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err && FAILED(hr)) fprintf(stderr, "%s", (const char*)err->GetBufferPointer());
    check(hr, entry);
    if (FILE* f = fopen(cfile.c_str(), "wb")) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
    return code;
}
struct Prog { ID3D11VertexShader* vs; ID3D11PixelShader* ps; };
static Prog program(const std::string& path, const char* v, const char* p) {
    Prog r;
    ID3DBlob *vb = compile(path, v, "vs_5_0"), *pb = compile(path, p, "ps_5_0");
    check(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &r.vs), v);
    check(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &r.ps), p);
    vb->Release(); pb->Release();
    return r;
}
// a step's constant: `static const uint NAME = V;` or `#define NAME V`
static uint32_t step_const(const std::string& src, const char* name, uint32_t dflt) {
    std::string n = name;
    for (const std::string& form : {"#define " + n + " ", "uint " + n + " = "}) {
        size_t at = src.find(form);
        if (at != std::string::npos) return (uint32_t)atoi(src.c_str() + at + form.size());
    }
    return dflt;
}

// ---- The card's machine (STATE.md: the textures, a pass's draws in order)
struct PassParams { uint32_t ticks, ramEnd, timeLo, timeHi, player, flags, ramRows, ext0, ncores, pad[3], ext[8]; };
static uint32_t WCN = 130, RAM_W = 8192, T_WC = 40, OP_SLOTS = 32, OP_SPAN = 272, NTEX, SPIX;
struct Gpu {
    uint32_t cores = 1, rows = 0, romRows = 1;
    Tex ram, rom, stage, ops[2], set[2][8];
    int cur = 0, oc = 0;
    Prog tick, commit, stg, wrt, prog;
    ID3D11Buffer *cb = nullptr, *ccb[4] = {};
    ID3D11BlendState* mask[4] = {};
    PassParams pass = {};
    std::vector<ID3D11Texture2D*> snaps;   // a pass's state and console, read back after a batch
    uint32_t snapW = 0, snapH = 0;
    ID3D11Texture2D* rd = nullptr;         // RAM rows read back
    static const uint32_t RD_ROWS = 64;

    void make(const std::vector<std::vector<uint32_t>>& state) {
        rows = O.ram_end / 16 / RAM_W;
        ram = make_tex(RAM_W, rows, g_ram.data());
        romRows = std::max(1u, (uint32_t)((g_rom.size() + 16 * RAM_W - 1) / (16 * RAM_W)));
        std::vector<uint8_t> romImg((size_t)romRows * RAM_W * 16, 0);
        memcpy(romImg.data(), g_rom.data(), g_rom.size());
        rom = make_tex(RAM_W, romRows, romImg.data());
        std::vector<uint32_t> z((size_t)OP_SPAN * cores * OP_SLOTS * 4, 0);
        stage = make_tex(OP_SPAN, cores * OP_SLOTS, z.data());
        std::vector<uint32_t> oz((size_t)cores * 4, 0);
        for (int k = 0; k < 2; k++) ops[k] = make_tex(cores, 1, oz.data());
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) {
                std::vector<uint32_t> img((size_t)SPIX * cores * 4, 0);
                for (uint32_t c = 0; c < cores; c++)
                    for (uint32_t tx = t; tx < NTEX; tx += 8) memcpy(&img[((size_t)c * SPIX + tx / 8) * 4], &state[c][tx * 4], 16);
                set[k][t] = make_tex(SPIX, cores, img.data());
            }
        D3D11_BUFFER_DESC bd = {sizeof(PassParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(dev->CreateBuffer(&bd, nullptr, &cb), "cbuffer");
        for (uint32_t c = 0; c < 4; c++) {
            D3D11_BLEND_DESC b = {};
            b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            b.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1 << c);
            check(dev->CreateBlendState(&b, &mask[c]), "blend");
            uint32_t cdata[4] = {c, cores, 0, 0};
            D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA ci = {cdata, 0, 0};
            check(dev->CreateBuffer(&cd, &ci, &ccb[c]), "commit cbuffer");
        }
        snapW = std::max(SPIX, CON_TEXELS);
        snapH = 8 * cores + 1;
        rd = make_staging(RAM_W, RD_ROWS);
        pass = {O.budget, O.ram_end, 0, 0, 0, 0, rows, 0, cores, {0, 0, 0}, {0}};
    }
    void clear_srv() {
        ID3D11ShaderResourceView* none[12] = {};
        ctx->PSSetShaderResources(0, 12, none);
        ctx->VSSetShaderResources(0, 12, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void bind(int which, bool withRam, bool withStage) {   // t0 RAM, t1 ROM, t2-t9 a state set, t10 ops, t11 stage
        ID3D11ShaderResourceView* s[12] = {};
        if (withRam) s[0] = ram.srv;
        s[1] = rom.srv;
        for (int t = 0; t < 8; t++) s[2 + t] = set[which][t].srv;
        s[10] = ops[oc].srv;
        if (withStage) s[11] = stage.srv;
        ctx->PSSetShaderResources(0, 12, s);
        ctx->VSSetShaderResources(0, 12, s);
    }
    void viewport(UINT w, UINT h) { D3D11_VIEWPORT v = {0, 0, (float)w, (float)h, 0, 1}; ctx->RSSetViewports(1, &v); }
    void run_pass() {
        ctx->UpdateSubresource(cb, 0, nullptr, &pass, 0, 0);
        ID3D11Buffer* cbs[2] = {cb, ccb[0]};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->IASetInputLayout(nullptr);
        // the tick: set cur from set cur ^ 1
        clear_srv();
        ID3D11RenderTargetView* rt[8];
        for (int t = 0; t < 8; t++) rt[t] = set[cur][t].rtv;
        ctx->OMSetRenderTargets(8, rt, nullptr);
        bind(cur ^ 1, true, false);
        viewport(SPIX, cores);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(tick.vs, nullptr, 0); ctx->PSSetShader(tick.ps, nullptr, 0);
        ctx->Draw(3, 0);
        // the commit: four draws, a component each
        clear_srv();
        ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
        bind(cur, false, false);
        viewport(RAM_W, rows);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(commit.vs, nullptr, 0); ctx->PSSetShader(commit.ps, nullptr, 0);
        for (int c = 0; c < 4; c++) {
            ID3D11Buffer* b2[2] = {cb, ccb[c]};
            ctx->VSSetConstantBuffers(0, 2, b2);
            ctx->OMSetBlendState(mask[c], nullptr, 0xffffffff);
            ctx->Draw(cores * WCN, 0);
        }
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        // the operations: rounds of stage, write, progress; _OpState ping-pongs a round
        for (uint32_t round = 0; round < O.rounds; round++) {
            clear_srv();
            ctx->OMSetRenderTargets(1, &stage.rtv, nullptr);
            bind(cur, true, false);
            viewport(OP_SPAN, cores * OP_SLOTS);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(stg.vs, nullptr, 0); ctx->PSSetShader(stg.ps, nullptr, 0);
            ctx->DrawInstanced(6, cores * OP_SLOTS, 0, 0);
            clear_srv();
            ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
            bind(cur, false, true);
            viewport(RAM_W, rows);
            ctx->VSSetShader(wrt.vs, nullptr, 0); ctx->PSSetShader(wrt.ps, nullptr, 0);
            ctx->DrawInstanced(12, cores * OP_SLOTS, 0, 0);
            clear_srv();
            ctx->OMSetRenderTargets(1, &ops[oc ^ 1].rtv, nullptr);
            bind(cur, false, false);
            viewport(cores, 1);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ctx->VSSetShader(prog.vs, nullptr, 0); ctx->PSSetShader(prog.ps, nullptr, 0);
            ctx->Draw(cores, 0);
            oc ^= 1;
        }
        clear_srv();
        cur ^= 1;
    }
    // after a pass: its state (the set just drawn) and the console's texels into snapshot i
    void snapshot(uint32_t i) {
        while (snaps.size() <= i) snaps.push_back(make_staging(snapW, snapH));
        D3D11_BOX b = {0, 0, 0, SPIX, cores, 1};
        for (uint32_t t = 0; t < 8; t++) ctx->CopySubresourceRegion(snaps[i], 0, 0, t * cores, 0, set[cur ^ 1][t].t, 0, &b);
        if (O.console) {
            uint32_t ct = CON_BASE >> 4;
            D3D11_BOX cbx = {ct % RAM_W, ct / RAM_W, 0, ct % RAM_W + CON_TEXELS, ct / RAM_W + 1, 1};
            ctx->CopySubresourceRegion(snaps[i], 0, 0, 8 * cores, 0, ram.t, 0, &cbx);
        }
    }
    // snapshot i decoded: each core's NTEX texels, and the console's words
    void read_snapshot(uint32_t i, std::vector<std::vector<uint32_t>>& st, uint32_t con[CON_TEXELS * 4]) {
        D3D11_MAPPED_SUBRESOURCE m;
        check(ctx->Map(snaps[i], 0, D3D11_MAP_READ, 0, &m), "Map (a pass's state)");
        st.resize(cores);
        for (uint32_t c = 0; c < cores; c++) {
            st[c].resize(NTEX * 4);
            for (uint32_t T = 0; T < NTEX; T++)
                memcpy(&st[c][T * 4], (char*)m.pData + (size_t)((T & 7) * cores + c) * m.RowPitch + (T >> 3) * 16, 16);
        }
        if (O.console) memcpy(con, (char*)m.pData + (size_t)8 * cores * m.RowPitch, CON_TEXELS * 16);
        ctx->Unmap(snaps[i], 0);
    }
    // texel T of core c in the set the next tick reads
    void write_state(uint32_t c, uint32_t T, const uint32_t v[4]) {
        D3D11_BOX b = {T >> 3, c, 0, (T >> 3) + 1, c + 1, 1};
        ctx->UpdateSubresource(set[cur ^ 1][T & 7].t, 0, &b, v, 16, 16);
    }
    // RAM's texels [t0, t0 + n) read back
    void ram_read(uint32_t t0, uint32_t n, uint32_t* out) {
        while (n) {
            uint32_t y0 = t0 / RAM_W, y1 = std::min((t0 + n - 1) / RAM_W, y0 + RD_ROWS - 1);
            D3D11_BOX b = {0, y0, 0, RAM_W, y1 + 1, 1};
            ctx->CopySubresourceRegion(rd, 0, 0, 0, 0, ram.t, 0, &b);
            D3D11_MAPPED_SUBRESOURCE m;
            check(ctx->Map(rd, 0, D3D11_MAP_READ, 0, &m), "Map (RAM)");
            uint32_t end = std::min(t0 + n, (y1 + 1) * RAM_W);
            for (uint32_t t = t0; t < end; t++) memcpy(out + (size_t)(t - t0) * 4, (char*)m.pData + (size_t)(t / RAM_W - y0) * m.RowPitch + (t % RAM_W) * 16, 16);
            ctx->Unmap(rd, 0);
            out += (size_t)(end - t0) * 4;
            n -= end - t0;
            t0 = end;
        }
    }
    void ram_write(uint32_t t0, uint32_t n, const uint32_t* in) {
        while (n) {
            uint32_t x = t0 % RAM_W, k = std::min(n, RAM_W - x);
            D3D11_BOX b = {x, t0 / RAM_W, 0, x + k, t0 / RAM_W + 1, 1};
            ctx->UpdateSubresource(ram.t, 0, &b, in, k * 16, k * 16);
            in += (size_t)k * 4; n -= k; t0 += k;
        }
    }
};
static Gpu G;

// ---- The reference beside the card (--check)
struct RefCtl {   // a core's run of one pass: stop after k bundles; a partial last bundle (the tick ended inside it)
    uint32_t k = 0, count = 0, mode = 0, stop_slot = 0, ctl_at = 0, ctl_old = 0;
    bool stopped = false, snapped = false, patched = false, zero = false;
    gvm_core* snap = nullptr;
    struct Hist { uint32_t pc, t[4]; uint64_t n; } hist[64];
    uint64_t nhist = 0;
};
static gvm_machine R;
static std::vector<gvm_core> Rc;
static std::vector<RefCtl> Rctl;
static int ref_step(gvm_core* c) {
    RefCtl& S = *(RefCtl*)c->host;
    if (S.count >= S.k) { S.stopped = true; return 1; }
    uint32_t base = c->pc & ~15u;
    if (S.count == S.k - 1) {
        if (S.mode == 0) { memcpy(S.snap, c, sizeof *c); S.snapped = true; }
        else {   // run the ops of this bundle before the tick's stop alone, and no control word
            uint32_t from = (c->pc >> 2) & 3;
            if (S.stop_slot <= from) { S.zero = true; S.stopped = true; return 1; }
            S.ctl_at = (base >> 2) + 3; S.ctl_old = R.ram[S.ctl_at];
            R.ram[S.ctl_at] = (S.ctl_old & 0x67u) | S.stop_slot << 3 | GVM_CTRL_SPECIAL(GVM_SUB_NONE);
            S.patched = true;
        }
    }
    RefCtl::Hist& h = S.hist[S.nhist++ & 63];
    h.pc = c->pc; h.n = c->bundles_run;
    for (int k = 0; k < 4; k++) h.t[k] = base + 16 <= R.ram_end ? R.ram[(base >> 2) + k] : 0;
    if (S.patched && S.mode == 1) h.t[3] = S.ctl_old;
    S.count++;
    return 0;
}
static void ref_init(const std::vector<std::vector<uint32_t>>& state) {
    R.ram = (uint32_t*)calloc(O.ram_end / 4, 4);
    memcpy(R.ram, g_ram.data(), O.ram_end);
    R.ram_end = O.ram_end;
    R.rom = g_rom.data(); R.rom_size = g_rom_size;
    R.flags = 0;
    Rc.resize(O.ncores);
    Rctl.resize(O.ncores);
    for (uint32_t c = 0; c < O.ncores; c++) {
        gvm_core* k = &Rc[c];
        gvm_core_reset(k, c, state[c][V_PC]);
        for (int r = 0; r < 64; r++) k->x[r] = state[c][r];
        k->sr[GVM_SR_TVEC] = state[c][V_TVEC];
        k->halted = (state[c][V_FLAGS] & F_HALT) != 0;
        k->host_step = ref_step;
        k->host = &Rctl[c];
        Rctl[c].snap = (gvm_core*)malloc(sizeof(gvm_core));
    }
    R.cores = Rc.data(); R.ncores = O.ncores;
}
// the reference core against the card's state after a pass (and the words it stored), as text
static std::vector<std::string> ref_diff(uint32_t c, const std::vector<uint32_t>& s) {
    std::vector<std::string> d;
    gvm_core* k = &Rc[c];
    char b[160];
    for (int r = 1; r < 64; r++)
        if (s[r] != k->x[r]) { snprintf(b, sizeof b, "r%d: card %08x, reference %08x", r, s[r], k->x[r]); d.push_back(b); }
    if (s[V_PC] != k->pc) { snprintf(b, sizeof b, "pc: card %08x, reference %08x", s[V_PC], k->pc); d.push_back(b); }
    static const struct { int v, sr; const char* n; } srs[] = {{V_ST, GVM_SR_STATUS, "STATUS"}, {V_EPC, GVM_SR_EPC, "EPC"},
        {V_CAUSE, GVM_SR_CAUSE, "CAUSE"}, {V_TVAL, GVM_SR_TVAL, "TVAL"}, {V_TVEC, GVM_SR_TVEC, "TVEC"}, {V_SCRATCH, GVM_SR_SCRATCH, "SCRATCH"},
        {V_IEN, GVM_SR_IEN, "IEN"}, {V_TCMPLO, GVM_SR_TCMP_LO, "TCMP_LO"}, {V_TCMPHI, GVM_SR_TCMP_HI, "TCMP_HI"}};
    for (auto& e : srs)
        if (s[e.v] != k->sr[e.sr]) { snprintf(b, sizeof b, "%s: card %08x, reference %08x", e.n, s[e.v], k->sr[e.sr]); d.push_back(b); }
    uint32_t fl = (k->waiting ? F_WAIT : 0) | (k->soft ? F_SOFT : 0);
    if ((s[V_FLAGS] & (F_WAIT | F_SOFT)) != fl) { snprintf(b, sizeof b, "flags (wait, soft): card %x, reference %x", s[V_FLAGS] & 3, fl); d.push_back(b); }
    if (s[V_RND] != k->rnd) { snprintf(b, sizeof b, "RANDOM's state: card %08x, reference %08x", s[V_RND], k->rnd); d.push_back(b); }
    if (s[V_CYCLE] != (uint32_t)k->bundles_run) { snprintf(b, sizeof b, "CYCLE: card %u, reference %u", s[V_CYCLE], (uint32_t)k->bundles_run); d.push_back(b); }
    if (s[V_OPS] != (uint32_t)k->ops_run) { snprintf(b, sizeof b, "OPS: card %u, reference %u", s[V_OPS], (uint32_t)k->ops_run); d.push_back(b); }
    // the words stored in the pass: the card's write cache against the reference's write log
    std::map<uint32_t, uint32_t> cw, rw;
    for (uint32_t i = 0; i < WCN; i++) {
        uint32_t e = (T_WC + i / 2) * 4 + (i & 1) * 2;
        if (s[e] != 0xffffffffu) cw[s[e]] = s[e + 1];
    }
    for (uint32_t j = 0; j < k->wl.n; j++) { uint32_t h = k->wl.order[j]; rw[k->wl.key[h] - 1] = k->wl.val[h]; }
    int shown = 0;
    for (auto& e : cw) {
        auto it = rw.find(e.first);
        if (it == rw.end() || it->second != e.second) {
            if (shown++ < 6) {
                if (it == rw.end()) snprintf(b, sizeof b, "stored word %08x: card %08x, reference none", e.first * 4, e.second);
                else snprintf(b, sizeof b, "stored word %08x: card %08x, reference %08x", e.first * 4, e.second, it->second);
                d.push_back(b);
            }
        }
    }
    for (auto& e : rw)
        if (!cw.count(e.first) && shown++ < 6) { snprintf(b, sizeof b, "stored word %08x: card none, reference %08x", e.first * 4, e.second); d.push_back(b); }
    return d;
}
static void ref_report(uint32_t c, uint64_t pass, const std::vector<uint32_t>& prev, const std::vector<uint32_t>& s, const std::vector<std::string>& d, const std::vector<std::string>& alt) {
    out_flush();
    fprintf(stderr, "gvm_gpu --check: pass %llu (TIME %llu), core %u: the card and the reference differ\n", (unsigned long long)pass, (unsigned long long)R.time, c);
    fprintf(stderr, "  the card's pass: from pc %08x, %u bundles, ended by %s (%u), flags %x, %u operations in flight\n", prev[V_PC], s[V_LAST],
            s[V_END] < 6 ? end_name[s[V_END]] : "?", s[V_END], s[V_FLAGS], s[V_QN]);
    for (auto& x : d) fprintf(stderr, "  %s\n", x.c_str());
    if (!alt.empty()) {
        fprintf(stderr, "  (with the reference's last bundle run whole instead:)\n");
        for (auto& x : alt) fprintf(stderr, "    %s\n", x.c_str());
    }
    RefCtl& S = Rctl[c];
    fprintf(stderr, "  the reference's last bundles (bundle number, pc, words):\n");
    for (uint64_t i = S.nhist > 24 ? S.nhist - 24 : 0; i < S.nhist; i++) {
        RefCtl::Hist& h = S.hist[i & 63];
        fprintf(stderr, "    %10llu %08x  %08x %08x %08x %08x  %s\n", (unsigned long long)h.n, h.pc, h.t[0], h.t[1], h.t[2], h.t[3], dis_bundle(h.pc, h.t).c_str());
    }
    fprintf(stderr, "  the card's registers after the pass:\n");
    for (int r = 0; r < 64; r++) fprintf(stderr, "%sr%-2d %08x%s", r % 8 ? " " : "    ", r, s[r], r % 8 == 7 ? "\n" : "");
    fprintf(stderr, "  the card's state before the pass: pc %08x STATUS %08x flags %x\n", prev[V_PC], prev[V_ST], prev[V_FLAGS]);
}
// One pass of the reference: each core the bundles the card's ran, then the commit; false on a difference
static bool ref_pass(uint64_t pass, uint64_t time, const std::vector<std::vector<uint32_t>>& prev, const std::vector<std::vector<uint32_t>>& now) {
    R.time = time;
    for (uint32_t c = 0; c < O.ncores; c++) {
        const std::vector<uint32_t>& s = now[c];
        gvm_core* k = &Rc[c];
        RefCtl& S = Rctl[c];
        if (s[V_FLAGS] & F_AGAIN) continue;   // a pass to be made again: the card left the core as it found it
        uint32_t kb = (s[V_FLAGS] & F_HALT) ? 0 : s[V_LAST];
        S.k = kb; S.count = 0; S.mode = 0; S.stopped = S.snapped = S.patched = S.zero = false;
        if (kb > 0 && !k->halted) {
            gvm_run(&R, k, O.budget);
            if (S.stopped) k->halted = 0;
        }
        std::vector<std::string> d = ref_diff(c, s), alt;
        uint32_t P1 = s[V_PC];
        if (!d.empty() && S.snapped && (s[V_END] == E_SYNC || s[V_END] == E_FULL) && (P1 & 15) < 8 && ((P1 ^ S.snap->pc) & ~15u) == 0) {
            // the tick ended the pass inside its last bundle, before an op (an operation in flight held the access, or no
            // room for a store): the reference runs that bundle's ops before it alone
            alt = d;
            void* host = k->host;
            uint64_t nh = S.nhist;
            memcpy(k, S.snap, sizeof *k);
            k->host = host;
            S.nhist = nh - 1;
            S.mode = 1; S.stop_slot = (P1 >> 2) & 3; S.count = kb - 1; S.stopped = S.patched = S.zero = false;
            gvm_run(&R, k, S.snap->left);
            if (S.stopped) k->halted = 0;
            if (S.patched) R.ram[S.ctl_at] = S.ctl_old;
            if (S.zero) k->bundles_run++;   // (the tick counts a bundle it ends the pass in)
            if (S.patched || S.zero) k->pc = P1;
            d = ref_diff(c, s);
        }
        if (!d.empty()) { ref_report(c, pass, prev[c], s, d, alt); return false; }
    }
    gvm_commit(&R);
    return true;
}
static bool ref_ram_compare(uint64_t pass) {
    std::vector<uint32_t> buf((size_t)Gpu::RD_ROWS * RAM_W * 4);
    uint32_t texels = O.ram_end / 16, bad = 0;
    for (uint32_t t0 = 0; t0 < texels; t0 += Gpu::RD_ROWS * RAM_W) {
        uint32_t n = std::min(texels - t0, Gpu::RD_ROWS * RAM_W);
        G.ram_read(t0, n, buf.data());
        for (uint32_t i = 0; i < n * 4; i++)
            if (buf[i] != R.ram[(size_t)t0 * 4 + i] && bad++ < 8)
                fprintf(stderr, "gvm_gpu --check: pass %llu: RAM word %08x: card %08x, reference %08x\n", (unsigned long long)pass, (t0 * 4 + i) * 4, buf[i], R.ram[(size_t)t0 * 4 + i]);
    }
    if (bad) fprintf(stderr, "gvm_gpu --check: pass %llu: %u words of RAM differ\n", (unsigned long long)pass, bad);
    return bad == 0;
}

// ---- Guest memory between passes (every store of the pass committed): the card's RAM, the ROM's bytes
static bool readable(uint32_t a) { return a < O.ram_end || (a >= GVM_ROM && a < GVM_ROM_END); }
static bool guest_read(uint32_t a, void* dst, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    if (!n) return true;
    if (!readable(a) || !readable(a + n - 1) || a + n - 1 < a) return false;
    if (a >= GVM_ROM) {
        for (uint32_t i = 0; i < n; i++) d[i] = a + i - GVM_ROM < g_rom_size ? g_rom[a + i - GVM_ROM] : 0;
        return true;
    }
    uint32_t t0 = a >> 4, t1 = (a + n - 1) >> 4;
    std::vector<uint32_t> buf((size_t)(t1 - t0 + 1) * 4);
    G.ram_read(t0, t1 - t0 + 1, buf.data());
    memcpy(d, (uint8_t*)buf.data() + (a & 15), n);
    return true;
}
static bool guest_write(uint32_t a, const void* src, uint32_t n) {
    if (!n) return true;
    if (a < GVM_DEV || a + n - 1 >= O.ram_end || a + n - 1 < a) return false;
    uint32_t t0 = a >> 4, t1 = (a + n - 1) >> 4;
    std::vector<uint32_t> buf((size_t)(t1 - t0 + 1) * 4);
    G.ram_read(t0, t1 - t0 + 1, buf.data());
    memcpy((uint8_t*)buf.data() + (a & 15), src, n);
    G.ram_write(t0, t1 - t0 + 1, buf.data());
    if (O.check) memcpy((uint8_t*)R.ram + a, src, n);
    return true;
}

// ---- Semihosting (gvm_cpu's calls): a core in the stub with CAUSE syscall
static int g_stop = 0, g_exit = 0;
static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t len) {
    FILE* f = fd == 1 ? stdout : fd == 2 ? stderr : nullptr;
    if (!f) return -E_BADF;
    std::vector<uint8_t> tmp(len);
    if (!guest_read(buf, tmp.data(), len)) return -E_FAULT;
    fwrite(tmp.data(), 1, len, f);
    if (f == stderr) out_flush();
    return (int32_t)len;
}
static int32_t sys_call(const uint32_t* x) {
    uint32_t n = x[12], a0 = x[5], a1 = x[6], a2 = x[7];
    switch (n) {
    case 64: return sys_write(a0, a1, a2);
    case 66: {
        int32_t total = 0;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t v[8];
            if (!guest_read(a1 + 8 * i, v, 8)) return -E_FAULT;
            int32_t r = sys_write(a0, rd32(v), rd32(v + 4));
            if (r < 0) return total ? total : r;
            total += r;
        }
        return total;
    }
    case 63: {
        if (a0 != 0) return -E_BADF;
        if (!a2) return 0;
        uint8_t tmp[4096];
        fflush(stdout);
        int got = _read(0, tmp, a2 < sizeof tmp ? a2 : (unsigned)sizeof tmp);
        if (got < 0) return -E_INVAL;
        if (!guest_write(a1, tmp, (uint32_t)got)) return -E_FAULT;
        return got;
    }
    case 93: case 94: g_stop = 1; g_exit = (int)(a0 & 255); return 0;
    case 57: return 0;
    case 80: return -E_NOSYS;
    case 29: return -E_NOTTY;
    case 96: return 1;
    case 172: case 178: return 1;
    case 214:
        if (a0 >= g_heap_start && a0 <= g_mmap_top) g_brk = a0;
        return (int32_t)g_brk;
    case 222: {
        uint32_t len = (a1 + 4095) & ~4095u;
        if (!(x[8] & 0x20) || !len) return -E_INVAL;
        if (g_mmap_top - g_brk < len) return -E_NOMEM;
        g_mmap_top -= len;
        return (int32_t)g_mmap_top;
    }
    case 215: return 0;
    case 113: case 403: {
        uint8_t ts[16] = {0};
        uint64_t sec = g_time / 100000, nsec = (g_time % 100000) * 10000;
        if (n == 113) { wr32(ts, (uint32_t)sec); wr32(ts + 4, (uint32_t)nsec); }
        else { wr32(ts, (uint32_t)sec); wr32(ts + 4, (uint32_t)(sec >> 32)); wr32(ts + 8, (uint32_t)nsec); }
        return guest_write(a1, ts, n == 113 ? 8 : 16) ? 0 : -E_FAULT;
    }
    }
    return -E_NOSYS;
}
static const char* cause_name(uint32_t cause) {
    switch (cause) {
    case GVM_CAUSE_ILLEGAL: return "illegal instruction";
    case GVM_CAUSE_BREAK: return "break";
    case GVM_CAUSE_SYSCALL: return "syscall";
    case GVM_CAUSE_LOAD: return "load fault";
    case GVM_CAUSE_STORE: return "store fault";
    case GVM_CAUSE_FETCH: return "fetch fault";
    case GVM_CAUSE_PRIV: return "privileged";
    }
    return cause & GVM_CAUSE_IRQ ? "interrupt" : "unknown";
}
static void dump_core(const std::vector<uint32_t>& s) {
    uint32_t pc = s[V_EPC], t[4] = {0, 0, 0, 0};
    if ((pc & ~15u) + 16 <= O.ram_end) guest_read(pc & ~15u, t, 16);
    fprintf(stderr, "  bundle %08x: %s\n", pc & ~15u, dis_bundle(pc, t).c_str());
    for (int r = 0; r < 64; r++) fprintf(stderr, "%sr%-2d %08x%s", r % 8 ? " " : "  ", r, s[r], r % 8 == 7 ? "\n" : "");
    fprintf(stderr, "  status %08x epc %08x cause %08x tval %08x tvec %08x  bundles %u ops %u\n", s[V_ST], s[V_EPC], s[V_CAUSE],
            s[V_TVAL], s[V_TVEC], s[V_CYCLE], s[V_OPS]);
}

// ---- The console
static uint32_t g_con_printed = 0;
static int stdin_ready(uint8_t* buf, int max) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type = GetFileType(h), avail = 0;
    if (type == FILE_TYPE_CHAR) {
        int n = 0;
        while (n < max && _kbhit()) { int ch = _getch(); buf[n++] = (uint8_t)(ch == '\r' ? '\n' : ch); }
        return n;
    }
    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;
        if (!avail) return 0;
        if ((DWORD)max > avail) max = (int)avail;
    }
    int got = _read(0, buf, (unsigned)max);
    return got > 0 ? got : -1;
}
static void console_print(const uint32_t* con) {
    uint32_t written = con[CON_WRITTEN];
    if (written == g_con_printed) return;
    uint32_t n = written - g_con_printed;
    if (n > 256) { fprintf(stderr, "gvm_gpu: console: %u bytes lost\n", n - 256); g_con_printed = written - 256; }
    const uint8_t* ring = (const uint8_t*)(con + CON_RING);
    for (; g_con_printed != written; g_con_printed++) fputc(ring[g_con_printed & 255], stdout);
    fflush(stdout);
}
// input after a batch, once the guest has used the console; true when some was delivered
static bool console_input(const uint32_t* con) {
    if (!O.console_in || !(con[CON_WRITTEN] || con[CON_TAKEN]) || con[CON_TAKEN] != con[CON_DELIVERED]) return false;
    uint8_t in[4];
    int n = stdin_ready(in, 4);
    if (n < 0) { O.console_in = false; return false; }
    if (n == 0) return false;
    uint32_t w[4] = {con[0], con[1], con[2], con[3]};
    uint32_t last = 0;
    for (int k = n - 1; k >= 0; k--) last = last << 8 | in[k];
    w[CON_LAST] = last;
    w[CON_DELIVERED] += (uint32_t)n;
    G.ram_write(CON_BASE >> 4, 1, w);
    if (O.check) memcpy(R.ram + (CON_BASE >> 2), w, 16);
    return true;
}

// ---- Loading (gvm_cpu's)
static uint8_t* slurp_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "gvm_gpu: cannot open %s\n", path); exit(EXIT_USAGE); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "gvm_gpu: cannot read %s\n", path); exit(EXIT_USAGE); }
    fclose(f);
    *size = (uint32_t)n;
    return p;
}
static void place(uint32_t at, const uint8_t* src, uint32_t filesz, uint32_t memsz, const char* what) {
    if (memsz == 0) return;
    if ((uint64_t)at + memsz <= O.ram_end) {
        uint8_t* ram = (uint8_t*)g_ram.data();
        memcpy(ram + at, src, filesz);
        memset(ram + at + filesz, 0, memsz - filesz);
    } else if (at >= GVM_ROM && (uint64_t)at + memsz <= GVM_ROM_END) {
        uint32_t o = at - GVM_ROM, end = o + memsz;
        if (end > g_rom.size()) g_rom.resize(end, 0);
        memcpy(g_rom.data() + o, src, filesz);
        memset(g_rom.data() + o + filesz, 0, memsz - filesz);
        if (end > g_rom_size) g_rom_size = end;
    } else {
        fprintf(stderr, "gvm_gpu: %s at %08x (%u bytes) is outside RAM (to %08x) and the ROM\n", what, at, memsz, O.ram_end);
        exit(EXIT_USAGE);
    }
    if (at >= GVM_RAM && at + memsz <= O.ram_end && at + memsz > g_heap_start) g_heap_start = at + memsz;
}
static uint32_t load_elf(const char* path) {
    uint32_t size;
    uint8_t* p = slurp_file(path, &size);
    if (size < 52 || memcmp(p, "\177ELF", 4) || p[4] != 1 || p[5] != 1) { fprintf(stderr, "gvm_gpu: %s: not a 32-bit little-endian ELF\n", path); exit(EXIT_USAGE); }
    uint32_t mach = p[18] | p[19] << 8;
    if (mach != EM_GVM && mach != 0) { fprintf(stderr, "gvm_gpu: %s: e_machine %#x is not gpu_vm's (%#x)\n", path, mach, EM_GVM); exit(EXIT_USAGE); }
    uint32_t entry = rd32(p + 24), phoff = rd32(p + 28), phentsize = p[42] | p[43] << 8, phnum = p[44] | p[45] << 8;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t* ph = p + phoff + i * phentsize;
        if (phoff + (i + 1) * phentsize > size) { fprintf(stderr, "gvm_gpu: %s: program headers past the file's end\n", path); exit(EXIT_USAGE); }
        if (rd32(ph) != 1) continue;
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if (off + filesz > size || filesz > memsz) { fprintf(stderr, "gvm_gpu: %s: a segment past the file's end or larger than its memsz\n", path); exit(EXIT_USAGE); }
        place(vaddr, p + off, filesz, memsz, "a segment");
    }
    free(p);
    return entry;
}
static void setup_stack(std::vector<uint32_t>& x, int argc, char** argv) {
    uint32_t sp = O.ram_end - 16, ptr[256], heap = g_heap_start;
    if (argc > 250) argc = 250;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        place(sp, (const uint8_t*)argv[i], len, len, "an argument");
        ptr[i] = sp;
    }
    uint32_t words = (uint32_t)argc + 1 + 1 + 1 + 4, k = 0, v[260];
    sp = (sp - words * 4) & ~15u;
    v[k++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) v[k++] = ptr[i];
    v[k++] = 0; v[k++] = 0;
    v[k++] = 6; v[k++] = 4096;
    v[k++] = 0; v[k++] = 0;
    for (uint32_t i = 0; i < k; i++) g_ram[(sp >> 2) + i] = v[i];
    x[2] = sp; x[5] = (uint32_t)argc; x[6] = sp + 4; x[7] = sp + 4 + 4 * ((uint32_t)argc + 1);
    g_stack_limit = sp;
    g_heap_start = heap;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t fdt_memory_size(const uint8_t* f, uint32_t size) {
    if (size < 40 || be32(f) != 0xd00dfeedu) return 0;
    uint32_t st = be32(f + 8), strs = be32(f + 12), st_size = be32(f + 36), o = st, depth = 0, in_mem = 0;
    if ((uint64_t)st + st_size > size || strs >= size) return 0;
    while (o + 4 <= st + st_size) {
        uint32_t tok = be32(f + o);
        o += 4;
        if (tok == 1) {
            const char* name = (const char*)f + o;
            size_t len = 0;
            while (o + len < size && name[len]) len++;
            depth++;
            if (depth == 2 && (!strcmp(name, "memory") || !strncmp(name, "memory@", 7))) in_mem = depth;
            o += ((uint32_t)len + 4) & ~3u;
        } else if (tok == 2) {
            if (in_mem == depth) in_mem = 0;
            depth--;
        } else if (tok == 3) {
            if (o + 8 > size) return 0;
            uint32_t len = be32(f + o), nameoff = be32(f + o + 4);
            const uint8_t* v = f + o + 8;
            if (in_mem && in_mem == depth && strs + nameoff < size && !strcmp((const char*)f + strs + nameoff, "reg") && len >= 8 && o + 8 + len <= size)
                return len == 16 ? (uint64_t)be32(v + 8) << 32 | be32(v + 12) : be32(v + len - 4);
            o += 8 + ((len + 3) & ~3u);
        } else if (tok == 9) break;
    }
    return 0;
}

static void usage() {
    fprintf(stderr,
        "usage: gvm_gpu [options] PROGRAM.elf [ARGS...]\n"
        "       gvm_gpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]\n"
        "  --ram MB            RAM from 0x02000000 (default 64)\n"
        "  --rom FILE          the ROM at 0x40000000\n"
        "  --dtb FILE          a device tree at 0x1000, and a kernel's boot (r5 = 0x1000)\n"
        "  --budget N          bundles a pass, a core (default 8192)\n"
        "  --pass-time N       TIME's ticks (100 kHz) a pass (default 20: 0.2 ms)\n"
        "  --cores N           cores (default 1; the others are parked)\n"
        "  --max-bundles N     stop with exit code 3 after N bundles (at a batch's end)\n"
        "  --strace            each semihosted call on stderr\n"
        "  --stats             bundles, ops, passes, wall time and rates at the end, on stderr\n"
        "  --no-console        no console device\n"
        "  --no-stdin          the console delivers no input\n"
        "the card:\n"
        "  --shader FILE       the tick (default gpu_vm/tick/steps/x17.hlsl)\n"
        "  --warp              D3D11's WARP (on the processor) instead of the card\n"
        "  --batch N           passes at most between two readbacks (default 32)\n"
        "  --op-rounds N       the operations' rounds a pass (default 2)\n"
        "  --check             the reference beside the card: every pass compared (exit 4 on a difference)\n"
        "  --check-ram N       --check: all of RAM compared every N passes (default 4096; 0 at the end only)\n"
        "  --trace-pass        each pass on stderr: core 0's pc, bundles and why it ended\n");
    exit(EXIT_USAGE);
}
static uint32_t num(const char* s) { return (uint32_t)strtoul(s, NULL, 0); }

int main(int argc, char** argv) {
    const char *elf = nullptr, *bin = nullptr, *rom = nullptr, *dtb = nullptr;
    uint32_t at = 0, entry = 0, ram_mb = 64;
    bool have_at = false, have_entry = false;
    int i;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--strace")) O.strace = true;
        else if (!strcmp(a, "--stats")) O.stats = true;
        else if (!strcmp(a, "--no-console")) O.console = false;
        else if (!strcmp(a, "--no-stdin")) O.console_in = false;
        else if (!strcmp(a, "--check")) O.check = true;
        else if (!strcmp(a, "--trace-pass")) O.trace_pass = true;
        else if (!strcmp(a, "--warp")) O.warp = true;
        else if (!strcmp(a, "--ram") && more) ram_mb = num(argv[++i]);
        else if (!strcmp(a, "--rom") && more) rom = argv[++i];
        else if (!strcmp(a, "--dtb") && more) dtb = argv[++i];
        else if (!strcmp(a, "--bin") && more) bin = argv[++i];
        else if (!strcmp(a, "--at") && more) { at = num(argv[++i]); have_at = true; }
        else if (!strcmp(a, "--entry") && more) { entry = num(argv[++i]); have_entry = true; }
        else if (!strcmp(a, "--budget") && more) O.budget = num(argv[++i]);
        else if (!strcmp(a, "--pass-time") && more) O.pass_time = num(argv[++i]);
        else if (!strcmp(a, "--cores") && more) O.ncores = num(argv[++i]);
        else if (!strcmp(a, "--max-bundles") && more) O.max_bundles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--shader") && more) O.shader = argv[++i];
        else if (!strcmp(a, "--batch") && more) O.batch = num(argv[++i]);
        else if (!strcmp(a, "--op-rounds") && more) O.rounds = num(argv[++i]);
        else if (!strcmp(a, "--check-ram") && more) O.check_ram = num(argv[++i]);
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "gvm_gpu: unknown option %s\n", a); usage(); }
        else break;
    }
    if (!bin) { if (i >= argc) usage(); elf = argv[i]; }
    if (bin && !have_at) { fprintf(stderr, "gvm_gpu: --bin needs --at\n"); usage(); }
    if (O.ncores < 1 || O.ncores > 512 || !O.budget || ram_mb < 1 || !O.batch) usage();
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_exe_dir = exe;
    g_exe_dir = g_exe_dir.substr(0, g_exe_dir.find_last_of("\\/"));
    if (O.shader.empty()) O.shader = g_exe_dir + "\\..\\gpu_vm\\tick\\steps\\x17.hlsl";

    // the machine: RAM is address 0 up to GVM_RAM + its size (a multiple of the RAM texture's rows: 128 KB)
    if ((uint64_t)GVM_RAM + (uint64_t)ram_mb * (1u << 20) > 0x20000000u) { fprintf(stderr, "gvm_gpu: --ram %u: RAM ends past 0x20000000\n", ram_mb); return EXIT_USAGE; }
    O.ram_end = GVM_RAM + ram_mb * (1u << 20);
    g_ram.assign(O.ram_end / 4, 0);
    memcpy(g_ram.data(), STUB, sizeof STUB);
    g_heap_start = GVM_RAM;
    if (rom) {
        uint32_t n;
        uint8_t* p = slurp_file(rom, &n);
        if (n > GVM_ROM_END - GVM_ROM) { fprintf(stderr, "gvm_gpu: the ROM is larger than 512 MB\n"); return EXIT_USAGE; }
        place(GVM_ROM, p, n, n, "the ROM");
        free(p);
    }
    if (dtb) {
        uint32_t n;
        uint8_t* p = slurp_file(dtb, &n);
        if (GVM_DTB + n > GVM_DEV) { fprintf(stderr, "gvm_gpu: the device tree does not fit below 0x10000\n"); return EXIT_USAGE; }
        uint64_t mem = fdt_memory_size(p, n);
        if (!mem) fprintf(stderr, "gvm_gpu: %s: no memory node found\n", dtb);
        else if (mem != (uint64_t)ram_mb << 20) fprintf(stderr, "gvm_gpu: %s: the memory node says %llu MB, --ram %u\n", dtb, (unsigned long long)(mem >> 20), ram_mb);
        place(GVM_DTB, p, n, n, "the device tree");
        free(p);
    }
    g_heap_start = GVM_RAM;
    if (elf) entry = load_elf(elf);
    else {
        uint32_t n;
        uint8_t* p = slurp_file(bin, &n);
        place(at, p, n, n, "the image");
        free(p);
        if (!have_entry) entry = at;
    }
    if (elf && have_entry) fprintf(stderr, "gvm_gpu: --entry overrides the ELF's\n");

    // the tick's layout, from its source
    std::string src = slurp(O.shader);
    if (src.empty()) { fprintf(stderr, "gvm_gpu: cannot read the tick %s\n", O.shader.c_str()); return EXIT_USAGE; }
    WCN = step_const(src, "WCN", 130); RAM_W = step_const(src, "RAM_W", 8192); T_WC = step_const(src, "T_WC", 40);
    OP_SLOTS = step_const(src, "OP_SLOTS", 32); OP_SPAN = step_const(src, "OP_SPAN", 272);
    NTEX = T_WC + WCN / 2; SPIX = (NTEX + 7) / 8;
    if (RAM_W != 8192 || (O.ram_end / 16) % RAM_W) { fprintf(stderr, "gvm_gpu: RAM %08x is not whole rows of %u texels\n", O.ram_end, RAM_W); return EXIT_USAGE; }

    // the cores' state (STATE.md, "A new core"): core 0 at the entry, the others parked (halted)
    std::vector<std::vector<uint32_t>> state(O.ncores, std::vector<uint32_t>(NTEX * 4, 0));
    for (uint32_t c = 0; c < O.ncores; c++) {
        std::vector<uint32_t>& s = state[c];
        for (uint32_t e = 0; e < WCN; e++) s[(T_WC + e / 2) * 4 + (e & 1) * 2] = 0xffffffffu;
        s[V_PC] = entry;
        s[V_ST] = GVM_ST_S;
        s[V_RND] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
        if (c) s[V_FLAGS] = F_HALT;   // TODO(workers): parked until a mailbox gives them a job
    }
    if (dtb) { state[0][5] = GVM_DTB; g_stack_limit = O.ram_end; }
    else setup_stack(state[0], argc - i, argv + i);
    g_heap_start = (g_heap_start + 15) & ~15u;
    g_brk = g_heap_start;
    g_mmap_top = (g_stack_limit - (1u << 20)) & ~4095u;
    if (g_mmap_top < g_heap_start) { fprintf(stderr, "gvm_gpu: no room for the stack\n"); return EXIT_USAGE; }
    if (O.check) ref_init(state);

    // the card
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, O.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    G.cores = O.ncores;
    G.tick = program(O.shader, "vert", "frag");
    G.commit = program(O.shader, "commit_vert", "commit_frag");
    G.stg = program(O.shader, "op_stage_vert", "op_stage_frag");
    G.wrt = program(O.shader, "op_write_vert", "op_write_frag");
    G.prog = program(O.shader, "op_prog_vert", "op_prog_frag");
    G.make(state);
    g_ram.clear(); g_ram.shrink_to_fit();   // (the card's from here on)

    // the passes, in batches: up to --batch passes, each read back into a snapshot, then the host's work
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&f0);
    std::vector<std::vector<std::vector<uint32_t>>> views(O.batch);
    std::vector<std::vector<uint32_t>> cons(O.batch, std::vector<uint32_t>(CON_TEXELS * 4, 0));
    std::vector<uint64_t> times(O.batch);
    std::vector<std::vector<uint32_t>> prev = state;
    std::vector<uint32_t> last_st(O.ncores, GVM_ST_S), last_epc(O.ncores), last_cause(O.ncores), last_tval(O.ncores);
    uint64_t passes = 0, bundles = 0, ops = 0, readbacks = 0, again = 0;
    uint32_t K = 1;
    int code = -1;
    while (code < 0) {
        for (uint32_t p = 0; p < K; p++) {
            G.pass.timeLo = (uint32_t)g_time; G.pass.timeHi = (uint32_t)(g_time >> 32);
            times[p] = g_time;
            G.run_pass();
            G.snapshot(p);
            g_time += O.pass_time;
        }
        readbacks++;
        bool event = false;
        for (uint32_t p = 0; p < K && code < 0; p++) {
            G.read_snapshot(p, views[p], cons[p].data());
            const std::vector<std::vector<uint32_t>>& now = views[p];
            passes++;
            for (uint32_t c = 0; c < O.ncores; c++) {
                const std::vector<uint32_t>& s = now[c];
                if (s[V_FLAGS] & F_AGAIN) { again++; continue; }
                bundles += s[V_LAST];
                ops += (uint32_t)(s[V_OPS] - prev[c][V_OPS]);
                if (s[V_PC] >= 16) { last_st[c] = s[V_ST]; last_epc[c] = s[V_EPC]; last_cause[c] = s[V_CAUSE]; last_tval[c] = s[V_TVAL]; }
            }
            if (O.trace_pass) {
                const std::vector<uint32_t>& s = now[0];
                fprintf(stderr, "[pass %llu] TIME %llu pc %08x bundles %u end %s flags %x ops-in-flight %u\n", (unsigned long long)passes,
                        (unsigned long long)times[p], s[V_PC], s[V_FLAGS] & F_AGAIN ? 0 : s[V_LAST], s[V_FLAGS] & F_AGAIN ? "again" : s[V_END] < 6 ? end_name[s[V_END]] : "?", s[V_FLAGS], s[V_QN]);
            }
            if (O.console) console_print(cons[p].data());
            if (O.check) {
                if (!ref_pass(passes, times[p], prev, now)) { code = EXIT_CHECK; break; }
                bool idle = true;
                for (uint32_t c = 0; c < O.ncores; c++) idle = idle && now[c][V_QN] == 0;
                if (O.check_ram && passes % O.check_ram == 0 && idle && !ref_ram_compare(passes)) { code = EXIT_CHECK; break; }
            }
            prev = now;
        }
        if (code >= 0) break;
        const std::vector<std::vector<uint32_t>>& last = views[K - 1];
        // cores in the stub: a trap while TVEC was 0 (a semihosted call, or the report)
        bool all_halted = true, all_waiting = true;
        for (uint32_t c = 0; c < O.ncores && !g_stop; c++) {
            std::vector<uint32_t> s = last[c];
            if (s[V_FLAGS] & F_HALT) continue;
            all_halted = false;
            if (!(s[V_FLAGS] & F_WAIT)) all_waiting = false;
            if (s[V_PC] >= 16 || s[V_TVEC] >= 16) continue;
            event = true;
            if (s[V_CAUSE] != GVM_CAUSE_SYSCALL) {
                out_flush();
                fprintf(stderr, "gvm_gpu: core %u: %s (cause %#x) at pc %08x, tval %08x, with no handler (TVEC 0)\n", c, cause_name(s[V_CAUSE]), s[V_CAUSE], s[V_EPC], s[V_TVAL]);
                dump_core(s);
                code = EXIT_TRAP;
                break;
            }
            uint32_t a[3] = {s[5], s[6], s[7]}, n = s[12];
            int32_t r = sys_call(s.data());
            if (O.strace) { out_flush(); fprintf(stderr, "[%u] syscall %u(%#x, %#x, %#x) = %d\n", c, n, a[0], a[1], a[2], r); }
            if (g_stop) break;
            // on after the op, as if nothing had trapped (gvm_cpu's host_trap): r5 the result, the SRs as they were
            s[5] = (uint32_t)r;
            s[V_PC] = s[V_EPC] + 4;
            s[V_ST] = last_st[c]; s[V_EPC] = last_epc[c]; s[V_CAUSE] = last_cause[c]; s[V_TVAL] = last_tval[c];
            G.write_state(c, 1, &s[4]);
            G.write_state(c, 16, &s[64]);
            G.write_state(c, 17, &s[68]);
            if (O.check) {
                gvm_core* k = &Rc[c];
                k->x[5] = s[5]; k->pc = s[V_PC];
                k->sr[GVM_SR_STATUS] = s[V_ST]; k->sr[GVM_SR_EPC] = s[V_EPC]; k->sr[GVM_SR_CAUSE] = s[V_CAUSE]; k->sr[GVM_SR_TVAL] = s[V_TVAL];
            }
            views[K - 1][c] = s;
            prev[c] = s;
        }
        if (code >= 0) break;
        if (g_stop) { code = g_exit; break; }
        if (all_halted) { out_flush(); fprintf(stderr, "gvm_gpu: every core halted\n"); code = EXIT_TRAP; break; }
        if (O.console && console_input(cons[K - 1].data())) event = true;
        if (O.max_bundles && bundles >= O.max_bundles) {
            out_flush();
            fprintf(stderr, "gvm_gpu: %llu bundles run: stopped (--max-bundles), pc %08x\n", (unsigned long long)bundles, last[0][V_PC]);
            code = EXIT_LIMIT;
            break;
        }
        if (all_waiting) {   // every core in a supervisor's wait: on to the earliest timer that can wake one
            uint64_t next = 0;
            for (uint32_t c = 0; c < O.ncores; c++) {
                const std::vector<uint32_t>& s = last[c];
                if (s[V_FLAGS] & F_HALT) continue;
                uint64_t cmp = (uint64_t)s[V_TCMPHI] << 32 | s[V_TCMPLO];
                if ((s[V_IEN] & (1u << GVM_IRQ_SOFT)) && (s[V_FLAGS] & F_SOFT)) { next = g_time; break; }
                if ((s[V_IEN] & 1u << GVM_IRQ_TIMER) && cmp && (!next || cmp < next)) next = cmp;
            }
            if (!next) {
                out_flush();
                fprintf(stderr, "gvm_gpu: every core waits for an interrupt that cannot come\n");
                dump_core(last[0]);
                code = EXIT_TRAP;
                break;
            }
            if (next > g_time) g_time = next;
        }
        // the next batch: one pass after anything the host made (a call's answer, input), else twice the last, to --batch
        K = event ? 1 : std::min(O.batch, K * 2);
    }
    if (O.check && code != EXIT_CHECK && O.check_ram != ~0u) {
        bool idle = true;
        for (uint32_t c = 0; c < O.ncores; c++) idle = idle && prev[c][V_QN] == 0;
        if (idle && !ref_ram_compare(passes)) code = EXIT_CHECK;
        else if (code != EXIT_CHECK) fprintf(stderr, "gvm_gpu --check: %llu passes, the card and the reference the same%s\n", (unsigned long long)passes, idle ? " (RAM too)" : "");
    }
    QueryPerformanceCounter(&f1);
    out_flush();
    if (O.stats) {
        double wall = double(f1.QuadPart - f0.QuadPart) / fq.QuadPart;
        fprintf(stderr, "gvm_gpu: %llu bundles, %llu ops (%.2f a bundle), %llu passes (%llu made again), %llu readbacks, time %llu (100 kHz)\n",
                (unsigned long long)bundles, (unsigned long long)ops, bundles ? (double)ops / (double)bundles : 0.0, (unsigned long long)passes,
                (unsigned long long)again, (unsigned long long)readbacks, (unsigned long long)g_time);
        fprintf(stderr, "gvm_gpu: %.3f s on %s: %.2f M bundles/s, %.0f passes/s\n", wall, O.warp ? "WARP" : "the card", bundles / wall / 1e6, passes / wall);
    }
    return code;
}
