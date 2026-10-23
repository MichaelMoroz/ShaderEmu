"""The card's side of gpu_vm/PROFILE_Q3.md: one run's per-pass logs (profile_card.py) over a window of guest frames.

    python gpu_vm/bench_games/profile_q3.py BASE [BASE...] [--from 300] [--to 1300] [--csv DIR] [--watch NAMES]

BASE is a run's files without their ending (build/profile_q3/clean1/quake3.w2_v0009_LOGSmin). The window is from the pass
whose row brought "q3stat: frame FROM:" to the one that brought "q3stat: frame TO:" (or the last such line); "a frame"
is a mean over its windows of 100 frames, with the spread (min-max of the windows). What is printed:
  work      core 0's and each busy worker's instructions (RISC-V) or ops (gpu_vm) a frame; gpu_vm's bundles where an ends
            log is there (core 0's from --frame-log's stall word always)
  rates     the same a second of the wall
  use       each core's share of its budget and of passes it ran 64 or more (worked), 1-63 (looked), 0
  ends      why core 0's passes ended, its bundles or instructions at the end (p10, median, p90); the workers' ends
  passes    passes a frame, wall ms a pass, and with GVM_TIME_LOG the GPU's ms of each part, fitted against the bundles
  jobs      (q3stat lines) each job's instructions a frame and passes waited, as the game counts them
  handoffs  (a watch log, --watch) the chain of each job's words a frame: asked, made, taken
"""
import argparse, os, re, sys
import numpy as np

WHY_GVM = {0: 'budget', 1: 'sync', 2: 'wait', 3: 'full', 5: 'halt'}
WHY_RV = {0: 'none', 1: 'exit call', 2: 'CSR full', 3: 'full', 6: 'UART', 7: 'fence.i', 8: 'copy', 9: 'wfi/pause', 10: 'fill'}


def lines(base):
    out = []
    for l in open(base + '.lines', encoding='latin-1'):
        r, _, t = l.rstrip('\n').partition('\t')
        out.append((int(r), t))
    return out


def window(base, frm, to):
    """(first row, last row, [(row of each 100-frame mark, frames, ms)])"""
    marks = []
    for r, t in lines(base):
        m = re.match(r'q3stat: frame (\d+): (\d+) frames in (\d+) ms', t)
        if m and frm <= int(m[1]) <= to:
            marks.append((r, int(m[1]), int(m[2]), int(m[3])))
    return marks


def q3stat_jobs(base, frm, to):
    """per 100-frame window after FROM: the game's own counts of each job"""
    txt = [t for _, t in lines(base)]
    rows, cur = [], None
    for t in txt:
        m = re.match(r'q3stat: frame (\d+): (\d+) frames in (\d+) ms, (\d+) instructions a frame; in thousands: server (\d+), client (\d+), scene (\d+), swap (\d+), sound (\d+), rest (\d+)', t)
        if m:
            cur = dict(frame=int(m[1]), frames=int(m[2]), ms=int(m[3]), core0=int(m[4]))
            if frm < cur['frame'] <= to:
                rows.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'q3stat: the scene on a worker core: (\d+) frames of it to these \d+, (\d+) thousand instructions each, ([\d.]+) passes waited', t)
        if m:
            cur.update(scene_n=int(m[1]), scene_k=int(m[2]), scene_wait=float(m[3]))
        m = re.match(r'q3stat: the server on a worker core: (\d+) frames of its own to these \d+, (\d+) thousand instructions each, ([\d.]+) passes waited', t)
        if m:
            cur.update(server_n=int(m[1]), server_k=int(m[2]), server_wait=float(m[3]))
        m = re.match(r"q3stat: the client's game on a worker core: (\d+) frames of its own to these \d+, (\d+) thousand instructions each, ([\d.]+) passes waited", t)
        if m:
            cur.update(client_n=int(m[1]), client_k=int(m[2]), client_wait=float(m[3]))
        m = re.match(r"q3stat: the level's items on the client's helper core: (\d+) frames, \d+ items each, (\d+) thousand instructions each, ([\d.]+) passes waited", t)
        if m:
            cur.update(items_n=int(m[1]), items_k=int(m[2]), items_wait=float(m[3]))
        m = re.match(r"q3stat: the server made (\d+) frames of the game, in thousands each: the bots (\d+), the game (\d+), telling the clients (\d+); (\d+) bots' thinks, (\d+) of them on helpers", t)
        if m:
            cur.update(sv_frames=int(m[1]), sv_bots=int(m[2]), sv_game=int(m[3]), sv_tell=int(m[4]))
    # the per-frame lines come after the window's mark line: each belongs to the window it closes
    return rows


def pct(v, q):
    return float(np.percentile(v, q)) if len(v) else float('nan')


def spread(v):
    v = np.asarray(v, dtype=float)
    return f'{v.mean():,.0f} ({v.min():,.0f}-{v.max():,.0f})' if len(v) else '-'


def spreadf(v, d=2):
    v = np.asarray(v, dtype=float)
    return f'{v.mean():.{d}f} ({v.min():.{d}f}-{v.max():.{d}f})' if len(v) else '-'


def analyse(base, frm, to, csvdir=None, watch=None):
    gvm = 'rv' not in os.path.basename(base).split('_')[0].split('.')[-1]
    name = os.path.basename(base)
    marks = window(base, frm, to)
    if len(marks) < 2:
        print(f'{name}: no window'); return None
    cores = np.fromfile(base + '.cores', dtype=np.uint32).reshape(-1, 260)
    tag = cores[:, 0].astype(np.int64)
    budget = 8192
    m = re.search(r'TICKS(\d+)', name)
    if m:
        budget = int(m[1])
    ops = cores[:, 3:259].astype(np.int64)   # column c: core c (0: core 0's from its clock)
    ops[:, 0] = cores[:, 1]
    stall = cores[:, 2]
    us = cores[:, 3].astype(np.int64)
    out = {'name': name, 'gvm': gvm}
    W = []   # per 100-frame window
    for (r0, f0, _, _), (r1, f1, nf, ms) in zip(marks, marks[1:]):
        sel = (tag >= r0) & (tag < r1)
        if sel.sum() < 2:
            continue
        o = ops[sel]
        wall = (us[sel][-1] - us[sel][0]) / 1e3 * (sel.sum()) / max(1, sel.sum() - 1)
        W.append(dict(frames=f1 - f0, passes=int(sel.sum()), wall_ms=wall, game_ms=ms, core0=o[:, 0].sum(),
                      workers=o[:, 1:].sum(), per=o.sum(0), c0b=((stall[sel] >> 8).sum() if gvm else 0)))
    if not W:
        print(f'{name}: no passes in the window'); return None
    fr = np.array([w['frames'] for w in W], dtype=float)
    out['fps'] = [w['frames'] * 1000.0 / w['game_ms'] for w in W]
    out['passes_frame'] = [w['passes'] / w['frames'] for w in W]
    out['wall_pass_ms'] = [w['wall_ms'] / w['passes'] for w in W]
    out['core0_frame'] = [w['core0'] / w['frames'] for w in W]
    out['workers_frame'] = [w['workers'] / w['frames'] for w in W]
    out['core0_bundles_frame'] = [w['c0b'] / w['frames'] for w in W] if gvm else []
    per = np.array([w['per'] for w in W], dtype=float) / fr[:, None]   # windows x cores, a frame
    busy = [c for c in np.argsort(-per.mean(0)) if per.mean(0)[c] > 100][:12]
    out['per_core_frame'] = {int(c): per[:, c] for c in busy}
    wall_s = np.array([w['wall_ms'] for w in W]) / 1e3
    out['core0_s'] = [w['core0'] / s for w, s in zip(W, wall_s)]
    out['workers_s'] = [w['workers'] / s for w, s in zip(W, wall_s)]
    # the passes of the whole window
    sel = (tag >= marks[0][0]) & (tag < marks[-1][0])
    o, st = ops[sel], stall[sel]
    out['passes'] = int(sel.sum())
    use = {}
    for c in [0] + [int(x) for x in busy if x]:
        v = o[:, c]
        use[c] = dict(budget=float(np.minimum(v, budget).mean() / budget), worked=float((v >= 64).mean()), looked=float(((v > 0) & (v < 64)).mean()),
                      none=float((v == 0).mean()), full_budget=float((v >= budget).mean()))
    out['use'] = use
    # core 0's ends
    if gvm:
        why, n = (st & 0xff).astype(int), (st >> 8).astype(np.int64)
        names = WHY_GVM
    else:
        n = o[:, 0]
        why = np.where(n >= budget, -1, st.astype(int))
        names = {**WHY_RV, -1: 'budget'}
    ends = {}
    for k in np.unique(why):
        v = n[why == k]
        ends[names.get(int(k), str(int(k)))] = (float((why == k).mean()), pct(v, 10), pct(v, 50), pct(v, 90))
    out['c0_ends'] = ends
    out['c0_n'] = (pct(n, 10), pct(n, 50), pct(n, 90), float(n.mean()))
    # the workers' ends: gpu_vm's ends log in the mailbox log (each core's bundles and why), else by instructions
    if gvm and os.path.exists(base + '.mbox'):
        mb = np.fromfile(base + '.mbox', dtype=np.uint32)
        nrec = 514 + 1536
        mb = mb[:len(mb) // nrec * nrec].reshape(-1, nrec)
        mt = mb[:, 0].astype(np.int64)
        ms = (mt >= marks[0][0]) & (mt < marks[-1][0]) & (mb[:, 2:258].sum(1) > 0)
        e = mb[ms, 2:258]
        b, y = (e >> 8).astype(np.int64), (e & 0xff).astype(int)
        wk = {}
        act = b[:, 1:] >= 64
        for k in np.unique(y[:, 1:][act]):
            v = b[:, 1:][act & (y[:, 1:] == k)]
            wk[WHY_GVM.get(int(k), str(int(k)))] = (float(len(v) / act.sum()), pct(v, 10), pct(v, 50), pct(v, 90))
        out['w_ends'] = wk
        out['w_bundles_pass'] = (pct(b[:, 1:][act], 10), pct(b[:, 1:][act], 50), pct(b[:, 1:][act], 90))
        out['most_bundles'] = (pct(b[:, 1:].max(1), 10), pct(b[:, 1:].max(1), 50), pct(b[:, 1:].max(1), 90))
        out['ends_passes'] = int(ms.sum())
        out['pcs'] = (mb[ms, 258:514], b, y)
    else:
        w = o[:, 1:]
        act = w >= 64
        out['w_ends'] = {'budget': (float((w[act] >= budget).mean()), budget, budget, budget),
                         'early (pause, wait, full)': (float((w[act] < budget).mean()), pct(w[act][w[act] < budget], 10), pct(w[act][w[act] < budget], 50), pct(w[act][w[act] < budget], 90))}
        out['most_bundles'] = (pct(w.max(1), 10), pct(w.max(1), 50), pct(w.max(1), 90))
    out['busiest_full'] = float((o[:, 1:].max(1) >= budget).mean())
    # GPU times (gpu_vm, GVM_TIME_LOG): a record a frame in 8 or so
    if gvm and os.path.exists(base + '.time'):
        t = np.fromfile(base + '.time', dtype=np.uint32).reshape(-1, 16).astype(np.int64)
        ts = (t[:, 0] >= marks[0][0]) & (t[:, 0] < marks[-1][0])
        t = t[ts]
        if len(t):
            # (ns: 7 the tick, 8 the commit, 9 the operations' rounds, 10 the devices' list, 11 the list to the frame's end,
            # 15 the list's end to the control zones' end; 12 the whole frame)
            out['gpu'] = dict(n=len(t), tick=t[:, 7].mean() / 1e3, commit=t[:, 8].mean() / 1e3, rounds=t[:, 9].mean() / 1e3,
                              devices=t[:, 10].mean() / 1e3, zones=t[:, 15].mean() / 1e3, total=t[:, 12].mean() / 1e3,
                              rest=t[:, 11].mean() / 1e3, total_p=(pct(t[:, 12] / 1e3, 10), pct(t[:, 12] / 1e3, 50), pct(t[:, 12] / 1e3, 90)))
            # the pass's GPU time against core 0's bundles and the busiest worker's
            X = np.column_stack([np.ones(len(t)), t[:, 1], t[:, 3]]).astype(float)
            y = t[:, 12].astype(float) / 1e3
            coef, *_ = np.linalg.lstsq(X, y, rcond=None)
            r2 = 1 - ((y - X @ coef) ** 2).sum() / ((y - y.mean()) ** 2).sum()
            out['gpu_fit'] = (coef, r2)
            # ns a bundle by how far into a pass: the tick's time against the most bundles any core ran, in bins
            mx = np.maximum(t[:, 1], t[:, 3])
            tick = t[:, 7].astype(float) / 1e3
            bins = [0, 250, 500, 1000, 2000, 4000, 8192, 1 << 30]
            out['gpu_bins'] = [(lo, hi, int(((mx >= lo) & (mx < hi)).sum()), float(tick[(mx >= lo) & (mx < hi)].mean()) if ((mx >= lo) & (mx < hi)).any() else float('nan'),
                                float(mx[(mx >= lo) & (mx < hi)].mean()) if ((mx >= lo) & (mx < hi)).any() else float('nan')) for lo, hi in zip(bins, bins[1:])]
            Xt = np.column_stack([np.ones(len(t)), mx]).astype(float)
            ct, *_ = np.linalg.lstsq(Xt, tick, rcond=None)
            out['tick_fit'] = ct
            small = mx < 1000
            if small.sum() > 20 and (~small).sum() > 20:
                cs, *_ = np.linalg.lstsq(Xt[small], tick[small], rcond=None)
                cl, *_ = np.linalg.lstsq(Xt[~small], tick[~small], rcond=None)
                out['tick_fit_split'] = (cs, cl)
    # the wall a pass against the work in it (both machines; the host's clock, three passes in flight)
    wall = np.diff(us[sel]).astype(float) / 1e3
    if len(wall) > 100:
        X = np.column_stack([np.ones(len(wall)), o[1:, 0], o[1:, 1:].max(1)]).astype(float)
        coef, *_ = np.linalg.lstsq(X, wall, rcond=None)
        out['wall_fit'] = coef
    out['jobs'] = q3stat_jobs(base, frm, to)
    if csvdir:
        os.makedirs(csvdir, exist_ok=True)
        with open(os.path.join(csvdir, name + '.windows.csv'), 'w') as f:
            f.write('window_end_frame,frames,passes,wall_ms,game_ms,core0_work,workers_work,core0_bundles\n')
            for (r1, f1, _, _), w in zip(marks[1:], W):
                f.write(f"{f1},{w['frames']},{w['passes']},{w['wall_ms']:.1f},{w['game_ms']},{w['core0']},{w['workers']},{w['c0b']}\n")
        with open(os.path.join(csvdir, name + '.passes.csv'), 'w') as f:
            f.write('tag,us,core0_work,core0_stall,' + ','.join(f'core{c}' for c in range(1, 33)) + '\n')
            for i in np.nonzero(sel)[0]:
                f.write(f"{tag[i]},{us[i]},{ops[i, 0]},{stall[i]}," + ','.join(str(x) for x in ops[i, 1:33]) + '\n')
    return out


def report(o):
    if not o:
        return
    unit = 'ops' if o['gvm'] else 'instr.'
    print(f"\n## {o['name']}  ({o['passes']} passes in the window)")
    print(f"- guest fps {spreadf(o['fps'], 1)}; passes a frame {spreadf(o['passes_frame'])}; wall a pass {spreadf(o['wall_pass_ms'], 3)} ms")
    print(f"- core 0 a frame {spread(o['core0_frame'])} {unit}" + (f", {spread(o['core0_bundles_frame'])} bundles" if o['gvm'] else '') +
          f"; workers {spread(o['workers_frame'])} {unit}")
    print(f"- a second: core 0 {np.mean(o['core0_s']) / 1e6:.2f} M {unit}, workers {np.mean(o['workers_s']) / 1e6:.2f} M")
    print('- busy cores, a frame: ' + ', '.join(f'{c}: {v.mean():,.0f}' for c, v in o['per_core_frame'].items()))
    print('- use (share of budget run / passes worked >= 64 / looked 1-63 / none / at the whole budget):')
    for c, u in o['use'].items():
        print(f"    core {c}: {u['budget'] * 100:.1f}% / {u['worked'] * 100:.1f}% / {u['looked'] * 100:.1f}% / {u['none'] * 100:.1f}% / {u['full_budget'] * 100:.1f}%")
    print(f"- core 0's {unit if not o['gvm'] else 'bundles'} at a pass's end p10/50/90/mean: " + ' / '.join(f'{x:,.0f}' for x in o['c0_n']))
    print('- core 0 ends: ' + ', '.join(f'{k} {v[0] * 100:.1f}% (p10/50/90 {v[1]:,.0f}/{v[2]:,.0f}/{v[3]:,.0f})' for k, v in sorted(o['c0_ends'].items(), key=lambda kv: -kv[1][0])))
    print('- workers that ran (>= 64) ended: ' + ', '.join(f'{k} {v[0] * 100:.1f}% (p10/50/90 {v[1]:,.0f}/{v[2]:,.0f}/{v[3]:,.0f})' for k, v in sorted(o['w_ends'].items(), key=lambda kv: -kv[1][0])))
    print(f"- the busiest worker a pass p10/50/90: " + ' / '.join(f'{x:,.0f}' for x in o['most_bundles']) + f"; at its whole budget in {o['busiest_full'] * 100:.1f}% of passes")
    if 'gpu' in o:
        g = o['gpu']
        print(f"- GPU a pass ({g['n']} timed), us: tick {g['tick']:.1f}, commit {g['commit']:.1f}, rounds {g['rounds']:.1f}, list {g['devices']:.1f}, list to the end {g['rest']:.1f} (of it the zones {g['zones']:.1f}); total {g['total']:.1f} (p10/50/90 {g['total_p'][0]:.0f}/{g['total_p'][1]:.0f}/{g['total_p'][2]:.0f})")
        c, r2 = o['gpu_fit']
        print(f"- GPU total = {c[0]:.1f} us + {c[1] * 1e3:.1f} ns x core 0's bundles + {c[2] * 1e3:.1f} ns x the busiest worker's (R^2 {r2:.2f})")
        ct = o['tick_fit']
        print(f"- tick = {ct[0]:.1f} us + {ct[1] * 1e3:.1f} ns x the most bundles of any core" +
              (f"; under 1,000: {o['tick_fit_split'][0][0]:.1f} us + {o['tick_fit_split'][0][1] * 1e3:.1f} ns, from 1,000: {o['tick_fit_split'][1][0]:.1f} us + {o['tick_fit_split'][1][1] * 1e3:.1f} ns" if 'tick_fit_split' in o else ''))
        print('- tick by the most bundles: ' + '; '.join(f'{lo}-{hi if hi < 1 << 29 else "max"}: {n} passes, {t:.0f} us at {m:,.0f} ({t * 1e3 / m if m else 0:.0f} ns a bundle)' for lo, hi, n, t, m in o['gpu_bins'] if n))
    if 'wall_fit' in o:
        c = o['wall_fit']
        print(f"- wall a pass = {c[0] * 1e3:.1f} us + {c[1] * 1e6:.1f} ns x core 0's {unit} + {c[2] * 1e6:.1f} ns x the busiest worker's")
    if o['jobs']:
        J = o['jobs']
        def col(k):
            return [j[k] for j in J if k in j]
        print(f"- the game's counts a window of 100 frames (mean over {len(J)}): core 0 {np.mean(col('core0')):,.0f} a frame; "
              f"scene {np.mean(col('scene_n')):.0f} x {np.mean(col('scene_k')):.0f}k, waited {np.mean(col('scene_wait')):.1f} passes; "
              f"client's game {np.mean(col('client_n')):.0f} x {np.mean(col('client_k')):.0f}k, waited {np.mean(col('client_wait')):.1f}; "
              f"items {np.mean(col('items_n')):.0f} x {np.mean(col('items_k')):.0f}k, waited {np.mean(col('items_wait')):.1f}; "
              f"server {np.mean(col('server_n')):.0f} x {np.mean(col('server_k')):.0f}k, waited {np.mean(col('server_wait')):.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base', nargs='+')
    ap.add_argument('--from', dest='frm', type=int, default=300)
    ap.add_argument('--to', type=int, default=1300)
    ap.add_argument('--csv')
    a = ap.parse_args()
    for b in a.base:
        report(analyse(b, a.frm, a.to, a.csv))


if __name__ == '__main__':
    main()
