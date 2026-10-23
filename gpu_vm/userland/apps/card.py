"""tools/test_all.py's cases on the card: image.sh's machine in rvc_harness's gpu_vm mode (gpu_vm/platform.md), one at a
time through tools\\gpu_queue.ps1, checked as test_all.py checks them on gvm_cpu (gpu_vm's pictures by PSNR against the
references). From Windows (PowerShell or Git Bash):

    py -3 gpu_vm/userland/apps/card.py CASE_OR_TAG ... [--image build/gvm_apps/img] [--psnr 35]
"""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
import importlib.util   # noqa: E402
_spec = importlib.util.spec_from_file_location('gvm_test_all', os.path.join(HERE, 'test_all.py'))
G = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(G)
T = G.T


# the card's own values where its special functions take a float sum elsewhere than gvm_cpu's (test_all.py's OVERRIDES):
# the same in two runs on the card
CARD = {
    'quake3-server': dict(digest=[(r'^(q3srvsum: .*)', '4fab5e9b')]),
    'quake-particles': dict(digest=[], has=[r'particles at frame 960: 2e7780de']),
}


def run(c, a):
    log = os.path.join(a.logs, c['name'] + '.log')
    if os.path.exists(log):
        os.remove(log)
    a.save_refs = False
    cmd = ['pwsh', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'rvc_harness.exe'),
           '--gvm', a.image, '--no-viz', '--uart-log', log, '--seconds', str(int(c['wall'] * a.wall_scale)),
           '--expect', '@gpuvm:~# ', '--send', 'cd /; export TERM=linux; ' + c['cmd'] + G.output_cmd(c, a) + '; ' + T.DONE + '\\n', '--until', c['until']]
    more = list(c['more'])
    while more:
        k = more.pop(0)
        if k in ('--expect', '--send') and more:
            cmd += [k, more.pop(0)]
    t = time.time()
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace')
    text = open(log, encoding='latin-1').read().replace('\r', '') if os.path.exists(log) else ''
    text = re.sub(r'(@gpuvm:[^\n]*?# )', lambda m: m.group(1) + chr(10), text)
    err = p.stderr   # (the harness says '--until matched' there)
    text = G.judge(c, text, a)
    return text, err, time.time() - t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('select', nargs='+')
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'gvm_apps', 'img'))
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'gvm_apps', 'card'))
    ap.add_argument('--refs', default=os.path.join(ROOT, 'build', 'gvm_apps', 'refs'))
    ap.add_argument('--wall-scale', type=float, default=1.0)
    a = ap.parse_args()
    os.makedirs(a.logs, exist_ok=True)
    cases = [dict(G.gpu_vm_case(c), **CARD.get(c['name'], {})) for c in T.CASES if c['name'] in a.select or c['tags'] & set(a.select)]
    done, failed = {}, []
    for c in cases:
        if c['cmd'] is None or c['image'] or any(m in ('--host', '--host-from', '--host-wheel') for m in c['more']):
            print(f"SKIP {c['name']}")
            continue
        text, err, secs = run(c, a)
        done[c['name']] = text
        why = T.check(c, text, err, False, done)
        print(f"{'PASS' if not why else 'FAIL'} {c['name']:28} {secs:5.0f} s" + ('' if not why else '  ' + '; '.join(why)), flush=True)
        if why:
            failed.append(c['name'])
    print(f'{len(cases) - len(failed)} of {len(cases)} passed on the card' + (f'; failed: {" ".join(failed)}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
