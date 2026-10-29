"""Where a pixel shader's time goes, line by line of its DXIL, from an Nsight Graphics GPU Trace (docs/nsight.md,
"Line by line"): opens the trace in ngfx-ui, shows the first pixel shader in Shader Source and copies the table (the
UI's own copy: tab-separated, a DXIL line a row with its share of the samples and its stall reasons), then sums the
samples by basic block. The SASS of each line is in tools/nsight_sass.py's listing (`line N` comments).

    python tools/nsight_lines.py logs/nsight/N/*.ngfx-gputrace [-o lines.tsv] [--top 30] [--lines FROM-TO]

A trace for this wants many samples: a DXIL trace taken with a PC sample every 64 cycles for a second (tools/gpu_trace.ps1 of rvc_trace12 is DXBC) (0.01% a sample).
Under a minute: the UI is killed at 50 s. Needs pywinauto."""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

NGFX_UI = r'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx-ui.exe'


def copy_source(trace):
    from pywinauto import Desktop, keyboard
    import pywinauto.clipboard as cb
    subprocess.run(['taskkill', '/F', '/IM', 'ngfx-ui.exe'], capture_output=True)
    ui = subprocess.Popen([NGFX_UI, trace])
    t0 = time.time()
    try:
        w = tab = None
        while time.time() - t0 < 30:   # (as tools/nsight_table.py)
            time.sleep(1)
            ws = [x for x in Desktop(backend='uia').windows(process=ui.pid)]
            for x in ws:
                for b in x.descendants(control_type='Button'):
                    if b.element_info.name in ('No', 'OK'):
                        b.invoke()
            w = next((x for x in ws if 'Nsight Graphics' in x.window_text()), None)
            if w:
                try:
                    w.maximize()
                except Exception:
                    pass
                tab = next((c for c in w.descendants(control_type='CheckBox') if c.element_info.name == 'Shader Pipelines'), None)
                if tab:
                    break
        if not tab:
            sys.exit('nsight_lines: the trace did not open in 30 s')
        tree = None
        while tree is None and time.time() - t0 < 35:
            tab.click_input()
            time.sleep(1)
            tree = next((t for t in tab.parent().descendants(control_type='Tree') if t.is_visible() and t.descendants(control_type='TreeItem')), None)
        if tree is None:
            sys.exit('nsight_lines: no Shader Pipelines table')
        cells = tree.descendants(control_type='TreeItem')
        if cells[0].element_info.name != 'Pixel':
            sys.exit(f'nsight_lines: the first shader is a {cells[0].element_info.name} shader')
        cells[0].double_click_input()   # (opens it in Shader Source)
        time.sleep(2)
        next(c for c in w.descendants(control_type='CheckBox') if c.element_info.name == 'Shader Source').click_input()
        time.sleep(3)
        sd = next(d for d in w.descendants() if d.element_info.class_name == 'NV::ShaderProfiler::UI::SourceDetails' and d.is_visible())
        table = next(d for d in sd.descendants(control_type='Table') if d.is_visible())
        head = [h.element_info.name for h in table.descendants(control_type='HeaderItem')]
        r = table.rectangle()
        table.click_input(coords=(r.width() // 2, min(60, r.height() // 2)))
        time.sleep(0.5)
        keyboard.send_keys('^a')
        time.sleep(0.5)
        keyboard.send_keys('^c')
        time.sleep(1.5)
        return head, cb.GetData()
    finally:
        ui.kill()


def share(x):
    x = x.strip()
    return 0.005 if x.startswith('<') else float(x[:-1]) if x.endswith('%') else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('-o', '--out', help='the copied table (tab-separated)')
    ap.add_argument('--top', type=int, default=30, help='blocks listed')
    ap.add_argument('--lines', help='FROM-TO: print these DXIL lines with their shares')
    a = ap.parse_args()
    head, text = copy_source(os.path.abspath(glob.glob(a.trace)[0]))
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(text)
    if any(head):   # (the source table's header is not exposed to UI Automation in 2026.3.1)
        print('columns:', ', '.join(head))
    rows = [l.split('\t') for l in text.splitlines()]
    blocks, cur = [], None
    for r in rows:
        if len(r) < 3 or not r[0].strip().isdigit():
            continue
        t = r[1].strip()
        m = re.match(r'^([\w.$]+):', t)
        if m or cur is None:
            cur = dict(name=m.group(1) if m else 'entry', line=int(r[0]), s=0.0, n=0, ld=0, st=0, tex=0, end='')
            blocks.append(cur)
            if m:
                continue
        if not t:
            continue
        cur['n'] += 1
        cur['s'] += share(r[2])
        cur['ld'] += ' = load ' in t
        cur['st'] += t.startswith('store ')
        cur['tex'] += 'textureLoad' in t
        if t.startswith(('br ', 'switch', 'ret')):
            cur['end'] = t[:80]
    if a.lines:
        lo, hi = (int(x) for x in a.lines.split('-'))
        for r in rows:
            if len(r) > 2 and r[0].strip().isdigit() and lo <= int(r[0]) <= hi:
                print(f'{r[0]:>6} {r[2].strip():>6} | {r[1].strip()[:140]}')
    total = sum(b['s'] for b in blocks)
    print(f'{len(blocks)} blocks, {total:.1f}% of the samples; the busiest (share, running sum, first DXIL line, '
          'instructions, loads, stores, texture loads, how it ends):')
    run = 0.0
    for b in sorted(blocks, key=lambda b: -b['s'])[:a.top]:
        run += b['s']
        print(f"{b['s']:6.2f}% {run:6.1f}% L{b['line']:<6} {b['name'][:22]:22} n={b['n']:<4} ld={b['ld']:<3} st={b['st']:<3} "
              f"tex={b['tex']:<2} {b['end']}")


if __name__ == '__main__':
    main()
