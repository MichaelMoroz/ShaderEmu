#include "common.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hrToString(HRESULT hr) {
    char buf[32];
    snprintf(buf, sizeof buf, "0x%08X", (unsigned)hr);
    return buf;
}

bool readFileBinary(const std::string& path, std::string& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    if (size > 0) f.read(out.data(), size);
    return (bool)f;
}

bool writeFileBinary(const std::string& path, const void* data, size_t size) {
    auto p = std::filesystem::u8path(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // Write to a temp file and rename, so a killed process never leaves a torn cache entry.
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write((const char*)data, (std::streamsize)size);
        if (!f) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

uint64_t fnv1a64(const void* data, size_t size, uint64_t h) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
