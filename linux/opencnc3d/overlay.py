#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The brain's program and what it reads into a private image's root, never the shared one.

    python3 linux/opencnc3d/overlay.py ROOT CACHE PROGRAM

ROOT is a copy of a flat root (build/nommu/flatroot, or build/images/nommu/overlay) that
linux/nommu/image.py then makes an image of; CACHE is bake.py's and data.py's cache (its
dos/ and missions/); PROGRAM the machine's opencnc3d-brain (build.sh's OPENCNC3D_OUT), made
flat here with linux/nommu/flat.py. Into ROOT: /usr/bin/opencnc3d-brain, and in
/usr/share/opencnc3d the DOS theaters' MIX files (their palettes are all the brain reads of the
1995 data) and every mission bake.py made (the cartridge's .INI and its .MAP as a .BIN).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    root, cache, program = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, "usr", "bin"))
        p = os.path.join(t, "usr", "bin", "opencnc3d-brain")
        shutil.copyfile(program, p)
        os.chmod(p, 0o755)
        subprocess.check_call([sys.executable, os.path.join(HERE, "..", "nommu", "flat.py"), t])
        os.makedirs(os.path.join(root, "usr", "bin"), exist_ok=True)
        shutil.copy(p, os.path.join(root, "usr", "bin", "opencnc3d-brain"))
    share = os.path.join(root, "usr", "share", "opencnc3d")
    os.makedirs(share, exist_ok=True)
    n = 0
    for name in ("TEMPERAT.MIX", "DESERT.MIX", "WINTER.MIX"):
        src = os.path.join(cache, "dos", name)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(share, name))
            n += 1
    for name in sorted(os.listdir(os.path.join(cache, "missions"))):
        if name.endswith((".INI", ".BIN")):
            shutil.copyfile(os.path.join(cache, "missions", name), os.path.join(share, name))
            n += 1
    print("overlay: /usr/bin/opencnc3d-brain and %d files in /usr/share/opencnc3d" % n)


if __name__ == "__main__":
    main()
