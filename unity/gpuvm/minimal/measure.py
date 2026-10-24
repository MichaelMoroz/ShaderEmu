#!/usr/bin/env python3
"""One measuring sequence for gpu_vm in Unity and in the harness (unity/gpuvm/README.md, "Measuring").

    python unity/gpuvm/minimal/measure.py probe C:/Development/VRChat/ShaderEmu-GPUVM   (writes its gvm_probe.txt)
    python unity/gpuvm/minimal/measure.py harness OUT.log [--ticks N]                     (the same typed to the harness)
    python unity/gpuvm/minimal/measure.py table LOG...                                     (a harness log or gvm_probe.log)

The console is typed three lines, each when the one before has printed its mark: the idle console 4 s, `nx` and 30 s,
the idle desktop 4 s, then nxterm, nxedit and nxfiles each started and killed after 4 s (wide.py's measure: the busy
jiffies of /proc/stat in the window, less the idle desktop's), then 330 frames of imdemo with each of its imstat lines
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
    '; '.join([W('im.0'), 'IMDEMO_FRAMES=330 imdemo 2>&1 | while read l; do echo IM $(cut -d" " -f1 /proc/uptime) $l; done',
               W('im.1'), 'echo AL""LDONE']),
]

PROBE = """# unity/gpuvm/minimal/measure.py's sequence
mark boot
wait gpuvm: up in
wait root@gpuvm:~#
mark prompt
sample 5 console-idle
type {0}\\n
wait M1D
shot desktop
type {1}\\n
wait M2D
type {2}\\n
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
        open(os.path.join(sys.argv[2], "gvm_probe.txt"), "w", newline="\n").write(PROBE.format(*LINES))
    elif cmd == "harness":
        out = os.path.abspath(sys.argv[2])
        args = ["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "gpu_queue.ps1"), os.path.join(ROOT, "bin", "rvc_harness.exe"),
                "--gvm", "build/gvm_apps/img-cur", "--gvm-tick", "gpu_vm/tick/steps/v0009.hlsl", "--no-viz", "--no-stdin",
                "--uart-log", out, "--seconds", "200", "--expect", "root@gpuvm:~# ", "--send", LINES[0] + "\\n",
                "--expect", "M1D", "--send", LINES[1] + "\\n", "--expect", "M2D", "--send", LINES[2] + "\\n", "--until", "ALLDONE"]
        subprocess.run(args + sys.argv[3:], cwd=ROOT)
        table(out)
    elif cmd == "table":
        for p in sys.argv[2:]:
            table(p)


if __name__ == "__main__":
    main()
