"""The UAV tick against the current one on the card, with tools/rv_compare's parts and sections (each harness run under a
minute, through tools\\gpu_queue.ps1): the machines below, all one harness (bin\\rvc_harness.exe of this tree), one image
(build/images/nommu) and one shell snapshot (build/uav/shell.snap). Logs and rows go to build/uav/cmp (RVCMP_DATA),
never to tools/rv_compare/results.jsonl.

    python experiments/rvc_uav/compare.py run --machines base,uav8 --parts doom --rounds 1 [--sections N]
    python experiments/rvc_uav/compare.py report base uav8 uav16

Machines: base (experiments/rvc_opt, the harness without --uav); uavN (experiments/rvc_uav, --uav, --ticks N*1024);
a suffix g: RVC_UAV_GLC=1 (the RAM's UAV globallycoherent); f: UAV_MBOX_FENCE."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(SE, 'build', 'uav', 'cmp')
os.environ['RVCMP_DATA'] = DATA
os.environ.setdefault('RVCMP_BASE', SE)
sys.path.insert(0, os.path.join(SE, 'tools', 'rv_compare'))
import machines, cmp  # noqa: E402
from parts import PARTS  # noqa: E402

# the shell loop: core 0 alone in busybox's ash (instructions/s from the harness's IPS line too)
PARTS['shell-loop'] = dict(group='terminal', cmd='i=0; while [ $i -lt 10000 ]; do i=$((i+1)); done; echo LO""OPDONE', frames=2700,
                           until='LOOPDONE', what='ash: 10,000 turns of a while loop',
                           metrics={'10,000 turns': ('s', 'low', r'^LOOPDONE')})

IMAGE = os.path.join(SE, 'build', 'images', 'nommu')
SHELL = os.path.join(SE, 'build', 'uav', 'shell.snap')


def machine(name):
    args, env = [], {}
    if name.startswith('uav'):
        spec = name[3:]
        k = ''.join(c for c in spec if c.isdigit())
        args = ['--rvc', 'experiments/rvc_uav', '--uav'] + (['--ticks', str(int(k) * 1024)] if k else [])
        if 'g' in spec:
            env['RVC_UAV_GLC'] = '1'
        if 'f' in spec:
            args += ['--define', 'UAV_MBOX_FENCE']
    elif name.startswith('base'):
        k = ''.join(c for c in name[4:] if c.isdigit())
        args = ['--ticks', str(int(k) * 1024)] if k else []
    else:
        raise SystemExit(f'{name}: no such machine here')
    return dict(root=SE, exe=os.path.join('bin', 'rvc_harness.exe'), image=IMAGE, shell=SHELL, args=args, env=env)


def install(names):
    for n in names:
        m = machine(n)
        machines.MACHINES[n] = m
        cmp.MACHINES[n] = m


# the machine's own environment (RVC_UAV_GLC) rides with every section
_section = cmp.section
def section(machine_name, *a, **k):
    env = cmp.MACHINES[machine_name].get('env') or {}
    old = {key: os.environ.get(key) for key in env}
    os.environ.update(env)
    try:
        return _section(machine_name, *a, **k)
    finally:
        for key, v in old.items():
            if v is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = v
cmp.section = section
cmp.RESULTS = os.path.join(DATA, 'results.jsonl')
os.makedirs(DATA, exist_ok=True)

if __name__ == '__main__':
    what = sys.argv[1]
    if what == 'run':
        ms = sys.argv[sys.argv.index('--machines') + 1].split(',')
        install(ms)
        import run, report
        report.main = lambda *a, **k: None   # (its REPORT.md is the tree's: `compare.py report` writes ours)
        sys.argv = ['run.py'] + sys.argv[2:]
        run.main()
    elif what == 'report':
        install(sys.argv[2:])
        import report
        report.OUT = os.path.join(DATA, 'REPORT.md')
        report.HERE = DATA
        report.main(*sys.argv[2:4])
        print(open(report.OUT).read())
