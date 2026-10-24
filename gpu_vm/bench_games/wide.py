"""The wider comparison with the RISC-V machine (gpu_vm/bench_games/RESULTS.md): programs that are not games with a
frame rate, each timed by the guest's clock from a desktop checkpoint, on the card.

    python gpu_vm/bench_games/wide.py save  --machine gvm|rv          (once per image: the desktop, nano-X up)
    python gpu_vm/bench_games/wide.py bench [OUT] --machine rv,v0009,x23 [--progs a,b] [--rounds 2]
    python gpu_vm/bench_games/wide.py table OUT

A run loads its machine's checkpoint `desk.snap` (run.py's folders: GVM_CKPT for gpu_vm, rv), types the program's
command in a wrapper and ends at its last line. The wrapper prints /proc/uptime and /proc/stat's cpu line before and
after: a program that ends (or prints its `until` text) is timed by the uptime (the harness's TIME follows the wall),
and a desktop program's start (`kind` busy) by the jiffies the whole machine was busy in the 4 s after its command, less
the same 4 s with no command (`idle`). Every run goes through tools/gpu_queue.ps1 (run.py's harness()).
"""
import argparse, datetime, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run as R   # noqa: E402

# the RISC-V machine's image: run.py's (Uplink's data) with the bench's CoreMark and minigzip (gpu_vm/bench_real: the same
# sources and clang as gpu_vm's) in /usr/bin, made by linux/nommu/image.py from a copy of nommu_hl's overlay; the
# checkpoints of both machines in build/checkpoints/wide/{rv,GVM_CKPT}
R.RV_IMAGE = os.environ.get('WIDE_RV_IMAGE', os.path.join(R.ROOT, 'build', 'images', 'nommu_hlb'))
R.CKPT = os.path.join(R.CKPT, 'wide')
WAD = '/usr/share/halflife/valve/decals.wad'   # (924,840 bytes, the same file in both images)

X = 'nano-X -p & sleep 2; '   # (the checkpoint has the server: a program's own X is not wanted)
CC = 'mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
PATHS = (('nxpath', '', '160x120'), ('cornell', 'cornell', '120x120'), ('cornell-balls', 'cornell-balls', '120x120'),
         ('balls', 'balls', '160x120'), ('materials', 'materials', '160x120'), ('beer', 'beer', '160x120'),
         ('mirrors', 'mirrors', '160x120'), ('glow', 'glow', '160x120'))
# name: (command, the text that ends it (None: its end), a line of its output that says it ran right, kind)
PROGS = {
    **{f'path-{n}': (f'NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE={size} NXPATH_SAMPLES=2 nxpath {s}', None, r'pathstat: .* sum', 'time')
       for n, s, size in PATHS},
    'phys-wall': ('PHYS_SCENE=1 PHYS_STEPS=120 nxphys', None, r'sum [0-9a-f]{8}', 'time'),
    'phys-tower': ('PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300 nxphys', None, r'sum [0-9a-f]{8}', 'time'),
    'nes-thwaite': ('NES_SOUND=0 NES_FRAMES=600 nes thwaite', None, r'done', 'time'),
    'nes-nova': ('NES_SOUND=0 NES_FRAMES=600 nes nova', None, r'done', 'time'),
    'classicube': (CC + 'CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube', 'classicube: steady', r'classicube: steady', 'time'),
    'micropolis': ('MICROPOLIS_SUM=/usr/share/micropolis/cities/haight.cty,1000 nxmicropolis; MICROPOLIS_SUM=1,500 nxmicropolis; '
                   'MICROPOLIS_SUM=8,1500 nxmicropolis', None, r'micropolis: .* sum', 'time'),
    'tdawn-1300': (TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL', None, r'done at frame 1300', 'time'),
    'ralert-1500': ('TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 RALERT_FRAMES=1500 ralert',
                    None, r'done at frame 1500', 'time'),
    'quake-light': ('QUAKE_LIGHTPOINT=check quake +timedemo demo1', ' fps', r'not as made here|fps', 'time'),
    'qjs': ('qjs /examples/pi.js 2000 | tail -c 40', None, r'\d{20,}', 'time'),
    'micropython': ("micropython -c 'print(sum(i * i for i in range(30000)))'", None, r'^\d+', 'time'),
    'media-sound': ('; '.join(f'media_test audio /usr/share/play-test.{e} /tmp/o.raw {w}' for e in ('flac', 'mp3', 'mod') for w in (0, 63)),
                    None, r'sum', 'time'),
    'nxplay': ('; '.join(f'NXPLAY_SUM=1 nxplay /usr/share/play-test.{e}' for e in ('wav', 'flac', 'mod', 'mp3')), None, r'sum', 'time'),
    'nxvideo': ('NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo; NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo /root/Videos/Big*.avi', None, r'sum', 'time'),
    'unarc': ('cd /tmp; rm -rf z1 t1; mkdir z1 t1; unarc zip /usr/share/classicube-default.zip z1; '
              'tar czf u.tgz /usr/share/font-mono-1*.sef /usr/share/web-*.html 2>/dev/null; unarc tgz u.tgz t1; ls z1 t1 | wc -l; cd /',
              None, r'^\d+', 'time'),
    'hlsl': ('HLSL_WORKERS=31 hlsl_bench /usr/share/example-fractal.hlsl 160 120 1 eye=1.9095,1.2344,-2.7911 '
             'forward=-0.53042,-0.3429,0.77531 right=0.82534,0,0.56464 up=-0.19362,0.9394,0.28301 size=160,120 turn=1,0,0,0,1,0,0,0,1',
             None, r'hlsl_bench: sum', 'time'),
    'coremark': ('coremark 0x0 0x0 0x66 300', None, r'crcfinal.*', 'time'),
    'gzip': (f'minigzip < {WAD} > /tmp/g.gz; ls -l /tmp/g.gz', None, r'\d{6,} ', 'time'),
    'gunzip': (f'minigzip -d < /tmp/g.gz | md5sum', None, r'[0-9a-f]{32}', 'time', f'minigzip < {WAD} > /tmp/g.gz'),
    'idle': ('sleep 4', None, None, 'busy'),
    **{f'start-{p}': (f'{p} & sleep 4; kill $!', None, None, 'busy')
       for p in ('nxterm', 'nxcalc', 'nxedit', 'nxfiles', 'nxmon', 'nxsettings', 'nxide')},
}


def wrapper(cmd, until, pre=''):
    now = 'echo WT $(cut -d" " -f1 /proc/uptime) $(head -1 /proc/stat)'
    run = f'({cmd}) > /tmp/w.out 2>&1 & P=$!; '
    run += (f"while ! grep -q '{until}' /tmp/w.out; do kill -0 $P 2>/dev/null || break; sleep 0.2; done; " if until else 'wait $P; ')
    return f'cd /; {pre + "; " if pre else ""}{now}; {run}{now}; kill $P 2>/dev/null; tail -c 600 /tmp/w.out; echo WD""ONE'


def snap_of(m):
    return os.path.join(R.CKPT, 'rv' if m == 'rv' else R.GVM_CKPT, 'desk.snap')


def save(a):
    R.GAMES['desk'] = dict(cmd=X + 'echo MA""RKW', mark='MARKW')
    a.games = 'desk'
    R.save(a)


def bench(a):
    out = os.path.abspath(a.out) if a.out else os.path.join(R.ROOT, 'build', 'bench_games', 'wide-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(out, exist_ok=True)
    progs = a.progs.split(',') if a.progs else list(PROGS)
    for r in range(a.rounds):
        for g in progs:
            cmd, until = PROGS[g][:2]
            pre = PROGS[g][4] if len(PROGS[g]) > 4 else ''
            for m in a.machine.split(',')[::(-1 if r % 2 else 1)]:   # every other round the other way
                base = os.path.join(out, f'{g}.{m}.{r}')
                if os.path.exists(base + '.log') and 'WDONE' in open(base + '.log', encoding='latin-1').read():
                    continue   # (a batch taken up again)
                c, cwd = R.harness(m, ['--load-state', snap_of(m), '--seconds', str(a.limit), '--input',
                                       wrapper(cmd, until, pre) + '\\n', '--until', 'WDONE'], base + '.log')
                t = time.time()
                p = subprocess.run(c, cwd=cwd, capture_output=True, text=True, errors='replace')
                open(base + '.err', 'w').write(p.stderr)
                print(f'{g} {m} round {r}: exit {p.returncode}, {time.time() - t:.0f} s, {measure(base)[0]}', flush=True)
    table(out)


def measure(base):
    """(seconds or busy seconds, the check's line or why not) of one run's log."""
    try:
        log = open(base + '.log', encoding='latin-1').read()
    except OSError:
        return None, 'no log'
    g = os.path.basename(base).split('.')[0]
    marks = re.findall(r'WT ([\d.]+) cpu +((?:\d+ ?)+)', log)
    if len(marks) < 2:
        return None, 'unfinished'
    (t0, c0), (t1, c1) = marks[-2], marks[-1]
    if PROGS[g][3] == 'busy':
        a, b = [list(map(int, c.split())) for c in (c0, c1)]
        busy = lambda v: sum(v[:3]) + sum(v[4:8])   # user nice system, iowait irq softirq steal: all but idle
        return (busy(b) - busy(a)) / 100.0, ''
    tail = log[log.index('\n', log.rindex('WT ' + t1)) + 1:]   # what the command printed, after its last mark
    ok = PROGS[g][2]
    if not ok:
        return float(t1) - float(t0), ''
    lines = [ln.strip() for ln in tail.splitlines() if re.search(ok, ln)]
    sums = re.findall(r'sum:? ([0-9a-f]{8})', ' '.join(lines))   # the sums, without the lines' times
    check = 'sums ' + ' '.join(sums) if sums else (re.search(ok, lines[0])[0][:90] if lines else 'NO CHECK LINE')
    return float(t1) - float(t0), check


def table(out):
    rows = {}
    for f in sorted(os.listdir(out)):
        m = re.match(r'([\w-]+)\.(\w+)\.(\d+)\.log$', f)
        if m:
            rows.setdefault(m[1], {}).setdefault(m[2], []).append(measure(os.path.join(out, f[:-4])))
    idle = {m: [v for v, _ in r if v is not None] for m, r in rows.get('idle', {}).items()}
    machines = sorted({m for r in rows.values() for m in r}, key=lambda m: (m != 'rv', m))
    print(f'\n{out}\n')
    print('| program | ' + ' | '.join(machines) + ' | ' + ' | '.join(f'RISC-V / {m}' for m in machines if m != 'rv') + ' | check |')
    print('|---|' + '---|' * (2 * len(machines)))
    for g in [p for p in PROGS if p in rows]:
        mean = {}
        for m in machines:
            v = [x for x, _ in rows[g].get(m, []) if x is not None]
            if v:
                mean[m] = sum(v) / len(v)
                if PROGS[g][3] == 'busy' and g != 'idle' and idle.get(m):
                    mean[m] -= sum(idle[m]) / len(idle[m])
        checks = {c for r in rows[g].values() for _, c in r if c}
        cells = [f'{mean[m]:.2f}' + (' (' + ' '.join(f'{x:.2f}' for x, _ in rows[g][m] if x is not None) + ')') if m in mean else '-'
                 for m in machines]
        ratios = [f'{mean["rv"] / mean[m]:.2f}' if 'rv' in mean and m in mean and mean[m] > 0 else '-' for m in machines if m != 'rv']
        print(f'| {g} | ' + ' | '.join(cells) + ' | ' + ' | '.join(ratios) + ' | ' + ('' if len(checks) < 2 else 'DIFFER: ') + '; '.join(sorted(checks))[:120] + ' |')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=('save', 'bench', 'table'))
    ap.add_argument('out', nargs='?')
    ap.add_argument('--machine', default='rv,v0009,x23')
    ap.add_argument('--progs', default='')
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--limit', type=int, default=300, help="a run's most seconds")
    a = ap.parse_args()
    if a.what == 'save':
        save(a)
    elif a.what == 'bench':
        bench(a)
    else:
        table(a.out)


if __name__ == '__main__':
    main()
