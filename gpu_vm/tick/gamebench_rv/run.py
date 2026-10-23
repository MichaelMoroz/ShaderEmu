"""The game bench on the RISC-V machine (GAMEBENCH.md): boots build/images/gamebench_rv (build.sh, then image.py) on the
main checkout's rvc_harness with experiments/rvc_opt (the card) or rvc_cpu (--cpu), runs `gamebench NAME` for each
case and prints frames a second (the mean of its three lines). Through tools/gpu_queue.ps1.

    python gpu_vm/tick/gamebench_rv/run.py [--cpu] [--cases g_logic,g_verts,g_ray1,g_q3,g_hl,g_ray] [--seconds 3]
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
RV_ROOT = os.environ.get('RV_ROOT', os.path.join(os.path.dirname(ROOT), 'ShaderEmu'))
IMAGE = os.path.join(ROOT, 'build', 'images', 'gamebench_rv')
CASES = 'g_logic,g_verts,g_ray1,g_q3,g_hl,g_ray'


def folder():
    """A folder of junctions to the main checkout whose build/images/linux and nommu are the image."""
    work = os.path.join(ROOT, 'build', 'gamebench', 'rvroot')
    links = [('experiments', os.path.join(RV_ROOT, 'experiments')), ('harness', os.path.join(RV_ROOT, 'harness')),
             (os.path.join('build', 'shadercache'), os.path.join(RV_ROOT, 'build', 'shadercache')),
             (os.path.join('build', 'images', 'linux'), IMAGE), (os.path.join('build', 'images', 'nommu'), IMAGE)]
    os.makedirs(os.path.join(work, 'build', 'images'), exist_ok=True)
    for link, target in links:
        if not os.path.exists(os.path.join(work, link)):
            subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(work, link), target], capture_output=True, check=True)
    return work


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpu', action='store_true')
    ap.add_argument('--cases', default=CASES)
    ap.add_argument('--seconds', default='3')
    ap.add_argument('--log', default=os.path.join(ROOT, 'build', 'gamebench', 'rv.log'))
    a = ap.parse_args()
    send = '; '.join(f'gamebench {c} {a.seconds}' for c in a.cases.split(',')) + "; echo GB-D''ONE\\n"
    if os.path.exists(a.log):
        os.remove(a.log)
    q = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1')]
    if a.cpu:
        cmd = q + [os.path.join(RV_ROOT, 'bin', 'rvc_cpu.exe'), '--image', 'linux-net', '--quiet', '--uart-log', a.log,
                   '--expect', '/ # ', '--send', send, '--until', 'GB-DONE', '--wall', '3000']
    else:
        cmd = q + [os.path.join(RV_ROOT, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', 'experiments/rvc_opt', '--image', 'linux-net',
                   '--no-desktop', '--no-stdin', '--uart-log', a.log, '--expect', '/ # ', '--send', send, '--until', 'GB-DONE',
                   '--seconds', '1200']
    os.environ.setdefault('GPU_QUEUE_WHO', 'gamebench_rv')
    p = subprocess.run(cmd, cwd=folder(), capture_output=True, text=True, errors='replace')
    log = open(a.log, encoding='latin-1').read() if os.path.exists(a.log) else ''
    if p.returncode:
        print(f'exit {p.returncode}', p.stderr[-1500:])
    for c in a.cases.split(','):
        v = [float(x) for x in re.findall(rf'gamebench: {c} [\d.]+ frames in [\d.]+ s: ([\d.]+) a second', log)]
        other = re.findall(rf'gamebench: (\d+ workers of \d+)', log)
        print(f'{c:10s} {sum(v) / len(v) if v else 0:10.3f} frames a second  ({" ".join(f"{x:.3f}" for x in v)})')
    for o in sorted(set(re.findall(r'gamebench: \d+ workers of \d+', log))):
        print(o)


if __name__ == '__main__':
    main()
