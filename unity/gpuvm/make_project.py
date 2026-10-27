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
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
WORLD = os.path.join(REPO, "unity", "ShaderEmu")
TEMPLATE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "VRChatCreatorCompanion", "VRCTemplates", "World")
SDK = ("com.vrchat.base", "com.vrchat.worlds", "com.vrchat.core.vpm-resolver")
SHADERS = ("Display.shader", "DisplayShow.shader", "Terminal.shader", "MachineBlit.cginc")
# the RISC-V machine beside it (ShadersRv: the world's own, named ShaderEmuRv/...; Scripts/EmuRvMachine.cs: the world's
# EmuMachine under another name), to compare the two in one project
RV_SHADERS = ("Machine.shader", "MachineTickLean.shader", "MachineCommitPoints.shader", "MachineGather.shader", "GpuDraw.shader",
              "GpuDrawPass.cginc", "Readback.shader", "Display.shader", "MachineBlit.cginc", "MachineCommit.cginc",
              "MachineWindow.cginc", "lean/tick.hlsl")


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


def write(path, text):
    """Only a changed file: an untouched one keeps its date, and Unity compiles nothing again for it."""
    if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8", newline="\n").write(text)


def write_src(src, dst):
    """rvc_opt's src/*.h as .cginc, as the world's "Sync shader sources" makes them."""
    for f in sorted(os.listdir(src)):
        if f.endswith(".h"):
            text = re.sub(r'(#include\s+"[^"]+)\.h"', r'\1.cginc"', open(os.path.join(src, f), encoding="utf-8").read())
            write(os.path.join(dst, f[:-2] + ".cginc"), text)


def sub(text, old, new):
    assert old in text, "EmuMachine.cs has no: " + old
    return text.replace(old, new, 1)


def rv_machine(world_cs):
    """The world's EmuMachine as EmuRvMachine, with the measuring hooks of unity/gpuvm's and its host flags a field."""
    s = open(world_cs, encoding="utf-8").read()
    s = sub(s, "public class EmuMachine : UdonSharpBehaviour", "public class EmuRvMachine : UdonSharpBehaviour")
    s = sub(s, "    public bool fetchTest;\n", r'''    public bool fetchTest;
    // (unity/gpuvm/minimal: GvmProbe) typed at the console, and what it printed; the host flags' bits 0-1 (the desktop
    // at boot, the console as tabs: 0 is the harness's --no-desktop)
    [HideInInspector] public string hostInput = "";
    [HideInInspector] public string consoleOut = "";
    [HideInInspector] public int consolePrinted;
    public int hostWants = 3;
''')
    s = sub(s, "        if (running) Inputs();\n", r'''        if (running && hostInput.Length > 0)
        {
            int typed = Mathf.Clamp(254 - ((charTail - charHead) & 255), 0, hostInput.Length);
            for (int i = 0; i < typed; i++) RemoteChar(hostInput[i] == '\n' ? '\r' : hostInput[i]);
            hostInput = hostInput.Substring(typed);
        }
        if (running) Inputs();
''')
    s = sub(s, "            if (c != 0 && terminal != null) terminal.PutChar(c);\n", r'''            if (c != 0 && terminal != null) terminal.PutChar(c);
            if (c != 0)
            {
                consoleOut += (char)c;
                consolePrinted++;
            }
''')
    s = sub(s, "        if (terminal != null) terminal.Clear();\n",
            "        if (terminal != null) terminal.Clear();\n        consoleOut = \"\";\n        consolePrinted = 0;\n")
    s = sub(s, "        if (Time.time - statsAt >= 0.5f) ShowStats();\n",
            "        if (consoleOut.Length > 32768) consoleOut = consoleOut.Substring(consoleOut.Length - 16384);\n"
            "        if (Time.time - statsAt >= 0.5f) ShowStats();\n")
    s = sub(s, '_HostFlags", 3 |', '_HostFlags", (hostWants & 3) |')
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project")
    ap.add_argument("--sdk-from", help="a VCC project whose Packages hold com.vrchat.base and com.vrchat.worlds")
    ap.add_argument("--template", default=TEMPLATE)
    ap.add_argument("--tick", help="a gpu_vm step to draw in place of the committed tick.cginc's (v0009), e.g. "
                    "build/tick_gen/c0e/v0003.hlsl: gpu_vm/platform/unity_tick.py makes it")
    ap.add_argument("--world", action="store_true", help="the whole room (unity/ShaderEmu, its models and textures as "
                    "committed) under the gpu_vm files, and the room's shader packages (world/install_packages.py)")
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
    if a.world:
        tree(WORLD, se, skip_meta=False)
        scenes = os.path.join(p, "Assets", "Scenes")   # "Build world" starts from VCC's default scene
        if not os.path.exists(scenes) and a.sdk_from:
            shutil.copytree(os.path.join(a.sdk_from, "Assets", "Scenes"), scenes)
        subprocess.check_call([sys.executable, os.path.join(REPO, "world", "install_packages.py"), p])
    tree(os.path.join(WORLD, "Scripts"), os.path.join(se, "Scripts"))
    for f in SHADERS:
        copy(os.path.join(WORLD, "Shaders", f), os.path.join(se, "Shaders", f))
    src = os.path.join(REPO, "experiments", "rvc_opt", "src")
    write_src(src, os.path.join(se, "Shaders", "src"))
    # the RISC-V machine: the world's shaders as ShaderEmuRv/..., with rvc_opt's src/ unchanged, and its EmuMachine
    rv = os.path.join(se, "ShadersRv")
    for f in RV_SHADERS:
        text = open(os.path.join(WORLD, "Shaders", f), encoding="utf-8").read()
        write(os.path.join(rv, f), re.sub(r'^Shader "ShaderEmu/', 'Shader "ShaderEmuRv/', text, flags=re.M))
    for f in ("crt.cginc", "helpers.cginc"):
        copy(os.path.join(REPO, "experiments", "rvc_opt", f), os.path.join(rv, f))
    write_src(src, os.path.join(rv, "src"))
    write(os.path.join(se, "Scripts", "EmuRvMachine.cs"), rv_machine(os.path.join(WORLD, "Scripts", "EmuMachine.cs")))
    copy(os.path.join(WORLD, "Editor", "ShaderEmuImages.cs"), os.path.join(se, "Editor", "ShaderEmuImages.cs"))
    copy(os.path.join(WORLD, "Textures", "TerminalFont.png"), os.path.join(se, "Textures", "TerminalFont.png"))
    tree(os.path.join(HERE, "Assets", "ShaderEmu"), se)                       # the gpu_vm copy's files over the world's
    if not a.world:   # (the world's builder has what minimal's scene builders stand in for, under the same names)
        tree(os.path.join(HERE, "minimal", "Editor"), os.path.join(se, "Editor"))
    tree(os.path.join(HERE, "minimal", "Measure"), os.path.join(se, "Measure"))
    timer = os.path.join(HERE, "minimal", "GpuTimer", "GpuTimer.dll")   # (a measure: GpuTimer/build.bat makes it)
    if os.path.exists(timer):
        copy(timer, os.path.join(se, "Measure", "Plugins", "x86_64", "GpuTimer.dll"))
    if a.tick:
        subprocess.check_call([sys.executable, os.path.join(REPO, "gpu_vm", "platform", "unity_tick.py"), a.tick,
                               os.path.join(se, "Shaders", "gvm")])
    print("project at", p, "with", ", ".join("%s %s" % (n, v["version"]) for n, v in locked.items()))


if __name__ == "__main__":
    main()
