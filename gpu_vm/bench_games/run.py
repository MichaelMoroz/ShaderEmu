"""Games from checkpoints on the RISC-V machine and on gpu_vm, side by side (gpu_vm/bench_games/README.md).

    python gpu_vm/bench_games/run.py save  --machine gvm|rv  [--games quake3,halflife,nxray]
    python gpu_vm/bench_games/run.py bench --machine rv,x18,x21,x22 [--games ...] [--seconds 15] [--rounds 1]
    python gpu_vm/bench_games/run.py table [LOGDIR]

`save` boots the machine, starts the game and writes a snapshot when the game prints its mark (one per image: a
checkpoint is bound to the image it was made from). `bench` runs each game from its checkpoint for --seconds on each
machine in turn (a machine is `rv`, the RISC-V machine's rvc_opt, or a step of gpu_vm/tick/steps), every run through
tools/gpu_queue.ps1, and prints a table: the game's own frames a second, core 0's bundles (RISC-V: instructions) a
pass, why passes ended, the workers' ops a second and the STATS line. Logs go to build/bench_games/<time>/.

The checkpoints are machine-local and not in git: build/checkpoints/{gvm,rv}/GAME.snap (this worktree's build/).
gpu_vm boots build/gvm_apps/img with this worktree's bin/rvc_harness.exe; RISC-V boots RV_IMAGE (below) with the main
checkout's bin/rvc_harness.exe and experiments/rvc_opt (RV_ROOT), from a folder of junctions whose build/images/linux
and nommu are that image (tools/game_par.py's card_folder).
"""
import argparse, datetime, os, re, subprocess, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RV_ROOT = os.environ.get('RV_ROOT', os.path.join(os.path.dirname(ROOT), 'ShaderEmu'))
GVM_IMAGE = os.environ.get('GVM_IMAGE', os.path.join(ROOT, 'build', 'gvm_apps', 'img'))   # (another image: its own GVM_CKPT)
GVM_CKPT = os.environ.get('GVM_CKPT', 'gvm')   # gpu_vm's checkpoints' folder under build/checkpoints
# the RISC-V image: linux/prebuilt/nommu with Uplink's data (`python linux/nommu/image.py nommu_hl` in this worktree,
# with a junction rvc -> the main checkout's rvc): the main checkout's build/images/nommu has no Half-Life data
RV_IMAGE = os.environ.get('RV_IMAGE', os.path.join(ROOT, 'build', 'images', 'nommu_hl'))
CKPT = os.path.join(ROOT, 'build', 'checkpoints')
X = 'nano-X -p & sleep 2; '


def fps_q3(log):
    got = [(int(f), int(ms)) for f, ms in re.findall(r'q3stat: frame \d+: (\d+) frames in (\d+) ms', log)]
    f, ms = sum(a for a, _ in got), sum(b for _, b in got)
    return (f * 1000.0 / ms if ms else None), f'{len(got)} windows, {f} frames in {ms} ms'


def fps_hl(log):
    m = re.search(r'hlmark: ends, (\d+) frames to ([\d.]+)', log)
    if not m:
        return None, 'no hlmark: ends'
    return int(m[1]) / (float(m[2]) - HL_FROM), f'{m[1]} frames in {float(m[2]) - HL_FROM:.2f} s of the level'


def fps_ray(log):
    got = re.findall(r'raystat: \d+ x \d+ with \d+ workers: (\d+)\.(\d) pictures a second', log)
    v = [int(a) + int(b) / 10 for a, b in got]
    return (sum(v) / len(v) if v else None), ' '.join(f'{x:.1f}' for x in v)


def fps_doom(log):
    got = [(int(f), int(ms)) for f, ms in re.findall(r'doom: (\d+) frames in (\d+) ms', log)]
    f, ms = sum(a for a, _ in got), sum(b for _, b in got)
    return (f * 1000.0 / ms if ms else None), f'{f} frames in {ms} ms'


def fps_quake(log):
    m = re.search(r'(\d+) frames ([\d.]+) seconds ([\d.]+) fps', log)
    return (float(m[3]), m[0]) if m else (None, 'no timedemo line')


def fps_coremark(log):
    m = re.search(r'Iterations/Sec\s*:\s*([\d.]+)', log)
    return (float(m[1]), m[0]) if m else (None, 'no Iterations/Sec')


def fps_count(word):
    def f(log):   # (lines of a loop's rounds over the run's seconds: rounds a second; the run's length is fixed)
        n = len(re.findall(word, log))
        return (n / 15.0 if n else None), f'{n} rounds'
    return f


HL_FROM = 31
GAMES = {
    'quake3': dict(cmd=X + 'QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3',
                   mark='q3stat: frame 100:', fps=fps_q3),
    # (its level's 12 s take longer than 15 s of the wall on the RISC-V machine: 40 s for every machine)
    'halflife': dict(cmd=X + f'HL_MARK={HL_FROM},{HL_FROM + 12} halflife +map hldemo1', mark='hlmark: begins', fps=fps_hl,
                     seconds=40),
    'nxray': dict(cmd=X + 'nxray', mark='raystat: 320 x 240', fps=fps_ray),
    'doom': dict(cmd=X + 'doom -1 -timedemo demo1', mark='doom: ', fps=fps_doom),
    # programs that declare no REST mask (the builder's wad 5-7 cap nothing in them): saved at a word typed before them
    'doomt': dict(cmd=X + 'echo MA""RKD; doom -1 -timedemo demo1', mark='MARKD', fps=fps_doom, seconds=60),
    'quake1': dict(cmd=X + 'echo MA""RKQ; quake +timedemo demo1', mark='MARKQ', fps=fps_quake, seconds=60),
    'coremark': dict(cmd='echo MA""RKC; coremark', mark='MARKC', fps=fps_coremark, seconds=60),
    'gzipl': dict(cmd='echo MA""RKZ; i=0; while true; do gzip -c /usr/bin/coremark > /dev/null; i=$((i+1)); echo gzround $i; done',
                  mark='MARKZ', fps=fps_count(r'gzround \d+'), seconds=15),
}
WHY_GVM = {0: 'budget', 1: 'sync', 2: 'wait', 3: 'full', 5: 'halt'}
WHY_RV = {0: 'none', 1: 'exit call', 2: 'CSR full', 3: 'full', 6: 'UART', 7: 'fence.i', 8: 'copy', 9: 'wfi/pause',
          10: 'fill'}


def rv_folder():
    """A folder of junctions to the main checkout whose build/images/linux and nommu are its nommu image."""
    image = RV_IMAGE
    work = os.path.join(ROOT, 'build', 'bench_games', 'rvroot-' + os.path.basename(image))
    links = [('experiments', os.path.join(RV_ROOT, 'experiments')), ('harness', os.path.join(RV_ROOT, 'harness')),
             (os.path.join('build', 'shadercache'), os.path.join(RV_ROOT, 'build', 'shadercache')),
             (os.path.join('build', 'images', 'linux'), image), (os.path.join('build', 'images', 'nommu'), image)]
    os.makedirs(os.path.join(work, 'build', 'images'), exist_ok=True)
    for link, target in links:
        if not os.path.exists(os.path.join(work, link)):
            subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(work, link), target], capture_output=True, check=True)
    return work


def harness(machine, extra, log):
    """The command and folder for one run: machine 'rv' or a gpu_vm step's name."""
    q = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1')]
    rounds = []   # (NAME__rK: the machine NAME with K tick rounds a host frame, --tick-rounds K)
    if '__r' in machine:
        machine, k = machine.split('__r')
        rounds = ['--tick-rounds', k]
    if machine == 'rv':
        # (from a snapshot the harness takes the full machine unless told: the no-MMU Linux runs without paging)
        # (RV_EXE: another harness for the RISC-V machine, this worktree's bin/rvc_harness.exe for RVC_LIST_LAZY say)
        return q + [os.environ.get('RV_EXE', os.path.join(RV_ROOT, 'bin', 'rvc_harness.exe')), '--d3d11', '--rvc', 'experiments/rvc_opt',
                    '--define', 'NO_PAGING', '--image', 'linux-net', '--no-desktop', '--no-stdin', '--uart-log', log] + rounds + extra, rv_folder()
    a = q + [os.path.join(ROOT, 'bin', 'rvc_harness.exe'), '--gvm', GVM_IMAGE, '--no-viz', '--no-stdin', '--uart-log', log]
    if machine != 'gvm':   # a step of gpu_vm/tick/steps, or BATCH_VNNNN: the builder's build/tick_gen/BATCH/vNNNN.hlsl
        step = f'gpu_vm/tick/steps/{machine}.hlsl'
        if not os.path.exists(os.path.join(ROOT, step)) and '_v' in machine:
            step = 'build/tick_gen/%s/%s.hlsl' % tuple(machine.split('_', 1))
        a += ['--gvm-tick', step]
    a += os.environ.get('GVM_EXTRA', '').split() + rounds   # (more of the harness's options, as --ticks N)
    return a + extra, ROOT


def save(a):
    m = 'rv' if a.machine == 'rv' else GVM_CKPT
    os.makedirs(os.path.join(CKPT, m), exist_ok=True)
    for g in a.games.split(','):
        G = GAMES[g]
        snap = os.path.join(CKPT, m, g + '.snap')
        log = os.path.join(CKPT, m, g + '.save.log')
        prompt = '/ # ' if m == 'rv' else '@gpuvm:~# '
        pre = '' if m == 'rv' else 'cd /; export TERM=linux; '
        pre += os.environ.get('GVM_PRE', '')   # (commands before the game's: a kernel switch, say)
        # (GVM_SAVE_TICK: the tick that boots and saves, for an image whose code needs one: late branches, fx1)
        cmd, cwd = harness(os.environ.get('GVM_SAVE_TICK', 'x22') if m != 'rv' else m, ['--seconds', str(a.limit), '--expect', prompt, '--send', pre + G['cmd'] + '\\n',
                               '--until', G['mark'], '--save-state', snap], log)
        t = time.time()
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
        ok = p.returncode == 0 and os.path.exists(snap)
        print(f'{m} {g}: {"saved " + snap if ok else "FAILED (exit %d)" % p.returncode} in {time.time() - t:.0f} s')
        if not ok:
            print(p.stderr[-2000:])


def passes(path, machine):
    """(core 0's bundles or instructions a pass, {why: share}) from a --frame-log."""
    if not os.path.exists(path) or os.path.getsize(path) < 16:
        return None, {}
    d = np.fromfile(path, dtype=np.uint32).reshape(-1, 4)
    if machine.split('__r')[0] == 'rv':
        n = d[:, 1].astype(np.int64)
        why = np.where(n >= 8192, -1, d[:, 2].astype(np.int64))
        names = {**WHY_RV, -1: 'budget'}
    else:
        n = (d[:, 2] >> 8).astype(np.int64)
        why = (d[:, 2] & 0xff).astype(np.int64)
        names = WHY_GVM
    hist = {names.get(int(k), str(int(k))): float((why == k).mean()) for k in np.unique(why)}
    w = path + '.workers'   # per pass: the workers' instructions or ops, how many ran, how many ran a whole pass, how many only looked
    if os.path.exists(w) and os.path.getsize(w) >= 16:
        v = np.fromfile(w, dtype=np.uint32).reshape(-1, 4)
        hist['(a worker ran its whole budget)'] = float((v[:, 2] > 0).mean())
    return float(n.mean()), hist


def bench(a):
    out = os.path.abspath(a.out) if a.out else os.path.join(ROOT, 'build', 'bench_games', datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(out, exist_ok=True)
    machines = a.machine.split(',')
    for r in range(a.rounds):
        for g in a.games.split(','):
            for m in machines:
                snap = os.path.join(CKPT, 'rv' if m.split('__r')[0] == 'rv' else GVM_CKPT, g + '.snap')
                if not os.path.exists(snap):
                    print(f'{m} {g}: no checkpoint {snap}')
                    continue
                base = os.path.join(out, f'{g}.{m}.{r}')
                # --pass-ms: a pass a frame, TIME that much a pass (gpu_vm only: GVM_DT_TICKS), --passes of them (a world's speed)
                lim = ['--seconds', str(GAMES[g].get('seconds', a.seconds))] if not a.pass_ms else ['--seconds', '300', '--frames', str(a.passes)]
                if a.pass_ms:
                    os.environ['GVM_DT_TICKS'] = str(int(a.pass_ms * 100))
                cmd, cwd = harness(m, ['--load-state', snap] + lim + ['--stats-after', '2',
                                       '--frame-log', base + '.frames'], base + '.log')
                t = time.time()
                p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
                open(base + '.err', 'w').write(p.stderr)
                print(f'{g} {m} round {r}: exit {p.returncode}, {time.time() - t:.0f} s', flush=True)
    table(out)


def table(out):
    out = os.path.abspath(out)
    rows = []
    for f in sorted(os.listdir(out)):
        m = re.match(r'(\w+)\.(\w+)\.(\d+)\.log$', f)
        if not m:
            continue
        g, mach, r = m.groups()
        base = os.path.join(out, f[:-4])
        log = open(base + '.log', encoding='latin-1').read()
        err = open(base + '.err', encoding='latin-1').read() if os.path.exists(base + '.err') else ''
        fps, what = GAMES[g]['fps'](log)
        per, hist = passes(base + '.frames', mach)
        st = re.search(r'STATS seconds=([\d.]+) instructions=\d+ ips=(\d+) frames=\d+ fps=([\d.]+) per_frame=([\d.]+)', err)
        wk = re.search(r'STATS workers: instructions=\d+ ips=(\d+); all cores ips=(\d+)', err)
        gp = re.search(r'STATS gpu per frame: tick ([\d.]+) ms, commit ([\d.]+) ms, device ([\d.]+) ms; wall ([\d.]+) ms', err)
        rows.append((g, mach, r, fps, what, per, hist, st, wk, gp))
    print(f'\n{out}\n')
    print('| game | machine | round | guest fps | core 0 a pass | passes ended | core 0 M/s | workers M/s | passes/s | game\'s lines |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    order = {'rv': 0}
    for g, mach, r, fps, what, per, hist, st, wk, gp in sorted(rows, key=lambda x: (x[0], order.get(x[1], 1), x[1], x[2])):
        ms = ' / '.join(gp.groups()) if gp else ''
        ns = f'{float(gp[1]) * 1e6 / per:.0f}' if gp and per else ''
        h = ', '.join(f'{k} {v * 100:.0f}%' for k, v in sorted(hist.items(), key=lambda kv: -kv[1]) if v >= 0.005)
        print(f'| {g} | {mach} | {r} | {fps:.1f} | {per:,.0f} | {h} | {int(st[2]) / 1e6:.2f} | '
              f'{int(wk[1]) / 1e6 if wk else 0:.1f} | {st[3]} | {ms} | {ns} | {what} |' if fps is not None and st and per is not None else
              f'| {g} | {mach} | {r} | - | {per or 0:,.0f} | {h} | {int(st[2]) / 1e6 if st else 0:.2f} | | {st[3] if st else ""} | {ms} | {ns} | {what} |')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=('save', 'bench', 'table'))
    ap.add_argument('out', nargs='?')
    ap.add_argument('--machine', default='rv,x18,x21,x22')
    ap.add_argument('--games', default='quake3,halflife,nxray')
    ap.add_argument('--seconds', type=int, default=15)
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--pass-ms', type=float, default=0, help='bench: TIME a pass (ms), as a world drawing a pass a frame')
    ap.add_argument('--passes', type=int, default=1500, help='bench with --pass-ms: the passes a run')
    ap.add_argument('--limit', type=int, default=280, help='save: the run\'s most seconds')
    a = ap.parse_args()
    if a.what == 'save':
        save(a)
    elif a.what == 'bench':
        bench(a)
    else:
        table(a.out)


if __name__ == '__main__':
    main()
