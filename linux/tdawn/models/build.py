#!/usr/bin/env python3
"""Makes `tdawn-models.dat` (pack.py) from the models made by script, and says each one's size.

    python3 linux/tdawn/models/build.py OUT.dat [--png SHEET.png]

The triangle budget of the plan (step 3: a vehicle 100 to 300, a building 200 to 800) is said
beside each; a model over it says so. `--png` draws every model from the tilted view at eight of
its facings (view.py), for looking at them.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildings  # noqa: E402
import pack  # noqa: E402
import vehicles  # noqa: E402

BUDGET = {"unit": (100, 300), "building": (200, 800), "aircraft": (100, 300), "infantry": (10, 60), "terrain": (10, 200)}


def models():
    return [make() for make in vehicles.ALL + buildings.ALL]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--png")
    a = p.parse_args()
    made = models()
    data = pack.pack(made)
    with open(a.out, "wb") as f:
        f.write(data)
    over = 0
    for m in made:
        low, high = BUDGET[m.kind]
        n = m.triangles()
        note = "" if n <= high else "  over the budget's %d" % high
        over += n > high
        print("%-5s %-8s %4d triangles in %d parts%s" % (m.name, m.kind, n, len(m.parts), note))
    print("%d models, %d triangles, %d bytes: %s" % (len(made), sum(m.triangles() for m in made), len(data), a.out))
    if a.png:
        import view
        view.sheet(data, a.png)
        print("pictures: " + a.png)
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main())
