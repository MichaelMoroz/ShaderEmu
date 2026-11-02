#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OpenCNC3D's play folder, made by its own bake scripts from the cache: what its
game/make-build.sh stages beside the program, as far as the machine's build draws it.

    python3 linux/opencnc3d/play.py SRC CACHE

SRC is build.sh's fetched OpenCNC3D tree, CACHE bake.py's and data.py's cache (dos/, missions/,
packs/). OpenCNC3D's bakers look for the 1995 data in three places of their tree, each made a
link into CACHE/dos here: data/dosdata (most), playable/dosdata (bake_doscrate.py) and
dosart/ with dosart/cd1/ (bake_dossidebar.py, through menu/tools/mixshp.py's MIX_PATHS);
and for TRANSIT.MIX, dosdata/transit/. CACHE/play then gets:
  - the packs bake.py baked (the cartridge's scenarios, bake5.py's), at the top;
  - missions/: the cartridge's .INI and .BIN;
  - dosdata/: the 1995 MIX files, TRANSIT.MIX in dosdata/transit/ where the game reads it;
  - every pack of BAKERS below, each baked by its own script in its own folder, in order.
Nothing of it goes into this repository; overlay.py copies the folder into a private image.
OPENCNC3D_PACKS="a.pack b.pack" bakes only those (the others are left out).
"""
import hashlib
import os
import shutil
import subprocess
import sys

# name -> (folder of SRC the baker runs in, its script and arguments, where the pack comes out
# (relative to SRC), extra folders of SRC for its imports)
BAKERS = [
    ("dosmenu.pack", "menu", ["bake_dosmenu.py"], "menu/dosmenu.pack", []),
    ("campaign.pack", "game", ["bake_campaign.py"], "game/campaign.pack", []),
    ("dossidebar.pack", "game", ["bake_dossidebar.py"], "game/dossidebar.pack", ["menu/tools"]),
    ("hud640.pack", "tools/sidebar_redesign", ["bake_pack.py"], "playable/hud640.pack", []),
    ("dosinfantry.pack", "game", ["bake_dosinfantry.py"], "game/dosinfantry.pack", []),
    ("efx.pack", "game", ["bake_efx.py"], "game/efx.pack", []),
    ("logos.pack", "game", ["bake_logos.py"], "game/logos.pack", []),
    ("n64infantry.pack", "game", ["bake_n64infantry.py"], "game/n64infantry.pack", []),
    ("dostib.pack", "game", ["bake_dostiberium.py"], "game/dostib.pack", []),
    ("verdict.pack", "game", ["bake_verdict.py"], "game/verdict.pack", []),
    ("smudge.pack", "game", ["bake_smudges.py"], "game/smudge.pack", []),
    ("doscrate.pack", "game", ["bake_doscrate.py"], "game/doscrate.pack", []),
]


def link(at, to):
    os.makedirs(os.path.dirname(at), exist_ok=True)
    if os.path.islink(at):
        if os.readlink(at) == to:
            return
        os.unlink(at)
    elif os.path.isdir(at):
        shutil.rmtree(at)
    elif os.path.exists(at):
        os.unlink(at)
    os.symlink(to, at)


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest() if os.path.exists(path) else None


def main():
    src, cache = sys.argv[1:3]
    dos = os.path.abspath(os.path.join(cache, "dos"))
    link(os.path.join(dos, "transit", "TRANSIT.MIX"), os.path.join(dos, "TRANSIT.MIX"))
    link(os.path.join(src, "data", "dosdata"), dos)
    link(os.path.join(src, "playable", "dosdata"), dos)
    for name in os.listdir(dos):
        if name.endswith(".MIX"):
            link(os.path.join(src, "dosart", name), os.path.join(dos, name))
            link(os.path.join(src, "dosart", "cd1", name), os.path.join(dos, name))
    play = os.path.join(cache, "play")
    if os.path.isdir(play):
        shutil.rmtree(play)
    os.makedirs(os.path.join(play, "missions"))
    os.makedirs(os.path.join(play, "dosdata", "transit"))
    n = 0
    for name in sorted(os.listdir(os.path.join(cache, "packs"))):
        if name.endswith(".pack"):
            shutil.copyfile(os.path.join(cache, "packs", name), os.path.join(play, name))
            n += 1
    for name in sorted(os.listdir(os.path.join(cache, "missions"))):
        shutil.copyfile(os.path.join(cache, "missions", name), os.path.join(play, "missions", name))
        n += 1
    # (no links in the folder: Windows, which makes the image, cannot read WSL's; the brain's
    # theaters beside the program are linked by overlay.py's ocnc-play when it runs)
    for name in sorted(os.listdir(dos)):
        if name.endswith(".MIX"):
            to = os.path.join("transit", name) if name == "TRANSIT.MIX" else name
            shutil.copyfile(os.path.join(dos, name), os.path.join(play, "dosdata", to))
            n += 1
    want = os.environ.get("OPENCNC3D_PACKS", "").split() or [b[0] for b in BAKERS]
    layout = os.path.join(src, "game", "hud640_layout.h")
    layout_was = md5(layout)
    env = dict(os.environ, OPENCNC3D_ROOT=os.path.abspath(src))
    for name, folder, cmd, made, imports in BAKERS:
        if name not in want:
            continue
        print("play.py: baking %s (%s/%s)" % (name, folder, cmd[0]))
        env["PYTHONPATH"] = os.pathsep.join(os.path.join(src, p) for p in imports)
        r = subprocess.run([sys.executable] + cmd, cwd=os.path.join(src, folder), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            sys.stdout.write(r.stdout[-3000:])
            raise SystemExit("play.py: %s failed" % cmd[0])
        shutil.copyfile(os.path.join(src, made), os.path.join(play, name))
        if name == "dossidebar.pack":       # bake_doscrate.py reads it from playable/
            shutil.copyfile(os.path.join(src, made), os.path.join(src, "playable", name))
        n += 1
    if md5(layout) != layout_was:
        print("play.py: hud640.pack's bake changed game/hud640_layout.h: build the game again")
    print("play.py: %s, %d files" % (play, n))


if __name__ == "__main__":
    main()
