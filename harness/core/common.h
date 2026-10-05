// Small shared helpers for the harness (Windows only).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
std::string hrToString(HRESULT hr);

bool readFileBinary(const std::string& pathUtf8, std::string& out);
bool writeFileBinary(const std::string& pathUtf8, const void* data, size_t size);

// 64-bit FNV-1a, used for shader cache keys.
uint64_t fnv1a64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull);
