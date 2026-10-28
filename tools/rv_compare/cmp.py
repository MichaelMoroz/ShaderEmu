"""The RISC-V machine's comparison on the card, machine against machine, every part run as a world runs it: GPU-VM's
compare/ (docs/COMPARE.md there) with RISC-V alone. `--unity 11.1,1.4,0.75`: the guest's clock 11.1 ms a host frame
(90 Hz), as many rounds a frame as fit 9.7 ms of the card, each round costing at least 0.75 ms (R, Unity's CPU a
round, as COMPARE measured RISC-V's world), 8,192 instructions a pass.

Each part starts from the machine's shell snapshot (`nano-X -p` up), made on the processor (`cmp.py shell MACHINE`).
The command is typed after `echo GO`; the console's lines and the count of GPU lists drawn are logged by host frame
(RVC_LINE_LOG, RVC_FRAMES_LOG), so every time is frames x 11.1 ms of the world's clock. A run longer than SECTION
frames goes on in sections, each saved and loaded, every call under a minute (timeout 55 s).

    python tools/rv_compare/cmp.py shell MACHINE          # the shell snapshot, on the processor (once a machine)
    python tools/rv_compare/cmp.py MACHINE PART [ROUND]   # one section; the part's row once its run is over
    python tools/rv_compare/cmp.py again MACHINE PART [ROUND]   # the row again from the logs (metrics changed)
    python tools/rv_compare/run.py ...                    # the whole set, section by section (run.py's text)

Machines are machines.py's (before: the frozen 7624f55; after: the backport's checkout)."""
import json, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machines import MACHINES, BASE

F_MS = 11.1
ROUND_MS = 0.75   # Unity's CPU a round for RISC-V's world (GPU-VM compare/cmp.py, the machine-alone project, 24 October 2026)
SECTION = 3000    # host frames a call: 33 s of the guest's clock, about 31 s of the wall
TIMEOUT = 50      # the harness's own limit on the wall (--seconds: from its first frame; its start, the shader and the snapshot, is 3 to 5 s)
HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
DATA = os.environ.get('RVCMP_DATA', os.path.join(BASE, 'build', 'rv_compare'))   # the logs and section snapshots: large, not tracked
LOGS = os.path.join(DATA, 'logs')
QUEUE = ['pwsh', '-NoProfile', '-File', os.path.join(SE, 'tools', 'gpu_queue.ps1')]
RESULTS = os.path.join(HERE, 'results.jsonl')
REFUSED = 75   # the harnesses' lock (run_lock): nothing ran


def harness(machine):
    M = MACHINES[machine]
    return [os.path.join(M['root'], M['exe']), '--d3d11', '--rvc', 'experiments/rvc_opt', '--define', 'NO_PAGING',
            '--image', M['image'], '--no-desktop', '--no-viz', '--no-stdin', '--seconds', str(TIMEOUT)] + list(M.get('args', ()))


def queued(cmd, cwd, env=None, timeout=900):
    """One harness run through tools/gpu_queue.ps1 (the harness stops itself at TIMEOUT s; `timeout` covers the wait for the card too);
    a run the lock refused (another session's CPU harness) is tried again after a pause, up to 10 minutes"""
    t0 = time.time()
    while True:
        t = time.time()
        try:
            p = subprocess.run(QUEUE + cmd, cwd=cwd, env=env, capture_output=True, text=True, errors='replace', timeout=timeout)
            out, code = p.stderr + p.stdout, p.returncode
        except subprocess.TimeoutExpired as e:
            out, code = (e.stderr or b'').decode('latin-1') + (e.stdout or b'').decode('latin-1') + '\n[rv_compare] over the time', -1
        if code == REFUSED and time.time() - t0 < 600:
            print('  refused by the harness lock (a CPU harness runs): again in 20 s', flush=True)
            time.sleep(20)
            continue
        return out, code, time.time() - t


def make_shell(machine):
    """The shell snapshot on the processor: a cold boot, `nano-X -p` up 10 s, saved at the line after it
    (rvc_harness --cpu --save-state writes every core where the shader machine keeps it)"""
    M = MACHINES[machine]
    os.makedirs(os.path.dirname(M['shell']), exist_ok=True)
    ulog = M['shell'] + '.uart'
    cmd = harness(machine) + ['--cpu', '--frames', '1000000', '--uart-log', ulog,
                              '--input', 'nano-X -p & sleep 10; echo SH""READY\\n', '--until', 'SHREADY', '--save-state', M['shell']]
    out, code, wall = queued(cmd, M['root'])
    ok = '--until matched' in out and os.path.exists(M['shell'])
    print(f'{machine}: shell snapshot {"made" if ok else "NOT made"} in {wall:.1f} s: {M["shell"]}')
    if not ok:
        print(out[-1500:])
    return ok


def section(machine, tag, k, frames, typed, until, sends, load, extra=(), penv=None):
    M = MACHINES[machine]
    lines, flog, ulog = (os.path.join(LOGS, f'{tag}.{k}{s}') for s in ('.lines', '.frames', '.uart'))
    save = os.path.join(LOGS, f'{tag}.{k}.snap')
    for f in (lines, flog, ulog, save):
        if os.path.exists(f):
            os.remove(f)
    env = dict(os.environ, RVC_LINE_LOG=lines, RVC_FRAMES_LOG=flog, GPU_QUEUE_WHO='rv_compare')
    for key, v in (penv or {}).items():   # (a part's own: {stem} in a value is the section's log stem)
        env[key] = v.format(stem=os.path.join(LOGS, f'{tag}.{k}'))
    a = ['--unity', f'{F_MS},1.4,{ROUND_MS}', '--stats-after', '0', '--frames', str(frames), '--uart-log', ulog, '--save-state', save,
         '--load-state', load]
    if typed:
        a += ['--input', typed]
    for e, s in sends:
        a += ['--expect', e, '--send', s]
    if until:
        a += ['--until', until]
    a += list(extra)
    err, code, wall = queued(harness(machine) + a, M['root'], env)
    m = re.search(r'([\d.]+) ms on the card a frame', err)
    n = re.search(r'(\d+) frames in', err)
    return dict(wall=round(wall, 1), frames=int(n[1]) if n else 0, card_ms=float(m[1]) if m else None, code=code,
                until='--until matched' in err, save=save if os.path.exists(save) else None, tail=err[-1500:],
                stats=re.findall(r'(?:STATS|\[harness\]) [^\n]*(?:workers|unity|IPS)[^\n]*', err))


def tag_of(machine, part, rnd):
    return f'{machine}_{part}_r{rnd}'


def run(machine, part, rnd=0, new=False):
    """One section (a call under a minute): the next one of the part's run, or its first. Returns the run once it is
    over, else None."""
    P = PARTS[part]
    os.makedirs(LOGS, exist_ok=True)
    tag = tag_of(machine, part, rnd)
    meta = os.path.join(LOGS, tag + '.meta')
    secs = [] if new or not os.path.exists(meta) else json.load(open(meta))
    if secs and secs[-1].get('done'):
        secs = []   # (a finished run is run again from its start)
    k = len(secs)
    load = secs[-1]['save'] if secs else MACHINES[machine]['shell']
    if not os.path.exists(load):
        raise SystemExit(f'{load}: no snapshot (cmp.py shell {machine} makes the shell one)')
    left = P['frames'] - sum(s['frames'] for s in secs)
    typed = None if secs else 'echo GO; ' + P['cmd'] + '\\n'
    s = section(machine, tag, k, min(left, P.get('section', SECTION)), typed, P.get('until'), P.get('sends', ()) if k == 0 else (),
                load, P.get('args', ()), P.get('env'))
    if secs and secs[-1]['save'] and os.path.exists(secs[-1]['save']):
        os.remove(secs[-1]['save'])   # (300 MB each: the last is kept)
    s['done'] = s['until'] or not s['save'] or s['frames'] == 0 or s['frames'] >= left
    secs.append(s)
    json.dump(secs, open(meta, 'w'))
    print(f'  {tag} section {k}: {s["frames"]} frames, {s["wall"]} s, card {s["card_ms"]} ms a frame'
          f'{", until matched" if s["until"] else ""}{"" if s["done"] else ", more to come"}', flush=True)
    if not s['done']:
        return None
    if s['save'] and os.path.exists(s['save']):
        os.remove(s['save'])
    return load_logs(machine, part, rnd)


def load_logs(machine, part, rnd=0):
    tag = tag_of(machine, part, rnd)
    secs = json.load(open(os.path.join(LOGS, tag + '.meta')))
    L, Fr, base = [], [], 0
    for k, s in enumerate(secs):
        p = os.path.join(LOGS, f'{tag}.{k}.lines')
        if os.path.exists(p):
            for l in open(p, encoding='latin-1'):
                f, _, text = l.rstrip('\n').partition('\t')
                L.append((base + int(f), text))
        p = os.path.join(LOGS, f'{tag}.{k}.frames')
        if os.path.exists(p):
            Fr += [(base + int(f), int(c)) for f, c in (l.split() for l in open(p))]
        base += s['frames']
    cards = [s['card_ms'] for s in secs if s['card_ms']]
    rounds = [float(m[1]) for s in secs for l in s.get('stats', []) for m in [re.search(r'([\d.]+) rounds a frame', l)] if m]
    return dict(machine=machine, part=part, round=rnd, frames=base, lines=L, lists=Fr, wall=sum(s['wall'] for s in secs),
                card_ms=round(sum(cards) / len(cards), 3) if cards else None, sections=len(secs),
                rounds=round(sum(rounds) / len(rounds), 1) if rounds else None,
                error=next((f'harness exit {s["code"]}' for s in secs if s.get('code') not in (0, None) and not s['frames']), None))


def first(L, pat, after=0):
    r = re.compile(pat)
    return next(((k, t) for k, t in L if k >= after and r.search(t)), (None, None))


def steady(Fr, go, gap=1.0):
    """The frame of the first list drawn after the run's last stretch of GAP seconds with none (a level loaded)"""
    pts = [f for f, c in Fr if f > go]
    k = next((i for i in range(len(pts) - 1, 0, -1) if (pts[i] - pts[i - 1]) * F_MS / 1000 > gap), None)
    return pts[k] if k is not None else pts[0] if pts else None


def measure(r, m, go):
    """One metric of a run: ('time', READY) GO to a line (None: the first list drawn after GO), seconds;
    ('rate', FROM, COUNT, SCALE) lines matching COUNT (None: lists drawn) a second from the line FROM (a number: that
    many seconds after GO) to the run's end, times SCALE; ('value', RE) a number a line prints (its group 1)"""
    L, Fr, end = r['lines'], r['lists'], r['frames']
    if isinstance(m, str):
        m = ('time', m)
    kind = m[0]
    if kind == 'tolast':   # (a line to the last list drawn)
        a0, _ = first(L, m[1], go)
        k = max((f for f, c in Fr if f > (a0 or go)), default=None)
        return None if a0 is None or k is None else round((k - a0) * F_MS / 1000, 2)
    if kind == 'lastlist':   # (GO to the last list drawn: a picture that then holds still)
        k = max((f for f, c in Fr if f > go), default=None)
        return None if k is None else round((k - go) * F_MS / 1000, 2)
    if kind == 'steady':   # (GO to the first list after the last second with none)
        k = steady(Fr, go)
        return None if k is None else round((k - go) * F_MS / 1000, 2)
    if kind == 'time':
        if m[1]:
            k, _ = first(L, m[1], go)
        else:
            base = next((c for f, c in reversed(Fr) if f <= go), None)
            k = next((f for f, c in Fr if f > go and base is not None and c != base), None)
        return None if k is None else round((k - go) * F_MS / 1000, 2)
    if kind == 'rate':
        frm, count, scale = m[1], m[2], m[3] if len(m) > 3 else 1
        s0 = go + int(frm * 1000 / F_MS) if isinstance(frm, (int, float)) else steady(Fr, go) if frm == 'steady' else first(L, frm, go)[0]
        if s0 is None or end <= s0:
            return None
        secs = (end - s0) * F_MS / 1000
        if count:
            n = sum(1 for f, t in L if s0 <= f <= end and re.search(count, t))
        else:
            inside = [c for f, c in Fr if f >= s0]
            before = [c for f, c in Fr if f < s0]
            n = inside[-1] - before[-1] if inside and before else 0
        return round(n * scale / secs, 2) if secs > 0 else None
    if kind in ('meandiff', 'meangap'):   # (a line's group 2 less group 1, or a line's group 1 less the last line's, their mean)
        got = [re.search(m[1], t) for f, t in L if f >= go and re.search(m[1], t)][5:]
        if kind == 'meandiff':
            v = [float(g[2]) - float(g[1]) for g in got]
        else:
            v = [float(b[1]) - float(a[1]) for a, b in zip(got, got[1:])]
        return round(sum(v) / len(v), 1) if v else None
    if kind == 'perms':   # (group 3 summed over the sum of group 2 less group 1, in ms: millions a second)
        got = [re.search(m[1], t) for f, t in L if f >= go and re.search(m[1], t)][5:]
        ms = sum(float(g[2]) - float(g[1]) for g in got)
        return round(sum(float(g[3]) for g in got) / ms / 1000, 2) if ms else None
    if kind == 'count':
        return float(sum(1 for f, t in L if f >= go and re.search(m[1], t)))
    if kind == 'gapline':   # (the time from one line to the next given one)
        a0, _ = first(L, m[1], go)
        b0, _ = first(L, m[2], a0 or go)
        return None if a0 is None or b0 is None else round((b0 - a0) * F_MS / 1000, 2)
    if kind == 'svclock':   # (QUAKE_TRACK's lines: the server's game time over the world's, percent, from the third line on)
        T = [(f, float(re.search(r'time (\d+) ms', t)[1])) for f, t in L if f >= go and re.search(r'track \d+: time \d+ ms', t)]
        if len(T) < 4 or T[-1][0] == T[2][0]:
            return None
        return round(100 * (T[-1][1] - T[2][1]) / ((T[-1][0] - T[2][0]) * F_MS), 1)
    if kind == 'pertimes':   # (sum of a line's group 1 over its group 2, times the frames a second of another line's sums)
        got = [re.search(m[1], t) for f, t in L if f >= go and re.search(m[1], t)]
        per = sum(float(g[1]) for g in got) / sum(float(g[2]) for g in got) if got else None
        fps = measure(r, ('sumratio', m[2], 1000), go)
        return round(per * fps, 2) if per and fps else None
    if kind == 'sumrate':   # (a count a line gives, summed over the lines after the first, a second between the first and the last)
        got = [(f, float(re.search(m[1], t)[1])) for f, t in L if f >= go and re.search(m[1], t)]
        if len(got) < 2 or got[-1][0] == got[0][0]:
            return None
        return round(sum(v for _, v in got[1:]) / ((got[-1][0] - got[0][0]) * F_MS / 1000), 2)
    if kind == 'sumratio':   # (the sums of a line's two numbers over the run, their ratio times SCALE: frames in ms)
        got = [re.search(m[1], t) for f, t in L if f >= go and re.search(m[1], t)]
        den = sum(float(g[2]) for g in got)
        return round(sum(float(g[1]) for g in got) * (m[2] if len(m) > 2 else 1) / den, 2) if den else None
    if kind in ('value', 'mean', 'last'):
        scale = m[2] if len(m) > 2 else 1
        v = [float(re.search(m[1], t)[1]) for f, t in L if f >= go and re.search(m[1], t)]
        if not v:
            return None
        x = v[0] if kind == 'value' else v[-1] if kind == 'last' else sum(v) / len(v)
        return round(x * scale, 3)
    raise ValueError(kind)


def pacing(r, go, start=None):
    """Frame pacing over the run's second half after GO (its steady part), or from START (a part's `pace`): the times
    between frames drawn (a host frame that drew k lists counts k frames 1/k of its gap apart), median / 95th / 99th
    percentile in ms, and hitches: gaps over twice the median, a second"""
    Fr, end = r['lists'], r['frames']
    s0 = go + (end - go) // 2 if start is None else start
    pts = [(f, c) for f, c in Fr if f >= s0]
    gaps = []
    for (f0, c0), (f1, c1) in zip(pts, pts[1:]):
        k = c1 - c0
        if k > 0:
            gaps += [(f1 - f0) * F_MS / k] * k
    if len(gaps) < 10:
        return {}
    g = sorted(gaps)
    q = lambda x: round(g[min(len(g) - 1, int(x * len(g)))], 1)
    med = q(0.5)
    secs = (end - s0) * F_MS / 1000
    return {'frame time median': med, 'frame time p95': q(0.95), 'frame time p99': q(0.99),
            'hitches a second': round(sum(1 for x in gaps if x > 2 * med) / secs, 2) if secs > 0 else None}


def summary(r):
    P = PARTS[r['part']]
    go, _ = first(r['lines'], r'^GO$')
    out = dict(machine=r['machine'], part=r['part'], round=r.get('round', 0), when=time.strftime('%Y-%m-%d %H:%M'),
               wall_s=round(r['wall'], 1), card_ms_frame=r['card_ms'], rounds_frame=r.get('rounds'),
               guest_s=round(r['frames'] * F_MS / 1000, 1), sections=r['sections'])
    if go is None:
        out['error'] = r.get('error') or 'no GO'
        return out
    out['metrics'] = {name: measure(r, m, go) for name, (unit, better, m) in P['metrics'].items()}
    pace = P.get('pace')
    start = None if pace is None else steady(r['lists'], go) if pace == 'steady' else first(r['lines'], pace, go)[0]
    out['metrics'].update(pacing(r, go, start) if pace is None or start is not None else {})
    return out


from parts import PARTS   # noqa: E402


def record(s):
    with open(RESULTS, 'a') as f:
        f.write(json.dumps(s) + '\n')


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == 'shell':
        sys.exit(0 if make_shell(a[1]) else 1)
    if a[0] == 'again':
        s = summary(load_logs(a[1], a[2], int(a[3]) if len(a) > 3 else 0))
    else:
        r = run(a[0], a[1], int(a[2]) if len(a) > 2 else 0)
        if r is None:
            print('MORE')
            sys.exit(0)
        s = summary(r)
    record(s)
    import report
    report.main()
    print(json.dumps(s))
