#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The brain's objects at tick 0 against the mission's own INI (OpenCNC3D's brain/README.md check:
"SCG01EA (GDI 1) matches 60/60", by type, owner, cell and health).

    python3 check_objects.py MISSION.INI RUN.LOG

RUN.LOG holds the OBJ| lines OpenCNC3D_Dump_Objects printed after the start (opencnc3d-brain
prints them once). An INI object is matched by kind, type, owner and cell, and its health
(0..256 of the type's strength, Fixed_To_Cardinal) must be the object's. Prints
"objects: N of M as the INI says" and exits 1 on any difference.
"""
import sys
from collections import Counter

KIND = {"INFANTRY": "INFANTRY", "UNITS": "UNIT", "STRUCTURES": "BUILDING", "TERRAIN": "TERRAIN",
        "AIRCRAFT": "AIRCRAFT"}


def ini_objects(path):
    out, sec = [], None
    for raw in open(path, encoding="latin-1"):
        line = raw.split(";")[0].strip()
        if not line:
            continue
        if line.startswith("["):
            sec = line.strip("[]").upper()
            continue
        if sec not in KIND or "=" not in line:
            continue
        key, val = line.split("=", 1)
        f = [x.strip() for x in val.split(",")]
        if sec == "TERRAIN":
            out.append((KIND[sec], f[0].upper(), "None", int(key), None))
        else:
            out.append((KIND[sec], f[1].upper(), f[0], int(f[3]), int(f[2])))
    return out


def brain_objects(path):
    out = []
    for line in open(path, encoding="latin-1"):
        if not line.startswith("OBJ|"):
            continue
        if "|limbo=" in line:
            f = line.strip().split("|")
            kv = dict(x.split("=", 1) for x in f[4:] if "=" in x)
            out.append((f[1], f[2].upper(), f[3], int(kv["inicell"]), int(kv["str"]), int(kv["max"])))
    return out


def main():
    want = ini_objects(sys.argv[1])
    have = brain_objects(sys.argv[2])
    pool = Counter((k, t, o, c) for k, t, o, c, _s, _m in have)
    strength = {}
    for k, t, o, c, s, m in have:
        strength.setdefault((k, t, o, c), []).append((s, m))
    ok, bad = 0, []
    for k, t, o, c, hp in want:
        key = (k, t, o, c)
        if pool[key] <= 0:
            bad.append("not in the brain: %s %s %s cell %d" % key)
            continue
        pool[key] -= 1
        s, m = strength[key].pop(0)
        if hp is not None:
            expect = min(m, max(1, (m * hp + 0x80) >> 8)) if hp else 0
            if s != expect and not (hp >= 256 and s == m):
                bad.append("%s %s %s cell %d: strength %d of %d, the INI's %d/256 is %d"
                           % (k, t, o, c, s, m, hp, expect))
                continue
        ok += 1
    extra = sum(v for v in pool.values() if v > 0)
    print("objects: %d of %d as the INI says (%d in the brain, %d not in the INI)"
          % (ok, len(want), len(have), extra))
    for b in bad:
        print("  " + b)
    return 0 if ok == len(want) and not extra else 1


if __name__ == "__main__":
    sys.exit(main())
