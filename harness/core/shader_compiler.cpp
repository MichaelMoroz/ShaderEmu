#include "shader_compiler.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

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

    // Cache key: preprocessed text + entry + profile + flags + compiler identity.
    std::string keyExtra = entry + "|" + profile + "|" + std::to_string(settings.flags) + "|d3dcompiler_47|v1";
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
                r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                return r;
            }
        }
    }

    // Compile the preprocessed text (it carries #line directives, so messages still point at
    // the original files).
    errors.Reset();
    hr = D3DCompile(pre->GetBufferPointer(), pre->GetBufferSize(), sourceName.c_str(), nullptr, nullptr,
                    entry.c_str(), profile.c_str(), settings.flags, 0, &r.bytecode, &errors);
    r.log = blobText(errors.Get());
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (FAILED(hr) || !r.bytecode) {
        if (r.log.empty()) r.log = "D3DCompile failed: " + hrToString(hr);
        r.bytecode.Reset();
        return r;
    }
    r.ok = true;
    if (!cachePath.empty())
        writeFileBinary(cachePath, r.bytecode->GetBufferPointer(), r.bytecode->GetBufferSize());
    return r;
}
