"""Pictures of Tiberian Dawn's missions held at a frame, in 2D and in the tilted view (docs/games/tdawn.md, "In 3D"),
under rvc_harness --cpu --warp through the queue (no card time): the GPU's lists as drawn, from a snapshot at the end.

    python tools/td3d_shots.py --image build/images/NAME --out DIR [--scen 1,3,10] [--views 2d,3d,3d-heights,3d-sprites]
            [--frame 300] [--pitch 60] [--harness EXE] [--sleep S] [--env NAME=VALUE ...]

Writes DIR/SCEN-VIEW.png (the display) and .log (the console) for each.
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VIEWS = {
    '2d': '',
    '3d': 'TDAWN_VIEW=3d',
    '3d-heights': 'TDAWN_VIEW=3d TDAWN_HEIGHTS=1',
    '3d-sprites': 'TDAWN_VIEW=3d TDAWN_MODELS=0',
}


def shot(a, scen, view, out):
    env = VIEWS[view] + (f' TDAWN_PITCH={a.pitch}' if a.pitch and view != '2d' else '') + ''.join(' ' + e for e in a.env)
    cmd = (f"nano-X -p & sleep 2; TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_STATS_MS=0 {env} TDAWN_SCEN={scen} "
           f"TDAWN_STOP_FRAME={a.frame} tdawn FROMINSTALL & sleep {a.sleep}; echo LX-''DONE\n")
    snap = out + '.snap'
    run = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), a.harness, '--cpu', '--warp', '--image', a.image,
           '--no-viz', '--no-stdin', '--seconds', str(a.sleep * 3 + 90), '--uart-log', out + '.log', '--expect', '/ # ', '--send', cmd,
           '--until', 'LX-DONE', '--save-state', snap]
    subprocess.run(run, cwd=ROOT, capture_output=True, env=dict(os.environ, RVC_THREADS=os.environ.get('RVC_THREADS', '8')))
    if not os.path.exists(snap):
        print(f'{scen} {view}: no snapshot (see {out}.log)')
        return False
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'snapshot_screen.py'), snap, out + '.png'], check=True, capture_output=True)
    os.remove(snap)
    held = [l.strip() for l in open(out + '.log', encoding='latin-1') if 'stopped at frame' in l or 'pick check' in l or 'no models' in l]
    print(f'{scen} {view}: {out}.png', *held, sep='\n    ')
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--harness', default=os.path.join(ROOT, 'bin', 'rvc_harness.exe'))
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--scen', default='1,3,10')
    ap.add_argument('--views', default='2d,3d')
    ap.add_argument('--frame', type=int, default=300)
    ap.add_argument('--pitch', type=int, default=0)
    ap.add_argument('--sleep', type=int, default=60)
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game too (TDAWN_DRAG=20 moves the view)')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    bad = 0
    for scen in a.scen.split(','):
        for view in a.views.split(','):
            bad += not shot(a, int(scen), view, os.path.join(a.out, f'{int(scen):02d}-{view}'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
