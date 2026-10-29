"""The UAV tick against the current one on the card, with tools/rv_compare's parts and sections (each harness run under a
minute, through tools\\gpu_queue.ps1): the machines below, all one harness (bin\\rvc_harness.exe of this tree), one image
(build/images/nommu) and one shell snapshot (build/uav/shell.snap). Logs and rows go to build/uav/cmp (RVCMP_DATA),
never to tools/rv_compare/results.jsonl.

    python experiments/rvc_uav/compare.py run --machines base,uav8 --parts doom --rounds 1 [--sections N]
    python experiments/rvc_uav/compare.py report base uav8 uav16

Machines: base[K] (experiments/rvc_opt, the harness without --uav, --ticks K*1024); uav[K][g][f][pN] (experiments/rvc_uav,
--uav, --ticks K*1024; g: RVC_UAV_GLC=1, the RAM's UAV globallycoherent; f: UAV_MBOX_FENCE; pN: UAV_PITCH=N)."""
import os, re, sys
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
# Doom's timedemo with no drawing: the tics alone (the drawn timedemo holds at a list a host frame, 90 a second)
PARTS['doom-nodraw'] = dict(group='game', cmd='DOOM_HASH_TICS=100 doom -1 -timedemo demo1 -nodraw', frames=3000, until='timed ',
                            what='Doom: timedemo demo1 -nodraw (tics only)',
                            metrics={'timedemo tics a second': ('tics/s', 'high', ('rate', r'^doom: tic 100 ', r'^doom: tic \d+ state', 100))})

IMAGE = os.path.join(SE, 'build', 'images', 'nommu')
SHELL = os.path.join(SE, 'build', 'uav', 'shell.snap')


def machine(name):
    args, env = [], {}
    if name.startswith('uav'):
        m = re.fullmatch(r'uav(\d*)(g?)(f?)(?:p(\d+))?', name)
        if not m:
            raise SystemExit(f'{name}: uav[K][g][f][pN]')
        args = ['--rvc', 'experiments/rvc_uav', '--uav'] + (['--ticks', str(int(m[1]) * 1024)] if m[1] else [])
        if m[2]:
            env['RVC_UAV_GLC'] = '1'
        if m[3]:
            args += ['--define', 'UAV_MBOX_FENCE']
        if m[4]:
            args += ['--define', 'UAV_PITCH=' + m[4]]
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
    elif what == 'table':
        # every run's sections summed: guest instructions a second of the world's clock, rounds a frame, card ms a round
        import glob, json, re
        rows = {}
        for meta in sorted(glob.glob(os.path.join(DATA, 'logs', '*.meta'))):
            tag = os.path.basename(meta)[:-5]
            secs = json.load(open(meta))
            ins = fr = rnd = 0
            card = []
            for s in secs:
                for l in s.get('stats', []):
                    m = re.search(r'(\d+) frames in [\d.]+s, (\d+) guest instructions.*?, (\d+) commits', l)
                    if m:
                        fr += int(m[1]); ins += int(m[2]); rnd += int(m[3])
                    m = re.search(r'([\d.]+) ms on the card a frame', l)
                    if m:
                        card.append((float(m[1]), s['frames']))
            if not fr:
                continue
            cms = sum(c * f for c, f in card) / max(1, sum(f for c, f in card))
            res = [json.loads(l) for l in open(cmp.RESULTS)] if os.path.exists(cmp.RESULTS) else []
            mach, _, rest = tag.partition('_')
            part, _, r = rest.rpartition('_r')
            met = next((x.get('metrics') for x in res if x['machine'] == mach and x['part'] == part and str(x.get('round', 0)) == r), {})
            print(f"{tag:34} {ins / (fr * cmp.F_MS / 1000) / 1e6:6.2f} M instr/s  {rnd / fr:5.2f} rounds/frame  {cms:6.3f} card ms/frame  "
                  f"{cms * fr / rnd:6.3f} card ms/round  {ins / rnd:7.0f} instr/round  " +
                  json.dumps({k: v for k, v in (met or {}).items() if not k.startswith(('frame time', 'hitches'))}))
    elif what == 'report':
        install(sys.argv[2:])
        import report
        report.OUT = os.path.join(DATA, 'REPORT.md')
        report.HERE = DATA
        report.main(*sys.argv[2:4])
        print(open(report.OUT).read())
