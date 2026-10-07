// d3dcompiler_47 wrapper with Unity-like settings and an on-disk bytecode cache. The DLL beside
// the executable is fxc2 (tools/fxc2, docs/fxc2.md); without it Windows' own FXC is loaded.
//
// The cache key is a hash of the *preprocessed* source (cheap to produce) plus entry point,
// profile, flags and which compiler it is. Editing an included file invalidates exactly the
// shaders that use it.
#pragma once

#include "common.h"

#include <d3dcommon.h>
#include <d3dcompiler.h>

#include <string>
#include <utility>
#include <vector>

using Defines = std::vector<std::pair<std::string, std::string>>;

struct CompileSettings {
    // Unity's D3D11 path compiles with backwards compatibility on (it lets shaders assign to
    // uniforms, which rvc relies on) and full optimisation.
    UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    std::vector<std::string> includeDirs;  // searched after the including file's directory
    Defines defines;
    std::string cacheDir;                   // empty = no cache
    bool printWarnings = false;
};

// "fxc2" when the d3dcompiler_47.dll in use is not Windows' own, else "FXC".
const char* shaderCompilerName();

struct StageResult {
    bool ok = false;
    bool fromCache = false;
    double seconds = 0;
    ComPtr<ID3DBlob> bytecode;
    std::string log;  // errors and warnings
};

// Runs only the preprocessor (includes and macros resolved), for handing the source to another
// compiler such as DXC.
bool preprocessStage(const std::string& source, const std::string& sourceName, const std::string& rootDir,
                     const Defines& extraDefines, const CompileSettings& settings, std::string& out, std::string& err);

// rootDir: directory used to resolve #include "..." from the top-level source.
StageResult compileStage(const std::string& source, const std::string& sourceName, const std::string& rootDir,
                         const std::string& entry, const std::string& profile, const Defines& extraDefines,
                         const CompileSettings& settings);
