"""wl on the card's harness (programs/wl/README.md): boots the image with wl, types the commands, keeps the console, and
collects the WL lines into one JSON per round.

    python programs/wl/run.py OUT [--cmd "wl -s 3 all" ...] [--rounds 1] [--limit 900]

The image is RV_IMAGE (build/images/nommu_wl), the harness bin/rvc_harness.exe with experiments/rvc_opt (RV_EXE: another
harness); RV_EXTRA adds harness options (--ticks 8192). Every run goes through tools/gpu_queue.ps1.
"""
import argparse, json, os, re, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RV_IMAGE = os.environ.get('RV_IMAGE', os.path.join(ROOT, 'build', 'images', 'nommu_wl'))


def harness(extra, log):
    """The command line of one run (the no-MMU Linux runs without paging)."""
    return ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'),
            os.environ.get('RV_EXE', os.path.join(ROOT, 'bin', 'rvc_harness.exe')), '--d3d11', '--rvc', 'experiments/rvc_opt',
            '--define', 'NO_PAGING', '--image', RV_IMAGE, '--no-desktop', '--no-stdin', '--uart-log', log] + \
        os.environ.get('RV_EXTRA', '').split() + extra


def one(cmds, log, limit):
    line = '; '.join(cmds) + '; echo WL-ALL-""DONE'
    cmd = harness(['--seconds', str(limit), '--expect', '/ # ', '--send', line + '\\n', '--until', 'WL-ALL-DONE'], log)
    t = time.time()
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace')
    text = open(log, errors='replace').read() if os.path.exists(log) else ''
    rows = []
    for m in re.finditer(r'WL (\S+) (\S+) (\d+) ([\d.]+) ([\d.]+) ([0-9a-f]{8})', text, re.M):
        rows.append(dict(name=m[1], mode=m[2], units=int(m[3]), seconds=float(m[4]), rate=float(m[5]), sum=m[6]))
    notes = re.findall(r'WL .*(?:skipped|gave|have a core).*$', text, re.M)
    print(f'{len(rows)} results in {time.time() - t:.0f} s (exit {p.returncode})' + (f'; {len(notes)} notes' if notes else ''))
    return rows, notes


def main():
    a = argparse.ArgumentParser()
    a.add_argument('out')
    a.add_argument('--cmd', action='append')
    a.add_argument('--rounds', type=int, default=1)
    a.add_argument('--limit', type=int, default=900)
    a = a.parse_args()
    cmds = a.cmd or ['wl -s 3 all']
    os.makedirs(a.out, exist_ok=True)
    for r in range(a.rounds):
        log = os.path.abspath(os.path.join(a.out, f'rv.{r}.log'))
        rows, notes = one(cmds, log, a.limit)
        json.dump(dict(machine='rv', round=r, cmds=cmds, extra=os.environ.get('RV_EXTRA', ''), rows=rows, notes=notes),
                  open(os.path.join(a.out, f'rv.{r}.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
