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
    src = src.replace('#include "../../isa.h"', '#include "isa.cginc"')
    src, n1 = re.subn(r"cbuffer PassParams : register\(b0\) \{[^}]*\};", '#include "params.cginc"', src)
    src, n2 = re.subn(r"cbuffer CommitParams : register\(b1\) \{[^}]*\};", "", src)
    src = re.sub(r"\s*:\s*register\(t\d+\)", "", src)
    assert n1 == 1 and n2 == 1, "the step's cbuffers are not as expected"
    head = "// %s for Unity, made by gpu_vm/platform/unity_tick.py: do not edit\n" % os.path.basename(step)
    open(os.path.join(out, "tick.cginc"), "w", newline="\n").write(head + src)
    open(os.path.join(out, "isa.cginc"), "w", newline="\n").write(open(os.path.join(HERE, "..", "isa.h")).read())
    print("tick.cginc and isa.cginc from", step)


if __name__ == "__main__":
    main()
