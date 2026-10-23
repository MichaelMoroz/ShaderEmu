"""Quake III's handoffs between core 0 and its jobs, pass by pass, for gpu_vm/PROFILE_Q3.md ("Handoffs").

    python gpu_vm/bench_games/profile_handoffs.py BASE [--from F --to T] [--csv FILE]

BASE.watch is a GVM_WATCH_LOG or RVC_WATCH_LOG of the words profile_card.py's batch watched (the order of
build/profile_q3/batch4.sh): the client's asked and made frames, the items' task and done, the scene's recorded and
drawn. A job's frame is posted in the pass its asked word first shows it and finished in the pass its done word does;
the frame (core 0's period) runs from one client post to the next. BASE.cores gives core 0's stall word for the passes
in which core 0 waited with every job finished or with one outstanding. The window is the q3stat frames F to T, as in
profile_q3.py.
"""
import argparse, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_q3 as P

# (texel index, word) of each counter in the watch record, by machine
SPEC = {
    'gvm': dict(client_asked=(0, 0), client_made=(1, 0), items_task=(2, 0), items_done=(3, 0), scene_asked=(4, 0),
                scene_made=(5, 0)),
}


def series(w, n, t, k):
    return w[:, 2 + 4 * t + k].astype(np.int64)


def first_pass(rows, v, n):
    """the pass (row index) at which counter v first reaches n, or -1"""
    i = np.searchsorted(np.maximum.accumulate(v), n)
    return int(rows[i]) if i < len(v) else -1


def analyse(base, frm, to, spec, csv=None):
    name = os.path.basename(base)
    marks = P.window(base, frm, to)
    r0, r1 = marks[0][0], marks[-1][0]
    nwords = 2 + 4 * (max(t for t, k in spec.values()) + 1)
    raw = np.fromfile(base + '.watch', dtype=np.uint32)
    for n in range(nwords, nwords + 4 * 32, 4):   # the record's width: the watched count is not in the file
        if len(raw) % n == 0 and (raw.reshape(-1, n)[:, 0][1:] >= raw.reshape(-1, n)[:, 0][:-1]).all():
            w = raw.reshape(-1, n)
            break
    rows = w[:, 0].astype(np.int64)
    sel = (rows >= r0) & (rows < r1)
    w, rows = w[sel], rows[sel]
    S = {k: series(w, 0, t, j) for k, (t, j) in spec.items()}
    ca = S['client_asked']
    posts = np.flatnonzero(np.diff(ca) > 0) + 1   # the passes a new client frame shows
    out = []
    for a, b in zip(posts, posts[1:]):
        N = ca[a]
        p0, p1 = rows[a], rows[b]
        d = dict(frame=int(N), passes=int(p1 - p0))
        made = first_pass(rows, S['client_made'], N)
        d['client'] = made - p0 if made >= 0 else -1
        # the scene and the items: the first frame posted at or after this one's post, finished when
        sa, it = S['scene_asked'][a:b], S['items_task'][a:b]
        for job, ask, done in (('scene', S['scene_asked'], S['scene_made']), ('items', S['items_task'], S['items_done'])):
            seg = ask[a:b]
            ch = np.flatnonzero(np.diff(np.concatenate([[ask[a - 1] if a else seg[0]], seg])) > 0)
            if len(ch):
                q = a + ch[0]
                f = first_pass(rows, done, ask[q])
                d[job + '_post'] = int(rows[q] - p0)
                d[job] = int(f - rows[q]) if f >= 0 else -1
            else:
                d[job + '_post'] = d[job] = -1
        out.append(d)
    def col(k):
        return np.array([d[k] for d in out if d[k] >= 0], dtype=float)
    per = col('passes')
    print(f'{name}: {len(out)} frames, {per.mean():.1f} passes a frame (median {np.median(per):.0f})')
    ends = {}
    for k in ('client', 'scene', 'items'):
        v = col(k)
        print(f'  {k:7s}: posted to finished {v.mean():5.1f} passes (median {np.median(v):.0f}, 90% {np.percentile(v, 90):.0f})'
              + (f', posted {col(k + "_post").mean():.1f} passes into the frame' if k != 'client' else ''))
    # the critical one: which job finishes last in the frame, and core 0's own part after it
    last = []
    for d in out:
        fin = {'client': d['client']}
        for k in ('scene', 'items'):
            if d[k] >= 0:
                fin[k] = d[k + '_post'] + d[k]
        j = max(fin, key=fin.get)
        last.append((j, fin[j], d['passes']))
    for j in ('client', 'scene', 'items'):   # (finished counts from the frame's client post: past the frame's passes is pipelined)
        L = [x for x in last if x[0] == j]
        if L:
            print(f'  last to finish {j}: {len(L) / len(last) * 100:.0f}% of frames, finished {np.mean([x[1] for x in L]):.1f} '
                  f'of the frame\'s {np.mean([x[2] for x in L]):.1f} passes')
    for k in ('client', 'scene', 'items'):
        print(f'  {k}: frames in flight (posted to finished over the frame) {col(k).mean() / per.mean():.2f}')
    if csv:
        with open(csv, 'w') as f:
            ks = list(out[0])
            f.write(','.join(ks) + '\n')
            for d in out:
                f.write(','.join(str(d[k]) for k in ks) + '\n')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('--from', dest='frm', type=int, default=300)
    ap.add_argument('--to', type=int, default=1300)
    ap.add_argument('--csv')
    ap.add_argument('--spec', default='gvm', help='gvm, or rv:NAME=T.K,... for another order')
    a = ap.parse_args()
    if a.spec == 'gvm':
        spec = SPEC['gvm']
    else:
        spec = {kv.split('=')[0]: tuple(int(x) for x in kv.split('=')[1].split('.')) for kv in a.spec.split(':', 1)[1].split(',')}
    analyse(a.base, a.frm, a.to, spec, a.csv)


if __name__ == '__main__':
    main()
