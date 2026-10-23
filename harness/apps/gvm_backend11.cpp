// gpu_vm's machine in rvc_harness (gpu_vm/platform.md, "The harness"): `rvc_harness --gvm DIR`. The tick is a gpu_vm
// step (gpu_vm/tick/steps/x18.hlsl by default; its contract gpu_vm/tick/STATE.md: the cores' state a row a core in eight
// textures, RAM 8192 texels wide from address 0, the ROM a texture of its own, the commit's four draws and the
// operations' rounds, as gvm_gpu draws them), the devices the RISC-V machine's (gpu_vm/platform/devices.shader over
// experiments/rvc_opt/src/gpu.h with GVM_RAM), and the host rvc_harness.cpp as it is: what it reads back is the RISC-V
// machine's row (core 0's texels made from the gpu_vm core's, the control words, the network's row), its devices'
// uniforms are the material's. The worker cores (gpu_vm/platform.md, "Workers") are rows started at the firmware in the
// boot area when the kernel lays them out (the control words' geometry).
#include "image.h"
#include "memview.h"
#include "readback.h"
#include "rvc_backend.h"

#include "../../gpu_vm/isa.h"
#include "../../gpu_vm/platform/worker_fw.h"

#include <algorithm>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

const UINT kRamW = 8192;                 // RAM's texture's width (STATE.md)
const UINT kMaxCores = 256;              // core 0 and 255 workers
const uint32_t kIndexBias = 0x600000u;   // a RISC-V RAM index less this is gpu_vm's texel (the window moved down)
// the tick's state (STATE.md): flags (18.z)
const uint32_t F_HALT = 4;

struct PassParams { uint32_t ticks, ramEnd, timeLo, timeHi, player, flags, ramRows, ext0, ncores, pad[3], ext[8]; };

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT w = 0, h = 0;
};

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
// a step's constant: `static const uint NAME = V;` or `#define NAME V` (gvm_gpu's)
uint32_t stepConst(const std::string& src, const char* name, uint32_t dflt) {
    std::string n = name;
    for (const std::string& form : {"#define " + n + " ", "uint " + n + " = "}) {
        size_t at = src.find(form);
        if (at != std::string::npos) return (uint32_t)atoi(src.c_str() + at + form.size());
    }
    return dflt;
}

class GvmBackend : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader&, Material& mat, std::string& err) override {
        if (!gpu_.init(opt.gpu, err)) return false;
        fprintf(stderr, "[harness] gpu_vm on %s%s, %s (gpu_vm/platform.md)\n", gpu_.adapterName.c_str(), opt.gpu.warp ? " (WARP)" : "",
                shaderCompilerName());
        ID3D11Device* dev = gpu_.device.Get();
        // ---- the image: vmlinux, gvm.dtb, rootfs.romfs (gpu_vm/userland/romroot.sh, apps/image.sh)
        fs::path dir = fs::u8path(opt.gvmDir);
        uint32_t ramMB = opt.gvmRamMB;
        if (!ramMB) {
            std::vector<uint8_t> m;
            ramMB = readFile(dir / "ram_mb", m) ? (uint32_t)atoi(std::string(m.begin(), m.end()).c_str()) : 0;
            if (!ramMB) ramMB = 192;
        }
        ramEnd_ = GVM_RAM + (ramMB << 20);
        rows_ = ramEnd_ / 16 / kRamW;
        std::vector<uint8_t> ram((size_t)ramEnd_, 0), elf, dtb, rom;
        if (!readFile(dir / "vmlinux", elf) || elf.size() < 52 || memcmp(elf.data(), "\177ELF", 4) != 0) {
            err = "--gvm " + opt.gvmDir + ": no vmlinux (an ELF) there";
            return false;
        }
        uint32_t entry = rd32(&elf[24]), phoff = rd32(&elf[28]), phentsize = elf[42] | elf[43] << 8, phnum = elf[44] | elf[45] << 8;
        for (uint32_t i = 0; i < phnum; i++) {
            const uint8_t* ph = &elf[phoff + i * phentsize];
            if (rd32(ph) != 1) continue;   // PT_LOAD
            uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
            if ((uint64_t)vaddr + memsz > ramEnd_ || (size_t)off + filesz > elf.size()) {
                err = "vmlinux: a segment outside RAM";
                return false;
            }
            memcpy(&ram[vaddr], &elf[off], filesz);
        }
        if (readFile(dir / "gvm.dtb", dtb)) {
            if (GVM_DTB + dtb.size() > GVM_DEV) { err = "gvm.dtb does not fit below 0x10000"; return false; }
            memcpy(&ram[GVM_DTB], dtb.data(), dtb.size());
        } else fprintf(stderr, "[harness] --gvm: no gvm.dtb in %s\n", opt.gvmDir.c_str());
        // the boot area: { sync ; j 0 } at 0, where a trap with no handler lands (gvm_gpu's stub), and the worker firmware
        const uint32_t stub[4] = {GVM_X_SYNC << 12, 0, 0, 1u << 3 | GVM_CTRL_J};
        memcpy(&ram[0], stub, sizeof stub);
        memcpy(&ram[GVM_WORKER_FW], gvm_worker_fw, sizeof gvm_worker_fw);
        if (!readFile(dir / "rootfs.romfs", rom)) fprintf(stderr, "[harness] --gvm: no rootfs.romfs in %s: no ROM\n", opt.gvmDir.c_str());
        romRows_ = (std::max)(1u, (uint32_t)((rom.size() + 16 * kRamW - 1) / (16 * kRamW)));
        rom.resize((size_t)romRows_ * kRamW * 16, 0);
        fprintf(stderr, "[harness] gpu_vm: %s, entry %08x, %u MB of RAM, %.1f MB of ROM\n", opt.gvmDir.c_str(), entry, ramMB, rom.size() / 1048576.0);

        // ---- the tick (the ISA agent's step: --gvm-tick)
        std::string tickPath = opt.gvmTick.empty() ? "gpu_vm/tick/steps/x18.hlsl" : opt.gvmTick, src;
        if (!readFileBinary(tickPath, src)) { err = "--gvm-tick: cannot read " + tickPath; return false; }
        wcn_ = stepConst(src, "WCN", 514);
        tWc_ = stepConst(src, "T_WC", 40);
        opSlots_ = stepConst(src, "OP_SLOTS", 32);
        opSpan_ = stepConst(src, "OP_SPAN", 272);
        if (stepConst(src, "RAM_W", 8192) != kRamW) { err = tickPath + ": RAM is not 8192 texels wide"; return false; }
        ntex_ = stepConst(src, "NTEX", tWc_ + wcn_ / 2);   // (a step whose entries are not two a texel says)
        spix_ = (ntex_ + 7) / 8;
        geo_ = stepConst(src, "GEO_BLOCKS", 0);
        if (geo_ && spix_ > 48) { err = tickPath + ": GEO_BLOCKS takes 48 state pixels a core at most"; return false; }
        if (src.find("#ifndef CORE0") != std::string::npos) {   // x25: core 0's own write cache, its tick compiled with CORE0=1
            wcn0_ = stepConst(src, "WCN0", 0);
            spix0_ = (stepConst(src, "NTEX0", 0) + 7) / 8;
            if (!geo_ || !wcn0_ || spix0_ > 64 + 8 * 128) { err = tickPath + ": CORE0 needs GEO_BLOCKS, WCN0 and NTEX0"; return false; }
        }
        CompileSettings cs;
        cs.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        cs.cacheDir = opt.compile.cacheDir;
        std::string rootDir = fs::u8path(tickPath).parent_path().u8string();
        auto prog = [&](const char* v, const char* p, Prog& out, const Defines& defs = {}) {
            StageResult vs = compileStage(src, tickPath, rootDir, v, "vs_5_0", defs, cs);
            StageResult ps = compileStage(src, tickPath, rootDir, p, "ps_5_0", defs, cs);
            if (!vs.ok || !ps.ok) { err = tickPath + ": " + vs.log + ps.log; return false; }
            return SUCCEEDED(dev->CreateVertexShader(vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize(), nullptr, &out.vs)) &&
                   SUCCEEDED(dev->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &out.ps));
        };
        if (!prog("vert", "frag", tick_) || !prog("commit_vert", "commit_frag", commit_) || !prog("op_stage_vert", "op_stage_frag", stage_) ||
            !prog("op_write_vert", "op_write_frag", write_) || !prog("op_prog_vert", "op_prog_frag", progress_)) {
            if (err.empty()) err = "creating the tick's shaders failed";
            return false;
        }
        if (wcn0_ && !prog("vert", "frag", tick0_, {{"CORE0", "1"}})) {
            if (err.empty()) err = "creating core 0's tick failed";
            return false;
        }
        fprintf(stderr, "[harness] gpu_vm: tick %s (write cache %u, %u state texels a core; core 0's %u)\n", tickPath.c_str(), wcn_, ntex_,
                wcn0_ ? wcn0_ : wcn_);

        // ---- the devices (gpu_vm/platform/devices.shader)
        devices_ = opt.gpuShader != nullptr;
        if (devices_) {
            SLShader ds;
            if (!loadShaderLab("gpu_vm/platform/devices.shader", ds, err)) return false;
            ShaderBuildOptions bo;
            bo.verbose = opt.verbose;
            bo.settings.flags = opt.compile.flags;
            bo.settings.cacheDir = opt.compile.cacheDir;
            bo.settings.includeDirs = opt.compile.includeDirs;
            for (auto& d : opt.compile.defines)   // Unity's own, not the RISC-V machine's
                if (d.first == "SHADER_API_D3D11" || d.first == "SHADER_TARGET" || d.first == "UNITY_COMPILER_HLSL" || d.first == "UNITY_VERSION")
                    bo.settings.defines.push_back(d);
            std::vector<std::string> names = {"GPUDraw", "GPUControl", "GPUWriteback"};
            if (opt.soundShader) names.push_back("SoundMix");
            if (!buildPasses(gpu_, ds, names, bo, devPasses_, err)) return false;
            if (!createGpuTarget(err)) return false;
            if (opt.soundShader && !createSoundTarget(err)) return false;
        }

        // ---- the textures (STATE.md)
        if (!makeTex(ram_, kRamW, rows_, ram.data(), err) || !makeTex(rom_, kRamW, romRows_, rom.data(), err) ||
            !makeTex(stageTex_, opSpan_, kMaxCores * opSlots_, nullptr, err) || !makeTex(ops_[0], kMaxCores, 1, nullptr, err) ||
            !makeTex(ops_[1], kMaxCores, 1, nullptr, err) || !makeTex(zone_, kRamW, 3, nullptr, err) || !makeTex(ctl_, 256, 1, nullptr, err) ||
            !makeTex(info_, 8, 1, nullptr, err))
            return false;
        {
            std::vector<uint32_t> none((size_t)spix_ * 8 * 4, 0);
            for (size_t i = 0; i < none.size(); i += 2) none[i] = 0xffffffffu;   // (an empty write cache entry: word 0xffffffff)
            if (!makeTex(none_, geo_ ? 8 : spix_, geo_ ? 8 : 1, none.data(), err)) return false;
        }
        std::vector<uint32_t> state((size_t)ntex_ * 4, 0);
        state[16 * 4 + 0] = entry;          // pc
        state[16 * 4 + 1] = GVM_ST_S;       // supervisor
        state[18 * 4 + 3] = 0x9e3779b9u;    // RANDOM's state (core 0)
        state[1 * 4 + 1] = GVM_DTB;         // r5: the device tree
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) {
                std::vector<uint32_t> img((size_t)setW() * setH() * 4, 0);
                for (uint32_t tx = t; tx < ntex_; tx += 8) memcpy(&img[setAt(0, tx / 8) * 4], &state[tx * 4], 16);
                if (!makeTex(set_[k][t], setW(), setH(), img.data(), err)) return false;
            }
        D3D11_BUFFER_DESC bd = {sizeof(PassParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) { err = "a constant buffer"; return false; }
        bd.ByteWidth = 16;
        for (uint32_t c = 0; c < 4; c++) {
            D3D11_BLEND_DESC b = {};
            b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            b.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1 << c);
            if (FAILED(dev->CreateBlendState(&b, &mask_[c])) || FAILED(dev->CreateBuffer(&bd, nullptr, &ccb_[c]))) { err = "commit states"; return false; }
        }
        pass_ = {opt.gvmBudget ? opt.gvmBudget : 8192u, ramEnd_, 0, 0, 0, 0, rows_, 0, 1, {0, 0, 0}, {0}};
        const UINT controlTexels = kControlTexels;
        if (!infoRb_.init(dev, DXGI_FORMAT_R32G32B32A32_UINT, 16, 8, 1, 3, err) ||
            !mboxRb_.init(dev, DXGI_FORMAT_R32G32B32A32_UINT, 16, 0x800, 1, 3, err) ||
            !controlRb_.init(dev, DXGI_FORMAT_R32G32B32A32_UINT, 16, controlTexels, 1, 3, err) ||
            !netRb_.init(dev, DXGI_FORMAT_R32G32B32A32_UINT, 16, kNetRead, 1, 3, err))
            return false;
        mat.setInt("_GvmParkLo", GVM_FW_PARK);
        mat.setInt("_GvmParkHi", GVM_FW_PARK_END);
        return true;
    }

    bool loadPayload(Material&, const std::string&, const std::string&, const std::string&, std::string&) override { return true; }
    bool setState(const void* texels, std::string& err) override {
        if (!texels) return true;
        err = "gpu_vm has no snapshots (--load-state)";
        return false;
    }
    bool deliverHostData(Material& mat, const uint8_t* rgba, std::string& err) override {
        ImageRGBA8 img;
        img.width = img.height = kFetchSide;
        img.pixels.assign(rgba, rgba + (size_t)kFetchSide * kFetchSide * 4);
        hostData_ = createTextureRGBA8(gpu_.device.Get(), img, /*flipY=*/false, err);
        if (!hostData_) return false;
        mat.setTexture("_HostData", hostData_.Get(), kFetchSide, kFetchSide);
        deliver_ = true;
        return true;
    }
    bool deliverNet(Material& mat, const uint8_t* rgba, std::string& err) override {
        ImageRGBA8 img;
        img.width = kNetSlot * 4;
        img.height = kNetMost;
        img.pixels.assign(rgba, rgba + (size_t)img.width * img.height * 4);
        netData_ = createTextureRGBA8(gpu_.device.Get(), img, /*flipY=*/false, err);
        if (!netData_) return false;
        mat.setTexture("_NetData", netData_.Get(), img.width, img.height);
        deliverNet_ = true;
        return true;
    }

    bool frame(Material& mat, uint64_t tag, bool timeIt) override {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        timeIt = timeIt && ensureQueries() && !tsPending_;
        if (timeIt) {
            ctx->Begin(tsDisjoint_.Get());
            ctx->End(tsQuery_[0].Get());
        }
        // TIME (100 kHz) from the host's clock (rvc_harness's _Time is t / 20, t, 2t, 3t), a tick more each pass at least
        uint64_t wall = (uint64_t)(mat.getFloat("_Time") * 20.0 * 100000.0);
        uint32_t rounds = (std::max)(1u, tickRounds);
        for (uint32_t r = 0; r < rounds; r++) {
            static const uint64_t dt = getenv("GVM_DT_TICKS") ? strtoull(getenv("GVM_DT_TICKS"), nullptr, 10) : 0;
            time_ = dt ? time_ + dt : (std::max)(time_ + 1, wall);   // (GVM_DT_TICKS: TIME this much a pass, as in a world of a pass a frame)
            runPass(timeIt && r == rounds - 1);
        }
        if (devices_) {
            // the control words as the cores left them, for the writeback; then the copy the last frame's list asked for
            D3D11_BOX cbx = {0, (GVM_CONTROL >> 4) / kRamW, 0, 256, (GVM_CONTROL >> 4) / kRamW + 1, 1};
            ctx->CopySubresourceRegion(ctl_.t.Get(), 0, 0, 0, 0, ram_.t.Get(), 0, &cbx);
            writeback(mat);
            gpuDraw(mat);
            if (soundMix && devPasses_.size() > 3) soundDraw(mat, tag);   // (before the control zone moves the voices on)
            mat.setInt("_GvmPasses", (int64_t)(uint32_t)passes_);
            mat.setInt("_GvmWorkers", workers_);
            mat.setInt("_GvmGeo", geo_);
            mat.setTexture("_GvmS0", set_[cur_ ^ 1][0].srv.Get(), setW(), setH());
            mat.setTexture("_GvmS3", set_[cur_ ^ 1][3].srv.Get(), setW(), setH());
            bool merge = countsAdded;
            if (merge) {
                zone(mat, 0x6c1000u, 256);   // COUNT_TOTAL (src/gpu.h)
                countsAdded = false;
            }
            mat.setInt("_CountMerged", merge ? 2 : 1);
            zone(mat, 0x700000u, kControlTexels);
            if (conDelivered) {
                zone(mat, 0x76b828u, 1);     // CON_HOST
                conDelivered = false;
            }
            if (workers_) zone(mat, 0x76ba00u, 72);   // MC_STATS_ALL
            if (deliver_) {
                zone(mat, 0x76c000u, 0x4000);   // FETCH_DATA
                deliver_ = false;
            }
            if (deliverNet_) {
                zone(mat, 0x76b840u, 40 * 8);   // NET_RX
                deliverNet_ = false;
            }
        }
        if (timeIt) {
            ctx->End(tsQuery_[3].Get());
            ctx->End(tsDisjoint_.Get());
            tsPending_ = true;
        }
        // what the host reads: core 0's texels 16-23 (the set just drawn), the control words, the network's row
        for (UINT t = 0; t < 8; t++) {
            UINT x = geo_ ? 0 : 2, y = geo_ ? 2 : 0;
            D3D11_BOX b = {x, y, 0, x + 1, y + 1, 1};
            ctx->CopySubresourceRegion(info_.t.Get(), 0, t, 0, 0, set_[cur_ ^ 1][t].t.Get(), 0, &b);
        }
        infoRb_.request(ctx, info_.t.Get(), 0, 0, tag);
        mboxRb_.request(ctx, ram_.t.Get(), 0, (GVM_MBOX >> 4) / kRamW, tag);
        uint32_t ct = 0x100000u, nt = 0x16b800u;
        controlRb_.request(ctx, ram_.t.Get(), ct % kRamW, ct / kRamW, tag);
        netRb_.request(ctx, ram_.t.Get(), nt % kRamW, nt / kRamW, tag);
        ctx->Flush();
        return true;
    }
    bool rowFull() const override { return infoRb_.full() || controlRb_.full() || netRb_.full() || mboxRb_.full(); }
    size_t rowPending() const override { return infoRb_.pending(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override {
        std::vector<uint8_t> info, control, net, mbox;
        uint64_t t1, t2, t4;
        if (!infoRb_.pop(gpu_.ctx.Get(), info, tag) || !controlRb_.pop(gpu_.ctx.Get(), control, t1) || !netRb_.pop(gpu_.ctx.Get(), net, t2) ||
            !mboxRb_.pop(gpu_.ctx.Get(), mbox, t4))
            return false;
        // the RISC-V machine's row 0 as rvc_harness reads it: the instructions (core 0's ops), the commits (passes), the
        // pc; no UART (texel 11's last word 0xffffffff): the console is the network row's words, which both machines have
        out.assign((size_t)(64 + kControlTexels + kNetRead + kFieldRead) * 16, 0);
        uint32_t* w = (uint32_t*)out.data();
        const uint32_t* s = (const uint32_t*)info.data();   // texels 16 to 23 of core 0
        w[28 * 4 + 1] = s[3 * 4 + 1];        // OPS (texel 19.y)
        w[28 * 4 + 2] = (uint32_t)passesDone_;
        w[36 * 4 + 3] = s[0 * 4 + 0];        // pc
        // why its last pass ended (texel 19.w), as the RISC-V machine's last stall, and above bit 8 the bundles it ran
        // (texel 19.z), which --frame-log keeps (gpu_vm/bench_games)
        w[40 * 4 + 2] = (s[3 * 4 + 3] & 0xffu) | (s[3 * 4 + 2] << 8);
        w[11 * 4 + 3] = 0xffffffffu;
        memcpy(out.data() + 64 * 16, control.data(), (size_t)kControlTexels * 16);
        memcpy(out.data() + (64 + kControlTexels) * 16, net.data(), (size_t)kNetRead * 16);
        passesDone_ = passes_;
        // the mix drawn in that frame, if it drew one (rvc_backend11's)
        lastSound_.clear();
        if (!soundTags_.empty() && soundTags_.front() == tag) {
            soundTags_.pop_front();
            uint64_t t3;
            if (!sound_.pop(gpu_.ctx.Get(), lastSound_, t3)) lastSound_.clear();
        }
        // the workers the kernel has laid out (the geometry's texel 0x3d: shaping, how many, their size)
        const uint32_t* geo = (const uint32_t*)control.data() + 0x3d * 4;
        uint32_t want = (geo[0] & 1) || geo[2] < 3 || geo[2] > 6 ? 0 : (std::min)(geo[1], kMaxCores - 1);
        if (want != workers_) setWorkers(want);
        if (workers_) freeze((const uint32_t*)mbox.data(), (const uint32_t*)net.data() + (kCoreStatsAt + 64) * 4);
        return true;
    }
    bool readState(UINT, UINT, std::vector<uint8_t>&) override { return false; }
    // A snapshot: RAM, both state sets, the operations' textures, the GPU device's picture and the counters. The ROM is
    // the image's own (resume with the same image); idle workers are thawed and freeze again.
    bool saveSnapshot(std::vector<uint8_t>& out) override {
        SnapHeader h = snapHeader();
        out.assign((const uint8_t*)&h, (const uint8_t*)&h + sizeof h);
        bool ok = readTex(ram_.t.Get(), 16, out);
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) ok = ok && readTex(set_[k][t].t.Get(), 16, out);
        ok = ok && readTex(ops_[0].t.Get(), 16, out) && readTex(ops_[1].t.Get(), 16, out);
        if (devices_) ok = ok && readTex(gpuColor_.Get(), 4, out);
        return ok;
    }
    bool loadSnapshot(const uint8_t* data, size_t size, std::string& err) override {
        SnapHeader want = snapHeader(), h{};
        if (size >= sizeof h) memcpy(&h, data, sizeof h);
        uint32_t fromSpix = (h.ntex + 7) / 8, keep = h.ntex == ntex_ ? spix_ : tWc_ / 8;   // (another cache: the texels before it)
        size_t setBytes = h.pad ? (size_t)((kMaxCores - 1) / 128 * 16 + 8) * 1024 * 16 : (size_t)fromSpix * kMaxCores * 16;
        size_t need = sizeof h + (size_t)kRamW * rows_ * 16 + 16 * setBytes + 2 * (size_t)kMaxCores * 16 +
                      (devices_ ? (size_t)kGpuTarget * kGpuTarget * 4 : 0);
        if (size != need || h.pad > 1 || memcmp(h.magic, want.magic, 8) != 0 || h.ramEnd != want.ramEnd || h.romRows != want.romRows ||
            (h.ntex != want.ntex && (h.ntex < tWc_ || h.ntex > 48 * 8)) || h.devices != want.devices) {
            err = "not a snapshot of this image, tick and RAM (the same --gvm, --gvm-tick and --gvm-ram)";
            return false;
        }
        const uint8_t* p = data + sizeof h;
        p = writeTex(ram_.t.Get(), kRamW, rows_, 16, p);
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) {   // (a snapshot of the other layout: its pixels moved to this one's places)
                std::vector<uint8_t> img((size_t)setW() * setH() * 16, 0);
                for (uint32_t c = 0; c < kMaxCores; c++)
                    for (uint32_t j = 0; j < keep; j++) {
                        size_t from = h.pad ? (size_t)((c >> 7) * 16 + j % 8) * 1024 + (c & 127) * 8 + j / 8 : (size_t)c * fromSpix + j;
                        memcpy(&img[setAt(c, j) * 16], p + from * 16, 16);
                    }
                writeTex(set_[k][t].t.Get(), setW(), setH(), 16, img.data());
                p += setBytes;
            }
        p = writeTex(ops_[0].t.Get(), kMaxCores, 1, 16, p);
        p = writeTex(ops_[1].t.Get(), kMaxCores, 1, 16, p);
        if (devices_) writeTex(gpuColor_.Get(), kGpuTarget, kGpuTarget, 4, p);
        cur_ = (int)h.cur, oc_ = (int)h.oc, time_ = h.time, passes_ = passesDone_ = h.passes;
        workers_ = h.workers, ncores_ = 1 + h.workers;
        for (uint32_t c = 0; c < kMaxCores; c++) frozen_[c] = false, quiet_[c] = 0;
        makeRuns();
        return true;
    }
    // RISC-V rows (2,048 texels each, from row 64 RAM's index 0) as gpu_vm's texels
    bool readRows(UINT row, UINT rows, std::vector<uint8_t>& out) override {
        if (row < 64) return false;
        uint32_t i0 = (row - 64) * 2048, n = rows * 2048;
        if (i0 < kIndexBias) return false;
        out.assign((size_t)n * 16, 0);
        uint32_t t0 = i0 - kIndexBias;
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        uint32_t y0 = t0 / kRamW, y1 = (t0 + n - 1) / kRamW;
        if (y1 >= rows_) return false;
        RegionReadback rb;
        std::string err;
        std::vector<uint8_t> got;
        uint64_t tag;
        if (!rb.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kRamW, y1 - y0 + 1, 1, err)) return false;
        rb.request(ctx, ram_.t.Get(), 0, y0, 0);
        if (!rb.pop(ctx, got, tag)) return false;
        memcpy(out.data(), got.data() + (size_t)(t0 % kRamW) * 16, out.size());
        return true;
    }
    bool uploadRows(UINT row, UINT rows, const void* texels) override {
        if (row < 64 || (row - 64) * 2048 < kIndexBias) return false;
        uint32_t t0 = (row - 64) * 2048 - kIndexBias, n = rows * 2048;
        const uint8_t* in = (const uint8_t*)texels;
        while (n) {
            uint32_t x = t0 % kRamW, k = (std::min)(n, kRamW - x);
            D3D11_BOX b = {x, t0 / kRamW, 0, x + k, t0 / kRamW + 1, 1};
            gpu_.ctx->UpdateSubresource(ram_.t.Get(), 0, &b, in, k * 16, k * 16);
            in += (size_t)k * 16, n -= k, t0 += k;
        }
        return true;
    }

    bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) override {
        if (tsPending_) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ts[4] = {};
            const UINT fl = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            bool ok = gpu_.ctx->GetData(tsDisjoint_.Get(), &dj, sizeof dj, fl) == S_OK;
            for (int k = 3; ok && k >= 0; k--) ok = gpu_.ctx->GetData(tsQuery_[k].Get(), &ts[k], sizeof ts[k], fl) == S_OK;
            static const bool splitLog = getenv("GVM_SPLIT_LOG") != nullptr;   // (a measure: core 0's draws apart from the workers')
            UINT64 t4 = 0;
            if (ok && splitLog && gpu_.ctx->GetData(tsQuery_[4].Get(), &t4, sizeof t4, fl) == S_OK && !dj.Disjoint && dj.Frequency) {
                static double a = 0, b = 0;
                static int n = 0;
                a += (double)(t4 - ts[0]) * 1000.0 / dj.Frequency, b += (double)(ts[1] - t4) * 1000.0 / dj.Frequency;
                if (++n == 32) fprintf(stderr, "[gvm] split: first draws %.3f ms, the rest %.3f ms\n", a / n, b / n), a = b = 0, n = 0;
            }
            if (ok) {
                if (!dj.Disjoint && dj.Frequency) {
                    tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
                    commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)dj.Frequency;
                    deviceMs_ = (double)(ts[3] - ts[2]) * 1000.0 / (double)dj.Frequency;
                }
                tsPending_ = false;
            }
        }
        tickMs = tickMs_, commitMs = commitMs_, deviceMs = deviceMs_;
        return tickMs_ >= 0;
    }
    bool takeSound(std::vector<uint8_t>& out) override {
        if (lastSound_.empty()) return false;
        out.swap(lastSound_);
        lastSound_.clear();
        return true;
    }
    bool gpuCapture(const std::string& path) override {
        if (!gpuColor_) return false;
        D3D11_TEXTURE2D_DESC td{};
        gpuColor_->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.device->CreateTexture2D(&td, nullptr, &staging))) return false;
        gpu_.ctx->CopyResource(staging.Get(), gpuColor_.Get());
        if (FAILED(gpu_.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        bool ok = memoryViewWriteBmp(path, td.Width, td.Height, (const uint8_t*)m.pData, m.RowPitch);
        gpu_.ctx->Unmap(staging.Get(), 0);
        return ok;
    }
    std::string deviceRemoved() override { return gpu_.deviceRemovedReason(); }

    bool viewInit(std::string& err) override {
        view_.setGvm(true);
        return view_.init(gpu_.device.Get(), kRamW, rows_, 0, err);
    }
    bool viewOpen() const override { return view_.open(); }
    void viewRender(bool present) override { view_.render(gpu_.ctx.Get(), ram_.srv.Get(), present, gpuSrv_.Get()); }
    void viewText(const std::vector<std::vector<std::string>>& columns) override { view_.setText(gpu_.ctx.Get(), columns); }
    void viewTitle(const std::string& title) override { view_.setTitle(title); }
    bool viewCapture(const std::string& path) override { return view_.capture(gpu_.ctx.Get(), path); }
    void viewClose() override { view_.close(); }

private:
    struct Prog { ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; };
    struct SnapHeader {
        char magic[8];
        uint32_t ramEnd, romRows, ntex, devices, cur, oc, workers, pad;
        uint64_t time, passes;
    };
    SnapHeader snapHeader() const {
        SnapHeader h = {{'G', 'V', 'M', 'S', 'N', 'A', 'P', '1'}, ramEnd_, romRows_, ntex_, devices_ ? 1u : 0u,
                        (uint32_t)cur_, (uint32_t)oc_, workers_, geo_, time_, passes_};
        return h;
    }
    // A whole texture of `bpp` bytes a texel appended to `out`, row 0 first (a blocking read)
    bool readTex(ID3D11Texture2D* t, UINT bpp, std::vector<uint8_t>& out) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.device->CreateTexture2D(&td, nullptr, &staging))) return false;
        gpu_.ctx->CopyResource(staging.Get(), t);
        if (FAILED(gpu_.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        size_t at = out.size(), row = (size_t)td.Width * bpp;
        out.resize(at + row * td.Height);
        for (UINT y = 0; y < td.Height; y++) memcpy(&out[at + y * row], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, row);
        gpu_.ctx->Unmap(staging.Get(), 0);
        return true;
    }
    const uint8_t* writeTex(ID3D11Texture2D* t, UINT w, UINT h, UINT bpp, const uint8_t* p) {
        gpu_.ctx->UpdateSubresource(t, 0, nullptr, p, w * bpp, 0);
        return p + (size_t)w * h * bpp;
    }

    bool makeTex(Tex& t, UINT w, UINT h, const void* init, std::string& err) {
        t.w = w, t.h = h;
        D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
        std::vector<uint8_t> zeros;
        if (!init) zeros.assign((size_t)w * h * 16, 0), init = zeros.data();
        D3D11_SUBRESOURCE_DATA sd = {init, w * 16, 0};
        HRESULT hr = gpu_.device->CreateTexture2D(&d, &sd, &t.t);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateShaderResourceView(t.t.Get(), nullptr, &t.srv);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(t.t.Get(), nullptr, &t.rtv);
        if (FAILED(hr)) err = "a gpu_vm texture (" + std::to_string(w) + " x " + std::to_string(h) + "): " + hrToString(hr);
        return SUCCEEDED(hr);
    }
    void clearSrv() {
        ID3D11ShaderResourceView* none[16] = {};
        gpu_.ctx->PSSetShaderResources(0, 16, none);
        gpu_.ctx->VSSetShaderResources(0, 16, none);
        gpu_.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void bindTick(int which, bool withRam, bool withStage) {   // t0 RAM, t1 ROM, t2-t9 a state set, t10 ops, t11 stage
        ID3D11ShaderResourceView* s[12] = {};
        if (withRam) s[0] = ram_.srv.Get();
        s[1] = rom_.srv.Get();
        for (int t = 0; t < 8; t++) s[2 + t] = set_[which][t].srv.Get();
        s[10] = ops_[oc_].srv.Get();
        if (withStage) s[11] = stageTex_.srv.Get();
        gpu_.ctx->PSSetShaderResources(0, 12, s);
        gpu_.ctx->VSSetShaderResources(0, 12, s);
    }
    // The state textures: a row a core, or (the step's GEO_BLOCKS) a block of 8 x 8 a core, 128 a band of 16 rows
    UINT setW() const { return geo_ ? 1024 : spix_; }
    UINT setH() const { return geo_ ? (kMaxCores - 1) / 128 * 16 + 8 : kMaxCores; }
    size_t setAt(uint32_t c, uint32_t j) const {   // the texel of state pixel j of core c (x25: core 0's from 64 on in rows 8-15)
        if (geo_ && c == 0 && j >= 64) return (size_t)(8 + j % 8) * 1024 + j / 8 - 8;
        return geo_ ? (size_t)((c >> 7) * 16 + j % 8) * 1024 + (c & 127) * 8 + j / 8 : (size_t)c * spix_ + j;
    }
    void viewport(UINT w, UINT h, UINT y = 0, UINT x = 0) {
        D3D11_VIEWPORT v = {(float)x, (float)y, (float)w, (float)h, 0, 1};
        gpu_.ctx->RSSetViewports(1, &v);
    }
    // A pass (STATE.md, "A pass, its draws in order"): the tick, the commit, two rounds of the operations
    void runPass(bool timed) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        pass_.timeLo = (uint32_t)time_, pass_.timeHi = (uint32_t)(time_ >> 32);
        pass_.ncores = top_;
        ctx->UpdateSubresource(cb_.Get(), 0, nullptr, &pass_, 0, 0);
        uint32_t cdata[4] = {0, top_, 0, 0};
        for (uint32_t c = 0; c < 4; c++) {
            cdata[0] = c;
            ctx->UpdateSubresource(ccb_[c].Get(), 0, nullptr, cdata, 0, 0);
        }
        ID3D11Buffer* cbs[2] = {cb_.Get(), ccb_[0].Get()};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        clearSrv();
        ID3D11RenderTargetView* rt[8];
        for (int t = 0; t < 8; t++) rt[t] = set_[cur_][t].rtv.Get();
        ctx->OMSetRenderTargets(8, rt, nullptr);
        bindTick(cur_ ^ 1, true, false);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(tick_.vs.Get(), nullptr, 0);
        ctx->PSSetShader(tick_.ps.Get(), nullptr, 0);
        static const int split = getenv("GVM_SPLIT0") ? atoi(getenv("GVM_SPLIT0")) : 0;   // (a measure: 1 core 0 drawn apart first, 2 last)
        bool apart = wcn0_ || split, last = split == 2;
        auto core0 = [&]() {   // core 0 by its own tick: its block, then its pixels in rows 8-15 of the first band
            if (wcn0_) ctx->PSSetShader(tick0_.ps.Get(), nullptr, 0);
            viewport(8, 8);
            ctx->Draw(3, 0);
            if (wcn0_ && spix0_ > 64) {
                viewport((spix0_ - 64 + 7) / 8, 8, 8);
                ctx->Draw(3, 0);
            }
            if (wcn0_) {
                ctx->PSSetShader(tick_.ps.Get(), nullptr, 0);
            }
        };
        if (apart && !last) core0();
        if (timed && apart) ctx->End(tsQuery_[4].Get());   // (GVM_SPLIT_LOG: the tick's time to here, its first draws')
        for (auto& run : runs_) {   // (the triangle over each run's rows or blocks: a pixel's place is its core, the viewport's offset kept)
            if (!geo_) {
                viewport(spix_, run.second - run.first, run.first);
                ctx->Draw(3, 0);
                continue;
            }
            for (uint32_t c = (std::max)(run.first, apart ? 1u : 0u); c < run.second;) {   // (the run's blocks in one band at a time)
                uint32_t end = (std::min)(run.second, (c / 128 + 1) * 128);
                viewport((end - c) * 8, 8, (c / 128) * 16, (c % 128) * 8);
                ctx->Draw(3, 0);
                c = end;
            }
        }
        if (apart && last) core0();
        if (timed) ctx->End(tsQuery_[1].Get());
        clearSrv();
        ctx->OMSetRenderTargets(1, ram_.rtv.GetAddressOf(), nullptr);
        bindTick(cur_, false, false);
        viewport(kRamW, rows_);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(commit_.vs.Get(), nullptr, 0);
        ctx->PSSetShader(commit_.ps.Get(), nullptr, 0);
        for (int c = 0; c < 4; c++) {
            ID3D11Buffer* b2[2] = {cb_.Get(), ccb_[c].Get()};
            ctx->VSSetConstantBuffers(0, 2, b2);
            ctx->OMSetBlendState(mask_[c].Get(), nullptr, 0xffffffff);
            ctx->Draw(wcn0_ ? wcn0_ + (top_ - 1) * wcn_ : top_ * wcn_, 0);   // (a frozen row's write cache is empty: its points draw nothing)
        }
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        for (uint32_t round = 0; round < 2; round++) {
            clearSrv();
            ctx->OMSetRenderTargets(1, stageTex_.rtv.GetAddressOf(), nullptr);
            bindTick(cur_, true, false);
            viewport(opSpan_, top_ * opSlots_);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(stage_.vs.Get(), nullptr, 0);
            ctx->PSSetShader(stage_.ps.Get(), nullptr, 0);
            ctx->DrawInstanced(6, top_ * opSlots_, 0, 0);
            clearSrv();
            ctx->OMSetRenderTargets(1, ram_.rtv.GetAddressOf(), nullptr);
            bindTick(cur_, false, true);
            viewport(kRamW, rows_);
            ctx->VSSetShader(write_.vs.Get(), nullptr, 0);
            ctx->PSSetShader(write_.ps.Get(), nullptr, 0);
            ctx->DrawInstanced(12, top_ * opSlots_, 0, 0);
            clearSrv();
            ctx->OMSetRenderTargets(1, ops_[oc_ ^ 1].rtv.GetAddressOf(), nullptr);
            bindTick(cur_, false, false);
            viewport(top_, 1);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            ctx->VSSetShader(progress_.vs.Get(), nullptr, 0);
            ctx->PSSetShader(progress_.ps.Get(), nullptr, 0);
            ctx->Draw(top_, 0);
            oc_ ^= 1;
        }
        clearSrv();
        if (timed) ctx->End(tsQuery_[2].Get());
        cur_ ^= 1;
        passes_++;
    }
    // Texel T of core c in the set the next tick reads
    void writeState(uint32_t c, uint32_t T, const uint32_t v[4]) {
        size_t at = setAt(c, T >> 3);
        UINT x = (UINT)(at % setW()), y = (UINT)(at / setW());
        D3D11_BOX b = {x, y, 0, x + 1, y + 1, 1};
        gpu_.ctx->UpdateSubresource(set_[cur_ ^ 1][T & 7].t.Get(), 0, &b, v, 16, 16);
    }
    // The workers: cores 1 to n started at the firmware (gpu_vm/platform/worker.S) in supervisor mode with r5 their
    // number, the others halted; the tick draws the rows of those there are
    void setWorkers(uint32_t n) {
        for (uint32_t c = 1; c < kMaxCores; c++) {
            if (c <= n && c > workers_) {
                std::vector<uint32_t> s((size_t)ntex_ * 4, 0);
                for (uint32_t T = 40; T < ntex_; T++) s[T * 4] = s[T * 4 + 2] = 0xffffffffu;   // (an empty write cache)
                s[16 * 4 + 0] = GVM_FW_ENTRY;
                s[16 * 4 + 1] = GVM_ST_S;
                s[18 * 4 + 3] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
                s[1 * 4 + 1] = c;   // r5
                for (uint32_t T = 0; T < ntex_; T++) writeState(c, T, &s[T * 4]);
            } else if (c > n && c <= workers_) {
                const uint32_t halt[4] = {0, 0, F_HALT, 0};
                writeState(c, 18, halt);
            }
        }
        fprintf(stderr, "[harness] gpu_vm: %u worker cores (the kernel's layout)\n", n);
        workers_ = n;
        ncores_ = 1 + n;
        for (uint32_t c = 0; c < kMaxCores; c++) frozen_[c] = false, quiet_[c] = 0;
        makeRuns();
    }
    // The runs of rows the tick and the commit draw: core 0 and every worker that is not frozen
    void makeRuns() {
        runs_.clear();
        for (uint32_t c = 0; c < ncores_; c++) {
            if (c && frozen_[c]) continue;
            if (!runs_.empty() && runs_.back().second == c) runs_.back().second = c + 1;
            else runs_.push_back({c, c + 1});
        }
        top_ = runs_.back().second;
        static const bool log = getenv("GVM_FREEZE_LOG") != nullptr;
        if (log) {
            uint32_t drawn = 0;
            for (auto& r : runs_) drawn += r.second - r.first;
            fprintf(stderr, "[gvm] rows drawn %u in %zu runs, to %u\n", drawn, runs_.size(), top_);
        }
    }
    // Workers with nothing to do are not drawn (as the RISC-V tick's mc_idle: docs/machine/multicore.md, "Workers with
    // nothing to do"): one parked in the firmware, or asleep (its last pass ended in a user's wait: mcw_loop's), whose
    // start texel and job word have been as they are for kQuiet readbacks, is frozen: its row is the same state in both
    // sets (copied from the set the last pass drew, so it goes on as it was whichever it reads) and neither the tick
    // nor the commit draws it. A change to its start texel or its job word, which wakes it on the RISC-V machine, thaws
    // it, as does a count that says it runs (the readback is some frames behind what is frozen). `mbox` is RAM from
    // the mailboxes (0x00c00000, 0x800 texels), `bits` MC_STATS_ALL's texels of which run and which are asleep.
    static const uint32_t kQuiet = 24;
    void freeze(const uint32_t* mbox, const uint32_t* bits) {
        static const bool never = getenv("GVM_NO_FREEZE") != nullptr;   // (a measure: every worker drawn every pass)
        if (never) return;
        bool changed = false;
        for (uint32_t k = 1; k <= workers_; k++) {
            const uint32_t* start = mbox + 4 * k;
            uint32_t job = mbox[4 * (0x400 + 4 * k)];
            if (memcmp(start, seenStart_[k], 16) != 0 || job != seenJob_[k]) {
                memcpy(seenStart_[k], start, 16);
                seenJob_[k] = job;
                quiet_[k] = 0;
            } else if (quiet_[k] < kQuiet) quiet_[k]++;
            bool runs = (bits[4 * (k / 32)] >> (k % 32)) & 1, asleep = (bits[4 * (k / 32) + 1] >> (k % 32)) & 1;
            bool idle = !runs || asleep, freeze = idle && quiet_[k] >= kQuiet;
            if (freeze == frozen_[k]) continue;
            if (freeze) {   // its row as the last pass left it, into the set the next pass draws, and its write cache empty
                UINT wc0 = tWc_ / 8;   // (the write cache's first pixel: texels from T_WC, a multiple of 8)
                for (UINT t = 0; t < 8; t++) {
                    if (!geo_) {
                        D3D11_BOX b = {0, k, 0, spix_, k + 1, 1}, nb = {wc0, 0, 0, spix_, 1, 1};
                        gpu_.ctx->CopySubresourceRegion(set_[cur_][t].t.Get(), 0, 0, k, 0, set_[cur_ ^ 1][t].t.Get(), 0, &b);
                        for (int w = 0; w < 2; w++) gpu_.ctx->CopySubresourceRegion(set_[w][t].t.Get(), 0, wc0, k, 0, none_.t.Get(), 0, &nb);
                        continue;
                    }
                    UINT x0 = (k % 128) * 8, y0 = (k / 128) * 16, cols = (spix_ + 7) / 8;
                    D3D11_BOX b = {x0, y0, 0, x0 + cols, y0 + 8, 1}, nb = {0, 0, 0, 1, 1, 1};
                    gpu_.ctx->CopySubresourceRegion(set_[cur_][t].t.Get(), 0, x0, y0, 0, set_[cur_ ^ 1][t].t.Get(), 0, &b);
                    for (uint32_t j = wc0; j < spix_; j++)   // (pixel j of the block: column j / 8, row j % 8)
                        for (int w = 0; w < 2; w++)
                            gpu_.ctx->CopySubresourceRegion(set_[w][t].t.Get(), 0, x0 + j / 8, y0 + j % 8, 0, none_.t.Get(), 0, &nb);
                }
            }
            frozen_[k] = freeze;
            changed = true;
        }
        if (changed) makeRuns();
    }

    // ---- the devices
    // A zone of the control pass: RISC-V indices [index, index + n) drawn into the scratch texture reading RAM, then
    // copied into RAM (whole rows of it when the zone is longer than a row's rest)
    void zone(Material& mat, uint32_t index, uint32_t n) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        uint32_t t0 = index - kIndexBias, x0 = t0 % kRamW, y0 = t0 / kRamW;
        bool rows = x0 + n > kRamW;
        UINT w = rows ? kRamW : n, h = rows ? (x0 + n + kRamW - 1) / kRamW : 1, x = rows ? 0 : x0;
        if (h > zone_.h) return;
        mat.setInt("_GvmZoneX", x);
        mat.setInt("_GvmZoneY", y0);
        mat.setTexture("_State", ram_.srv.Get(), kRamW, rows_);
        GpuPass& pass = devPasses_[1];
        clearSrv();
        ctx->OMSetRenderTargets(1, zone_.rtv.GetAddressOf(), nullptr);
        viewport(w, h);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mat.bind(ctx, pass, gpu_);
        ctx->Draw(3, 0);
        clearSrv();
        D3D11_BOX box = {0, 0, 0, w, h, 1};
        ctx->CopySubresourceRegion(ram_.t.Get(), 0, x, y0, 0, zone_.t.Get(), 0, &box);
    }
    // The picture into RAM, as the control pass noted it last frame (devices.shader's GPUWriteback)
    void writeback(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = devPasses_[2];
        mat.setTexture("_GvmCtl", ctl_.srv.Get(), 256, 1);
        mat.setTexture("_GpuTarget", gpuSrv_.Get(), kGpuTarget, kGpuTarget);
        mat.setInt("_GvmRows", rows_);
        clearSrv();
        ctx->OMSetRenderTargets(1, ram_.rtv.GetAddressOf(), nullptr);
        viewport(kRamW, rows_);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (int c = 0; c < 4; c++) {
            mat.setInt("_GvmComp", c);
            mat.bind(ctx, pass, gpu_);
            ctx->OMSetBlendState(mask_[c].Get(), nullptr, 0xffffffff);
            ctx->Draw(6, 0);
        }
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        clearSrv();
    }
    bool createGpuTarget(std::string& err) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kGpuTarget;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, &gpuColor_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(gpuColor_.Get(), nullptr, &gpuRtv_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateShaderResourceView(gpuColor_.Get(), nullptr, &gpuSrv_);
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateTexture2D(&td, nullptr, &depth);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateDepthStencilView(depth.Get(), nullptr, &gpuDsv_);
        for (int k = 0; k < 3 && SUCCEEDED(hr); ++k) {   // (rvc_backend11's: depth tested and written, tested, unused)
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = k < 2;
            dd.DepthWriteMask = k == 0 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            hr = gpu_.device->CreateDepthStencilState(&dd, &gpuDepthState_[k]);
        }
        for (int k = 0; k < 4 && SUCCEEDED(hr); ++k) {   // blending: none, alpha, additive, multiply
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = k != 0;
            rt.SrcBlend = rt.SrcBlendAlpha = k == 3 ? D3D11_BLEND_DEST_COLOR : D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = rt.DestBlendAlpha = k == 1 ? D3D11_BLEND_INV_SRC_ALPHA : k == 2 ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
            if (k == 3) rt.SrcBlendAlpha = D3D11_BLEND_ZERO, rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            hr = gpu_.device->CreateBlendState(&bd, &gpuBlend_[k]);
        }
        if (FAILED(hr)) {
            err = "GPU target: " + hrToString(hr);
            return false;
        }
        const float black[4] = {0, 0, 0, 1};
        gpu_.ctx->ClearRenderTargetView(gpuRtv_.Get(), black);
        return true;
    }
    void gpuDraw(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = devPasses_[0];
        mat.setTexture("_State", ram_.srv.Get(), kRamW, rows_);
        mat.setTexture("_GvmRom", rom_.srv.Get(), kRamW, romRows_);
        clearSrv();
        ctx->OMSetRenderTargets(1, gpuRtv_.GetAddressOf(), gpuDsv_.Get());
        ctx->ClearDepthStencilView(gpuDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        viewport(kGpuTarget, kGpuTarget);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (int p = 0; p < 8; ++p) {
            if (!((gpuPasses >> p) & 1)) continue;
            mat.setInt("_GpuPass", p);
            mat.bind(ctx, pass, gpu_);
            ctx->OMSetBlendState(gpuBlend_[p & 3].Get(), nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(gpuDepthState_[p == 0 ? 0 : p < 4 ? 1 : 2].Get(), 0);
            ctx->Draw(kGpuTriangles * 3, 0);
        }
        clearSrv();
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    bool createSoundTarget(std::string& err) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kSoundSide;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, &soundTex_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(soundTex_.Get(), nullptr, &soundRtv_);
        if (FAILED(hr)) {
            err = "sound target: " + hrToString(hr);
            return false;
        }
        return sound_.init(gpu_.device.Get(), td.Format, 8, kSoundSide, kSoundSide, 3, err);
    }
    void soundDraw(Material& mat, uint64_t tag) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = devPasses_[3];
        mat.setTexture("_State", ram_.srv.Get(), kRamW, rows_);
        mat.setTexture("_GvmRom", rom_.srv.Get(), kRamW, romRows_);
        clearSrv();
        ctx->OMSetRenderTargets(1, soundRtv_.GetAddressOf(), nullptr);
        viewport(kSoundSide, kSoundSide);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mat.bind(ctx, pass, gpu_);
        ctx->Draw(3, 0);
        clearSrv();
        sound_.request(ctx, soundTex_.Get(), 0, 0, tag);
        soundTags_.push_back(tag);
    }
    bool ensureQueries() {
        if (!tsDisjoint_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            gpu_.device->CreateQuery(&qd, &tsDisjoint_);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& q : tsQuery_) gpu_.device->CreateQuery(&qd, &q);
        }
        return tsDisjoint_ && tsQuery_[3];
    }

    Gpu gpu_;
    uint32_t ramEnd_ = 0, rows_ = 0, romRows_ = 1, wcn_ = 514, tWc_ = 40, opSlots_ = 32, opSpan_ = 272, ntex_ = 297, spix_ = 38, geo_ = 0, wcn0_ = 0, spix0_ = 0;
    Prog tick_, tick0_, commit_, stage_, write_, progress_;
    Tex ram_, rom_, stageTex_, ops_[2], set_[2][8], zone_, ctl_, info_, none_;
    ComPtr<ID3D11Buffer> cb_, ccb_[4];
    ComPtr<ID3D11BlendState> mask_[4];
    PassParams pass_{};
    int cur_ = 0, oc_ = 0;
    uint32_t ncores_ = 1, workers_ = 0, top_ = 1;   // (top_: the rows below the highest one drawn: the passes' cores)
    std::vector<std::pair<uint32_t, uint32_t>> runs_{{0u, 1u}};   // rows [first, end) drawn
    bool frozen_[kMaxCores] = {};
    uint32_t quiet_[kMaxCores] = {}, seenStart_[kMaxCores][4] = {}, seenJob_[kMaxCores] = {};
    uint64_t time_ = 0, passes_ = 0, passesDone_ = 0;
    bool devices_ = false, deliver_ = false, deliverNet_ = false;
    std::vector<GpuPass> devPasses_;
    ComPtr<ID3D11Texture2D> gpuColor_;
    ComPtr<ID3D11RenderTargetView> gpuRtv_;
    ComPtr<ID3D11ShaderResourceView> gpuSrv_;
    ComPtr<ID3D11DepthStencilView> gpuDsv_;
    ComPtr<ID3D11DepthStencilState> gpuDepthState_[3];
    ComPtr<ID3D11BlendState> gpuBlend_[4];
    ComPtr<ID3D11Texture2D> soundTex_;
    ComPtr<ID3D11RenderTargetView> soundRtv_;
    RegionReadback sound_, infoRb_, controlRb_, netRb_, mboxRb_;
    std::vector<uint8_t> lastSound_;
    std::deque<uint64_t> soundTags_;
    ComPtr<ID3D11ShaderResourceView> hostData_, netData_;
    ComPtr<ID3D11Query> tsDisjoint_, tsQuery_[5];
    bool tsPending_ = false;
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;
    MemoryView view_;
};

}   // namespace

std::unique_ptr<RvcBackend> makeBackendGvm() { return std::make_unique<GvmBackend>(); }
