"""The write cache's size against what a pass does (GPU-VM's compare/wcsize.py, 28 October 2026): does a bigger cache
give core 0 more of a pass, and a game more frames where a round costs a large fixed time (a VR frame, few rounds)?

    python tools/wcsize.py model [--bits 5,6,7,8]                    # rvc_cpu, RVC_WCACHE=1 RVC_WC_BITS=N
    python tools/wcsize.py card [--bits 6,7] [--unity 11.1,7.1,1.0]  # rvc_harness, --define L1_TABLE_BITS=N

The load is Doom's timedemo (demo1, 5,026 tics). `model` runs it on the processor with core 0's write cache at 2^N
buckets (and half and a quarter, three texels a bucket: 7 x 2^N x 3 / 4 texels) and prints the passes core 0 ended
full and the demo's frames a second on the guest's clock. `card` runs it from tools/test_card.py's shell snapshot with
the tick built at L1_TABLE_BITS=N, through tools\\gpu_queue.ps1, under a world's frame (--unity F,U,R: `11.1,1.4,0.76`
the comparison's, `11.1,7.1,1.0` a VR frame with some 4 ms left for the machine), and prints the demo's frames a
second and the harness's STATS unity line. Every run is under a minute; the sizes run in turn."""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOOM = "nano-X -p & sleep 2; DOOM_HASH_TICS=100 doom -1 -timedemo demo1; echo LX-''DONE"
TIMED = r'timed (\d+) gametics in (\d+) realtics'


def fps(text):
    m = re.search(TIMED, text)
    return (int(m[1]) * 35 / int(m[2]), int(m[2])) if m else (None, None)


def locked(args, env=None):
    for _ in range(60):   # (the harness lock: CPU runs together, a GPU run alone)
        p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=58)
        if 'refused' not in p.stderr:
            return p
        time.sleep(15)
    return p


def model(bits):
    log = os.path.join(ROOT, 'build', 'wcsize', f'model{bits}.log')
    env = dict(os.environ, RVC_WCACHE='1', RVC_WC_BITS=str(bits), RVC_THREADS='4', MSYS_NO_PATHCONV='1')
    p = locked([os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--quiet', '--uart-log', log, '--wall', '55', '--image',
                os.path.join(ROOT, 'build', 'images', 'nommu'), '--expect', '/ # ', '--send', DOOM + '\\n', '--until',
                'LX-DONE'], env)
    full = re.search(r'write cache: (\d+) passes of core 0 and (\d+) of workers ended full', p.stderr)
    passes = re.search(r'(\d+) passes in', p.stderr)
    f, real = fps(open(log, encoding='latin-1').read() if os.path.exists(log) else '')
    texels = 7 * (1 << bits) * 3 // 4
    print(f'| {bits} | {texels} | {full[1] if full else "-"} | {full[2] if full else "-"} | {passes[1] if passes else "-"} | '
          f'{real or "-"} | {f"{f:.1f}" if f else "-"} |', flush=True)


def card(bits, unity):
    snap = os.path.join(ROOT, 'build', 'test_card', 'shell.snap')
    if not os.path.exists(snap):
        sys.exit(f'{snap}: make it with python tools/test_card.py doom-3 first')
    log = os.path.join(ROOT, 'build', 'wcsize', f'card{bits}.log')
    p = locked(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'rvc_harness.exe'),
                '--d3d11', '--rvc', 'experiments/rvc_opt', '--define', 'NO_PAGING', '--define', f'L1_TABLE_BITS={bits}',
                '--image', os.path.join(ROOT, 'build', 'images', 'nommu'), '--no-desktop', '--no-viz', '--no-stdin',
                '--load-state', snap, '--uart-log', log, '--seconds', '52', '--stats-after', '0', '--unity', unity,
                '--input', DOOM + '\\n', '--until', 'LX-DONE'])
    f, real = fps(open(log, encoding='latin-1').read() if os.path.exists(log) else '')
    stats = re.search(r'STATS unity: (.*)', p.stderr)
    print(f'| {bits} | {unity} | {real or "-"} | {f"{f:.1f}" if f else "-"} | {stats[1] if stats else p.stderr.strip()[-200:]} |', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=('model', 'card'))
    ap.add_argument('--bits', default='')
    ap.add_argument('--unity', default='11.1,7.1,1.0')
    a = ap.parse_args()
    os.makedirs(os.path.join(ROOT, 'build', 'wcsize'), exist_ok=True)
    if a.what == 'model':
        print('| bits | texels (core 0) | passes core 0 ended full | passes workers ended full | passes | realtics | fps (guest) |')
        print('|---|---|---|---|---|---|---|')
        for b in map(int, (a.bits or '5,6,7,8').split(',')):
            model(b)
    else:
        print('| bits | --unity (ms) | realtics | fps (guest) | the harness |')
        print('|---|---|---|---|---|')
        for b in map(int, (a.bits or '6,7').split(',')):
            card(b, a.unity)


if __name__ == '__main__':
    main()
