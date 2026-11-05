"""The WARP stamp in seconds: the tick and its shaders run on WARP from a moment of a busy machine.

    python tools/warp_stamp.py [--rvc shader_vm] [--image build/images/nommu] [--secs 3] [--count 1] [--fresh] [--cmd "..."]

The card runs a processor folder only when its warp_ok holds the folder's stamp: its sources hashed (`rvc_harness
--stamp`), or its code, every pass's compiled bytecode hashed (`rvc_harness --code-stamp`): what the card would draw must
have run in software first, and a change that compiles to the same bytes (a rename, a comment, a trim) is the same code.
WARP is a software device, so this guards the card against a shader that would hang it (a timeout, "device removed"),
nothing more: the tick runs and the devices run.

1. The code's stamp (compiled from the shader cache, under a second, no card and no queue): already in warp_ok, nothing
   runs.
2. Else a snapshot of a busy machine (`--cmd`: mctest's worker cores and glxgears' GPU draws), made once on the processor
   and kept in build/warp_stamp/stored.snap for that image (made again when the image changes, with --fresh, or when the
   harness refuses it, as after a layout change).
3. It runs `--secs` on WARP (`--count` moments of it, a snapshot each with --fresh). A run passes when the harness ends of
   itself at its time, the guest went on (instructions run, and frames), and its console says nothing of a panic, an oops, a
   program's fault or a refusal. All passed: the sources' stamp and the code's go into warp_ok. Every run goes through
   tools\\gpu_queue.ps1; a WARP run is on the processor's side of the harness lock.
"""
import argparse, os, random, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAD = re.compile(r'Kernel panic|Oops|unhandled signal|BUG:|segfault|REFUSED|\[lock\].*refused|device removed|TDR', re.I)


def harness(a, *more, queue=True):
    run = [a.harness, '--d3d11', '--rvc', a.rvc, '--define', 'NO_PAGING', '--image', os.path.abspath(a.image),
           '--no-desktop', '--no-viz', '--no-stdin'] + list(more)
    return (['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1')] if queue else []) + run


def run(args, wait=120):
    for _ in range(wait):   # (the harness lock refuses a run beside one of the other side: wait for it)
        p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            return p
        time.sleep(5)
    return p


def stamp_of(a, flag):
    p = subprocess.run(harness(a, '--warp', flag, queue=False) if flag == '--code-stamp' else [a.harness, '--rvc', a.rvc, flag],
                       cwd=ROOT, capture_output=True, text=True, errors='replace')
    m = re.search(r'stamp (c?[0-9a-f]{16})', p.stdout)
    if not m:
        raise SystemExit(f'rvc_harness {flag} said no stamp: ' + (p.stdout + p.stderr)[-600:])
    return m[1]


def image_key(image):
    """What a kept snapshot is of: the image's kernel and ROM, by size and time."""
    parts = []
    for n in ('linux_payload.bin', 'rootfs.bin'):
        s = os.stat(os.path.join(image, n))
        parts.append(f'{n} {s.st_size} {int(s.st_mtime)}')
    return '; '.join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rvc', default='shader_vm', help='the processor folder stamped')
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'images', 'nommu'))
    ap.add_argument('--harness', default=os.path.join(ROOT, 'bin', 'rvc_harness.exe'))
    ap.add_argument('--count', type=int, default=1, help='moments run (more than one: a fresh snapshot each)')
    ap.add_argument('--secs', type=float, default=3, help='seconds each runs on WARP')
    ap.add_argument('--fresh', action='store_true', help='make the snapshot again, at a moment chosen at random')
    ap.add_argument('--cmd', default='nano-X -p & sleep 1; glxgears & mctest 3', help='what the machine does')
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'warp_stamp'))
    ap.add_argument('--no-mark', action='store_true', help='run, but do not write the stamp')
    a = ap.parse_args()
    os.makedirs(a.logs, exist_ok=True)
    t0 = time.time()
    ok_path = os.path.join(ROOT, a.rvc, 'warp_ok')
    ok = open(ok_path).read() if os.path.exists(ok_path) else ''
    source, code = stamp_of(a, '--stamp'), stamp_of(a, '--code-stamp')
    if code in ok and not a.no_mark:
        print(f'{a.rvc}: its code {code} is stamped already (sources {source}): nothing run, {time.time() - t0:.1f} s')
        return 0
    stored = os.path.join(a.logs, 'stored.snap')
    key_file = os.path.join(a.logs, 'stored.txt')
    key = image_key(a.image) + ' | ' + a.cmd
    fresh = a.fresh or a.count > 1 or not os.path.exists(stored) or not os.path.exists(key_file) or open(key_file).read() != key
    failed = 0
    for k in range(a.count):
        snap = stored if a.count == 1 else os.path.join(a.logs, f'{k}.snap')
        if fresh or k > 0:
            at = round(random.Random(time.time_ns()).uniform(3, 6), 1) if a.fresh or a.count > 1 else 5.0
            p = run(harness(a, '--cpu', '--input', a.cmd + '\\n', '--seconds', str(at), '--save-state', snap))
            if not os.path.exists(snap):
                print(f'{k}: no snapshot at {at} s on the processor: ' + ' '.join(p.stderr.strip().splitlines()[-2:]))
                return 1
            if a.count == 1:
                open(key_file, 'w').write(key)
        log = os.path.join(a.logs, f'{k}.log')
        if os.path.exists(log):
            os.remove(log)
        p = run(harness(a, '--warp', '--load-state', snap, '--uart-log', log, '--seconds', f'{a.secs:g}'))
        if 'snapshot' in p.stderr and 'refused' in p.stderr and not fresh:
            # (a snapshot of a layout before: made again once)
            os.remove(stored)
            return main()
        m = re.search(r'\[harness\] (\d+) frames in [\d.]+s, (\d+) guest instructions', p.stderr)
        console = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
        bad = BAD.search(console + p.stderr)
        why = ('exit %d' % p.returncode if p.returncode else '') or ('no frames' if not m or int(m[1]) == 0 else '') or \
              ('the guest stood still' if int(m[2]) < 100000 else '') or (f'"{bad[0]}"' if bad else '')
        print(f"{'PASS' if not why else 'FAIL'} {k}: {a.secs:g} s on WARP from a snapshot of `{a.cmd}`" +
              (f': {m[1]} frames, {int(m[2]):,} instructions of core 0' if m else '') + (f'  ({why})' if why else ''), flush=True)
        failed += bool(why)
    if failed or a.no_mark:
        print(f'{a.count - failed} of {a.count} passed in {time.time() - t0:.0f} s' + ('; no stamp written' if failed else ''))
        return 1 if failed else 0
    with open(ok_path, 'a', newline='\n') as f:
        f.write(f"{source}  {code}  {time.strftime('%Y-%m-%d')}  WARP ran {a.count} snapshot of `{a.cmd}` {a.secs:g} s (tools/warp_stamp.py)\n")
    print(f'{a.count} of {a.count} passed in {time.time() - t0:.0f} s; {a.rvc}: stamps {source} (sources) and {code} (code) written to its warp_ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
