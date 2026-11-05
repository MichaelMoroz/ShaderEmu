#include <algorithm>
#include "shader_compiler.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

void noteCode(uint64_t h);   // (below: compiledCode under its lock)

namespace fs = std::filesystem;

namespace {

class IncludeHandler final : public ID3DInclude {
public:
    IncludeHandler(fs::path root, std::vector<fs::path> sys) : root_(std::move(root)), sys_(std::move(sys)) {}

    HRESULT __stdcall Open(D3D_INCLUDE_TYPE type, LPCSTR fileName, LPCVOID parentData, LPCVOID* ppData,
                           UINT* pBytes) override {
        fs::path name = fs::u8path(fileName);
        std::vector<fs::path> candidates;
        if (name.is_absolute()) {
            candidates.push_back(name);
        } else {
            if (type == D3D_INCLUDE_LOCAL) {
                auto it = dirs_.find(parentData);
                fs::path parentDir = it != dirs_.end() ? it->second : root_;
                candidates.push_back(parentDir / name);
                if (parentDir != root_) candidates.push_back(root_ / name);
            }
            for (auto& d : sys_) candidates.push_back(d / name);
            if (type == D3D_INCLUDE_SYSTEM) candidates.push_back(root_ / name);
        }
        for (auto& c : candidates) {
            std::string data;
            if (!readFileBinary(c.u8string(), data)) continue;
            char* buf = new char[data.size() + 1];
            memcpy(buf, data.data(), data.size());
            buf[data.size()] = 0;
            dirs_[buf] = c.parent_path();
            *ppData = buf;
            *pBytes = (UINT)data.size();
            return S_OK;
        }
        return E_FAIL;
    }

    HRESULT __stdcall Close(LPCVOID pData) override {
        dirs_.erase(pData);
        delete[] (const char*)pData;
        return S_OK;
    }

private:
    fs::path root_;
    std::vector<fs::path> sys_;
    std::map<LPCVOID, fs::path> dirs_;
};

std::string blobText(ID3DBlob* b) {
    if (!b) return {};
    return std::string((const char*)b->GetBufferPointer(), b->GetBufferSize());
}

}  // namespace

const char* shaderCompilerName() {
    static const char* name = nullptr;
    if (!name) {
        wchar_t path[MAX_PATH] = L"", windows[MAX_PATH] = L"";
        GetModuleFileNameW(GetModuleHandleW(L"d3dcompiler_47.dll"), path, MAX_PATH);
        GetWindowsDirectoryW(windows, MAX_PATH);
        name = _wcsnicmp(path, windows, wcslen(windows)) == 0 ? "FXC" : "fxc2";
    }
    return name;
}

bool preprocessStage(const std::string& source, const std::string& sourceName, const std::string& rootDir,
                     const Defines& extraDefines, const CompileSettings& settings, std::string& out, std::string& err) {
    Defines all = settings.defines;
    all.insert(all.end(), extraDefines.begin(), extraDefines.end());
    std::vector<D3D_SHADER_MACRO> macros;
    for (auto& d : all) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});
    std::vector<fs::path> sys;
    for (auto& d : settings.includeDirs) sys.push_back(fs::u8path(d));
    IncludeHandler inc(fs::u8path(rootDir), sys);
    // The text is for another compiler: what fxc2's preprocessor says of itself must not hold.
    std::string forOther = "#undef __FXC2__\n#line 1\n" + source;
    ComPtr<ID3DBlob> pre, errors;
    HRESULT hr = D3DPreprocess(forOther.data(), forOther.size(), sourceName.c_str(), macros.data(), &inc, &pre, &errors);
    if (FAILED(hr)) {
        err = "preprocess failed (" + hrToString(hr) + "):\n" + blobText(errors.Get());
        return false;
    }
    out = blobText(pre.Get());
    return true;
}

StageResult compileStage(const std::string& source, const std::string& sourceName, const std::string& rootDir,
                         const std::string& entry, const std::string& profile, const Defines& extraDefines,
                         const CompileSettings& settings) {
    StageResult r;
    auto t0 = std::chrono::steady_clock::now();

    Defines all = settings.defines;
    all.insert(all.end(), extraDefines.begin(), extraDefines.end());
    std::vector<D3D_SHADER_MACRO> macros;
    for (auto& d : all) macros.push_back({d.first.c_str(), d.second.c_str()});
    macros.push_back({nullptr, nullptr});

    std::vector<fs::path> sys;
    for (auto& d : settings.includeDirs) sys.push_back(fs::u8path(d));
    IncludeHandler inc(fs::u8path(rootDir), sys);

    ComPtr<ID3DBlob> pre, errors;
    HRESULT hr = D3DPreprocess(source.data(), source.size(), sourceName.c_str(), macros.data(), &inc, &pre, &errors);
    if (FAILED(hr)) {
        r.log = "preprocess failed (" + hrToString(hr) + "):\n" + blobText(errors.Get());
        return r;
    }

    // RVC_DUMP_PRE=DIR: each stage's preprocessed text written there (ENTRY_PROFILE_HASH.hlsl), to be
    // compiled by another compiler (DXC with debug information for Nsight Graphics, which fxc2 does not make)
    if (const char* dumpDir = getenv("RVC_DUMP_PRE")) {
        uint64_t ph = fnv1a64(pre->GetBufferPointer(), pre->GetBufferSize());
        char name[96];
        snprintf(name, sizeof name, "%s_%s_%016llx.hlsl", entry.c_str(), profile.c_str(), (unsigned long long)ph);
        writeFileBinary((fs::u8path(dumpDir) / name).u8string(), pre->GetBufferPointer(), pre->GetBufferSize() - 1);
    }
    // RVC_SHADER_DEBUG=1: the bytecode carries debug information (D3DCOMPILE_DEBUG, optimised as
    // ever), for a profiler to map the machine's code to the source (Nsight Graphics' shader profiler)
    static const bool debugInfo = getenv("RVC_SHADER_DEBUG") != nullptr;
    const unsigned flags = settings.flags | (debugInfo ? D3DCOMPILE_DEBUG : 0u);
    // Cache key: preprocessed text + entry + profile + flags + compiler identity.
    std::string keyExtra = entry + "|" + profile + "|" + std::to_string(flags) + "|d3dcompiler_47|v1";
    if (strcmp(shaderCompilerName(), "FXC") != 0) keyExtra += std::string("|") + shaderCompilerName();
    uint64_t h = fnv1a64(pre->GetBufferPointer(), pre->GetBufferSize());
    h = fnv1a64(keyExtra.data(), keyExtra.size(), h);
    char keyHex[17];
    snprintf(keyHex, sizeof keyHex, "%016llx", (unsigned long long)h);
    std::string cachePath;
    if (!settings.cacheDir.empty()) {
        cachePath = (fs::u8path(settings.cacheDir) / (std::string(keyHex) + ".cso")).u8string();
        std::string bytes;
        if (readFileBinary(cachePath, bytes) && !bytes.empty()) {
            if (SUCCEEDED(D3DCreateBlob(bytes.size(), &r.bytecode))) {
                memcpy(r.bytecode->GetBufferPointer(), bytes.data(), bytes.size());
                r.ok = true;
                r.fromCache = true;
                noteCode(fnv1a64(bytes.data(), bytes.size()));
                r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                return r;
            }
        }
    }

    // Compile the preprocessed text (it carries #line directives, so messages still point at
    // the original files).
    errors.Reset();
    hr = D3DCompile(pre->GetBufferPointer(), pre->GetBufferSize(), sourceName.c_str(), nullptr, nullptr,
                    entry.c_str(), profile.c_str(), flags, 0, &r.bytecode, &errors);
    r.log = blobText(errors.Get());
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (FAILED(hr) || !r.bytecode) {
        if (r.log.empty()) r.log = "D3DCompile failed: " + hrToString(hr);
        r.bytecode.Reset();
        return r;
    }
    r.ok = true;
    noteCode(fnv1a64(r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize()));
    if (!cachePath.empty())
        writeFileBinary(cachePath, r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize());
    return r;
}

std::vector<uint64_t>& compiledCode() {
    static std::vector<uint64_t> hashes;
    return hashes;
}
// (the stages compile on threads of their own, std::async in material.cpp: a push_back of two at once broke the heap,
// a harness that died now and then with 0xC0000005 as a shader's stages came back)
static std::mutex& codeLock() {
    static std::mutex m;
    return m;
}
void noteCode(uint64_t h) {
    std::lock_guard<std::mutex> hold(codeLock());
    compiledCode().push_back(h);
}

std::string codeStamp() {
    std::vector<uint64_t> sorted;
    {
        std::lock_guard<std::mutex> hold(codeLock());
        sorted = compiledCode();
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t v : sorted) h = fnv1a64(&v, sizeof v, h);
    char s[24];
    snprintf(s, sizeof s, "c%016llx", (unsigned long long)h);
    return s;
}
