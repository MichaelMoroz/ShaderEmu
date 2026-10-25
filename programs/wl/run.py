"""wl on both machines' GPU harnesses (programs/wl/README.md): boots each machine's image with wl, types the commands,
keeps the console, and collects the WL lines into one JSON per run.

    python programs/wl/run.py OUT --machine rv,c8_v0001 [--cmd "wl -s 3 all" ...] [--rounds 1] [--limit 900]

A machine is `rv` (the RISC-V machine's rvc_opt on build/images/nommu_wl) or a gpu_vm tick as gpu_vm/bench_games/run.py
names them (a step of gpu_vm/tick/steps, or BATCH_vNNNN of build/tick_gen), on build/wl/img; GVM_EXTRA adds harness
options (--ticks 8192). Every run goes through tools/gpu_queue.ps1 (run.py's harness()).
"""
import argparse, json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ.setdefault('GVM_IMAGE', os.path.join(ROOT, 'build', 'wl', 'img'))
os.environ.setdefault('RV_IMAGE', os.path.join(ROOT, 'build', 'images', 'nommu_wl'))
sys.path.insert(0, os.path.join(ROOT, 'gpu_vm', 'bench_games'))
import run as games   # (its harness(): the command line and folder of a machine)


def one(machine, cmds, log, limit):
    prompt = '/ # ' if machine == 'rv' else '@gpuvm:~# '
    pre = '' if machine == 'rv' else 'cd /; export TERM=linux; '
    line = pre + '; '.join(cmds) + '; echo WL-ALL-""DONE'
    cmd, cwd = games.harness(machine, ['--seconds', str(limit), '--expect', prompt, '--send', line + '\\n',
                                       '--until', 'WL-ALL-DONE'], log)
    t = time.time()
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
    text = open(log, errors='replace').read() if os.path.exists(log) else ''
    rows = []
    for m in re.finditer(r'WL (\S+) (\S+) (\d+) ([\d.]+) ([\d.]+) ([0-9a-f]{8})', text, re.M):
        rows.append(dict(name=m[1], mode=m[2], units=int(m[3]), seconds=float(m[4]), rate=float(m[5]), sum=m[6]))
    notes = re.findall(r'WL .*(?:skipped|gave|have a core).*$', text, re.M)
    print(f'{machine}: {len(rows)} results in {time.time() - t:.0f} s (exit {p.returncode})' + (f'; {len(notes)} notes' if notes else ''))
    return rows, notes


def main():
    a = argparse.ArgumentParser()
    a.add_argument('out')
    a.add_argument('--machine', default='rv,c8_v0001')
    a.add_argument('--cmd', action='append')
    a.add_argument('--rounds', type=int, default=1)
    a.add_argument('--limit', type=int, default=900)
    a = a.parse_args()
    cmds = a.cmd or ['wl -s 3 all']
    os.makedirs(a.out, exist_ok=True)
    for r in range(a.rounds):
        for m in a.machine.split(','):
            log = os.path.abspath(os.path.join(a.out, f'{m}.{r}.log'))
            rows, notes = one(m, cmds, log, a.limit)
            json.dump(dict(machine=m, round=r, cmds=cmds, extra=os.environ.get('GVM_EXTRA', ''), rows=rows, notes=notes),
                      open(os.path.join(a.out, f'{m}.{r}.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
