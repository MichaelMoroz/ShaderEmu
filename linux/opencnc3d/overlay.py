#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The brain's programs and what they read into a private image's root, never the shared one.

    python3 linux/opencnc3d/overlay.py ROOT CACHE OUT

ROOT is a copy of a flat root (build/nommu/flatroot, or build/images/nommu/overlay) that
linux/nommu/image.py then makes an image of; CACHE is bake.py's and data.py's cache (its
dos/, missions/ and packs/); OUT build.sh's OPENCNC3D_OUT, whose usr/bin/opencnc3d-brain and
usr/bin/opencnc3d are made flat here with linux/nommu/flat.py. Into ROOT: the two programs, and
in /usr/share/opencnc3d the DOS theaters' MIX files (their palettes are all the brain reads of the
1995 data), every mission bake.py made (the cartridge's .INI and its .MAP as a .BIN) and every
pack it baked; with the play folder `ocnc-play`, and with the program too the Start menu's entry
(`nxapps.78-opencnc3d`) and its starter `opencnc3d-start`.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    root, cache, out = sys.argv[1:4]
    if os.path.isfile(out):           # (a program's path, as before the picture)
        out = os.path.dirname(os.path.dirname(os.path.dirname(out)))
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, "usr", "bin"))
        names = [n for n in ("opencnc3d-brain", "opencnc3d")
                 if os.path.exists(os.path.join(out, "usr", "bin", n))]
        for n in names:
            p = os.path.join(t, "usr", "bin", n)
            shutil.copyfile(os.path.join(out, "usr", "bin", n), p)
            os.chmod(p, 0o755)
        subprocess.check_call([sys.executable, os.path.join(HERE, "..", "nommu", "flat.py"), t])
        os.makedirs(os.path.join(root, "usr", "bin"), exist_ok=True)
        for n in names:
            shutil.copy(os.path.join(t, "usr", "bin", n), os.path.join(root, "usr", "bin", n))
    share = os.path.join(root, "usr", "share", "opencnc3d")
    os.makedirs(share, exist_ok=True)
    n = 0
    for name in ("TEMPERAT.MIX", "DESERT.MIX", "WINTER.MIX"):
        src = os.path.join(cache, "dos", name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(share, name))
            n += 1
    for folder, ends in (("missions", (".INI", ".BIN")), ("packs", (".pack",))):
        d = os.path.join(cache, folder)
        for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if name.endswith(ends):
                shutil.copyfile(os.path.join(d, name), os.path.join(share, name))
                n += 1
    # the game's folder (play.py makes it in CACHE/play): what OpenCNC3D's make-build.sh stages
    # beside its program, read from the ROM; ocnc-play makes /tmp/ocnc of links to it, where
    # the program runs and writes its settings
    play = os.path.join(cache, "play")
    m = 0
    if os.path.isdir(play):
        dst = os.path.join(share, "play")
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        shutil.copytree(play, dst)
        m = sum(len(f) for _, _, f in os.walk(dst))
        starter = os.path.join(root, "usr", "bin", "ocnc-play")
        with open(starter, "w", newline="\n") as f:
            f.write("#!/bin/sh\n"
                    "# OpenCNC3D's folder: links to what it reads (/usr/share/opencnc3d/play), its own files beside\n"
                    "mkdir -p /tmp/ocnc && cd /tmp/ocnc || exit 1\n"
                    "for f in /usr/share/opencnc3d/play/*; do [ -e \"$(basename \"$f\")\" ] || ln -s \"$f\" .; done\n"
                    "# the brain opens the theaters where it runs\n"
                    "for f in TEMPERAT DESERT WINTER; do [ -e $f.MIX ] || ln -s dosdata/$f.MIX .; done\n")
        os.chmod(starter, 0o755)
        # the desktop's entry, only in an image that has the program and the cartridge's play folder:
        # the bar runs a command's words with no shell, so a starter of its own
        if "opencnc3d" in names:
            start = os.path.join(root, "usr", "bin", "opencnc3d-start")
            with open(start, "w", newline="\n") as f:
                f.write("#!/bin/sh\n"
                        "# OpenCNC3D from the Start menu: its folder made, then the program there\n"
                        "ocnc-play && cd /tmp/ocnc && exec opencnc3d \"$@\"\n")
            os.chmod(start, 0o755)
            with open(os.path.join(root, "usr", "share", "nxapps.78-opencnc3d"), "w", newline="\n") as f:
                f.write("Games/OpenCNC3D=opencnc3d-start\n")
    print("overlay: %s, %d files in /usr/share/opencnc3d and %d in its play folder" % (", ".join(names), n, m))


if __name__ == "__main__":
    main()
