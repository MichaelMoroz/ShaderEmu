#!/usr/bin/env python3
"""A gpu_vm tick step as the Unity world includes it (unity/gpuvm: Shaders/gvm/tick.cginc, isa.cginc). The step's two
cbuffers become the world's uniforms (gvm/params.cginc: a material's numbers are floats, so TIME comes in halves and
_Comp is each commit pass's own), and its textures lose their explicit registers; nothing else changes.

    python gpu_vm/platform/unity_tick.py gpu_vm/tick/steps/v0009.hlsl C:/Development/VRChat/ShaderEmu-GPUVM/Assets/ShaderEmu/Shaders/gvm
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


ONE_HELPERS = """// (gvm_one: the state set as one target, 8 rows: core c's pixel j, texels 8j to 8j + 7, is column one_x(c) + j, a texel
// a row; core 0 72 columns, a worker 48: each core whole 4 x 8 and 8 x 8 tiles of one big triangle, warps of its own)
static const uint ONE_C0W = 72u, ONE_W = 48u;
uint one_x(uint c) { return c == 0u ? 0u : ONE_C0W + ONE_W * (c - 1u); }
uint one_core(uint x) { return x < ONE_C0W ? 0u : 1u + (x - ONE_C0W) / ONE_W; }
int3 one_at(uint t, uint c) { return int3(one_x(c) + (t >> 3), t & 7, 0); }
uniform uint _GvmOne;   // (the commit's and operations' reads: the set drawn by GvmTickOne)
"""
ONE_FRAG = """
#ifdef GVM_ONE
// (gvm_one) the tick into one target: a pixel's texel is the one of its eight its row names
uint4 frag_one(v2f i) : SV_Target {
    tick_out o = frag(i);
    uint r = (uint)i.vertex.y & 7u;
    return r < 4u ? (r < 2u ? (r == 0u ? o.t0 : o.t1) : (r == 2u ? o.t2 : o.t3)) : (r < 6u ? (r == 4u ? o.t4 : o.t5) : (r == 6u ? o.t6 : o.t7));
}
#endif
"""


def one_target(src):
    """GVM_ONE (GvmTickOne.shader, GvmStateOne.shader): the state set as one texture 8 rows tall, core c's pixel j (its
    texels 8j to 8j + 7) at column one_x(c) + j and a texel a row; core 0 72 columns, a worker 48, so each core is whole
    4 x 8 and 8 x 8 tiles of one triangle. The readers (state_any: the commit, the operations) take it when _GvmOne is
    set. A step whose state functions are not c8's is left as it was."""
    satx = "int3 satx(uint j, uint c) {"
    sin = "uint4 state_in(uint t, uint c) {\n    int3 at = sat(t >> 3, c);"
    sany = "uint4 state_any(uint t, uint c) {\n    int3 at = satx(t >> 3, c);"
    pix = "    uint rx = p.x % 8u, ry = p.y % 16u;\n    core = (p.y / 16u) * 128u + core_at(p.x / 8u);\n"
    src = src.replace("\r\n", "\n")
    if not all(src.count(m) == 1 for m in (satx, sin, sany)) or "tick_out frag(v2f i) {" not in src:
        print("no one-target form (the step's state functions are not c8's)")
        return src
    src = src.replace(satx, ONE_HELPERS + satx)
    src = src.replace(sin, sin.replace("{\n", "{\n#ifdef GVM_ONE\n    return _S0.Load(one_at(t, c));\n#endif\n"))
    src = src.replace(sany, sany.replace("{\n", "{\n    [branch] if (_GvmOne != 0u) return _S0.Load(one_at(t, c));\n"))
    i = src.find(pix, src.index("tick_out frag(v2f i) {"))   # (frag's, not frag_copy's)
    if i < 0:
        print("no one-target form (frag's pixel is not c8's)")
        return src
    j = src.index("{\n", src.index("[branch] if (core == 0u ?", i))
    src = (src[:i] + "#ifdef GVM_ONE\n    core = one_core(p.x);\n"
           "    uint j = p.x - one_x(core);   // (gvm_one: the column is the pixel; its row picks the texel, frag_one)\n"
           "    [branch] if (core >= _NCores || j >= SPIX) {\n#else\n" + src[i:j] + "{\n#endif" + src[j + 1:])
    return src.rstrip("\n") + ONE_FRAG


def main():
    step, out = sys.argv[1], sys.argv[2]
    src = open(step).read()
    src, n0 = re.subn(r'#include "[./]*(gpu_vm/)?isa\.h"', '#include "isa.cginc"', src)   # (steps/ and build/tick_gen/B/)
    src, n1 = re.subn(r"cbuffer PassParams : register\(b0\) \{[^}]*\};", '#include "params.cginc"', src)
    src, n2 = re.subn(r"cbuffer CommitParams : register\(b1\) \{[^}]*\};", "", src)
    src = re.sub(r"\s*:\s*register\(t\d+\)", "", src)
    # the world has no external lines: their word is 0 where it is read (a static array indexed by the core became an
    # indexable temp of every pixel)
    src, n3 = re.subn(r"_Ext\[[^\]]*\]", "uint4(0, 0, 0, 0)", src)
    assert n0 == 1 and n1 == 1 and n2 == 1, "the step's include and cbuffers are not as expected"
    head = "// %s for Unity, made by gpu_vm/platform/unity_tick.py: do not edit\n" % os.path.basename(step)
    # c0nb (core 0's own write cache): the step compiled again with CORE0=1 draws core 0 (GvmTick0.shader), the plain
    # one every other core
    head += "#define GVM_HAS_CORE0 %d\n" % (1 if "#ifndef CORE0" in src else 0)
    # c0rt (as the RISC-V machine's tick sizes each core's cache by its geometry): core 0's cache chosen at run time, in the
    # same compile and draw, its quad C0ROWS tall (GvmTick.shader); a step without it has a core 0 as tall as a worker
    if "static const uint C0RT = 1;" not in src:
        head += "static const uint C0RT = 0;\nstatic const uint C0ROWS = 0;\n"
    # idlew 4: the tick's quads leave out the workers parked as they were, and GvmTick's second pass copies them (frag_copy)
    head += "#define GVM_COPY %d\n" % (1 if "tick_out frag_copy(" in src else 0)
    src = one_target(src)
    open(os.path.join(out, "tick.cginc"), "w", newline="\n").write(head + src)
    open(os.path.join(out, "isa.cginc"), "w", newline="\n").write(open(os.path.join(HERE, "..", "isa.h")).read())
    print("tick.cginc and isa.cginc from", step)


if __name__ == "__main__":
    main()
