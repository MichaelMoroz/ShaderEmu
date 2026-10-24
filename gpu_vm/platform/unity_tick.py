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
    open(os.path.join(out, "tick.cginc"), "w", newline="\n").write(head + src)
    open(os.path.join(out, "isa.cginc"), "w", newline="\n").write(open(os.path.join(HERE, "..", "isa.h")).read())
    print("tick.cginc and isa.cginc from", step)


if __name__ == "__main__":
    main()
