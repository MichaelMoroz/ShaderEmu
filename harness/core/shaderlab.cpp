#include "shaderlab.h"

#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>

namespace {

// Replace comments with spaces (keeping newlines so offsets and line numbers survive).
std::string blankComments(const std::string& s) {
    std::string r = s;
    size_t i = 0, n = s.size();
    bool inString = false;
    while (i < n) {
        char c = s[i];
        if (inString) {
            if (c == '\\' && i + 1 < n) { i += 2; continue; }
            if (c == '"' || c == '\n') inString = false;
            ++i;
        } else if (c == '"') {
            inString = true;
            ++i;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') r[i++] = ' ';
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            r[i++] = ' ';
            r[i++] = ' ';
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) {
                if (s[i] != '\n') r[i] = ' ';
                ++i;
            }
            if (i < n) { r[i++] = ' '; if (i < n) r[i++] = ' '; }
        } else {
            ++i;
        }
    }
    return r;
}

bool isIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Find whole-word occurrence of `word` at or after `from`.
size_t findWord(const std::string& s, const std::string& word, size_t from) {
    for (size_t p = s.find(word, from); p != std::string::npos; p = s.find(word, p + 1)) {
        bool startOk = p == 0 || !isIdent(s[p - 1]);
        bool endOk = p + word.size() >= s.size() || !isIdent(s[p + word.size()]);
        if (startOk && endOk) return p;
    }
    return std::string::npos;
}

int lineOf(const std::string& s, size_t pos) {
    return 1 + (int)std::count(s.begin(), s.begin() + (std::min)(pos, s.size()), '\n');
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lineDirective(int line, const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    return "#line " + std::to_string(line) + " \"" + p + "\"\n";
}

struct Block {
    enum Kind { Program, Include } kind;
    bool cg;  // CGPROGRAM/CGINCLUDE (Unity auto-includes HLSLSupport + UnityShaderVariables)
    size_t start, bodyStart, bodyEnd, end;
};

bool parseProperties(const std::string& body, std::vector<SLProperty>& props, std::string& err) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = trim(body.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty()) continue;

        // Skip attributes like [ToggleUI] [HideInInspector] [Enum(...)].
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            int depth = 0;
            for (; i < line.size(); ++i) {
                if (line[i] == '[') ++depth;
                else if (line[i] == ']' && --depth == 0) { ++i; break; }
            }
            while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        }
        size_t nameStart = i;
        while (i < line.size() && isIdent(line[i])) ++i;
        SLProperty p;
        p.name = line.substr(nameStart, i - nameStart);
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (p.name.empty() || i >= line.size() || line[i] != '(') {
            err = "cannot parse property line: " + line;
            return false;
        }
        int depth = 0;
        size_t open = i, close = std::string::npos;
        bool inStr = false;
        for (; i < line.size(); ++i) {
            char c = line[i];
            if (c == '"') inStr = !inStr;
            if (inStr) continue;
            if (c == '(') ++depth;
            else if (c == ')' && --depth == 0) { close = i; break; }
        }
        if (close == std::string::npos) {
            err = "unbalanced parentheses in property: " + line;
            return false;
        }
        std::string inner = line.substr(open + 1, close - open - 1);
        size_t q1 = inner.find('"');
        size_t q2 = q1 == std::string::npos ? q1 : inner.find('"', q1 + 1);
        size_t comma = q2 == std::string::npos ? q2 : inner.find(',', q2);
        if (comma == std::string::npos) {
            err = "cannot parse property declaration: " + line;
            return false;
        }
        p.display = inner.substr(q1 + 1, q2 - q1 - 1);
        p.type = trim(inner.substr(comma + 1));
        std::string rest = trim(line.substr(close + 1));
        if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
        // Texture defaults look like: "black" {}
        if (!rest.empty() && rest[0] == '"') {
            size_t e = rest.find('"', 1);
            rest = rest.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        }
        p.defaultValue = rest;
        props.push_back(p);
    }
    return true;
}

}  // namespace

const SLPass* SLShader::findPass(const std::string& n) const {
    for (auto& p : passes)
        if (p.name == n) return &p;
    return nullptr;
}

std::string shaderModelSuffix(const std::string& target) {
    double t = atof(target.c_str());
    if (t >= 4.5 || t == 0) return "5_0";
    if (t >= 4.0) return "4_0";
    return "5_0";
}

bool loadShaderLab(const std::string& path, SLShader& out, std::string& err) {
    std::string src;
    if (!readFileBinary(path, src)) {
        err = "cannot read " + path;
        return false;
    }
    if (src.size() >= 3 && (uint8_t)src[0] == 0xEF && (uint8_t)src[1] == 0xBB && (uint8_t)src[2] == 0xBF)
        src.erase(0, 3);  // UTF-8 BOM (rvc's main.shader has one)
    // Normalise CRLF so line counting is simple; FXC does not care.
    src.erase(std::remove(src.begin(), src.end(), '\r'), src.end());

    out = SLShader{};
    out.path = std::filesystem::absolute(std::filesystem::u8path(path)).u8string();
    std::string nc = blankComments(src);

    // 1. Locate program blocks.
    std::vector<Block> blocks;
    struct Opener { const char* open; const char* close; Block::Kind kind; bool cg; };
    const Opener openers[] = {
        {"CGPROGRAM", "ENDCG", Block::Program, true},
        {"CGINCLUDE", "ENDCG", Block::Include, true},
        {"HLSLPROGRAM", "ENDHLSL", Block::Program, false},
        {"HLSLINCLUDE", "ENDHLSL", Block::Include, false},
    };
    size_t scan = 0;
    while (true) {
        size_t best = std::string::npos;
        const Opener* bo = nullptr;
        for (auto& o : openers) {
            size_t p = findWord(nc, o.open, scan);
            if (p < best) { best = p; bo = &o; }
        }
        if (!bo) break;
        size_t bodyStart = best + strlen(bo->open);
        size_t endPos = findWord(nc, bo->close, bodyStart);
        if (endPos == std::string::npos) {
            err = std::string("missing ") + bo->close + " for block at line " + std::to_string(lineOf(src, best));
            return false;
        }
        blocks.push_back({bo->kind, bo->cg, best, bodyStart, endPos, endPos + strlen(bo->close)});
        scan = endPos + strlen(bo->close);
    }

    // 2. Structural text: comments and program bodies blanked out.
    std::string st = nc;
    for (auto& b : blocks)
        for (size_t i = b.start; i < b.end; ++i)
            if (st[i] != '\n') st[i] = ' ';

    std::smatch m;
    {
        std::regex re("Shader\\s*\"([^\"]*)\"");
        if (std::regex_search(st, m, re)) out.name = m[1];
    }

    // 3. Properties.
    size_t propPos = findWord(st, "Properties", 0);
    if (propPos != std::string::npos) {
        size_t open = st.find('{', propPos);
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < st.size(); ++i) {
            if (st[i] == '{') ++depth;
            else if (st[i] == '}' && --depth == 0) { close = i; break; }
        }
        if (open == std::string::npos || close == std::string::npos) {
            err = "malformed Properties block";
            return false;
        }
        if (!parseProperties(st.substr(open + 1, close - open - 1), out.properties, err)) return false;
    }

    // 4. Passes.
    std::string includeCode;  // accumulated CGINCLUDE/HLSLINCLUDE code
    std::regex nameRe("Name\\s*\"([^\"]*)\"");
    std::regex pragmaRe("^[ \\t]*#[ \\t]*pragma[ \\t]+(vertex|fragment|geometry|target)[ \\t]+([^ \\t\\n]+)");
    for (auto& b : blocks) {
        std::string body = src.substr(b.bodyStart, b.bodyEnd - b.bodyStart);
        int bodyLine = lineOf(src, b.bodyStart);
        if (b.kind == Block::Include) {
            includeCode += lineDirective(bodyLine, out.path) + body + "\n";
            continue;
        }
        SLPass pass;
        // Name: search the structural text between the closest preceding "Pass" and this block.
        size_t passKw = std::string::npos;
        for (size_t p = findWord(st, "Pass", 0); p != std::string::npos && p < b.start; p = findWord(st, "Pass", p + 1))
            passKw = p;
        if (passKw != std::string::npos) {
            std::string header = st.substr(passKw, b.start - passKw);
            if (std::regex_search(header, m, nameRe)) pass.name = m[1];
        }
        std::string bodyNc = blankComments(body);
        for (size_t ls = 0; ls < bodyNc.size();) {
            size_t le = bodyNc.find('\n', ls);
            if (le == std::string::npos) le = bodyNc.size();
            std::string line = bodyNc.substr(ls, le - ls);
            ls = le + 1;
            if (std::regex_search(line, m, pragmaRe)) {
                std::string kind = m[1], val = m[2];
                if (kind == "vertex") pass.vertexEntry = val;
                else if (kind == "fragment") pass.fragmentEntry = val;
                else if (kind == "geometry") pass.geometryEntry = val;
                else if (kind == "target") pass.target = val;
            }
        }
        std::string prelude;
        if (b.cg) prelude = "#include \"HLSLSupport.cginc\"\n#include \"UnityShaderVariables.cginc\"\n";
        pass.code = prelude + includeCode + lineDirective(bodyLine, out.path) + body + "\n";
        out.passes.push_back(std::move(pass));
    }
    if (out.passes.empty()) {
        err = "no passes found in " + path;
        return false;
    }
    return true;
}
