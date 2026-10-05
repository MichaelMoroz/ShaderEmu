// Minimal ShaderLab (.shader) reader: enough to pull the Properties block and the
// CGPROGRAM/HLSLPROGRAM code of each Pass out of a Unity shader, so the harness can
// compile the exact file Unity would.
#pragma once

#include <string>
#include <vector>

struct SLProperty {
    std::string name;          // e.g. _Ticks
    std::string display;       // e.g. "Ticks per Frame"
    std::string type;          // e.g. Int, Float, Range(0,1), 2D, Vector, Color
    std::string defaultValue;  // raw text: 1024, (1,1,1,1), black (texture default name)
};

struct SLPass {
    std::string name;            // from Name "..." (may be empty)
    std::string code;            // ready-to-compile HLSL, starting with #line directives
    std::string vertexEntry;     // #pragma vertex
    std::string fragmentEntry;   // #pragma fragment
    std::string geometryEntry;   // #pragma geometry (optional)
    std::string target = "5.0";  // #pragma target
};

struct SLShader {
    std::string path;
    std::string name;
    std::vector<SLProperty> properties;
    std::vector<SLPass> passes;

    const SLPass* findPass(const std::string& name) const;
};

bool loadShaderLab(const std::string& path, SLShader& out, std::string& err);

// "5.0" -> "5_0" etc. Everything SM5-capable maps to *_5_0, as Unity's D3D11 path does.
std::string shaderModelSuffix(const std::string& target);
