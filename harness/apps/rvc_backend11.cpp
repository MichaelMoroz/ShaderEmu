// D3D11 backend: FXC bytecode on a double-buffered Custom Render Texture, as Unity runs it.

#include "crt.h"
#include "image.h"
#include "memview.h"
#include "readback.h"
#include "rvc_backend.h"

#include <deque>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

class Backend11 : public RvcBackend {
public:
    bool init(const BackendOptions& opt, const SLShader& shader, Material& mat, std::string& err) override {
        if (!gpu_.init(opt.gpu, err)) return false;
        fprintf(stderr, "[harness] D3D11 + %s on %s%s, doubles: %s, extended doubles: %s\n", shaderCompilerName(), gpu_.adapterName.c_str(),
                opt.gpu.warp ? " (WARP)" : "", gpu_.doubles ? "yes" : "NO", gpu_.extendedDoubles ? "yes" : "no");
        if (!gpu_.doubles) fprintf(stderr, "[harness] warning: rvc uses doubles (MULH, timer); this device lacks them\n");

        ShaderBuildOptions bo;
        bo.verbose = opt.verbose;
        bo.settings = opt.compile;
        for (auto& d : opt.compile.defines)
            if (d.first == "COMMIT_BANDS") bands_ = true;
        for (auto& d : opt.compile.defines)
            if (d.first == "CORES") cores_ = (float)(std::max)(1, (std::min)(kCoresMost, atoi(d.second.c_str())));
        for (auto& d : opt.compile.defines)
            if (d.first == "CORE_PITCH") {}   // (no more: the workers are one strip)
        tickRows_ = (float)opt.tickRows;
        for (auto& d : opt.compile.defines)
            if (d.first == "TICK_MRT") mrt_ = true;
        for (auto& d : opt.compile.defines)
            if (d.first == "NO_SCATTER") scatter_ = false;
        inplace_ = opt.ramInplace;
        tickDirect_ = opt.tickDirect;
        laneField_ = opt.laneField && opt.ramInplace;
        scatter_ = scatter_ && mrt_ && bands_ && !inplace_;
        // the commit draws whole only the bands the cores' stores are not what changed (they are points)
        if (scatter_) bo.settings.defines.push_back({"COMMIT_SCATTER", "1"});
        if (inplace_) {
            // (the tick is compiled without RAM_INPLACE, which only the commit's passes know)
            ShaderBuildOptions c = bo;
            c.settings.defines.push_back({"RAM_INPLACE", "1"});
            std::vector<GpuPass> commit;
            if (!buildPasses(gpu_, shader, {"CPUTick"}, bo, passes_, err) || !buildPasses(gpu_, shader, {"Commit"}, c, commit, err)) return false;
            passes_.push_back(commit[0]);
        } else if (!buildPasses(gpu_, shader, {"CPUTick", "Commit"}, bo, passes_, err)) return false;
        if (inplace_) {
            // RAM in one texture (src/ram.h): the commit's gather and its RAM points, passes of
            // their own beside the state commit; none of the three reads the texture it writes
            SLShader gather = shader, points = shader;
            for (auto& p : gather.passes)
                if (p.name == "Commit") p.geometryEntry = "ram_gather_geom";
            for (auto& p : points.passes)
                if (p.name == "Commit") p.geometryEntry = "ram_commit_geom";
            ShaderBuildOptions g = bo, r = bo;
            g.settings.defines.push_back({"RAM_INPLACE", "1"});
            r.settings.defines.push_back({"RAM_INPLACE", "1"});
            g.settings.defines.push_back({"COMMIT_GATHER", "1"});
            r.settings.defines.push_back({"COMMIT_RAM", "1"});
            if (!buildPasses(gpu_, gather, {"Commit"}, g, gatherPasses_, err) || !buildPasses(gpu_, points, {"Commit"}, r, ramPasses_, err))
                return false;
            for (GpuPass* p : {&passes_[1], &gatherPasses_[0], &ramPasses_[0]})
                for (StageLayout* l : {&p->vsLayout, &p->gsLayout, &p->psLayout})
                    for (auto& t : l->textures)
                        if (t.name == "_SelfTexture2D") {
                            err = "RAM_INPLACE: a commit pass reads _SelfTexture2D, the texture it writes";
                            return false;
                        }
        }
        if (scatter_) {
            // the commit's pass twice more: its points, and the same points as copies
            SLShader withPoints = shader;
            for (auto& p : withPoints.passes)
                if (p.name == "Commit") p.geometryEntry = "commit_geom";
            ShaderBuildOptions points = bo;
            points.settings.defines.push_back({"COMMIT_POINTS", "1"});
            if (!buildPasses(gpu_, withPoints, {"Commit"}, points, pointPasses_, err)) return false;
            points.settings.defines.push_back({"COMMIT_COPY", "1"});
            if (!buildPasses(gpu_, withPoints, {"Commit"}, points, copyPasses_, err)) return false;
        }
        for (auto& d : opt.compile.defines)
            if (d.first == "TICK_LOAD") mrtLoad_ = (UINT)(std::max)(1, atoi(d.second.c_str()));
        if (mrt_) {
            // the tick's pass again, as the one that copies its eight targets into the state
            ShaderBuildOptions unpack = bo;
            unpack.settings.defines.push_back({"TICK_UNPACK", "1"});
            if (!buildPasses(gpu_, shader, {"CPUTick"}, unpack, unpackPasses_, err) || !createTickTargets(err)) return false;
            unpackBo_ = unpack;
        }
        tickBo_ = bo;
        tickVariants_.push_back({passes_[0], mrt_ ? unpackPasses_[0] : GpuPass{}});
        for (auto& d : opt.compile.defines)
            if (d.first == "RAM_DIRECT") csDirect_ = true;
        for (auto& d : opt.compile.defines)
            if (d.first == "RAM_BUFFER") csBuffer_ = true;
        if (getenv("RVC11_COMPUTE") && !buildComputeTick(shader, bo, err)) return false;
        if (!opt.computeShader.empty() && !buildComputeCpu(opt.computeShader, bo, err)) return false;
        std::string gpuErr;
        if (opt.gpuShader && (!buildPasses(gpu_, *opt.gpuShader, {"GPUDraw", "GPUControl"}, bo, gpuPasses_, gpuErr) ||
                              !createGpuTarget(gpuErr))) {
            fprintf(stderr, "[harness] GPU device disabled: %s\n", gpuErr.c_str());
            gpuPasses_.clear();
        }
        if (opt.soundShader && !gpuPasses_.empty() &&
            (!buildPasses(gpu_, *opt.soundShader, {"SoundMix"}, bo, soundPasses_, gpuErr) || !createSoundTarget(gpuErr))) {
            fprintf(stderr, "[harness] sound card disabled: %s\n", gpuErr.c_str());
            soundPasses_.clear();
        }

        if (opt.present && !createSwapChain(err)) return false;
        if (opt.profile && !createProfile(mat, err)) return false;

        if (inplace_) {
            if (!createInplace(err)) return false;
        } else {
            if (!crt_.init(gpu_.device.Get(), kWidth, kHeight, DXGI_FORMAT_R32G32B32A32_UINT, err)) return false;
            crt_.clear(gpu_.ctx.Get());
        }
        return rows_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, 64, 1, 3, err, opt.readbackBatch) &&
               control_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kControlTexels, 1, 3, err, opt.readbackBatch) &&
               net_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kNetRead, 1, 3, err, opt.readbackBatch) &&
               field_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kFieldRead, 1, 3, err, opt.readbackBatch) &&
               (!mboxLog() || mbox_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kMboxJobs, 1, 3, err, opt.readbackBatch)) &&
               watchInit(err);
    }
    // RVC_MBOX_LOG=FILE (a measure, off by default; gpu_vm/PROFILE_Q3.md "Handoffs"): every pass, the frame's tag and each
    // core's job, answer and taken words as the pass left them (programs/mc/mc.h: MC_JOB_AT(k) at rvlayout::kMbox + 0x4000 + 64 k), a
    // record of 2 + 6 x 256 words: tag (low, high), then for each core the job's seq, fn, a0, a1, the answer's done, the taken word
    static FILE* mboxLog() { static FILE* f = getenv("RVC_MBOX_LOG") ? fopen(getenv("RVC_MBOX_LOG"), "wb") : nullptr; return f; }
    static const UINT kMboxJobs = 1024, kMboxX = 1024, kMboxRow = 64 + rvlayout::devT(0xc0400) / 2048;   // (rvlayout::kMbox + 0x4000)
    void mboxRequest(ID3D11Texture2D* src, uint64_t tag) {
        if (mboxLog()) mbox_.request(gpu_.ctx.Get(), src, kMboxX, kMboxRow, tag);
        watchRequest(src, tag);
    }
    // RVC_WATCH=ADDR,ADDR,... and RVC_WATCH_LOG=FILE (a measure, off by default; gpu_vm/PROFILE_Q3.md "Handoffs"): the
    // RAM texels of those guest addresses (up to 32) as each pass left them, a record of 2 + 4 x N words: the tag (low,
    // high), then each texel's four words
    std::vector<uint32_t> watch_;
    ComPtr<ID3D11Texture2D> watchTex_;
    RegionReadback watchRb_;
    FILE* watchLog_ = nullptr;
    bool watchInit(std::string& err) {
        const char* w = getenv("RVC_WATCH");
        if (!w || !getenv("RVC_WATCH_LOG")) return true;
        for (const char* q = w; *q && watch_.size() < 32;) {
            char* end;
            uint32_t a = (uint32_t)strtoul(q, &end, 16);
            if (end == q) break;
            watch_.push_back(a);
            q = *end == ',' ? end + 1 : end;
        }
        if (watch_.empty()) return true;
        D3D11_TEXTURE2D_DESC td = {(UINT)watch_.size(), 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
        if (FAILED(gpu_.device->CreateTexture2D(&td, nullptr, &watchTex_))) { err = "RVC_WATCH's texture"; return false; }
        watchLog_ = fopen(getenv("RVC_WATCH_LOG"), "wb");
        return watchRb_.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, (UINT)watch_.size(), 1, 3, err);
    }
    void watchRequest(ID3D11Texture2D* src, uint64_t tag) {
        if (!watchLog_) return;
        for (UINT k = 0; k < watch_.size(); k++) {
            uint32_t t = (watch_[k] - 0x80000000u) >> 4;   // (RAM texel t from 0x80000000: row 64 + t / 2048)
            D3D11_BOX b = {t % 2048, 64 + t / 2048, 0, t % 2048 + 1, 64 + t / 2048 + 1, 1};
            gpu_.ctx->CopySubresourceRegion(watchTex_.Get(), 0, k, 0, 0, src, 0, &b);
        }
        watchRb_.request(gpu_.ctx.Get(), watchTex_.Get(), 0, 0, tag);
    }
    void watchPop() {
        std::vector<uint8_t> m;
        uint64_t t;
        if (!watchLog_ || !watchRb_.pop(gpu_.ctx.Get(), m, t)) return;
        uint32_t head[2] = {(uint32_t)t, (uint32_t)(t >> 32)};
        fwrite(head, 4, 2, watchLog_);
        fwrite(m.data(), 1, watch_.size() * 16, watchLog_);
    }
    void mboxPop() {
        std::vector<uint8_t> m;
        uint64_t t;
        if (!mboxLog() || !mbox_.pop(gpu_.ctx.Get(), m, t) || m.size() < kMboxJobs * 16) return;
        const uint32_t* q = (const uint32_t*)m.data();
        uint32_t rec[2 + 6 * 256] = {(uint32_t)t, (uint32_t)(t >> 32)};
        for (int k = 0; k < 256; k++) {   // (core k's four texels: the job, the answer, the fault, the resume; taken is the resume's .z)
            const uint32_t* j = q + 16 * k;
            uint32_t* r = rec + 2 + 6 * k;
            r[0] = j[0], r[1] = j[1], r[2] = j[2], r[3] = j[3], r[4] = j[4], r[5] = j[14];
        }
        fwrite(rec, 4, 2 + 6 * 256, mboxLog());
    }

    bool loadPayload(Material& mat, const std::string& dir, const std::string& prefix, const std::string& propBase,
                     std::string& err) override {
        if (prefix == "none") return true;
        if (propBase == "_Data_MTD") {
            // the ROM: one texture of 16-byte texels, RGBAFloat read by its bits (as the world's importer makes it,
            // docs/machine/gpu.md "The ROM as one texture")
            std::string bin;
            fs::path base = prefix.find_first_of("/\\") == std::string::npos ? fs::u8path(dir) / prefix : fs::u8path(prefix);
            if (!readFileBinary(base.u8string() + ".bin", bin)) {
                err = base.u8string() + ".bin: cannot read the ROM";
                return false;
            }
            UINT height = 0;
            auto srv = createTextureTexels(gpu_.device.Get(), bin, 2048, /*flipY=*/true, height, err);
            if (!srv) return false;
            mat.setTexture("_Data_ROM", srv.Get(), 2048, height);
            keep_.push_back(srv);
            fprintf(stderr, "[harness] _Data_ROM <- %s (2048x%u, RGBAFloat)\n", prefix.c_str(), height);
            return true;
        }
        const char* lanes[4] = {"r", "g", "b", "a"};
        const char* props[4] = {"_R", "_G", "_B", "_A"};
        for (int i = 0; i < 4; ++i) {
            std::string path = (fs::u8path(dir) / (prefix + "." + lanes[i] + ".png")).u8string();
            ImageRGBA8 img;
            if (!loadPayloadLane(dir, prefix, i, img, err)) return false;
            auto srv = createTextureRGBA8(gpu_.device.Get(), img, /*flipY=*/true, err);
            if (!srv) {
                err = path + ": " + err;
                return false;
            }
            mat.setTexture(propBase + props[i], srv.Get(), img.width, img.height);
            keep_.push_back(srv);
            if (i == 0)
                fprintf(stderr, "[harness] %s_{R,G,B,A} <- %s (%ux%u)\n", propBase.c_str(), prefix.c_str(), img.width, img.height);
        }
        return true;
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

    bool setState(const void* texels, std::string&) override {
        if (inplace_) {
            const float zero[4] = {0, 0, 0, 0};
            if (texels) gpu_.ctx->UpdateSubresource(ram_.Get(), 0, nullptr, texels, kWidth * 16, 0);
            else gpu_.ctx->ClearRenderTargetView(ramRtv_.Get(), zero);
        } else if (texels) crt_.load(gpu_.ctx.Get(), texels, kWidth * 16);
        else crt_.clear(gpu_.ctx.Get());
        return true;
    }

    bool frame(Material& mat, uint64_t tag, bool timeIt) override {
        if (inplace_) return frameInplace(mat, tag, timeIt);
        timeIt = timeIt && ensureQueries() && !tsPending_;
        // a timing experiment: the state is not written back, so every pass runs the same instructions
        static const bool freezeAsked = getenv("RVC_FREEZE") != nullptr;
        bool freeze = freezeAsked && mat.getFloat("_Init") == 0;
        if (timeIt) {
            gpu_.ctx->Begin(tsDisjoint_.Get());
            gpu_.ctx->End(tsQuery_[0].Get());
            timedRounds_ = (std::max)(1u, tickRounds);
            timedVariant_ = curVariant_;
        }
        // (the devices' words for this frame, before the control pass's draws clear them)
        bool importCon = conDelivered, importFetch = deliver_, importNet = deliverNet_, importCounts = false;
        bool computing = computeMode && csCpu_ && mat.getFloat("_Init") == 0;
        // --tick-rounds N: the tick and its commit N times a frame before the devices' passes, each round
        // a tick of the clock on (_TimePass) and the host's zap in the first only (it flips bits)
        const uint32_t rounds = (!computing && !cpuMode && !cs_ && mat.getFloat("_Init") == 0) ? (std::max)(1u, tickRounds) : 1u;
        const float zapKind = mat.getFloat("_ZapKind");
        for (uint32_t round = 0; round < rounds; round++) {
        if (rounds > 1) { mat.setInt("_TimePass", (int)round); mat.setInt("_TickFull", (int)(tickSplit ? tickSplit : rounds)); mat.setInt("_ZapKind", round == 0 ? (int)zapKind : 0); }
        if (computing) { profBegin(tag); runComputeCpu(mat); }
        else if (cpuMode && mat.getFloat("_Init") == 0) {}   // the processor ran the instructions
        else if (cs_ && mat.getFloat("_Init") == 0) runComputeTick(mat);
        else {
            // The tick's geometry shader draws the quads: core 0's rectangle, and a strip for
            // each worker that has something to run, where the machine's geometry puts it
            // (main.shader, tick_geom). What it does not draw of the two zones stays what it
            // was: they are copied into the buffer drawn into first, and back afterwards.
            std::vector<UpdateZone> zones{UpdateZone{32, kHeight - tickRows_ / 2, 64, tickRows_, 0}};
            // the workers' bands: 248 tiles of 8 x 8 each, 49 workers to a band (MC_STRIP_BANDS)
            float band = 1984.0f, bandRows = 8.0f * (((UINT)cores_ + 47) / 49);
            bool workers = cores_ > 1 && (workersNow || getenv("RVC_WORKERS_ALWAYS"));
            activeCores_ = workers ? (UINT)cores_ : 1;
            UINT groups = (activeCores_ + 15) / 16;   // a primitive of the pass draws 16 cores' quads
            if (workers) zones.push_back(UpdateZone{64 + band / 2, kHeight - bandRows / 2, band, bandRows, 0});
            // TICK_MRT: the tick draws a pixel for every eight texels, into eight targets of its
            // own, and the pass that follows puts them where the state has them
            static const char* const kPassLabels[] = {"tick", "unpack", "commit bands", "commit points", "commit copy", "devices", "", "", "", "", "", ""};
            if (rounds == 1 && getenv("RVC_PASS_PROF")) { prof_.labels = kPassLabels; profBegin(tag); }
            if (mrt_) tickDraw(mat, zones[0]);
            prof_.mark(gpu_.ctx.Get());
            // (tickDirect_, experiments/rvc_lean: the unpack draws into the state texture itself, which it
            // does not read, so no zone is copied in and back)
            if (!freeze && tickDirect_ && mrt_)
                drawInto(unpackPasses_[0], mat, crt_.currentRTV(), 0, (std::max)(6u, 3 * groups), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &zones[0]);
            else if (!freeze) {
            for (auto& z : zones) crt_.copyIn(gpu_, z);
            crt_.runZone(gpu_, mrt_ ? unpackPasses_[0] : passes_[0], mat, zones[0], (std::max)(6u, 3 * groups), false);
            for (auto& z : zones) crt_.copyZone(gpu_, z);
            }
            prof_.mark(gpu_.ctx.Get());
        }
        if (timeIt) gpu_.ctx->End(tsQuery_[1].Get());
        if (!freeze) {
        if (!gpuPasses_.empty()) mat.setTexture("_GpuTarget", gpuSrv_.Get(), kGpuTarget, kGpuTarget);   // Commit copies it back
        // With COMMIT_BANDS the vertex shader draws the state rows and the bands of RAM that changed;
        // the buffer drawn into holds the state of two commits ago, as on D3D12.
        if (scatter_ && mat.getFloat("_Init") == 0) {
            // the state rows and the bands other things wrote, then what the cores stored as
            // points (a geometry shader makes each the rectangle it stands for); and the same
            // points into the other buffer, as copies, so that both have them
            UpdateZone all{1024, kHeight / 2.0f, 2048, (float)kHeight, 1};
            UINT points = 258 + (activeCores_ - 1) * 130;   // a bucket a point: commit_geom
            crt_.runZone(gpu_, passes_[1], mat, all, 6 * kCommitQuads, false);
            prof_.mark(gpu_.ctx.Get());
            crt_.runZone(gpu_, pointPasses_[0], mat, all, points, false, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            prof_.mark(gpu_.ctx.Get());
            crt_.swap();
            crt_.runZone(gpu_, copyPasses_[0], mat, all, points, false, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            prof_.mark(gpu_.ctx.Get());
        } else
        crt_.runZone(gpu_, passes_[1], mat, UpdateZone{1024, kHeight / 2.0f, 2048, (float)kHeight, 1}, bands_ ? 6 * kCommitQuads : 6);
        }
        }
        if (rounds > 1) { mat.setInt("_TimePass", 0); mat.setInt("_ZapKind", (int)zapKind); }
        if (timeIt) gpu_.ctx->End(tsQuery_[2].Get());
        if (gpuPasses_.size() == 2 && !freeze) {
            bool listDrawn = !listLazy() || listDue > 0;   // (RVC_LIST_LAZY: only while a list is due)
            if (listDrawn) {
                gpuDraw(mat);
                if (listDue > 0) listDue--;
            }
            mat.setInt("_GpuListSkipped", listDrawn ? 0 : 1);   // (the control zone takes the submit word back only when drawn)
            bool mixed = soundMix && !soundPasses_.empty();
            if (mixed) soundDraw(mat, tag);   // before the control zone moves the voices on
            bool merge = countsAdded && (!cpuMode || computeMode);   // (--cpu: the interpreter adds them up itself)
            importCounts = merge;
            if (merge) {
                const float* m = kCountZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{m[0], m[1], m[2], m[3], 1});
                countsAdded = false;
            }
            mat.setInt("_CountMerged", merge ? 2 : 1);
            const float* z = kGpuControlZone;
            crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{z[0], z[1], z[2], z[3], 1});
            if (conDelivered) {
                const float* c = kConZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{c[0], c[1], c[2], c[3], 1});
                conDelivered = false;
            }
            if (cores_ > 16 && activeCores_ > 1) {
                const float* s = kCoreStatsZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{s[0], s[1], s[2], s[3], 1});
            }
            if (deliver_) {
                const float* f = kFetchZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{f[0], f[1], f[2], f[3], 1});
                deliver_ = false;
            }
            if (deliverNet_) {
                const float* n = kNetZone;
                crt_.runZone(gpu_, gpuPasses_[1], mat, UpdateZone{n[0], n[1], n[2], n[3], 1});
                deliverNet_ = false;
            }
        }
        if (computing) { prof_.mark(gpu_.ctx.Get()); computeImport(importCon, importFetch, importNet, importCounts); profEnd(); }
        else profEnd();   // (RVC_PASS_PROF: the devices' passes end it)
        if (timeIt) {
            gpu_.ctx->End(tsQuery_[3].Get());
            gpu_.ctx->End(tsDisjoint_.Get());
            tsPending_ = true;
        }
        if (swapChain_) {
            swapChain_->Present(0, 0);
            MSG msg;
            while (PeekMessageW(&msg, presentWnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        rows_.request(gpu_.ctx.Get(), crt_.current(), 0, 0, tag);
        control_.request(gpu_.ctx.Get(), crt_.current(), 0, kControlRow, tag);
        net_.request(gpu_.ctx.Get(), crt_.current(), 0, kNetRow, tag);
        field_.request(gpu_.ctx.Get(), crt_.current(), kFieldAt, kFieldRow, tag);
        mboxRequest(crt_.current(), tag);
        // hand the frame to the GPU now, as ExecuteCommandLists does on D3D12: without this it
        // starts when the readback's Map asks for it, after the host's own work for the frame
        if (!getenv("RVC11_NO_FLUSH")) gpu_.ctx->Flush();
        return true;
    }
    bool rowFull() const override { return rows_.full(); }
    size_t rowPending() const override { return rows_.pending(); }
    bool popRow(std::vector<uint8_t>& out, uint64_t& tag) override {
        std::vector<uint8_t> control;
        uint64_t controlTag;
        if (!rows_.pop(gpu_.ctx.Get(), out, tag) || !control_.pop(gpu_.ctx.Get(), control, controlTag)) return false;
        out.insert(out.end(), control.begin(), control.end());
        if (!net_.pop(gpu_.ctx.Get(), control, controlTag)) return false;
        out.insert(out.end(), control.begin(), control.end());
        if (!field_.pop(gpu_.ctx.Get(), control, controlTag)) return false;
        out.insert(out.end(), control.begin(), control.end());
        mboxPop();
        watchPop();
        lastSound_.clear();
        if (!soundTags_.empty() && soundTags_.front() == tag) {
            soundTags_.pop_front();
            if (!sound_.pop(gpu_.ctx.Get(), lastSound_, controlTag)) lastSound_.clear();
        }
        return true;
    }
    bool takeSound(std::vector<uint8_t>& out) override {
        if (lastSound_.empty()) return false;
        out.swap(lastSound_);
        lastSound_.clear();
        return true;
    }

    bool uploadRows(UINT row, UINT rows, const void* texels) override {
        D3D11_BOX box{0, row, 0, kWidth, row + rows, 1};
        gpu_.ctx->UpdateSubresource(stateTex(), 0, &box, texels, kWidth * 16, 0);
        return true;
    }
    bool uploadTexel(UINT x, UINT y, const uint32_t* words) override {
        D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
        gpu_.ctx->UpdateSubresource(stateTex(), 0, &box, words, 16, 0);
        return true;
    }
    bool readRows(UINT row, UINT rows, std::vector<uint8_t>& out) override {
        RegionReadback rb;
        std::string err;
        uint64_t tag;
        if (!rb.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, kWidth, rows, 1, err)) return false;
        rb.request(gpu_.ctx.Get(), stateTex(), 0, row, 0);
        return rb.pop(gpu_.ctx.Get(), out, tag);
    }

    bool readState(UINT w, UINT h, std::vector<uint8_t>& out) override {
        RegionReadback rb;
        std::string err;
        uint64_t tag;
        if (!rb.init(gpu_.device.Get(), DXGI_FORMAT_R32G32B32A32_UINT, 16, w, h, 1, err)) return false;
        rb.request(gpu_.ctx.Get(), stateTex(), 0, 0, 0);
        return rb.pop(gpu_.ctx.Get(), out, tag);
    }

    bool gpuTimes(double& tickMs, double& commitMs, double& deviceMs) override {
        if (tsPending_) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ts[4] = {};
            const UINT fl = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if (gpu_.ctx->GetData(tsDisjoint_.Get(), &dj, sizeof dj, fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[3].Get(), &ts[3], sizeof ts[3], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[2].Get(), &ts[2], sizeof ts[2], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[1].Get(), &ts[1], sizeof ts[1], fl) == S_OK &&
                gpu_.ctx->GetData(tsQuery_[0].Get(), &ts[0], sizeof ts[0], fl) == S_OK) {
                if (!dj.Disjoint && dj.Frequency) {
                    tickMs_ = (double)(ts[1] - ts[0]) * 1000.0 / (double)dj.Frequency;
                    commitMs_ = (double)(ts[2] - ts[1]) * 1000.0 / (double)dj.Frequency;
                    deviceMs_ = (double)(ts[3] - ts[2]) * 1000.0 / (double)dj.Frequency;
                    frameMs = (double)(ts[3] - ts[0]) * 1000.0 / (double)dj.Frequency, frameRounds = timedRounds_, frameTimes++;
                    frameVariant = timedVariant_;
                }
                tsPending_ = false;
            }
        }
        tickMs = tickMs_;
        commitMs = commitMs_;
        deviceMs = deviceMs_;
        return tickMs_ >= 0;
    }

    bool readProf(std::vector<uint32_t>& out) override {
        if (!profBuf_) return false;
        out.assign(kProfCount, 0);
        gpu_.ctx->CopyResource(profStaging_.Get(), profBuf_.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.ctx->Map(profStaging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        memcpy(out.data(), m.pData, kProfCount * 4);
        gpu_.ctx->Unmap(profStaging_.Get(), 0);
        return true;
    }

    std::string deviceRemoved() override { return gpu_.deviceRemovedReason(); }

    bool viewInit(std::string& err) override { return view_.init(gpu_.device.Get(), kWidth, kHeight, 64, err); }
    bool viewOpen() const override { return view_.open(); }
    void viewRender(bool present) override { view_.render(gpu_.ctx.Get(), stateSrv(), present, gpuSrv_.Get()); }
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
    void viewText(const std::vector<std::vector<std::string>>& columns) override { view_.setText(gpu_.ctx.Get(), columns); }
    void viewTitle(const std::string& title) override { view_.setTitle(title); }
    bool viewCapture(const std::string& path) override { return view_.capture(gpu_.ctx.Get(), path); }
    void viewClose() override { view_.close(); }

private:
    // Hidden swapchain, only to give external profilers a Present per frame.
    bool createSwapChain(std::string& err) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"rvc_harness_present";
        RegisterClassW(&wc);
        presentWnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"rvc_harness", WS_POPUP,
                                      0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);  // never shown
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 1;
        sd.OutputWindow = presentWnd_;
        sd.Windowed = TRUE;
        HRESULT hr = presentWnd_ ? gpu_.device.As(&dxgiDev) : E_FAIL;
        if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(gpu_.device.Get(), &sd, &swapChain_);
        if (FAILED(hr)) err = "creating the hidden swapchain failed: " + hrToString(hr);
        return SUCCEEDED(hr);
    }

    // Profiling counters: a uint buffer the tick pass increments through a UAV.
    bool createProfile(Material& mat, std::string& err) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kProfCount * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        HRESULT hr = gpu_.device->CreateBuffer(&bd, nullptr, &profBuf_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(profBuf_.Get(), nullptr, &profUav_);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateBuffer(&bd, nullptr, &profStaging_);
        if (FAILED(hr)) {
            err = "creating the profile buffer failed: " + hrToString(hr);
            return false;
        }
        const UINT zero[4] = {0, 0, 0, 0};
        gpu_.ctx->ClearUnorderedAccessViewUint(profUav_.Get(), zero);
        mat.setUav("_Prof", profUav_.Get());
        return true;
    }

    // The GPU device's colour and depth target, with the fixed depth test of its draw pass.
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
        // depth: tested and written, tested only, unused; blending: none, alpha, additive, multiply
        for (int k = 0; k < 3 && SUCCEEDED(hr); ++k) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = k < 2;
            dd.DepthWriteMask = k == 0 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            hr = gpu_.device->CreateDepthStencilState(&dd, &gpuDepthState_[k]);
        }
        for (int k = 0; k < 4 && SUCCEEDED(hr); ++k) {
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

    // Draws the GPU device's mesh. Depth starts fresh every frame; colour is kept, so the
    // picture stays until the guest submits another list.
    void gpuDraw(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = gpuPasses_[0];
        ctx->OMSetRenderTargets(1, gpuRtv_.GetAddressOf(), gpuDsv_.Get());
        ctx->ClearDepthStencilView(gpuDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{0, 0, (float)kGpuTarget, (float)kGpuTarget, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11ShaderResourceView* state = stateSrv();
        // one draw per pass in use, in order: each has its own blending and depth use
        for (int p = 0; p < 8; ++p) {
            if (!((gpuPasses >> p) & 1)) continue;
            mat.setInt("_GpuPass", p);
            mat.bind(ctx, pass, gpu_);
            // the state texture; the ROM's textures are the material's and already bound (the
            // vertex shader has them too: packed vertices may be a file's bytes in the ROM)
            for (auto& t : pass.vsLayout.textures)
                if (t.name == "_State") ctx->VSSetShaderResources(t.slot, 1, &state);
            for (auto& t : pass.psLayout.textures)
                if (t.name == "_State") ctx->PSSetShaderResources(t.slot, 1, &state);
            ctx->OMSetBlendState(gpuBlend_[p & 3].Get(), nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(gpuDepthState_[p == 0 ? 0 : p < 4 ? 1 : 2].Get(), 0);
            ctx->Draw(kGpuTriangles * 3, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        for (auto& t : pass.vsLayout.textures) ctx->VSSetShaderResources(t.slot, 1, &none);
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // The sound card's mix target: two floats a sample, read back whole.
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

    // Draws the mix from the committed state and queues its readback.
    void soundDraw(Material& mat, uint64_t tag) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = soundPasses_[0];
        ctx->OMSetRenderTargets(1, soundRtv_.GetAddressOf(), nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)kSoundSide, (float)kSoundSide, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mat.bind(ctx, pass, gpu_);
        ID3D11ShaderResourceView* state = stateSrv();
        for (auto& t : pass.psLayout.textures)
            if (t.name == "_State") ctx->PSSetShaderResources(t.slot, 1, &state);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        for (auto& t : pass.psLayout.textures) ctx->PSSetShaderResources(t.slot, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        sound_.request(ctx, soundTex_.Get(), 0, 0, tag);
        soundTags_.push_back(tag);
    }

    bool createTickTargets(std::string& err) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = tickWidth();
        td.Height = tickHeight() * mrtLoad_;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 8; ++i) {
            HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, &mrtTex_[i]);
            if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(mrtTex_[i].Get(), nullptr, &mrtRtv_[i]);
            if (SUCCEEDED(hr)) hr = gpu_.device->CreateShaderResourceView(mrtTex_[i].Get(), nullptr, &mrtSrv_[i]);
            if (FAILED(hr)) {
                err = "tick targets: " + hrToString(hr);
                return false;
            }
        }
        return true;
    }

    // The tick's primitives: 16 cores each, and with the lanes' field drawn one after all 256 cores' (its own).
    bool fieldDraw_ = false;
    int fieldSeen_ = 0;
    UINT tickPrims() const { return fieldDraw_ ? (UINT)(kCoresMost / 16 + 1) : (activeCores_ + 15) / 16; }

    // The tick with eight targets: the state is read where it is, as ever.
    void tickDraw(Material& mat, const UpdateZone& z) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        GpuPass& pass = passes_[0];
        mat.setVector("CustomRenderTextureCenters", z.centerX, z.centerY, 0.5, 0);
        mat.setVector("CustomRenderTextureSizesAndRotations", z.width, z.height, 1, 0);
        mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
        mat.setVector("CustomRenderTextureParameters", 1, 0, 0, 0);
        mat.setVector("_CustomRenderTextureInfo", kWidth, kHeight, 1, 0);
        mat.setTexture("_SelfTexture2D", stateSrv(), kWidth, kHeight);
        ID3D11ShaderResourceView* nulls[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        ctx->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ctx->GSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ID3D11RenderTargetView* rtvs[8];
        for (int i = 0; i < 8; ++i) rtvs[i] = mrtRtv_[i].Get();
        ctx->OMSetRenderTargets(8, rtvs, nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)tickWidth(), (float)(tickHeight() * mrtLoad_), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        if (const char* sc = getenv("RVC_TICK_SCISSOR")) {
            // a timing experiment: only these pixels of the tick are drawn (the machine does not go on)
            static ComPtr<ID3D11RasterizerState> rs;
            if (!rs) {
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_NONE;
                rd.DepthClipEnable = TRUE;
                rd.ScissorEnable = TRUE;
                gpu_.device->CreateRasterizerState(&rd, &rs);
            }
            int x0 = 0, y0 = 0, x1 = 1, y1 = 1;
            sscanf(sc, "%d %d %d %d", &x0, &y0, &x1, &y1);
            D3D11_RECT r{x0, y0, x1, y1};
            ctx->RSSetScissorRects(1, &r);
            ctx->RSSetState(rs.Get());
        }
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Material::dumpConstants = true;
        mat.bind(ctx, pass, gpu_);
        Material::dumpConstants = false;
        ctx->Draw((std::max)(6u, 3 * mrtLoad_ * tickPrims()), 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        static const char* names[8] = {"_TickOut0", "_TickOut1", "_TickOut2", "_TickOut3", "_TickOut4", "_TickOut5", "_TickOut6", "_TickOut7"};
        for (int i = 0; i < 8; ++i) mat.setTexture(names[i], mrtSrv_[i].Get(), tickWidth(), tickHeight());
    }

    // ---- RAM in one texture (RAM_INPLACE, src/ram.h) ----
    // R: the state rows and RAM, written in place; A: the 64 state rows after the tick; T: the
    // stage, what the commit's gather leaves its other passes; Z: a control zone's rows, drawn
    // there from R and copied back.
    static const UINT kStageRows = 21, kZoneRows = 8;   // STAGE_ROWS in src/ram.h; the tallest zone
    bool inplace_ = false;
    bool tickDirect_ = false;
    bool laneField_ = false;   // the tick has the lanes' field: its primitive and points while fieldNow
    // --replay: the tick variants (variant 0 is the machine's own), each its tick and its unpack
    ShaderBuildOptions tickBo_, unpackBo_;
    std::vector<std::pair<GpuPass, GpuPass>> tickVariants_;
    size_t curVariant_ = 0, timedVariant_ = 0;   // the variant in place, and the one of the frame being timed
    bool addTickVariant(const SLShader& shader, std::string& err) override {
        std::vector<GpuPass> t, u;
        if (!buildPasses(gpu_, shader, {"CPUTick"}, tickBo_, t, err)) return false;
        if (mrt_ && !buildPasses(gpu_, shader, {"CPUTick"}, unpackBo_, u, err)) return false;
        tickVariants_.push_back({t[0], mrt_ ? u[0] : GpuPass{}});
        return true;
    }
    bool useTickVariant(size_t i) override {
        if (i >= tickVariants_.size()) return false;
        curVariant_ = i;
        passes_[0] = tickVariants_[i].first;
        if (mrt_) unpackPasses_[0] = tickVariants_[i].second;
        return true;
    }
    std::vector<GpuPass> gatherPasses_, ramPasses_;
    ComPtr<ID3D11Texture2D> ram_, tickState_, stage_, zone_;
    ComPtr<ID3D11ShaderResourceView> ramSrv_, tickStateSrv_, stageSrv_;
    ComPtr<ID3D11RenderTargetView> ramRtv_, tickStateRtv_, stageRtv_, zoneRtv_;
    ID3D11Texture2D* stateTex() const { return inplace_ ? ram_.Get() : crt_.current(); }
    ID3D11ShaderResourceView* stateSrv() const { return inplace_ ? ramSrv_.Get() : crt_.currentSRV(); }

    bool createInplace(std::string& err) {
        struct T { ComPtr<ID3D11Texture2D>* tex; UINT rows; ComPtr<ID3D11ShaderResourceView>* srv; ComPtr<ID3D11RenderTargetView>* rtv; };
        T all[4] = {{&ram_, kHeight, &ramSrv_, &ramRtv_}, {&tickState_, 64, &tickStateSrv_, &tickStateRtv_},
                    {&stage_, kStageRows, &stageSrv_, &stageRtv_}, {&zone_, kZoneRows, nullptr, &zoneRtv_}};
        for (T& t : all) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kWidth;
            td.Height = t.rows;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | (t.srv ? D3D11_BIND_SHADER_RESOURCE : 0);
            HRESULT hr = gpu_.device->CreateTexture2D(&td, nullptr, t.tex->GetAddressOf());
            if (SUCCEEDED(hr) && t.srv) hr = gpu_.device->CreateShaderResourceView(t.tex->Get(), nullptr, t.srv->GetAddressOf());
            if (SUCCEEDED(hr)) hr = gpu_.device->CreateRenderTargetView(t.tex->Get(), nullptr, t.rtv->GetAddressOf());
            if (FAILED(hr)) {
                err = "RAM_INPLACE textures: " + hrToString(hr);
                return false;
            }
        }
        const float zero[4] = {0, 0, 0, 0};
        for (auto* r : {ramRtv_.Get(), tickStateRtv_.Get(), stageRtv_.Get()}) gpu_.ctx->ClearRenderTargetView(r, zero);
        fprintf(stderr, "[harness] RAM in one texture: %.0f MiB of state and RAM, %.1f MiB beside it\n",
                kWidth * (double)kHeight * 16 / 1048576.0, kWidth * (64.0 + kStageRows + kZoneRows) * 16 / 1048576.0);
        return true;
    }

    // One draw into `rtv`, its row 0 at the viewport's row `top` (a viewport as tall as the
    // state texture unless `rows` says, so that the passes' own coordinates hold), with R as _SelfTexture2D.
    void drawInto(GpuPass& pass, Material& mat, ID3D11RenderTargetView* rtv, float top, UINT vertices,
                  D3D11_PRIMITIVE_TOPOLOGY topology, const UpdateZone* z = nullptr, UINT rows = kHeight) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        if (z) {
            mat.setVector("CustomRenderTextureCenters", z->centerX, z->centerY, 0.5, 0);
            mat.setVector("CustomRenderTextureSizesAndRotations", z->width, z->height, 1, 0);
            mat.setFloat("CustomRenderTexturePrimitiveIDs", 0);
            mat.setVector("CustomRenderTextureParameters", 1, 0, 0, 0);
        }
        mat.setVector("_CustomRenderTextureInfo", kWidth, kHeight, 1, 0);
        mat.setTexture("_SelfTexture2D", ramSrv_.Get(), kWidth, kHeight);
        ID3D11ShaderResourceView* nulls[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        ctx->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ctx->GSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nulls);
        ID3D11UnorderedAccessView* uav = pass.psLayout.uavs.empty() ? nullptr : mat.uav(pass.psLayout.uavs[0].name);
        if (uav)
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, pass.psLayout.uavs[0].slot, 1, &uav, nullptr);
        else
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0, -top, (float)kWidth, (float)rows, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(gpu_.rasterNoCull.Get());
        ctx->OMSetBlendState(gpu_.blendOpaque.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(gpu_.depthOff.Get(), 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(topology);
        mat.bind(ctx, pass, gpu_);
        ctx->Draw(vertices, 0);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    // A control zone: drawn into Z from R, then copied into R.
    void zoneInplace(Material& mat, const float* zf) {
        UpdateZone z{zf[0], zf[1], zf[2], zf[3], 1};
        LONG x0 = (std::max)(0L, (LONG)std::lround(z.centerX - z.width * 0.5f));
        LONG y0 = (std::max)(0L, (LONG)std::lround((float)kHeight - (z.centerY + z.height * 0.5f)));
        LONG x1 = (std::min)((LONG)kWidth, x0 + (LONG)std::lround(z.width));
        LONG y1 = (std::min)((LONG)kHeight, y0 + (LONG)std::lround(z.height));
        if (x1 <= x0 || y1 <= y0 || y1 - y0 > (LONG)kZoneRows) return;
        drawInto(gpuPasses_[1], mat, zoneRtv_.Get(), (float)y0, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &z);
        D3D11_BOX box{(UINT)x0, 0, 0, (UINT)x1, (UINT)(y1 - y0), 1};
        gpu_.ctx->CopySubresourceRegion(ram_.Get(), 0, (UINT)x0, (UINT)y0, 0, zone_.Get(), 0, &box);
    }

    // Two zones of a row each (the control words, then the cores' stats) in one draw: A's texels and then B's in the
    // zone texture's first row, the pixel shader placing B's (_ZoneSplit), then a copy each. The stats are read
    // from the state as it was before the control words change, as when they were drawn after them: no pixel of
    // either reads the other's texels.
    void zonePairInplace(Material& mat, const float* za, const float* zb) {
        static const bool apart = getenv("RVC_ZONES_APART") && atoi(getenv("RVC_ZONES_APART")) != 0;
        auto box = [](const float* zf, LONG& x0, LONG& y0, LONG& w) {
            x0 = (std::max)(0L, (LONG)std::lround(zf[0] - zf[2] * 0.5f));
            y0 = (std::max)(0L, (LONG)std::lround((float)kHeight - (zf[1] + zf[3] * 0.5f)));
            w = (LONG)std::lround(zf[2]);
        };
        LONG ax, ay, aw, bx, by, bw;
        box(za, ax, ay, aw);
        box(zb, bx, by, bw);
        if (apart || za[3] != 1 || zb[3] != 1 || ax + aw + bw > (LONG)kWidth) {
            zoneInplace(mat, za);
            zoneInplace(mat, zb);
            return;
        }
        UpdateZone z{ax + (aw + bw) * 0.5f, (float)kHeight - ay - 0.5f, (float)(aw + bw), 1, 1};
        mat.setInt("_ZoneSplit", (int)(ax + aw));
        mat.setInt("_Zone2X", (int)bx);
        mat.setInt("_Zone2Y", (int)by);
        drawInto(gpuPasses_[1], mat, zoneRtv_.Get(), (float)ay, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &z);
        mat.setInt("_ZoneSplit", 0);
        D3D11_BOX a{(UINT)ax, 0, 0, (UINT)(ax + aw), 1, 1}, b{(UINT)(ax + aw), 0, 0, (UINT)(ax + aw + bw), 1, 1};
        gpu_.ctx->CopySubresourceRegion(ram_.Get(), 0, (UINT)ax, (UINT)ay, 0, zone_.Get(), 0, &a);
        gpu_.ctx->CopySubresourceRegion(ram_.Get(), 0, (UINT)bx, (UINT)by, 0, zone_.Get(), 0, &b);
    }

    bool frameInplace(Material& mat, uint64_t tag, bool timeIt) {
        timeIt = timeIt && ensureQueries() && !tsPending_;
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        bool init = mat.getFloat("_Init") != 0;
        if (timeIt) {
            ctx->Begin(tsDisjoint_.Get());
            ctx->End(tsQuery_[0].Get());
            timedRounds_ = init ? 1u : (std::max)(1u, tickRounds);
            timedVariant_ = curVariant_;
        }
        // the tick, from R; its state rows into A, which holds R's state rows for the rest
        bool ticks = !(cpuMode && !init);
        UpdateZone core0{32, kHeight - tickRows_ / 2, 64, tickRows_, 0};
        static const char* const kInplaceLabels[] = {"tick", "unpack", "gather", "commit rows", "commit points", "devices", "", "", "", "", "", ""};
        if (getenv("RVC_PASS_PROF") && !init) { prof_.labels = kInplaceLabels; profBegin(tag); }
        // --tick-rounds N (and --unity): the tick and its commit N times before the devices, a tick of the clock on each round
        const uint32_t rounds = ticks && !init ? (std::max)(1u, tickRounds) : 1u;
        if (ticks) {
            // (while a job has been seen in the last 8 frames, not rounds: the readback is frames behind, and a program's jobs
            // come one after another; with no job posted the field's primitive draws no pixel, but its 7,936 points cost a pass)
            fieldSeen_ = fieldNow ? 8 : fieldSeen_ > 0 ? fieldSeen_ - 1 : 0;
            fieldDraw_ = laneField_ && fieldSeen_ > 0;
        }
        for (uint32_t round = 0; round < rounds; round++) {
            if (rounds > 1) { mat.setInt("_TimePass", (int)round); mat.setInt("_TickFull", (int)(tickSplit ? tickSplit : rounds)); }
            if (ticks) {
                bool workers = cores_ > 1 && (workersNow || getenv("RVC_WORKERS_ALWAYS"));
                activeCores_ = workers ? (UINT)cores_ : 1;
                if (laneField_) mat.setInt("_LaneField", 1);
                if (mrt_) tickDraw(mat, core0);
            }
            prof_.mark(ctx);
            // RVC_FREEZE: as frame()'s, the tick alone, its state not written back (every pass the same instructions)
            static const bool freezeAsked = getenv("RVC_FREEZE") != nullptr;
            if (freezeAsked && !init) {
                profEnd();
                rows_.request(ctx, ram_.Get(), 0, 0, tag);
                control_.request(ctx, ram_.Get(), 0, kControlRow, tag);
                net_.request(ctx, ram_.Get(), 0, kNetRow, tag);
                field_.request(ctx, ram_.Get(), kFieldAt, kFieldRow, tag);
            mboxRequest(ram_.Get(), tag);
                if (!getenv("RVC11_NO_FLUSH")) ctx->Flush();
                return true;
            }
            D3D11_BOX rows{0, 0, 0, kWidth, 64, 1};
            ctx->CopySubresourceRegion(tickState_.Get(), 0, 0, 0, 0, ram_.Get(), 0, &rows);
            if (ticks) drawInto(mrt_ ? unpackPasses_[0] : passes_[0], mat, tickStateRtv_.Get(), 0,
                                (std::max)(6u, 3 * tickPrims()), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &core0);
            prof_.mark(ctx);
            if (timeIt && round == rounds - 1) ctx->End(tsQuery_[1].Get());
            // the commit: the gather into T, then the state rows and the points into R
            if (!gpuPasses_.empty()) mat.setTexture("_GpuTarget", gpuSrv_.Get(), kGpuTarget, kGpuTarget);
            mat.setTexture("_TickState", tickStateSrv_.Get(), kWidth, 64);
            mat.setTexture("_Ram", ramSrv_.Get(), kWidth, kHeight);
            drawInto(gatherPasses_[0], mat, stageRtv_.Get(), 0, 1 + activeCores_, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, nullptr, kStageRows);
            prof_.mark(ctx);
            mat.setTexture("_Stage", stageSrv_.Get(), kWidth, kStageRows);
            drawInto(passes_[1], mat, ramRtv_.Get(), 0, 6, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            prof_.mark(ctx);
            bool field = ticks && fieldDraw_;
            UINT points = 1 + 258 + (activeCores_ - 1) * 130 + (field ? kFieldPoints : 0) + 1;   // ram_commit_geom: the zap, the cores' places, the field's buckets, the picture
            mat.setInt("_CommitPoints", points);
            if (laneField_) mat.setInt("_FieldPoints", field ? kFieldPoints : 0);
            drawInto(ramPasses_[0], mat, ramRtv_.Get(), 0, points, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            prof_.mark(ctx);
        }
        if (timeIt) ctx->End(tsQuery_[2].Get());
        if (gpuPasses_.size() == 2) {
            bool listDrawn = !listLazy() || listDue > 0;   // (RVC_LIST_LAZY: only while a list is due)
            if (listDrawn) {
                gpuDraw(mat);
                if (listDue > 0) listDue--;
            }
            mat.setInt("_GpuListSkipped", listDrawn ? 0 : 1);   // (the control zone takes the submit word back only when drawn)
            bool mixed = soundMix && !soundPasses_.empty();
            if (mixed) soundDraw(mat, tag);   // before the control zone moves the voices on
            bool merge = countsAdded && !cpuMode;
            if (merge) {
                zoneInplace(mat, kCountZone);
                countsAdded = false;
            }
            mat.setInt("_CountMerged", merge ? 2 : 1);
            // (the control words and, with workers, the cores' stats in one draw: gpu_vm's feaddd5; RVC_ZONES_APART=1 two)
            if (cores_ > 16 && activeCores_ > 1) zonePairInplace(mat, kGpuControlZone, kCoreStatsZone);
            else zoneInplace(mat, kGpuControlZone);
            if (conDelivered) {
                zoneInplace(mat, kConZone);
                conDelivered = false;
            }
            if (deliver_) {
                zoneInplace(mat, kFetchZone);
                deliver_ = false;
            }
            if (deliverNet_) {
                zoneInplace(mat, kNetZone);
                deliverNet_ = false;
            }
        }
        profEnd();
        if (timeIt) {
            ctx->End(tsQuery_[3].Get());
            ctx->End(tsDisjoint_.Get());
            tsPending_ = true;
        }
        if (swapChain_) {
            swapChain_->Present(0, 0);
            MSG msg;
            while (PeekMessageW(&msg, presentWnd_, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
        rows_.request(ctx, ram_.Get(), 0, 0, tag);
        control_.request(ctx, ram_.Get(), 0, kControlRow, tag);
        net_.request(ctx, ram_.Get(), 0, kNetRow, tag);
        field_.request(ctx, ram_.Get(), kFieldAt, kFieldRow, tag);
        mboxRequest(ram_.Get(), tag);
        if (!getenv("RVC11_NO_FLUSH")) ctx->Flush();
        return true;
    }

    static bool listLazy() { static const bool on = getenv("RVC_LIST_LAZY") && atoi(getenv("RVC_LIST_LAZY")) != 0; return on; }
    bool ensureQueries() {
        if (!tsDisjoint_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            gpu_.device->CreateQuery(&qd, &tsDisjoint_);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& q : tsQuery_) gpu_.device->CreateQuery(&qd, &q);
        }
        return tsDisjoint_ && tsQuery_[3];
    }

    // RVC_COMPUTE_PROF=1: a frame in 16 timed on the GPU at each step of --compute's work, the means
    // printed every 200 of them (experiments/rv32_min/README.md); RVC_PASS_PROF=1 the same for the
    // fragment machine's passes (the tick, its unpack, the commit's draws, the devices)
    struct ComputeProf {
        static const int kMarks = 12;
        ComPtr<ID3D11Query> disjoint, marks[kMarks];
        bool on = false, pending = false;
        int at = 0;
        double sum[kMarks] = {}, n = 0;
        const char* names[kMarks] = {"cores", "merge_list", "merge", "count+state", "export", "commit+devices", "picture", "texels", "sync_cores", "", "", ""};
        const char* const* labels = nullptr;   // RVC_PASS_PROF: the fragment machine's passes instead
        void mark(ID3D11DeviceContext* ctx) { if (on && at < kMarks) ctx->End(marks[at++].Get()); }
        ~ComputeProf() { print(); }
        void print() {
            if (n == 0) return;
            fprintf(stderr, "[prof] %.0f frames:", n);
            const char* const* l = labels ? labels : names;
            for (int k = 1; k < kMarks && l[k - 1][0]; k++) fprintf(stderr, " %s %.3f ms", l[k - 1], sum[k] / n);
            fputc('\n', stderr);
        }
    } prof_;
    void profBegin(uint64_t frame) {
        static const bool asked = getenv("RVC_COMPUTE_PROF") != nullptr || getenv("RVC_PASS_PROF") != nullptr;
        prof_.on = false;
        if (!asked) return;
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        if (!prof_.disjoint) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            gpu_.device->CreateQuery(&qd, &prof_.disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& q : prof_.marks) gpu_.device->CreateQuery(&qd, &q);
        }
        if (prof_.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 ts[ComputeProf::kMarks] = {};
            bool ok = ctx->GetData(prof_.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            for (int k = 0; ok && k < prof_.at; k++) ok = ctx->GetData(prof_.marks[k].Get(), &ts[k], sizeof ts[k], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!ok) return;
            prof_.pending = false;
            if (!dj.Disjoint && dj.Frequency) {
                for (int k = 1; k < prof_.at; k++) prof_.sum[k] += (double)(ts[k] - ts[k - 1]) * 1000.0 / (double)dj.Frequency;
                prof_.n++;
                if (prof_.n >= 200) { prof_.print(); for (auto& v : prof_.sum) v = 0; prof_.n = 0; }
            }
        }
        if ((frame & 15) != 0) return;
        prof_.on = true;
        prof_.at = 0;
        ctx->Begin(prof_.disjoint.Get());
        prof_.mark(ctx);
    }
    void profEnd() {
        if (!prof_.on) return;
        prof_.mark(gpu_.ctx.Get());
        gpu_.ctx->End(prof_.disjoint.Get());
        prof_.on = false;
        prof_.pending = true;
    }

    Gpu gpu_;
    std::vector<GpuPass> passes_, gpuPasses_, soundPasses_, unpackPasses_, pointPasses_, copyPasses_;
    bool scatter_ = true;   // the cores' stores committed as points (with the tick in eight targets; --define NO_SCATTER: as bands)
    // TICK_MRT: the tick's eight targets (types.h: TICK_MRT_W wide, a band of rows for 128 workers)
    bool mrt_ = false;
    UINT mrtLoad_ = 1;   // TICK_LOAD: the workers' quads drawn so many times (a measurement)
    UINT activeCores_ = 1;   // the cores whose pixels are drawn this frame: all, or core 0 alone (workersNow)
    UINT tickWidth() const { return 1040; }   // TICK_MRT_W
    UINT tickHeight() const { return (UINT)tickRows_ * (((UINT)cores_ + 126) / 128); }   // TICK_BANDS
    ComPtr<ID3D11Texture2D> mrtTex_[8];
    ComPtr<ID3D11RenderTargetView> mrtRtv_[8];
    ComPtr<ID3D11ShaderResourceView> mrtSrv_[8];
    ComPtr<ID3D11Texture2D> soundTex_;
    ComPtr<ID3D11RenderTargetView> soundRtv_;
    RegionReadback sound_;
    std::deque<uint64_t> soundTags_;   // frames whose mix is still to be read back
    std::vector<uint8_t> lastSound_;
    ComPtr<ID3D11Texture2D> gpuColor_;
    ComPtr<ID3D11RenderTargetView> gpuRtv_;
    ComPtr<ID3D11ShaderResourceView> gpuSrv_;
    ComPtr<ID3D11DepthStencilView> gpuDsv_;
    ComPtr<ID3D11DepthStencilState> gpuDepthState_[3];
    ComPtr<ID3D11BlendState> gpuBlend_[4];
    std::vector<ComPtr<ID3D11ShaderResourceView>> keep_;
    CustomRenderTexture crt_;
    RegionReadback rows_, control_, net_, field_, mbox_;
    ComPtr<ID3D11ShaderResourceView> netData_;
    bool deliverNet_ = false;   // the ring's zone is drawn in the next frame

    // RVC11_COMPUTE=1 (experiments/rvc_compute): the tick as one thread of a compute shader, the
    // pass's entry tick_cs. It writes the CPU zone's texels into a 64 x 64 texture of its own,
    // which holds the zone before the dispatch and is copied back after it.
    ComPtr<ID3D11ComputeShader> cs_;
    bool csDirect_ = false, csBuffer_ = false, ramImported_ = false;
    static const UINT kRamTexels = 2048 * (kHeight - 64);
    ComPtr<ID3D11ComputeShader> csImport_;
    StageLayout csImportLayout_;
    ComPtr<ID3D11Buffer> ramBuf_;
    ComPtr<ID3D11UnorderedAccessView> ramUav_;
    StageLayout csLayout_;
    ComPtr<ID3D11Texture2D> tickTex_;
    ComPtr<ID3D11UnorderedAccessView> tickUav_;
    bool buildComputeTick(const SLShader& shader, const ShaderBuildOptions& bo, std::string& err) {
        const SLPass* p = shader.findPass("CPUTick");
        if (!p) { err = "no CPUTick pass"; return false; }
        std::string rootDir = std::filesystem::u8path(shader.path).parent_path().u8string();
        Defines extra = {{"SHADER_STAGE_COMPUTE", "1"}};
        StageResult r = compileStage(p->code, shader.path, rootDir, "tick_cs", "cs_5_0", extra, bo.settings);
        if (!r.ok) { err = "tick_cs: " + r.log; return false; }
        fprintf(stderr, "[harness] compute tick: %s in %.1fs (%zu bytes)\n", r.fromCache ? "cache hit" : "compiled", r.seconds,
                (size_t)r.bytecode->GetBufferSize());
        HRESULT hr = gpu_.device->CreateComputeShader(r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize(), nullptr, &cs_);
        if (FAILED(hr)) { err = "CreateComputeShader: " + hrToString(hr); return false; }
        if (!csLayout_.reflect(gpu_.device.Get(), r.bytecode.Get(), err)) return false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        hr = gpu_.device->CreateTexture2D(&td, nullptr, &tickTex_);
        if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(tickTex_.Get(), nullptr, &tickUav_);
        if (FAILED(hr)) { err = "compute tick target: " + hrToString(hr); return false; }
        if (csBuffer_) {
            // RAM_BUFFER: RAM as a raw buffer (u1), filled from the state texture by ram_import_cs
            // before the first compute tick
            StageResult ri = compileStage(p->code, shader.path, rootDir, "ram_import_cs", "cs_5_0", extra, bo.settings);
            if (!ri.ok) { err = "ram_import_cs: " + ri.log; return false; }
            hr = gpu_.device->CreateComputeShader(ri.bytecode->GetBufferPointer(), ri.bytecode->GetBufferSize(), nullptr, &csImport_);
            if (FAILED(hr)) { err = "CreateComputeShader (import): " + hrToString(hr); return false; }
            if (!csImportLayout_.reflect(gpu_.device.Get(), ri.bytecode.Get(), err)) return false;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kRamTexels * 16;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            hr = gpu_.device->CreateBuffer(&bd, nullptr, &ramBuf_);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kRamTexels * 4;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (SUCCEEDED(hr)) hr = gpu_.device->CreateUnorderedAccessView(ramBuf_.Get(), &ud, &ramUav_);
            if (FAILED(hr)) { err = "RAM buffer: " + hrToString(hr); return false; }
        }
        return true;
    }
    void runComputeTick(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        mat.setVector("_CustomRenderTextureInfo", crt_.width(), crt_.height(), 1, 0);
        mat.setTexture("_SelfTexture2D", crt_.currentSRV(), crt_.width(), crt_.height());
        D3D11_BOX box{0, 0, 0, 64, 64, 1};
        // RAM_DIRECT: the shader has the state texture itself, to read and to write
        if (!csDirect_) ctx->CopySubresourceRegion(tickTex_.Get(), 0, 0, 0, 0, crt_.current(), 0, &box);
        ctx->CSSetShader(cs_.Get(), nullptr, 0);
        mat.bindCompute(ctx, csLayout_, gpu_);
        ID3D11UnorderedAccessView* uavs[2] = {csDirect_ ? crt_.currentUAV() : tickUav_.Get(), ramUav_.Get()};
        if (csBuffer_ && !ramImported_) {
            // every RAM texel of the state texture into the buffer: 65,536 a row of thread groups
            ctx->CSSetShader(csImport_.Get(), nullptr, 0);
            mat.bindCompute(ctx, csImportLayout_, gpu_);
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            ctx->Dispatch(1024, (kRamTexels + 65535) / 65536, 1);
            ctx->CSSetShader(cs_.Get(), nullptr, 0);
            mat.bindCompute(ctx, csLayout_, gpu_);
            ramImported_ = true;
        }
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* none[2] = {};
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        ID3D11ShaderResourceView* nulls[16] = {};
        ctx->CSSetShaderResources(0, 16, nulls);
        if (!csDirect_) ctx->CopySubresourceRegion(crt_.current(), 0, 0, 0, 0, tickTex_.Get(), 0, &box);
    }
    // --compute: the processor (rv32_min*.hlsl's tick) and sync.hlsl's passes beside it, RAM and
    // the processor's words in buffers of their own, the ROM as a raw view (rvc_backend.h).
    ComPtr<ID3D11ComputeShader> csCpu_, csMerge_, csMergeList_, csCount_, csState_, csExport_, csExportPages_, csPlan_, csTake_, csCores_;
    ComPtr<ID3D11Buffer> cpuRam_, cpuState_, cpuRom_, cpuPass_, cpuSync_, cpuStateRead_, cpuDec_, cpuMarks_, cpuArgs_;
    ComPtr<ID3D11UnorderedAccessView> cpuRamUav_, cpuStateUav_, cpuDecUav_, cpuMarksUav_, cpuArgsUav_;
    ComPtr<ID3D11ShaderResourceView> cpuRomSrv_;
    uint32_t cpuSeed_ = 0x9e3779b9u, computeFrames_ = 0;
    static const UINT kCpuStateBytes = 512;   // a core's words
    static const UINT kMarkBytes = (3 * 73728 + 128 + 1024 + 32768) * 4;   // page marks, band marks, the pages to merge, the frame's (rv32_min_fast.hlsl, M_)
    static const UINT kArgsBytes = 48;   // merge's dispatch, export_pages's, the next pass's
    static ComPtr<ID3D11Buffer> makeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, UINT misc, D3D11_USAGE usage, UINT cpu,
                                           const void* init, std::string& err) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = usage;
        d.BindFlags = bind;
        d.MiscFlags = misc;
        d.CPUAccessFlags = cpu;
        D3D11_SUBRESOURCE_DATA s{init, 0, 0};
        ComPtr<ID3D11Buffer> b;
        HRESULT hr = dev->CreateBuffer(&d, init ? &s : nullptr, &b);
        if (FAILED(hr)) err = "a buffer of " + std::to_string(bytes) + " bytes: " + hrToString(hr);
        return b;
    }
    bool buildComputeCpu(const std::string& path, const ShaderBuildOptions& bo, std::string& err) {
        fs::path cpuPath = fs::u8path(path), syncPath = cpuPath.parent_path() / "sync.hlsl";
        if (!fs::exists(syncPath)) syncPath = fs::u8path("experiments/rv32_min/sync.hlsl");
        std::string cpuSource, syncSource;
        if (!readFileBinary(cpuPath.u8string(), cpuSource)) { err = "--compute: no shader " + path; return false; }
        if (!readFileBinary(syncPath.u8string(), syncSource)) { err = "--compute: no " + syncPath.u8string(); return false; }
        struct Entry { const std::string* source; fs::path file; const char* entry; ComPtr<ID3D11ComputeShader>* cs; };
        Entry entries[] = {{&cpuSource, cpuPath, "tick", &csCpu_}, {&syncSource, syncPath, "sync_count", &csCount_},
                           {&syncSource, syncPath, "sync_state", &csState_}, {&syncSource, syncPath, "export_rows", &csExport_},
                           {&syncSource, syncPath, "import_plan", &csPlan_}, {&syncSource, syncPath, "import_rows", &csTake_},
                           {&syncSource, syncPath, "sync_cores", &csCores_}, {&syncSource, syncPath, "export_pages", &csExportPages_}};
        // the merge after every pass (rv32_min_fast.hlsl's; the plain shader has one image and none)
        if (cpuSource.find("void merge(") != std::string::npos) {
            for (const char* entry : {"merge", "merge_list"}) {
                StageResult r = compileStage(cpuSource, cpuPath.u8string(), cpuPath.parent_path().u8string(), entry, "cs_5_0", {}, bo.settings);
                if (!r.ok) { err = cpuPath.u8string() + " (" + entry + "): " + r.log; return false; }
                HRESULT hr = gpu_.device->CreateComputeShader(r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize(), nullptr,
                                                              strcmp(entry, "merge") == 0 ? &csMerge_ : &csMergeList_);
                if (FAILED(hr)) { err = std::string("CreateComputeShader (") + entry + "): " + hrToString(hr); return false; }
            }
        }
        for (auto& e : entries) {
            StageResult r = compileStage(*e.source, e.file.u8string(), e.file.parent_path().u8string(), e.entry, "cs_5_0", {}, bo.settings);
            if (!r.ok) { err = e.file.u8string() + " (" + e.entry + "): " + r.log; return false; }
            HRESULT hr = gpu_.device->CreateComputeShader(r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize(), nullptr, e.cs->GetAddressOf());
            if (FAILED(hr)) { err = std::string("CreateComputeShader (") + e.entry + "): " + hrToString(hr); return false; }
            if (strcmp(e.entry, "tick") == 0)
                fprintf(stderr, "[harness] the processor as a compute shader: %s, %s in %.2f s (%zu bytes)\n", path.c_str(),
                        r.fromCache ? "cache hit" : "compiled", r.seconds, (size_t)r.bytecode->GetBufferSize());
        }
        ID3D11Device* dev = gpu_.device.Get();
        cpuPass_ = makeBuffer(dev, 48, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, nullptr, err);
        if (cpuPass_) cpuSync_ = makeBuffer(dev, 32, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, nullptr, err);
        if (cpuSync_) cpuStateRead_ = makeBuffer(dev, kCpuStateBytes, 0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, nullptr, err);
        return cpuStateRead_ != nullptr;
    }
    ComPtr<ID3D11UnorderedAccessView> rawUav(ID3D11Buffer* b, UINT bytes, std::string& err) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = bytes / 4;
        d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ComPtr<ID3D11UnorderedAccessView> v;
        HRESULT hr = gpu_.device->CreateUnorderedAccessView(b, &d, &v);
        if (FAILED(hr)) err = "a raw view: " + hrToString(hr);
        return v;
    }
    void setSync(uint32_t mask, uint32_t mode, uint32_t from, uint32_t count) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.ctx->Map(cpuSync_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        uint32_t cb[8] = {mask, mode, from, count, csMerge_ ? 24u : 32u};
        memcpy(m.pData, cb, sizeof cb);
        gpu_.ctx->Unmap(cpuSync_.Get(), 0);
    }
    void unbindCompute() {
        ID3D11UnorderedAccessView* none[6] = {};
        ID3D11ShaderResourceView* noSrv[2] = {};
        gpu_.ctx->CSSetUnorderedAccessViews(0, 6, none, nullptr);
        gpu_.ctx->CSSetShaderResources(0, 2, noSrv);
    }
    // The processor's passes of a frame (computePasses), each a thread group a core and then the
    // merge of what core 0 and the workers stored; then what was stored into the state texture
    // the commit reads.
    void runComputeCpu(Material& mat) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        const UINT ramBytes = kRamTexels * 16;
        uint64_t mtime = (uint64_t)mat.getFloat("_HostMtimeLo") | (uint64_t)mat.getFloat("_HostMtimeHi") << 32;
        ID3D11UnorderedAccessView* cpuUavs[5] = {cpuRamUav_.Get(), cpuStateUav_.Get(), cpuDecUav_.Get(), cpuMarksUav_.Get(), cpuArgsUav_.Get()};
        ID3D11ShaderResourceView* rom = cpuRomSrv_.Get();
        // --compute-split F: the first F passes always run, each core's at most the frame's instructions
        // over F; the rest of --compute-passes only when core 0 ended the one before early
        const uint32_t split = computeSplit;
        const uint32_t ticks = (uint32_t)mat.getFloat("_Ticks", 8192), slice = split ? (ticks + split - 1) / split : 0;
        for (uint32_t p = 0; p < computePasses; p++) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(cpuPass_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                // (every pass its own time, a tick of the clock apart: the kernel tells passes apart by it)
                cpuSeed_ = cpuSeed_ * 1664525u + 1013904223u;
                uint64_t t = mtime + p;
                uint32_t cb[12] = {(uint32_t)t, (uint32_t)(t >> 32), (uint32_t)(int64_t)mat.getFloat("_RTC0"), (uint32_t)(int64_t)mat.getFloat("_RTC1"),
                                   ticks, ramBytes, cpuSeed_, 0,   // (0: workers are workers)
                                   p, computePasses, computeCores, slice};
                memcpy(m.pData, cb, sizeof cb);
                ctx->Unmap(cpuPass_.Get(), 0);
            }
            ctx->CSSetShader(csCpu_.Get(), nullptr, 0);
            ctx->CSSetUnorderedAccessViews(0, 5, cpuUavs, nullptr);
            ctx->CSSetShaderResources(0, 1, &rom);
            ctx->CSSetConstantBuffers(0, 1, cpuPass_.GetAddressOf());
            if (p == 0 || !csMerge_ || p < split) ctx->Dispatch(computeCores, 1, 1);
            else {
                // a pass after the first only when core 0 ended the last early (merge_list says so)
                ID3D11UnorderedAccessView* noArgs = nullptr;
                ctx->CSSetUnorderedAccessViews(4, 1, &noArgs, nullptr);
                ctx->DispatchIndirect(cpuArgs_.Get(), 24);
                ctx->CSSetUnorderedAccessViews(4, 1, cpuUavs + 4, nullptr);
            }
            if (p == 0) prof_.mark(ctx);
            if (csMerge_) {
                // the pages stored to, listed; then a thread group each, as many as listed
                ctx->CSSetShader(csMergeList_.Get(), nullptr, 0);
                ctx->Dispatch(1, 1, 1);
                if (p == 0) prof_.mark(ctx);
                ID3D11UnorderedAccessView* noArgs = nullptr;
                ctx->CSSetUnorderedAccessViews(4, 1, &noArgs, nullptr);
                ctx->CSSetShader(csMerge_.Get(), nullptr, 0);
                ctx->DispatchIndirect(cpuArgs_.Get(), 0);
            }
        }
        prof_.mark(ctx);
        unbindCompute();
        ID3D11UnorderedAccessView* uavs[5] = {cpuRamUav_.Get(), cpuStateUav_.Get(), crt_.currentUAV(), cpuDecUav_.Get(), cpuMarksUav_.Get()};
        // Linux's own bands (which only the memory view shows) go into the texture at first and every
        // 60th frame while it is open; the GPU's memory and the control words every frame, as --cpu
        uint32_t mask = computeMask;
        if (csMerge_ && computeFrames_++ > 0 && !(viewOpen() && computeFrames_ % 60 == 0)) mask &= 0xff000000u;
        setSync(mask, 0, 0, computeCores);
        ctx->CSSetConstantBuffers(0, 1, cpuSync_.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
        ctx->CSSetShader(csCount_.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShader(csState_.Get(), nullptr, 0);
        ctx->Dispatch(1, 16, 1);
        prof_.mark(ctx);
        if (csMerge_) {
            // the GPU's pages stored to in this frame (merge_list listed them), a group each;
            // Linux's bands whole when they go
            ctx->CSSetShader(csExportPages_.Get(), nullptr, 0);
            ctx->DispatchIndirect(cpuArgs_.Get(), 12);
        }
        if (!csMerge_ || (mask & 0x00ffffffu) != 0) {
            ctx->CSSetShader(csExport_.Get(), nullptr, 0);
            ctx->Dispatch(8, kHeight - 64, 1);
        }
        prof_.mark(ctx);
        unbindCompute();
    }
    // What the devices wrote into RAM in this frame, back into the processor's.
    void computeImport(bool con, bool fetch, bool net, bool counts) {
        ID3D11DeviceContext* ctx = gpu_.ctx.Get();
        ID3D11UnorderedAccessView* uavs[6] = {cpuRamUav_.Get(), cpuStateUav_.Get(), nullptr, cpuDecUav_.Get(), cpuMarksUav_.Get(), cpuArgsUav_.Get()};
        ID3D11ShaderResourceView* state = crt_.currentSRV();
        ctx->CSSetUnorderedAccessViews(0, 6, uavs, nullptr);
        ctx->CSSetShaderResources(0, 1, &state);
        ctx->CSSetConstantBuffers(0, 1, cpuSync_.GetAddressOf());
        ctx->CSSetShader(csPlan_.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShader(csTake_.Get(), nullptr, 0);
        setSync(0, 1, 0, 0);   // (first: its words are the control words before they come back)
        ctx->Dispatch((kGpuTarget * kGpuTarget / 4 + 1 + 255) / 256, 1, 1);
        prof_.mark(ctx);
        auto texels = [&](uint32_t from, uint32_t count) {
            setSync(0, 0, from, count);
            ctx->Dispatch((count + 255) / 256, 1, 1);
        };
        const uint32_t net0 = (kNetRow - 64) * 2048;
        texels((kControlRow - 64) * 2048, kControlTexels);
        if (con) texels(net0 + kConHost, 1);
        if (net) texels(net0 + kNetRing, kNetSlot * kNetSlots);
        if (fetch) texels(rvlayout::devT(0x16c000), 0x4000);   // FETCH_DATA (src/gpu.h); its words are control words
        if (counts) texels(rvlayout::devT(0xc1000), 256);     // COUNT_TOTAL: the counters' totals the control pass added up
        // the cores as the control words have them (after those came back)
        prof_.mark(ctx);
        setSync(0, 0, 0, computeCores);
        ctx->CSSetShader(csCores_.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        unbindCompute();
    }
public:
    bool computeStart(const std::string& imageDir, std::string& err) override {
        if (!csCpu_) { err = "no compute processor was built"; return false; }
        std::string payload, rom;
        if (!readFileBinary((fs::u8path(imageDir) / "linux_payload.bin").u8string(), payload) ||
            !readFileBinary((fs::u8path(imageDir) / "rootfs.bin").u8string(), rom)) {
            err = "no image in " + imageDir + " (linux_payload.bin, rootfs.bin)";
            return false;
        }
        ID3D11Device* dev = gpu_.device.Get();
        const UINT RAW = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, ramBytes = kRamTexels * 16;
        {
            // three images of RAM (rv32_min_fast.hlsl): core 0's, the workers', as the last pass left them
            std::vector<uint8_t> ram((size_t)ramBytes * 3, 0);
            for (int i = 0; i < 3; i++) memcpy(ram.data() + (size_t)ramBytes * i, payload.data(), (std::min)(payload.size(), (size_t)ramBytes));
            cpuRam_ = makeBuffer(dev, ramBytes * 3, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, ram.data(), err);
        }
        rom.resize((rom.size() + 15) & ~(size_t)15, 0);
        // the hart as the kernel without an MMU starts: supervisor mode at rvlayout::kKernelAt, the device tree in
        // x11, no reservation; every band of RAM still to go into the texture
        std::vector<uint32_t> state((size_t)computeCores * kCpuStateBytes / 4, 0);
        state[11] = rvlayout::kTreeAt;
        state[80] = rvlayout::kKernelAt;
        state[81] = 1;
        state[83] = ~0u;
        state[90] = ~0u;
        const UINT stateBytes = computeCores * kCpuStateBytes;
        if (cpuRam_) cpuState_ = makeBuffer(dev, stateBytes, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, state.data(), err);
        if (cpuState_) cpuRom_ = makeBuffer(dev, (UINT)rom.size(), D3D11_BIND_SHADER_RESOURCE, RAW, D3D11_USAGE_DEFAULT, 0, rom.data(), err);
        {
            // decoded instructions, 8 bytes a word of RAM, none made yet; no page or band marked
            std::vector<uint8_t> zeros((size_t)ramBytes * 2, 0);
            if (cpuRom_) cpuDec_ = makeBuffer(dev, ramBytes * 2, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, zeros.data(), err);
            if (cpuDec_) cpuMarks_ = makeBuffer(dev, kMarkBytes, D3D11_BIND_UNORDERED_ACCESS, RAW, D3D11_USAGE_DEFAULT, 0, zeros.data(), err);
            if (cpuMarks_) cpuArgs_ = makeBuffer(dev, kArgsBytes, D3D11_BIND_UNORDERED_ACCESS, RAW | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, D3D11_USAGE_DEFAULT, 0,
                                                 zeros.data(), err);
        }
        if (!cpuArgs_) return false;
        cpuRamUav_ = rawUav(cpuRam_.Get(), ramBytes * 3, err);
        cpuStateUav_ = cpuRamUav_ ? rawUav(cpuState_.Get(), stateBytes, err) : nullptr;
        cpuDecUav_ = cpuStateUav_ ? rawUav(cpuDec_.Get(), ramBytes * 2, err) : nullptr;
        cpuMarksUav_ = cpuDecUav_ ? rawUav(cpuMarks_.Get(), kMarkBytes, err) : nullptr;
        cpuArgsUav_ = cpuMarksUav_ ? rawUav(cpuArgs_.Get(), kArgsBytes, err) : nullptr;
        if (!cpuArgsUav_) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx.NumElements = (UINT)(rom.size() / 4);
        d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        HRESULT hr = dev->CreateShaderResourceView(cpuRom_.Get(), &d, &cpuRomSrv_);
        if (FAILED(hr)) { err = "the ROM's view: " + hrToString(hr); return false; }
        fprintf(stderr, "[harness] --compute: %s (%zu bytes) in RAM, rootfs.bin (%zu bytes) as the ROM, %u cores\n", imageDir.c_str(),
                payload.size(), rom.size(), computeCores);
        computeMode = true;
        cpuMode = true;
        return true;
    }
    bool computeState(std::vector<uint32_t>& out) override {
        if (!cpuState_) return false;
        D3D11_BOX core0{0, 0, 0, kCpuStateBytes, 1, 1};
        gpu_.ctx->CopySubresourceRegion(cpuStateRead_.Get(), 0, 0, 0, 0, cpuState_.Get(), 0, &core0);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_.ctx->Map(cpuStateRead_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.assign((const uint32_t*)m.pData, (const uint32_t*)m.pData + kCpuStateBytes / 4);
        gpu_.ctx->Unmap(cpuStateRead_.Get(), 0);
        return true;
    }
private:
    ComPtr<ID3D11ShaderResourceView> hostData_;
    bool deliver_ = false;   // the fetch zone is drawn in the next frame
    ComPtr<IDXGISwapChain> swapChain_;
    HWND presentWnd_ = nullptr;
    ComPtr<ID3D11Buffer> profBuf_, profStaging_;
    ComPtr<ID3D11UnorderedAccessView> profUav_;
    ComPtr<ID3D11Query> tsDisjoint_, tsQuery_[4];
    uint32_t timedRounds_ = 1;   // (--unity: the rounds of the frame being timed)
    bool tsPending_ = false;
    double tickMs_ = -1, commitMs_ = -1, deviceMs_ = 0;
    bool bands_ = false;   // the commit draws only the bands of RAM that changed
    float tickRows_ = 64;  // BackendOptions::tickRows
    float cores_ = 1;      // CORES: more than one, and the tick draws the workers' strip too (docs/machine/multicore.md)
    MemoryView view_;
};

}  // namespace

std::unique_ptr<RvcBackend> makeBackend11() { return std::make_unique<Backend11>(); }
