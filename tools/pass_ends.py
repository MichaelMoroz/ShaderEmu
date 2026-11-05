"""Where core 0's passes end: a histogram of the instructions a pass ran, by what ended it (an SVG).

    python tools/pass_ends.py OUT.svg TICKS LOAD=FRAMES [LOAD=FRAMES ...]

Run a load with a long budget to see where it would stop (docs/machine/tick-costs.md, "Sixth round"):
    bin/rvc_harness.exe --d3d11 --rvc shader_vm --define NO_PAGING --load-state SNAP --ticks 32768 --seconds 30 --frame-log F

A frame log (rvc_harness --frame-log) has a record a pass: pc, instructions, stall, time. A pass that ran
TICKS ended on the budget; the others ended where the guest stopped: the write cache full, a wait (a
program's pause or a wait in the GPU driver: the cores waiting for each other or for the GPU), or the rest.
"""
import sys, struct

out, ticks = sys.argv[1], int(sys.argv[2])
loads = [a.split('=', 1) for a in sys.argv[3:]]
BIN = 1024
NB = ticks // BIN
KINDS = [('ran to the budget', '#2a78d6'), ('write cache full', '#eb6834'),
         ('waiting (pause or wfi)', '#1baf7a'), ('copy, fill, fence or other', '#eda100')]
INK, INK2, GRID, SURF = '#24231f', '#55544d', '#e8e7e2', '#fcfcfb'


def kind(n, stall):
    if n >= ticks: return 0
    if stall == 3: return 1
    if stall == 9: return 2
    return 3


W, PH, L, R, T = 960, 190, 60, 20, 70
H = T + len(loads) * (PH + 50) + 40
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Segoe UI, Arial, sans-serif">',
       f'<rect width="{W}" height="{H}" fill="{SURF}"/>',
       f'<text x="{L}" y="24" font-size="15" fill="{INK}" font-weight="600">Where core 0\'s passes end, by what ended them (passes of up to {ticks:,} instructions, 30 s each)</text>']
lx = L
for label, color in KINDS:
    svg.append(f'<rect x="{lx}" y="38" width="12" height="12" rx="2" fill="{color}"/><text x="{lx + 17}" y="49" font-size="12" fill="{INK2}">{label}</text>')
    lx += 30 + 7 * len(label)
table = []
pw = W - L - R
for p, (name, path) in enumerate(loads):
    raw = open(path, 'rb').read()
    recs = [struct.unpack_from('<4I', raw, i) for i in range(0, len(raw) - 15, 16)]
    hist = [[0] * NB for _ in KINDS]
    for pc, n, stall, us in recs:
        hist[kind(n, stall)][min(n // BIN, NB - 1)] += 1
    total = len(recs)
    pct = [[c / total * 100 for c in row] for row in hist]
    top = max(sum(pct[k][b] for k in range(len(KINDS))) for b in range(NB))
    ymax = max(10, -(-top // 10) * 10)
    y0 = T + p * (PH + 50) + 20
    svg.append(f'<text x="{L}" y="{y0 - 6}" font-size="13" fill="{INK}" font-weight="600">{name}: {total:,} passes</text>')
    for g in range(0, int(ymax) + 1, int(ymax) // 5 or 1):
        y = y0 + PH - g / ymax * PH
        svg.append(f'<line x1="{L}" x2="{L + pw}" y1="{y:.1f}" y2="{y:.1f}" stroke="{GRID}" stroke-width="1"/>')
        svg.append(f'<text x="{L - 6}" y="{y + 4:.1f}" font-size="10" fill="{INK2}" text-anchor="end">{g}%</text>')
    bw = pw / NB
    for b in range(NB):
        acc = 0
        for k, (label, color) in enumerate(KINDS):
            v = pct[k][b]
            if v <= 0: continue
            h = v / ymax * PH
            y = y0 + PH - acc - h
            svg.append(f'<rect x="{L + b * bw + 1:.1f}" y="{y:.1f}" width="{bw - 2:.1f}" height="{max(h - 1, 0.6):.1f}" fill="{color}">'
                       f'<title>{name}, {b * BIN:,} to {(b + 1) * BIN:,} instructions: {label} {v:.2f}% of passes</title></rect>')
            acc += h
    for x in (8192, 12288, 16384):
        if x < ticks:
            xx = L + x / ticks * pw
            svg.append(f'<line x1="{xx:.1f}" x2="{xx:.1f}" y1="{y0}" y2="{y0 + PH}" stroke="#6b6a63" stroke-dasharray="3 3"/>')
            svg.append(f'<text x="{xx + 3:.1f}" y="{y0 + 10}" font-size="10" fill="#6b6a63">{x // 1024}k</text>')
    for x in range(0, ticks + 1, 4096):
        xx = L + x / ticks * pw
        svg.append(f'<text x="{xx:.1f}" y="{y0 + PH + 14}" font-size="10" fill="{INK2}" text-anchor="middle">{x // 1024}k</text>')
    ns = [n for pc, n, s, us in recs]
    ks = [kind(n, s) for pc, n, s, us in recs]
    row = [name] + [f'{sum(1 for n in ns if n < x) / total * 100:.1f}%' for x in (8192, 12288, 16384, 24576)]
    row += [f'{ks.count(1) / total * 100:.1f}%', f'{ks.count(2) / total * 100:.1f}%', f'{ks.count(0) / total * 100:.1f}%']
    table.append(row)
svg.append(f'<text x="{L + pw / 2}" y="{H - 12}" font-size="11" fill="{INK2}" text-anchor="middle">instructions core 0 ran before its pass ended (bins of {BIN:,})</text>')
svg.append('</svg>')
open(out, 'w').write('\n'.join(svg))
print('load | ended before 8k | before 12k | before 16k | before 24k | write cache full | waiting | ran to the budget')
for r in table: print(' | '.join(r))
