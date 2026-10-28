"""The comparison's parts (parts.py) profiled on rvc_cpu: where core 0's instructions go, what fills the write cache and
what could be a worker's (docs/plans/riscv-profile.md). A run boots the image, starts `nano-X -p`, types the part's
command after `echo FL-GO` and stops at the part's `until` (or LX-DONE after the command); the profile is of the window
from FROM (the part's `pace` line, or FL-GO) on, every call under a minute (--wall).

    python tools/rv_compare/prof_parts.py PART [--mode prof|calls|plain] [--from TEXT] [--after S] [--wall S]
        prof:  RVC_PROF with RVC_WCACHE and RVC_SOUND (the card's write cache: passes ended full, and where)
        calls: RVC_CALLS (tools/mc_candidates.py)
        plain: RVC_WCACHE and RVC_SOUND alone: the counts of the window
    --after S: the window begins S seconds of the guest's after the command starts (`sleep S; echo FL-AT` beside it)
    --span S:  and ends S seconds after that (else at the part's end or the wall)

Before --count-from's line RVC_PROF runs as a plain run (rvc_cpu, 28 October 2026), so a window late in a long
program costs only its own profiled instructions. Output in OUTDIR/PART.MODE (the profile's folder, the console, the
counts)."""
import argparse, codecs, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from parts import PARTS  # noqa: E402

IMAGE = r'E:\Development\Emu\ShaderEmu-rvbp\build\before\build\images\nommu'

# The window profiled where the part's own (its pace line to its until, or the command to its until) is not the one
# its metrics measure: the program's steady state, or a part with no end line (which would run on, idle, to the wall)
WINDOWS = {
    'doom-attract': dict(after=5, span=12),
    'quake3-server': dict(frm="quake3: the level's first frame"),
    'quake3-menu': dict(after=30, span=20),
    'tdawn-menu': dict(after=0, span=40),
    'openttd': dict(frm='play PL-GO'),
    'openttd-title': dict(after=40, span=20),
    'video-mpg': dict(after=3, span=20),
    'nxedit': dict(after=0, span=20),
    'nxfiles': dict(after=0, span=20),
}


def guest_text(cmd):
    """the part's command as the harness's --input has it (\\n, \\xNN) made plain, then escaped for rvc_cpu's --send"""
    plain = codecs.decode(cmd, 'unicode_escape')
    return plain.replace('\\', '\\\\').replace('\n', '\\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('part')
    ap.add_argument('--mode', default='prof', choices=('prof', 'calls', 'plain'))
    ap.add_argument('--from', dest='frm')
    ap.add_argument('--after', type=float)
    ap.add_argument('--span', type=float)
    ap.add_argument('--until')
    ap.add_argument('--wall', type=float, default=50)
    ap.add_argument('--out', default=os.path.join(SE, 'build', 'prof_parts'))
    ap.add_argument('--image', default=IMAGE)
    ap.add_argument('--exe', default=os.path.join(SE, 'bin', 'rvc_cpu.exe'))
    ap.add_argument('--tag', default='')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE for rvc_cpu')
    ap.add_argument('--cmd', help='the command instead of the part\'s')
    a = ap.parse_args()
    P = PARTS.get(a.part, {})
    cmd = a.cmd or P['cmd']
    name = f'{a.part}{a.tag}.{a.mode}'
    out = os.path.join(a.out, name)
    os.makedirs(out, exist_ok=True)
    W = {} if a.cmd or a.frm or a.after is not None else WINDOWS.get(a.part, {})
    if a.after is None and 'after' in W:
        a.after, a.span = W['after'], W.get('span')
    until = a.until or P.get('until') or 'LX-DONE'
    frm = a.frm or W.get('frm') or (P.get('pace') if P.get('pace') != 'steady' else None) or 'FL-GO'
    body = cmd
    if a.after is not None:
        tail = f"; sleep {a.span}; echo LX-''AT" if a.span else ''
        body = f"( {cmd} ) & sleep {a.after}; echo FL-''AT{tail}"
        frm = 'FL-AT'
        if a.span:
            until = 'LX-AT'
    text = guest_text("nano-X -p & sleep 2; echo FL-''GO; " + body + "; echo LX-''DONE\\n")
    env = dict(os.environ, RVC_SOUND='1')
    if a.mode in ('prof', 'plain'):
        env['RVC_WCACHE'] = '1'
    if a.mode == 'prof':
        env['RVC_PROF'] = out
    if a.mode == 'calls':
        env['RVC_CALLS'] = os.path.join(out, 'calls.txt')
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    args = [a.exe, '--image', a.image, '--quiet', '--uart-log', os.path.join(out, 'uart.log'), '--expect', '/ # ', '--send', text,
            *[x for e, t in P.get('sends', ()) for x in ('--expect', e, '--send', t)],
            '--count-from', frm, '--until', until, '--wall', str(a.wall)]
    t = time.time()
    try:
        r = subprocess.run(args, env=env, capture_output=True, text=True, errors='replace', timeout=a.wall + 8)
        err, code = r.stderr, r.returncode
    except subprocess.TimeoutExpired as e:
        err, code = (e.stderr or b'').decode('latin-1') if isinstance(e.stderr, bytes) else (e.stderr or ''), -1
    wall = time.time() - t
    open(os.path.join(out, 'stderr.txt'), 'w').write(err)
    keep = [l for l in err.splitlines() if l.startswith(('COUNT', '[cpu] write cache', '[prof]', '[calls]')) or 'until' in l]
    print(f'{name}: {wall:.1f} s, exit {code}, from "{frm}" to "{until}"')
    for l in keep:
        print('  ' + l)
    return 0


if __name__ == '__main__':
    sys.exit(main())
