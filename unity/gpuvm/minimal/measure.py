#!/usr/bin/env python3
"""One measuring sequence for gpu_vm in Unity and in the harness (unity/gpuvm/README.md, "Measuring").

    python unity/gpuvm/minimal/measure.py probe C:/Development/VRChat/ShaderEmu-GPUVM [--rv] [--log NAME] [--file F]
                                         (writes its gvm_probe.txt, or F; --rv: the RISC-V machine's prompt)
    python unity/gpuvm/minimal/measure.py harness OUT.log [--ticks N]                     (the same typed to the harness)
    python unity/gpuvm/minimal/measure.py table LOG...                                     (a harness log or gvm_probe.log)

The console is typed three lines, each when the one before has printed its mark: the idle console 4 s, `nx` and 30 s,
the idle desktop 4 s, then nxterm, nxedit and nxfiles each started and killed after 4 s (wide.py's measure: the busy
jiffies of /proc/stat in the window, less the idle desktop's), then 300 frames of imdemo with each of its imstat lines
stamped by /proc/uptime. The probe then samples Unity's frames at two pass budgets on the idle desktop and in imdemo.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
W = lambda tag: 'echo WT %s $(cut -d" " -f1 /proc/uptime) $(head -1 /proc/stat)' % tag
LINES = [
    '; '.join(['cd /', 'export TERM=linux', W('idle.0'), 'sleep 4', W('idle.1'), W('nx.0'), 'nx', 'sleep 30', W('nx.1'),
               'echo M1""D']),
    '; '.join([W('idle2.0'), 'sleep 4', W('idle2.1'),
               'for p in nxterm nxedit nxfiles; do %s; $p & sleep 4; kill $!; %s; sleep 1; done' % (W('$p.0'), W('$p.1')),
               'echo M2""D']),
    '; '.join([W('im.0'), 'IMDEMO_FRAMES=300 imdemo 2>&1 | while read l; do echo IM $(cut -d" " -f1 /proc/uptime) $l; done',
               W('im.1'), 'echo AL""LDONE']),
]

PROBE = """log {log}
# unity/gpuvm/minimal/measure.py's sequence
mark boot
{boot}
mark prompt
sample 5 console-idle
type {l0}\\n
wait M1D
shot desktop
type {l1}\\n
wait M2D
type {l2}\\n
wait ALLDONE
mark done
sample 10 desktop-idle-8x8192
speed 6
sleep 2
sample 10 desktop-idle-16x8192
speed 5
tick 2
sleep 2
sample 10 desktop-idle-16x4096
type IMDEMO_FRAMES=5000 imdemo 2>/dev/null &\\n
sleep 8
tick 3
sleep 2
sample 10 imdemo-8x8192
speed 6
sleep 2
sample 10 imdemo-16x8192
speed 5
tick 2
sleep 2
sample 10 imdemo-16x4096
shot imdemo
stop
"""


def busy(v):
    return sum(v[:3]) + sum(v[4:8])   # all but idle, as wide.py counts


def table(path):
    text = open(path, encoding="latin-1").read()
    marks = {m[0]: (float(m[1]), list(map(int, m[2].split()))) for m in re.findall(r"WT (\S+) ([\d.]+) cpu +((?:\d+ ?)+)", text)}
    up = re.search(r"gpuvm: up in ([\d.]+) s", text)
    print(path)
    if up:
        print("  console up (guest): %s s" % up.group(1))
    if "idle.0" in marks:
        print("  uptime at the first command: %.2f s" % marks["idle.0"][0])
    wall = re.search(r"([\d.]+) mark: prompt", text)
    if wall:
        print("  console up (wall of play mode): %s s" % wall.group(1))
    span = lambda a: (marks[a + ".1"][0] - marks[a + ".0"][0], (busy(marks[a + ".1"][1]) - busy(marks[a + ".0"][1])) / 100.0)
    if "idle2.0" in marks and "idle2.1" in marks:
        t, b = span("idle2")
        rate = b / t
        print("  idle desktop: %.2f busy s in %.2f s" % (b, t))
        if "nx.1" in marks:
            t, b = span("nx")
            print("  nx (desktop start): %.2f busy s in %.2f s, %.2f less idle" % (b, t, b - rate * t))
        for p in ("nxterm", "nxedit", "nxfiles"):
            if p + ".1" in marks:
                t, b = span(p)
                print("  %s: %.2f busy s in %.2f s, %.2f less idle" % (p, b, t, b - rate * t))
    im = [(float(a), int(f)) for a, f in re.findall(r"IM ([\d.]+) imstat: frame (\d+),", text)]
    if len(im) >= 2:
        print("  imdemo: %.1f frames a second (frames %d-%d)" % ((im[-1][1] - im[0][1]) / (im[-1][0] - im[0][0]), im[0][1], im[-1][1]))
    for line in re.findall(r"sample [\w.-]+: .*", text):
        print("  " + line)


def main():
    cmd = sys.argv[1]
    if cmd == "probe":
        rest = sys.argv[3:]
        opt = lambda name, default: rest[rest.index(name) + 1] if name in rest else default
        boot = "wait / # " if "--rv" in rest else "wait gpuvm: up in\nwait root@gpuvm:~#"
        text = PROBE.format(l0=LINES[0], l1=LINES[1], l2=LINES[2], log=opt("--log", "gvm_probe"), boot=boot)
        open(os.path.join(sys.argv[2], opt("--file", "gvm_probe.txt")), "w", newline="\n").write(text)
    elif cmd == "harness":
        # --image DIR and --tick STEP for gpu_vm (img-cur, v0009 unless given); --rv: the RISC-V machine as run.py runs
        # it (experiments/rvc_opt, NO_PAGING, build/images/nommu); the rest goes to the harness
        out = os.path.abspath(sys.argv[2])
        rest = sys.argv[3:]
        def take(name, default):
            if name not in rest:
                return default
            i = rest.index(name)
            v = rest[i + 1]
            del rest[i:i + 2]
            return v
        image, tick = take("--image", "build/gvm_apps/img-cur"), take("--tick", "gpu_vm/tick/steps/v0009.hlsl")
        rv = "--rv" in rest
        if rv:
            rest.remove("--rv")
            machine, prompt = ["--d3d11", "--rvc", "experiments/rvc_opt", "--define", "NO_PAGING", "--image", "linux-net", "--no-desktop"], "/ # "
        else:
            machine, prompt = ["--gvm", image, "--gvm-tick", tick], "root@gpuvm:~# "
        args = ["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "gpu_queue.ps1"), os.path.join(ROOT, "bin", "rvc_harness.exe")] + machine + [
                "--no-viz", "--no-stdin", "--uart-log", out, "--seconds", "200", "--expect", prompt, "--send", LINES[0] + "\\n",
                "--expect", "M1D", "--send", LINES[1] + "\\n", "--expect", "M2D", "--send", LINES[2] + "\\n", "--until", "ALLDONE"]
        subprocess.run(args + rest, cwd=ROOT)
        table(out)
    elif cmd == "table":
        for p in sys.argv[2:]:
            table(p)


if __name__ == "__main__":
    main()
