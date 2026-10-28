"""Card runs for the side-by-side profile (gpu_vm/PROFILE_Q3.md): one game from its checkpoint on each machine with
every per-pass log on, through tools/gpu_queue.ps1 (bench_games/run.py's harness()).

    python gpu_vm/bench_games/profile_card.py OUTDIR CONFIG[,CONFIG...] [--game quake3] [--seconds 15]

A CONFIG is MACHINE[:NAME=VALUE[:NAME=VALUE...]], MACHINE as run.py's (rv, w2_v0009, ...); NAME=VALUE pairs are set
in the harness's environment (GVM_LIST_LAZY=0, RVC_LIST_LAZY=1, GVM_WC_LOG=1 ...) or, for TICKS=N, given as --ticks N;
LOGS=min leaves out the logs the card draws or reads back for (ends, times, mailboxes), LOGS=time keeps the ends and times.
GVM_WATCH and RVC_WATCH (hex addresses) in the environment: the watched words' log too, as BASE.watch.
Each run writes OUTDIR/GAME.TAG.{log,err,frames,lines,cores,mbox[,ends,time,wc]} (TAG the config with ':' and '=' as
'_'). The logs: --frame-log; RVC_LINE_LOG (console lines with their pass), RVC_CORES_LOG (each core's instructions or
ops a pass), RVC_MBOX_LOG / GVM_MBOX_LOG (jobs, answers and taken words a pass), GVM_ENDS_LOG and GVM_TIME_LOG.
"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run as R


def one(out, game, conf, seconds, unity=None):
    parts = conf.split(':')
    machine, env = parts[0], dict(p.split('=', 1) for p in parts[1:])
    tag = conf.replace(':', '_').replace('=', '')
    base = os.path.join(out, f'{game}.{tag}')
    e = dict(os.environ)
    gvm = machine != 'rv'
    extra = ['--unity', unity] if unity else []
    if 'TICKS' in env:
        extra += ['--ticks', env.pop('TICKS')]
    logs = env.pop('LOGS', 'all')   # (LOGS=min: no log the card draws for: no ends, times, mailboxes; LOGS=time: ends and times only)
    full = logs == 'all'
    wc = env.pop('GVM_WC_LOG', None)
    if wc and gvm:
        e['GVM_WC_LOG'] = base + '.wc'
    e.update(env)
    e['RVC_LINE_LOG'] = base + '.lines'
    e['RVC_CORES_LOG'] = base + '.cores'
    if gvm and (full or logs == 'time'):
        e['GVM_ENDS_LOG'] = base + '.ends'
        e['GVM_TIME_LOG'] = base + '.time'
    if gvm and full:
        e['GVM_MBOX_LOG'] = base + '.mbox'
    if gvm and e.get('GVM_WATCH'):   # (GVM_WATCH / RVC_WATCH in the environment: their log beside the others)
        e['GVM_WATCH_LOG'] = base + '.watch'
    if not gvm and e.get('RVC_WATCH'):
        e['RVC_WATCH_LOG'] = base + '.watch'
    if not gvm:
        if full:
            e['RVC_MBOX_LOG'] = base + '.mbox'
        e.setdefault('RV_EXE', os.path.join(R.ROOT, 'bin', 'rvc_harness.exe'))
    e['GPU_QUEUE_WHO'] = 'profile_card (machine profile agent)'
    if not gvm:
        os.environ['RV_EXE'] = e['RV_EXE']
    snap = os.path.join(R.CKPT, 'rv' if not gvm else R.GVM_CKPT, game + '.snap')
    cmd, cwd = R.harness(machine, ['--load-state', snap, '--seconds', str(seconds), '--stats-after', '2',
                                   '--frame-log', base + '.frames'] + extra, base + '.log')
    if os.path.exists(base + '.log'):
        os.remove(base + '.log')
    t = time.time()
    p = subprocess.run(cmd, cwd=cwd, env=e, capture_output=True, text=True, errors='replace')
    open(base + '.err', 'w').write(p.stderr)
    print(f'{game} {conf}: exit {p.returncode}, {time.time() - t:.0f} s', flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('configs')
    ap.add_argument('--game', default='quake3')
    ap.add_argument('--seconds', type=int, default=15)
    ap.add_argument('--unity', help='F,U: run.py --unity (a Unity-like frame of F ms, U of them not rounds)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for c in a.configs.split(','):
        one(os.path.abspath(a.out), a.game, c, a.seconds, a.unity)


if __name__ == '__main__':
    main()
