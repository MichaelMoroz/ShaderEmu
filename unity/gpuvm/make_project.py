#!/usr/bin/env python3
"""A Unity project of the gpu_vm machine alone (unity/gpuvm/README.md): no room, no packages but VRChat's SDK.

    python unity/gpuvm/make_project.py C:/Development/VRChat/ShaderEmu-GPUVM --sdk-from E:/Development/VRChat/VRCFluid

Makes the project from VRChat Creator Companion's World template (its ProjectSettings and Packages/manifest.json),
copies VRChat's Base and Worlds SDK (UdonSharp and ClientSim are in Worlds) from another VCC project's Packages, and
writes Assets/ShaderEmu: the world's Scripts with unity/gpuvm's over them, the shaders the machine draws with
(gvm/, src/ from experiments/rvc_opt as the world's "Sync shader sources" makes them, Display, DisplayShow, Terminal,
MachineBlit.cginc), ShaderEmuImages.cs and ShaderEmuGvm.cs, and minimal/'s builder and measuring script. Run again
it writes the repository's files anew and leaves the rest (Library, Generated, Images) alone.
"""
import argparse
import json
import os
import re
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
WORLD = os.path.join(REPO, "unity", "ShaderEmu")
TEMPLATE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "VRChatCreatorCompanion", "VRCTemplates", "World")
SDK = ("com.vrchat.base", "com.vrchat.worlds", "com.vrchat.core.vpm-resolver")
SHADERS = ("Display.shader", "DisplayShow.shader", "Terminal.shader", "MachineBlit.cginc")


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def tree(src, dst, skip_meta=True):
    for base, _, files in os.walk(src):
        for f in files:
            if skip_meta and f.endswith(".meta"):
                continue
            s = os.path.join(base, f)
            copy(s, os.path.join(dst, os.path.relpath(s, src)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project")
    ap.add_argument("--sdk-from", help="a VCC project whose Packages hold com.vrchat.base and com.vrchat.worlds")
    ap.add_argument("--template", default=TEMPLATE)
    a = ap.parse_args()
    p = os.path.abspath(a.project)
    os.makedirs(p, exist_ok=True)

    # the template's settings and Unity's packages, once
    if not os.path.exists(os.path.join(p, "ProjectSettings")):
        shutil.copytree(os.path.join(a.template, "ProjectSettings"), os.path.join(p, "ProjectSettings"))
    pk = os.path.join(p, "Packages")
    os.makedirs(pk, exist_ok=True)
    if not os.path.exists(os.path.join(pk, "manifest.json")):
        copy(os.path.join(a.template, "Packages", "manifest.json"), os.path.join(pk, "manifest.json"))
    locked = {}
    for name in SDK:
        if not os.path.exists(os.path.join(pk, name)):
            if not a.sdk_from:
                raise SystemExit("no %s: give --sdk-from a VCC project that has it (or add it with VCC or vpm)" % name)
            shutil.copytree(os.path.join(a.sdk_from, "Packages", name), os.path.join(pk, name))
        version = json.load(open(os.path.join(pk, name, "package.json"), encoding="utf-8"))["version"]
        locked[name] = {"version": version, "dependencies": {"com.vrchat.base": version} if name == "com.vrchat.worlds" else {}}
    json.dump({"dependencies": {n: {"version": v["version"]} for n, v in locked.items()}, "locked": locked},
              open(os.path.join(pk, "vpm-manifest.json"), "w", encoding="utf-8"), indent=2)

    # Assets/ShaderEmu
    se = os.path.join(p, "Assets", "ShaderEmu")
    tree(os.path.join(WORLD, "Scripts"), os.path.join(se, "Scripts"))
    for f in SHADERS:
        copy(os.path.join(WORLD, "Shaders", f), os.path.join(se, "Shaders", f))
    src = os.path.join(REPO, "experiments", "rvc_opt", "src")
    for f in sorted(os.listdir(src)):
        if f.endswith(".h"):
            text = re.sub(r'(#include\s+"[^"]+)\.h"', r'\1.cginc"', open(os.path.join(src, f), encoding="utf-8").read())
            os.makedirs(os.path.join(se, "Shaders", "src"), exist_ok=True)
            out = os.path.join(se, "Shaders", "src", f[:-2] + ".cginc")
            if not os.path.exists(out) or open(out, encoding="utf-8").read() != text:
                open(out, "w", encoding="utf-8", newline="\n").write(text)
    copy(os.path.join(WORLD, "Editor", "ShaderEmuImages.cs"), os.path.join(se, "Editor", "ShaderEmuImages.cs"))
    copy(os.path.join(WORLD, "Textures", "TerminalFont.png"), os.path.join(se, "Textures", "TerminalFont.png"))
    tree(os.path.join(HERE, "Assets", "ShaderEmu"), se)                       # the gpu_vm copy's files over the world's
    tree(os.path.join(HERE, "minimal", "Editor"), os.path.join(se, "Editor"))
    tree(os.path.join(HERE, "minimal", "Measure"), os.path.join(se, "Measure"))
    print("project at", p, "with", ", ".join("%s %s" % (n, v["version"]) for n, v in locked.items()))


if __name__ == "__main__":
    main()
