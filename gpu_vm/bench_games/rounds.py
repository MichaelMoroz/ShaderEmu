"""Work a pass, gpu_vm against RISC-V at the same budget: the passes each machine takes to do one job from the desktop
checkpoint (wide.py's desk.snap), counting only the passes in which core 0 was busy. In Unity a pass is a round and a
frame has a fixed number of them, so fewer busy passes is the job done in fewer of the world's frames; the harness's
passes a second do not enter (a sleep costs every machine the same rounds there, here not, so idle passes are left out).

    python gpu_vm/bench_games/rounds.py [OUT] --machine rv,v0009 [--ticks 4096,8192] [--jobs imdemo,nxterm,...]

A job's line is typed into the machine, which runs it and prints UIDONE; the run's --frame-log says why each pass
ended. Every run goes through tools/gpu_queue.ps1 (run.py's harness()).
"""
import argparse, datetime, os, subprocess, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run as R      # noqa: E402
import wide as Wd    # noqa: E402  (the checkpoints and the RISC-V image)

JOBS = {
    'imdemo': 'IMDEMO_FRAMES=300 imdemo',
    'imdemo-show': 'IMDEMO_SHOW=1 IMDEMO_FRAMES=150 imdemo',
    # a start: the busy passes of the 3 s after it, less idle's (a desktop is never quite quiet: SETTLE waited forever)
    'idle': 'sleep 3',
    **{f'start-{p}': f'{p} & sleep 3; kill $!' for p in ('nxterm', 'nxedit', 'nxfiles', 'nxcalc', 'nxsettings')},
    'coremark': 'coremark 0x0 0x0 0x66 300',
    'doomt': 'doom -1 -timedemo demo1',
    'quake1': ('quake +timedemo demo1', ' fps'),   # (it stays at its console after the timedemo: its fps line ends the job)
}
IDLE_GVM = {2}        # wait: core 0 had nothing to do (gpu_vm's frame log)
IDLE_RV = {9}         # wfi/pause (RISC-V's)


def busy_passes(path, machine):
    d = np.fromfile(path, dtype=np.uint32).reshape(-1, 4)
    if machine == 'rv':
        n, why, idle = d[:, 1].astype(np.int64), d[:, 2].astype(np.int64), IDLE_RV
    else:
        n, why, idle = (d[:, 2] >> 8).astype(np.int64), (d[:, 2] & 0xff).astype(np.int64), IDLE_GVM
    busy = ~np.isin(why, list(idle)) | (n > 2000)
    return len(d), int(busy.sum()), float(n[busy].mean()) if busy.any() else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?')
    ap.add_argument('--machine', default='rv,v0009')
    ap.add_argument('--ticks', default='4096,8192')
    ap.add_argument('--jobs', default='imdemo,idle,start-nxterm,start-nxedit,start-nxfiles')
    ap.add_argument('--limit', type=int, default=200)
    a = ap.parse_args()
    out = os.path.abspath(a.out) if a.out else os.path.join(R.ROOT, 'build', 'bench_games', 'rounds-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    os.makedirs(out, exist_ok=True)
    rows = []
    for job in a.jobs.split(','):
        for t in a.ticks.split(','):
            for m in a.machine.split(','):
                base = os.path.join(out, f'{job}.{m}.{t}')
                cmd, until = JOBS[job] if isinstance(JOBS[job], tuple) else (JOBS[job], 'UIDONE')
                line = f'cd /; {cmd}; echo UI""DONE'
                c, cwd = R.harness(m, ['--load-state', Wd.snap_of(m), '--seconds', str(a.limit), '--ticks', t, '--input', line + '\\n',
                                       '--until', until, '--frame-log', base + '.frames'], base + '.log')
                t0 = time.time()
                p = subprocess.run(c, cwd=cwd, capture_output=True, text=True, errors='replace')
                open(base + '.err', 'w').write(p.stderr)
                ok = p.returncode == 0 and os.path.exists(base + '.frames')
                allp, busy, per = busy_passes(base + '.frames', m) if ok else (0, 0, 0)
                rows.append((job, t, m, allp, busy, per))
                print(f'{job} {m} --ticks {t}: {"exit %d" % p.returncode if not ok else ""} {busy} busy passes of {allp}, '
                      f'core 0 {per:.0f} a busy pass ({time.time() - t0:.0f} s)', flush=True)
    print('\n| job | budget | ' + ' | '.join(a.machine.split(',')) + ' |')
    print('|---|---|' + '---|' * len(a.machine.split(',')))
    for job in a.jobs.split(','):
        for t in a.ticks.split(','):
            cells = [next((f'{r[4]}' for r in rows if r[:3] == (job, t, m)), '') for m in a.machine.split(',')]
            print(f'| {job} | {t} | ' + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main()
