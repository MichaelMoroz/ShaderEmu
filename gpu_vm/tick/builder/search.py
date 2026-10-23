#!/usr/bin/env python3
"""The builder's search: thousands of tick variants made, checked on WARP in parallel, timed on the card in batches.

    python gpu_vm/tick/builder/search.py gen BATCH [--n N] [--seed S] [--from JSON|BATCH:NAME] [--mutate K] [--grid k=a,b ...]
    python gpu_vm/tick/builder/search.py check BATCH [-j J] [--timeout S]
    python gpu_vm/tick/builder/search.py time BATCH [--cases A,B] [--rounds R] [--chunk N] [--passes N] [--top N] [--refs x23,x24]
    python gpu_vm/tick/builder/search.py report BATCH [--top N]

A batch is a folder build/tick_gen/BATCH: a variant is vNNNN.hlsl and its manifest vNNNN.json (gen.py's). `check` runs
gvmbench on WARP for every variant not yet checked (CHECK_CASES: gvmbench's default set, the p_ cases with idle and halted
cores, mc_int across bands), --mark-ok, a timeout each; a tick over D3D11's 4,096 temporary registers is refused by gvmbench
itself. Only a variant whose SHADER.warp_ok stamp matches its source reaches the card (gvmbench refuses the rest).
`time` runs the checked variants on the card through tools/gpu_queue.ps1, in chunks with the reference steps in every chunk,
in interleaved rounds (gvmbench's ABBA order), and keeps every round's number (BATCH/time_*.json). `report` scores each
variant against the reference in its chunk: the geometric mean of its speed ratios over the cases (> 1 faster).
"""
import argparse, glob, hashlib, json, os, random, re, subprocess, sys, time
import concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import gen

GVMB = os.environ.get('GVMBENCH') or os.path.join(ROOT, 'bin', 'gvmbench.exe')
CHECK_CASES = 'default,p_k1_m0,p_k4_m40,mc_int,p_spill,p_xspill'
CHECK_SETS = ''   # (the game bench's sets are inputs, not the gate: on WARP they take a minute a variant)
CHECK_CORES = '40'
TIME_CASES = 'arith,loads,stores,mix,coremark,gzip,p_k0_m0,p_k8_m40,p_spill,p_xspill'   # (mc_ at 255 cores: seconds each)
TIME_SETS = 'game,game_x'
LOWER_BETTER = lambda name: name.startswith(('p_', 'mc_')) or name in ('coremark', 'coremark_pie', 'gzip', 'gunzip')   # (g_: frames a second, higher)
REFS = {'x23': 'gpu_vm/tick/steps/x23.hlsl', 'x24': 'gpu_vm/tick/steps/x24.hlsl'}

def bdir(batch): return os.path.join(ROOT, 'build', 'tick_gen', batch)
def key_of(P): return hashlib.sha1(json.dumps(gen.full(P), sort_keys=True).encode()).hexdigest()[:16]

def sample(rng):
    S = gen.SPACE
    for _ in range(1000):
        bw, bh = rng.choice(S['block'])
        P = dict(nb=rng.choice(S['nb']), bloom=rng.choice(S['bloom']), hash=rng.choice(S['hash']), clear=rng.choice(S['clear']),
                 fetch=rng.choice(S['fetch']), int_first=rng.choice(S['int_first']), mem_order=rng.choice(S['mem_order']),
                 split=rng.choice(S['split']), bw=bw, bh=bh, band=bh * rng.choice(S['bandx']), per=rng.choice(S['per']),
                 jrow=rng.choice(S['jrow']), perm=rng.choice(S['perm']), mul=rng.choice(S['mul']), early=rng.choice(S['early']))
        if P['per'] * bw > 16384: continue
        if not gen.valid(gen.full(P)): return P
    raise RuntimeError('no valid point')

def mutate(P, rng, k):
    S = gen.SPACE
    for _ in range(1000):
        Q = dict(P)
        for key in rng.sample(['nb', 'bloom', 'hash', 'clear', 'fetch', 'int_first', 'mem_order', 'split', 'block', 'bandx', 'per',
                               'jrow', 'perm', 'mul', 'early'], k):
            if key == 'block': Q['bw'], Q['bh'] = rng.choice(S['block']); Q['band'] = max(Q['band'], Q['bh'])
            elif key == 'bandx': Q['band'] = Q['bh'] * rng.choice(S['bandx'])
            else: Q[key] = rng.choice(S[key])
        if Q['band'] < Q['bh']: Q['band'] = Q['bh']
        if not gen.valid(gen.full(Q)): return Q
    return P

def load_point(spec):
    if spec.endswith('.json'): return json.load(open(spec))['params']
    b, n = spec.split(':')
    return json.load(open(os.path.join(bdir(b), n + '.json')))['params']

def cmd_gen(a):
    d = bdir(a.batch); os.makedirs(d, exist_ok=True)
    have = {json.load(open(f)).get('key') for f in glob.glob(os.path.join(d, 'v*.json'))}
    n0 = len(glob.glob(os.path.join(d, 'v*.json')))
    rng = random.Random(a.seed)
    pts = []
    if a.grid:   # the default point (or --from) with every combination of the given values
        base = gen.full(load_point(a.from_) if a.from_ else {})
        pts = [dict(base)]
        for g in a.grid:
            k, vs = g.split('=')
            vals = [int(v) if isinstance(gen.DEFAULTS.get(k, 0), int) and k != 'block' else v for v in vs.split(',')]
            nxt = []
            for p in pts:
                for v in vals:
                    q = dict(p)
                    if k == 'block': q['bw'], q['bh'] = map(int, v.split('x')); q['band'] = max(q['band'], q['bh'])
                    else: q[k] = v
                    nxt.append(q)
            pts = nxt
    while len(pts) < a.n and not a.grid:
        pts.append(mutate(load_point(a.from_), rng, a.mutate) if a.from_ else sample(rng))
    made = 0
    for P in pts:
        P = gen.full(P)
        if gen.valid(P): continue
        src = gen.emit(P)
        k = hashlib.sha1(src.encode()).hexdigest()[:16]   # (two points that make one source are one variant)
        if k in have: continue
        have.add(k)
        name = 'v%04d' % (n0 + made)
        with open(os.path.join(d, name + '.hlsl'), 'w', newline='\n') as f: f.write(src)
        m = gen.manifest(P); m['key'] = k; m['name'] = name
        with open(os.path.join(d, name + '.json'), 'w') as f: json.dump(m, f, indent=1)
        made += 1
    print('gen: %d new variants in %s (%d in all)' % (made, d, n0 + made))

def check_one(path, timeout):
    t0 = time.time()
    try:
        r = subprocess.run([GVMB, '--warp', '--mark-ok', '--shader', path, '--case', CHECK_CASES] + (['--set', CHECK_SETS] if CHECK_SETS else []) + ['--cores', CHECK_CORES, '--rounds', '0'],
                           capture_output=True, text=True, timeout=timeout, cwd=ROOT)
        out = r.stdout + r.stderr
        regs = re.search(r'(\d+) temporary registers', out)
        size = re.search(r'fragment shader is (\d+) bytes', out)
        ok = r.returncode == 0 and 'same as the reference' in out
        why = '' if ok else ('registers' if 'more than D3D11' in out else 'wrong' if 'WRONG' in out else 'exit %d' % r.returncode)
        return dict(ok=ok, why=why, regs=int(regs.group(1)) if regs else None, bytes=int(size.group(1)) if size else None,
                    seconds=round(time.time() - t0, 1), tail=out.strip().splitlines()[-1][-200:] if out.strip() else '')
    except subprocess.TimeoutExpired:
        return dict(ok=False, why='timeout', seconds=timeout)

def cmd_check(a):
    d = bdir(a.batch)
    res_f = os.path.join(d, 'checks.json')
    res = json.load(open(res_f)) if os.path.exists(res_f) else {}
    todo = []
    for f in sorted(glob.glob(os.path.join(d, 'v*.hlsl'))):
        n = os.path.basename(f)[:-5]
        stamp = os.path.exists(f + '.warp_ok')
        if n in res and (res[n]['ok'] == stamp): continue
        todo.append((n, f))
    print('check: %d variants on WARP, %d at a time' % (len(todo), a.j))
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.j) as ex:
        futs = {ex.submit(check_one, f, a.timeout): n for n, f in todo}
        for i, fu in enumerate(cf.as_completed(futs)):
            n = futs[fu]; res[n] = fu.result()
            if not res[n]['ok'] or i % 20 == 0:
                print('  %s %s %s regs %s (%d/%d, %.0f s)' % (n, 'ok' if res[n]['ok'] else 'FAILED', res[n]['why'], res[n].get('regs'), i + 1, len(todo), time.time() - t0), flush=True)
            if i % 10 == 0: json.dump(res, open(res_f, 'w'), indent=1)
    json.dump(res, open(res_f, 'w'), indent=1)
    print('check: %d of %d pass' % (sum(1 for r in res.values() if r['ok']), len(res)))

def gpu_run(args, log):
    cmd = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), GVMB] + args
    env = dict(os.environ, GVM_PROGRESS="1")
    with open(log, "w") as f:
        r = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=900, env=env)
    return r.returncode

def cmd_time(a):
    d = bdir(a.batch)
    res = json.load(open(os.path.join(d, 'checks.json')))
    names = [n for n in sorted(res) if res[n]['ok'] and os.path.exists(os.path.join(d, n + '.hlsl.warp_ok'))]
    if a.only: names = [n for n in names if n in a.only.split(',')]
    if a.top:   # the best of an earlier timing
        sc = scores(a.batch)
        names = [n for n, _ in sorted(sc.items(), key=lambda x: -x[1]['score']) if n in names][:a.top]
    done = set()
    for f in glob.glob(os.path.join(d, 'time_*.json')):
        if a.retime: break
        for v in json.load(open(f))['variants']: done.add(v['name'])
    names = [n for n in names if n not in done or a.retime]
    refs = [r for r in a.refs.split(',') if r]
    print('time: %d variants, chunks of %d with %s, %d rounds, cases %s' % (len(names), a.chunk, refs, a.rounds, a.cases))
    tag = time.strftime('%H%M%S')
    for c in range(0, len(names), a.chunk):
        part = names[c:c + a.chunk]
        vf = os.path.join(d, 'chunk_%s_%03d.txt' % (tag, c // a.chunk))
        with open(vf, 'w') as f:
            for r in refs: f.write('%s %s\n' % (r, REFS[r]))
            for n in part: f.write('%s %s\n' % (n, os.path.join(d, n + '.hlsl').replace('\\', '/')))
        jf = os.path.join(d, 'time_%s_%03d.json' % (tag, c // a.chunk))
        t0 = time.time()
        rc = gpu_run(['--variants', vf, '--nocheck', '--case', a.cases, '--rounds', str(a.rounds), '--passes', str(a.passes),
                      '--cores', str(a.cores), '--json', jf, '--deadline', str(a.deadline), '--max-passes', str(a.passes)] + (['--set', a.sets] if a.sets else []), jf[:-5] + '.log')
        print('  chunk %d: %d variants, exit %d, %.0f s' % (c // a.chunk, len(part), rc, time.time() - t0), flush=True)
    cmd_report(a)

def scores(batch, ref='x23'):
    d = bdir(batch)
    out = {}
    for f in sorted(glob.glob(os.path.join(d, 'time_*.json'))):
        try: J = json.load(open(f))
        except Exception: continue
        V = {v['name']: v for v in J['variants']}
        if ref not in V: continue
        R = V[ref]['cases']
        mean = lambda x: sum(x) / len(x) if x else 0
        for n, v in V.items():
            rat = {}
            for c, xs in v['cases'].items():
                m, rm = mean(xs), mean(R.get(c, []))
                if m > 0 and rm > 0: rat[c] = rm / m if LOWER_BETTER(c) else m / rm
            if not rat: continue
            import math
            g = math.exp(sum(math.log(x) for x in rat.values()) / len(rat))
            spread = max(((max(xs) - min(xs)) / mean(xs) if len(xs) > 1 and mean(xs) > 0 else 0) for xs in v['cases'].values())
            key = n if n not in REFS else '%s@%s' % (n, os.path.basename(f))
            U = v.get('util', {})
            gu = [sum(x) / len(x) for c, x in U.items() if c.startswith('g_') and x]
            out[key] = dict(util=sum(gu) / len(gu) if gu else 0, score=g, ratios=rat, raw={c: mean(xs) for c, xs in v['cases'].items()}, spread=spread, file=os.path.basename(f))
    return out

def cmd_report(a):
    sc = scores(a.batch)
    d = bdir(a.batch)
    rows = sorted(sc.items(), key=lambda x: -x[1]['score'])
    cases = sorted({c for v in sc.values() for c in v['ratios']}, key=lambda c: TIME_CASES.split(',').index(c) if c in TIME_CASES.split(',') else 99)
    print('%-26s %6s %5s ' % ('variant', 'score', 'util') + ' '.join('%8s' % c[:8] for c in cases) + '  params')
    for n, v in rows[:a.top or 30]:
        P = {}
        mf = os.path.join(d, n + '.json')
        if os.path.exists(mf):
            p = json.load(open(mf))['params']
            P = {k: p[k] for k in p if p[k] != gen.DEFAULTS[k]}
        print('%-26s %6.3f %5.2f ' % (n, v['score'], v['util']) + ' '.join('%8.3f' % v['ratios'].get(c, 0) for c in cases) + '  ' + ' '.join('%s=%s' % kv for kv in P.items()))

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd')
    g = sp.add_parser('gen'); g.add_argument('batch'); g.add_argument('--n', type=int, default=24); g.add_argument('--seed', type=int, default=1)
    g.add_argument('--from', dest='from_'); g.add_argument('--mutate', type=int, default=2); g.add_argument('--grid', nargs='*')
    c = sp.add_parser('check'); c.add_argument('batch'); c.add_argument('-j', type=int, default=8); c.add_argument('--timeout', type=int, default=300)
    t = sp.add_parser('time'); t.add_argument('batch'); t.add_argument('--cases', default=TIME_CASES); t.add_argument('--rounds', type=int, default=3)
    t.add_argument('--chunk', type=int, default=24); t.add_argument('--passes', type=int, default=100); t.add_argument('--refs', default='x23,x24')
    t.add_argument('--top', type=int, default=0); t.add_argument('--only'); t.add_argument('--retime', action='store_true'); t.add_argument('--cores', type=int, default=255); t.add_argument('--sets', default=TIME_SETS); t.add_argument('--deadline', type=float, default=480)
    r = sp.add_parser('report'); r.add_argument('batch'); r.add_argument('--top', type=int, default=30)
    a = ap.parse_args()
    {'gen': cmd_gen, 'check': cmd_check, 'time': cmd_time, 'report': cmd_report}[a.cmd](a)
