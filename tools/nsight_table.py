"""The Shader Pipelines table of an Nsight Graphics GPU Trace, as CSV, without a person (the nsight-gpu-trace skill): opens the
.ngfx-gputrace in ngfx-ui, shows the table, selects it all and copies it (the UI's own copy writes CSV to the
clipboard), and closes the UI. Each shader's registers a thread (# Reg), warps an SM (# Warp), live registers, samples
and stall reasons.

    python tools/nsight_table.py logs/nsight/s1/*.ngfx-gputrace [-o table.csv]

Prints the pixel and vertex shaders' main columns. Under a minute: the UI is killed at 50 s. Needs pywinauto."""
import argparse
import csv
import glob
import io
import os
import subprocess
import sys
import time

NGFX_UI = r'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx-ui.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    trace = os.path.abspath(glob.glob(a.trace)[0])
    from pywinauto import Desktop, keyboard
    import pywinauto.clipboard as cb
    subprocess.run(['taskkill', '/F', '/IM', 'ngfx-ui.exe'], capture_output=True)
    ui = subprocess.Popen([NGFX_UI, trace])
    t0 = time.time()
    try:
        w = tab = None
        while time.time() - t0 < 40:   # the window, any first-run question answered No, the table's tab
            time.sleep(1)
            ws = [x for x in Desktop(backend='uia').windows(process=ui.pid)]
            for x in ws:
                for b in x.descendants(control_type='Button'):
                    if b.element_info.name in ('No', 'OK'):
                        b.invoke()
            w = next((x for x in ws if 'Nsight Graphics' in x.window_text()), None)
            if w:
                try:   # (the UI opens as it was closed, minimized too: nothing in it is then visible)
                    w.maximize()
                except Exception:
                    pass
                tab = next((c for c in w.descendants(control_type='CheckBox') if c.element_info.name == 'Shader Pipelines'), None)
                if tab:
                    break
        if not tab:
            sys.exit('nsight_table: the trace did not open in 40 s')
        tree = None
        while tree is None and time.time() - t0 < 45:
            tab.click_input()
            time.sleep(1)
            tree = next((t for t in tab.parent().descendants(control_type='Tree') if t.is_visible() and t.descendants(control_type='TreeItem')), None)
        if tree is None:
            sys.exit('nsight_table: no Shader Pipelines table')
        tree.descendants(control_type='TreeItem')[0].click_input()
        keyboard.send_keys('^a^c')
        time.sleep(0.5)
        text = cb.GetData()
    finally:
        ui.kill()
    rows = list(csv.reader(io.StringIO(text)))
    hdr = [h.replace('""', ' ') for h in rows[0]]
    # (the UI's copy names one column fewer than it writes: after Dependency-Attributed Samples comes an unnamed one)
    k = hdr.index('Dependency-Attributed Samples') + 1
    hdr = hdr[:k] + ['(unnamed)'] + hdr[k:-1]
    rows = [dict(zip(hdr, r)) for r in rows[1:]]
    if a.out:
        with open(a.out, 'w', newline='') as f:
            cw = csv.writer(f)
            cw.writerow(hdr)
            for r in rows:
                cw.writerow([r.get(h, '') for h in hdr])
    cols = ['Type', 'Hash', 'Samples', '# Warp', '# Reg', 'Live Registers', 'Top Stall #1 (Type)', 'Top Stall #1 (% Samples)',
            'Top Stall #2 (Type)', 'Top Stall #2 (% Samples)', 'Top Stall #3 (Type)']   # (the copy leaves #3's share out: the columns after it are not as named)
    print('\t'.join(cols))
    for r in rows:
        if r.get('Type') in ('Pixel', 'Vertex'):
            print('\t'.join(r.get(c, '') for c in cols))


if __name__ == '__main__':
    main()
