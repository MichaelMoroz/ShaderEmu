"""What a holodeck would have shown, from rvc_cpu's --volume-log (docs/world/holodeck.md, "Checking it"): the frames a 3D
program gave a host, as runs of one kind: a flat screen (bit 31: the large screen before the seat), a world (draws with a place
in space) with what is drawn flat over it (the HUD layer: a status bar, text, a cross-hair, a console), or neither (an empty
world: the room would show nothing). Prints each run with its seconds of the guest's and frames, and a line of totals.

    python tools/volume_frames.py LOG [--empty-ok]

Exit 1 when a run of more than a second is an empty world (not flat, no world draw), unless --empty-ok.
"""
import re
import sys

LINE = re.compile(r'^([\d.]+) frame (\d+) list ([0-9a-f]+) commands (\d+) size (\d+)x(\d+) up (\d) flat (\d): world (\d+), flat (\d+), text (\d+)')


def kind(m):
    if m.group(8) == '1':
        return 'flat screen'
    if int(m.group(9)) == 0:
        return 'EMPTY world' if int(m.group(4)) or int(m.group(10)) or int(m.group(11)) else 'nothing'
    return 'world + HUD' if int(m.group(10)) + int(m.group(11)) else 'world alone'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__)
        return 2
    runs = []   # [kind, from s, to s, frames, most flat draws]
    for line in open(args[0], encoding='latin-1'):
        m = LINE.match(line)
        if not m or m.group(2) == '0':
            continue
        k, t, hud = kind(m), float(m.group(1)), int(m.group(10)) + int(m.group(11))
        if runs and runs[-1][0] == k:
            runs[-1][2] = t
            runs[-1][3] += 1
            runs[-1][4] = max(runs[-1][4], hud)
        else:
            runs.append([k, t, t, 1, hud])
    bad = 0
    for k, a, b, n, hud in runs:
        print(f'{a:9.2f} to {b:9.2f} s  {n:6d} frames  {k}' + (f' (at most {hud} drawn flat)' if hud else ''))
        bad += k == 'EMPTY world' and b - a > 1.0
    totals = {}
    for k, a, b, n, hud in runs:
        totals[k] = totals.get(k, 0) + n
    print('frames: ' + ', '.join(f'{k} {n}' for k, n in totals.items()) + f'; empty worlds over a second: {bad}')
    return 1 if bad and '--empty-ok' not in sys.argv else 0


if __name__ == '__main__':
    sys.exit(main())
