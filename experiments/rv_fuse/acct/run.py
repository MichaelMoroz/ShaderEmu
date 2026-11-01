#!/usr/bin/env python3
"""One card run for the account of a pass: a tick, a snapshot's load, its code file, flags; through the queue; what it says and what the guest said, a line in acct.jsonl.
    python run.py NAME --load doom --tick rvc_opt_w3x --code doom.3.2.code [--ticks N] [--frames N] [--env K=V ...] [--more "--flag x"]
Prints a line: ms a frame, instructions a frame, ns an instruction, whether the guest went on."""
import argparse, json, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SNAP = os.environ.get('AGENTS_ISA', r'C:\Users\micha\agents_isa')
def run(name, load='doom', tick='rvc_lean', code='', ticks=0, frames=4000, env={}, more=[], snap=None, image=None, keep=False, statsafter=1, quiet=False):
    e = dict(os.environ)
    for k in ('RVC_TICK_ALT', 'RVC_CODE_FILE', 'RVC_FREEZE', 'RVC_PASS_PROF'): e.pop(k, None)
    e.update(env)
    if code: e['RVC_CODE_FILE'] = code if ':' in code else os.path.join(SNAP, 'snap', 'w3', code)
    u = os.path.join(ROOT, 'logs', f'acct_{name}.uart'); f = os.path.join(ROOT, 'logs', f'acct_{name}.frames')
    if os.path.exists(u): os.remove(u)
    s = snap or os.path.join(SNAP, 'snap', load, load + '.snap')
    cmd = [os.path.join(ROOT, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', os.path.join('experiments', tick), '--define', 'NO_PAGING', '--image', image or os.path.join(SNAP, 'img'),
           '--no-desktop', '--no-viz', '--no-stdin', '--load-state', s, '--frames', str(frames), '--stats-after', str(statsafter), '--frame-log', f, '--uart-log', u]
    if ticks: cmd += ['--ticks', str(ticks)]
    cmd += more
    q = os.path.join(ROOT, 'tools', 'gpu_queue.ps1')
    for tr in range(100000):
        p = subprocess.run(['pwsh', '-NoProfile', '-File', q] + cmd, cwd=ROOT, env=e, capture_output=True, text=True, errors='replace')
        out = p.stdout + p.stderr
        if 'refused' not in out.lower(): break
        time.sleep(2)
    m = re.findall(r'^STATS seconds=([\d.]+) instructions=(\d+) ips=(\d+) frames=(\d+) fps=([\d.]+) per_frame=([\d.]+)', out, re.M)
    r = dict(name=name, load=load, tick=tick, code=code, ticks=ticks, env=env, more=more, date=time.strftime('%H:%M'))
    if m:
        sec, ins, ips, fr, fps, pf = m[-1]
        r.update(ms=1000 * float(sec) / int(fr), per=float(pf), ns=float(sec) * 1e9 / max(int(ins), 1), ips=int(ips))
    text = open(u, encoding='latin-1').read() if os.path.exists(u) else ''
    r['bad'] = len(re.findall(r'Oops|panic|Unable to handle|unhandled signal', text))
    r['uart_bytes'] = len(text)
    r['tail'] = text[-160:]
    r['prof'] = [l for l in out.split('\n') if re.search(r'^(STATS (tick|gpu|workers)|\[prof)', l)][-6:]
    r['clock'] = subprocess.run(['nvidia-smi', '--query-gpu=clocks.gr', '--format=csv,noheader,nounits'], capture_output=True, text=True).stdout.strip()
    if not m: r['out'] = out[-1500:]
    with open(os.path.join(HERE, 'acct.jsonl'), 'a') as fo: fo.write(json.dumps(r) + '\n')
    if not quiet:
        print(f"{name} {load} {tick} ticks={ticks} ms={r.get('ms', 0):.4f} per={r.get('per', 0):.0f} ns={r.get('ns', 0):.1f} bad={r['bad']} uart={r['uart_bytes']}", flush=True)
    r['stdout'] = out
    return r
if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('name'); ap.add_argument('--load', default='doom'); ap.add_argument('--tick', default='rvc_lean'); ap.add_argument('--code', default=''); ap.add_argument('--ticks', type=int, default=0); ap.add_argument('--frames', type=int, default=4000); ap.add_argument('--env', nargs='*', default=[]); ap.add_argument('--more', default=''); ap.add_argument('--show', action='store_true')
    a = ap.parse_args()
    r = run(a.name, a.load, a.tick, a.code, a.ticks, a.frames, dict(x.split('=', 1) for x in a.env), a.more.split())
    if a.show: print(r['stdout'][-4000:]); print('GUEST', r['tail'])
