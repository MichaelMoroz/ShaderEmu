"""The WARP stamp in about a minute: the tick and its shaders run on WARP from a few moments of a busy machine.

    python tools/warp_stamp.py [--rvc shader_vm] [--image build/images/nommu] [--count 3] [--secs 10] [--cmd "..."]

The card runs a processor folder only when its warp_ok holds the folder's stamp (its sources hashed, `rvc_harness
--stamp`): what the card would draw must have run in software first. This makes the snapshots itself, so they are of
the image and tick as they are now: the image booted on the processor (`rvc_harness --cpu`, seconds), a workload of
worker cores and GPU draws typed (`--cmd`; mctest's cores and glxgears' draws by default), and the machine saved at
`--count` moments chosen at random (each a run of its own from boot, 3 to 8 s of the guest's in, each waiting its turn in
the queue). Each snapshot then runs `--secs` on WARP, one after another, the tick's and the GPU's shaders as the card has them. A run passes when the harness ends of
itself at its time, the guest went on (instructions run, and frames), and its console says nothing of a panic, an
oops, a program's fault or a refusal. All passed: the stamp goes into warp_ok, as `tools/test_card.py --warp --mark-ok`
writes it after whole cases (which is still the way to check what a case says, at WARP's 300 thousand instructions a
second). Every run goes through tools\\gpu_queue.ps1.
"""
import argparse, os, random, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAD = re.compile(r'Kernel panic|Oops|unhandled signal|BUG:|segfault|REFUSED|\[lock\].*refused|device removed|TDR', re.I)


def harness(a, *more):
    return ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), a.harness, '--d3d11', '--rvc', a.rvc,
            '--define', 'NO_PAGING', '--image', os.path.abspath(a.image), '--no-desktop', '--no-viz', '--no-stdin'] + list(more)


def run(args, wait=60):
    for _ in range(wait):   # (the harness lock refuses a GPU harness beside CPU ones: wait for them)
        p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            return p
        time.sleep(10)
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rvc', default='shader_vm', help='the processor folder stamped')
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'images', 'nommu'))
    ap.add_argument('--harness', default=os.path.join(ROOT, 'bin', 'rvc_harness.exe'))
    ap.add_argument('--count', type=int, default=3, help='snapshots, each at a moment chosen at random')
    ap.add_argument('--secs', type=float, default=6, help='seconds each runs on WARP')
    ap.add_argument('--cmd', default='nano-X -p & sleep 1; glxgears & mctest 3', help='what the machine does')
    ap.add_argument('--seed', type=int, help='the moments (default: the clock)')
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'warp_stamp'))
    ap.add_argument('--no-mark', action='store_true', help='run, but do not write the stamp')
    a = ap.parse_args()
    os.makedirs(a.logs, exist_ok=True)
    rng = random.Random(a.seed if a.seed is not None else time.time_ns())
    t0, failed = time.time(), []
    moments = [round(rng.uniform(3, 8), 1) for _ in range(a.count)]
    made = []
    for k, at in enumerate(moments):   # (started together: they take their turns in tools\gpu_queue.ps1)
        snap, log = os.path.join(a.logs, f'{k}.snap'), os.path.join(a.logs, f'{k}.log')
        for f in (snap, log):
            if os.path.exists(f):
                os.remove(f)
        made.append(subprocess.Popen(harness(a, '--cpu', '--uart-log', log + '.cpu', '--input', a.cmd + '\\n', '--seconds', str(at),
                                             '--save-state', snap), cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                     text=True, errors='replace'))
    errs = [m.communicate()[1] for m in made]
    for k, at in enumerate(moments):
        snap, log = os.path.join(a.logs, f'{k}.snap'), os.path.join(a.logs, f'{k}.log')
        if not os.path.exists(snap):
            failed.append(k)
            print(f'{k}: no snapshot at {at} s on the processor: ' + ' '.join(errs[k].strip().splitlines()[-2:]), flush=True)
            continue
        p = run(harness(a, '--warp', '--load-state', snap, '--uart-log', log, '--seconds', f'{a.secs:.0f}'))
        m = re.search(r'\[harness\] (\d+) frames in [\d.]+s, (\d+) guest instructions', p.stderr)
        console = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
        bad = BAD.search(console + p.stderr)
        why = ('exit %d' % p.returncode if p.returncode else '') or ('no frames' if not m or int(m[1]) == 0 else '') or \
              ('the guest stood still' if int(m[2]) < 100000 else '') or (f'"{bad[0]}"' if bad else '')
        print(f"{'PASS' if not why else 'FAIL'} {k}: from {at} s of `{a.cmd}`, {a.secs:.0f} s on WARP" +
              (f': {m[1]} frames, {int(m[2]):,} instructions of core 0' if m else '') + (f'  ({why})' if why else ''), flush=True)
        if why:
            failed.append(k)
        else:
            os.remove(snap)
    if failed or a.no_mark:
        print(f"{a.count - len(failed)} of {a.count} passed in {time.time() - t0:.0f} s" + ('; no stamp written' if failed else '') +
              f' (consoles in {a.logs}' + (", and the failed runs' snapshots" if failed else '') + ')')
        return 1 if failed else 0
    p = subprocess.run([a.harness, '--rvc', a.rvc, '--stamp'], cwd=ROOT, capture_output=True, text=True)
    stamp = re.search(r'stamp ([0-9a-f]{16})', p.stdout)
    if not stamp:
        raise SystemExit('rvc_harness --stamp said no stamp: ' + p.stdout + p.stderr)
    with open(os.path.join(ROOT, a.rvc, 'warp_ok'), 'a') as f:
        f.write(f"{stamp[1]}  {time.strftime('%Y-%m-%d')}  WARP ran {a.count} snapshots of `{a.cmd}` {a.secs:.0f} s each (tools/warp_stamp.py)\n")
    print(f'{a.count} of {a.count} passed in {time.time() - t0:.0f} s; {a.rvc}: stamp {stamp[1]} written to its warp_ok', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
