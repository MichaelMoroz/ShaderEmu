#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bake OpenCNC3D's mission packs from the owner's European cartridge, outside the tree.

    python3 linux/opencnc3d/bake.py --tree OCNC --cache DIR [--rom PATH] SCEN...

OCNC is OpenCNC3D's tree as linux/opencnc3d/build.sh fetched it (its bake5.py, its
romdump tools, its support tables); DIR a cache outside this repository. The cartridge
(OPENCNC3D_ROM, or --rom) is checked against the European dump's SHA-1, extracted with
OpenCNC3D's own tools/romdump/archive.py into DIR/extracted, laid out by type as
OpenCNC3D's asset store expects (DIR/share/assets/extracted/<EXT>/), and every pack is
written to DIR/packs/<SCEN>.pack and read back by OpenCNC3D's packinspect.py.

Our parts are bake/pk4mod.py (the module bake5.py imports, whose original is lost) and
bake/n64_terrain.py (the terrain resolver, whose original is not published); both are
copied into the fetched tree at each run. Nothing here is written into this repository.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROM_SHA1 = "0d5211e211e7fc063c63c3e8235b62bc288ce305"   # Command & Conquer (Europe) (En,Fr)
ROM_SIZE = 33554432
# The main archive table, then the English trailing one (re-findings M1); the French
# table carries the same names and is left out.
TABLES = ["dir_0460540", "dir_0A4D778", "dir_019EE70", "dir_019F380", "dir_019F890"]


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(dst) or os.path.exists(dst):
        if os.path.islink(dst) and os.readlink(dst) == src:
            return
        if os.path.isdir(dst) and not os.path.islink(dst):
            shutil.rmtree(dst)
        else:
            os.remove(dst)
    os.symlink(src, dst)


def stage(tree, cache, rom):
    """The cartridge into the cache, and OpenCNC3D's asset store pointed at it."""
    crom = os.path.join(cache, "cnc_eu.z64")
    if not (os.path.exists(crom) and os.path.getsize(crom) == ROM_SIZE
            and sha1(crom) == ROM_SHA1):
        if not rom or not os.path.exists(rom):
            raise SystemExit("OPENCNC3D-NO-DATA: no cartridge (set OPENCNC3D_ROM or --rom)")
        if os.path.getsize(rom) != ROM_SIZE or sha1(rom) != ROM_SHA1:
            raise SystemExit("%s is not the European cartridge (SHA-1 %s expected)"
                             % (rom, ROM_SHA1))
        shutil.copyfile(rom, crom)
    ex = os.path.join(cache, "extracted")
    if not os.path.isdir(os.path.join(ex, TABLES[0])):
        subprocess.check_call([sys.executable, "-I",
                               os.path.join(tree, "tools", "romdump", "archive.py"),
                               crom, "extract", ex])
    share = os.path.join(cache, "share")
    byext = os.path.join(share, "assets", "extracted")
    if not os.path.isdir(byext):
        for t in TABLES:
            d = os.path.join(ex, t)
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                ext = os.path.splitext(fn)[1].lstrip(".").upper() or "NONE"
                dst = os.path.join(byext, ext, fn.upper())
                if not os.path.exists(dst):
                    link(os.path.join(d, fn), dst)
    link(crom, os.path.join(share, "rom", "cnc_eu.z64"))
    # Where OpenCNC3D's tools look for the cartridge and the asset store.
    bakery = os.path.join(tree, "tools", "bakery")
    link(crom, os.path.join(tree, "data", "rom", "cnc_eu.z64"))
    link(share, os.path.join(bakery, "sharecopy_store"))
    link(crom, os.path.join(bakery, "sharecopy", "rom", "cnc_eu.z64"))
    link(share, os.path.join(bakery, "support", "share", "OpenCNC3D"))
    for f in ("pk4mod.py", "n64_terrain.py"):
        shutil.copyfile(os.path.join(HERE, "bake", f), os.path.join(bakery, f))
    return share


def missions(tree, share, cache, scens):
    """The brain's mission files: the cartridge's .INI verbatim and its .MAP as a PC .BIN
    by OpenCNC3D's game/missions/n64map_to_bin.py, into DIR/missions; each compared with
    the copy OpenCNC3D's tree carries, when it carries one."""
    import filecmp
    import importlib.util
    os.environ["OPENCNC3D_EXTRACTED"] = os.path.join(share, "assets", "extracted")
    path = os.path.join(tree, "game", "missions", "n64map_to_bin.py")
    spec = importlib.util.spec_from_file_location("n64map_to_bin", path)
    conv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(conv)
    out = os.path.join(cache, "missions")
    os.makedirs(out, exist_ok=True)
    ex = os.environ["OPENCNC3D_EXTRACTED"]
    for scen in scens:
        ini = os.path.join(out, scen + ".INI")
        shutil.copyfile(os.path.join(ex, "INI", scen + ".INI"), ini)
        conv.convert(os.path.join(ex, "MAP", scen + ".MAP"), ini, os.path.join(out, scen + ".BIN"))
        for ext in ("INI", "BIN"):
            theirs = os.path.join(tree, "game", "missions", "%s.%s" % (scen, ext))
            if os.path.exists(theirs):
                same = filecmp.cmp(os.path.join(out, "%s.%s" % (scen, ext)), theirs, shallow=False)
                print("missions: %s.%s %s OpenCNC3D's game/missions copy"
                      % (scen, ext, "is byte for byte" if same else "DIFFERS from"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--rom", default=os.environ.get("OPENCNC3D_ROM"))
    ap.add_argument("--missions-only", action="store_true",
                    help="only the brain's .INI and .BIN, no pack")
    ap.add_argument("scen", nargs="+")
    a = ap.parse_args()
    tree, cache = os.path.abspath(a.tree), os.path.abspath(a.cache)
    os.makedirs(cache, exist_ok=True)
    share = stage(tree, cache, a.rom)
    missions(tree, share, cache, a.scen)
    if a.missions_only:
        return
    bakery = os.path.join(tree, "tools", "bakery")
    packs = os.path.join(cache, "packs")
    os.makedirs(packs, exist_ok=True)
    os.makedirs(os.path.join(bakery, "game"), exist_ok=True)
    for scen in a.scen:
        subprocess.check_call([sys.executable, os.path.join(bakery, "n64_terrain.py"),
                               os.path.join(share, "assets"), scen])
        subprocess.check_call([sys.executable, os.path.join(bakery, "bake5.py"), scen],
                              cwd=bakery)
        out = os.path.join(packs, scen + ".pack")
        shutil.move(os.path.join(bakery, "game", scen + ".pack"), out)
        subprocess.check_call([sys.executable, os.path.join(bakery, "packinspect.py"),
                               "summary", out])


if __name__ == "__main__":
    main()
