"""tools/test_all.py's cases on gpu_vm (gpu_vm/userland/apps/README.md, "Checks"): each case's line typed into
image.sh's machine on gvm_cpu, its console checked by test_all's own rules (has, sums, digest, same ...). Run in WSL:

    python3 gpu_vm/userland/apps/test_all.py [CASE or TAG ...] [--jobs N] [--cores N] [--list]

gvm_cpu has the devices' words but draws no picture (frames are counted, as rvc_cpu does), so a case that types into
a window or reads the host (`--host`, `--fixed-dt`, `--host-wheel`) is skipped and said so. A case's `--expect`/`--send`
pairs are typed when their text appears. The kernel, ROM and device tree are image.sh's (W, build/gvm_apps/img).
"""
import argparse, os, re, select, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import test_all as T   # noqa: E402

PROMPT = re.compile(rb'@gpuvm:[^\n]*?# ')
UNSUPPORTED = ('--host', '--host-from', '--host-wheel', '--fixed-dt', '--pointer-sweep')


def run_case(c, a, w):
    """The console's text and the harness-like error text (with '--until matched' when the end came)."""
    more = list(c['more'])
    if any(m in UNSUPPORTED for m in more) or c['cmd'] is None or c['image']:
        return None
    pairs = []
    while more:
        k = more.pop(0)
        if k == '--expect' and more:
            pairs.append([more.pop(0).encode(), None])
        elif k == '--send' and more and pairs:
            pairs[-1][1] = more.pop(0).encode().decode('unicode_escape').encode('latin-1')
    cores = 256 if re.search(r'\b(mctest|lanetest|steptest) 255|255 workers|with 255', c['cmd'] + ' '.join(c.get('has', []))) else a.cores
    ram = open(os.path.join(w, 'ram_mb')).read().strip()
    emu = os.path.join(ROOT, 'build', 'gvm_cpu', 'gvm_cpu')
    p = subprocess.Popen([emu, '--cores', str(cores), '--ram', ram, '--rom', os.path.join(w, 'rootfs.romfs'),
                          '--dtb', os.path.join(w, 'gvm.dtb'), os.path.join(w, 'vmlinux')],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    out, sent, until, t0 = b'', 0, c['until'].encode(), time.time()
    reached = False
    while time.time() - t0 < c['wall'] * a.wall_scale:
        r, _, _ = select.select([p.stdout], [], [], 0.5)
        if r:
            chunk = os.read(p.stdout.fileno(), 65536)
            if not chunk:
                break
            out += chunk.replace(b'\r', b'')
        prompts = len(PROMPT.findall(out))
        if sent == 0 and prompts >= 1:
            p.stdin.write(b'stty -echo\n'); sent = 1
        elif sent == 1 and prompts >= 2:
            p.stdin.write(b'cd /; ' + c['cmd'].encode() + b'; ' + T.DONE.encode() + b'\n'); sent = 2
            mark = len(out)
        elif sent == 2:
            for pr in pairs:
                if pr[1] is not None and pr[0] in out[mark:]:
                    p.stdin.write(pr[1]); pr[1] = None
            if until in out[mark:]:
                reached = True
                break
    try:
        p.stdin.write(b'poweroff -f\n')
        p.wait(10)
    except Exception:
        pass
    if p.poll() is None:
        p.kill()
    # (with stty -echo a command's output follows its prompt on the line: the RISC-V console's echo ends that line)
    text = re.sub(r'(@gpuvm:[^\n]*?# )', lambda m: m.group(1) + chr(10), out.decode('latin-1'))
    with open(os.path.join(a.logs, c['name'] + '.log'), 'w', encoding='latin-1') as f:
        f.write(text)
    return text, ('--until matched' if reached else ''), time.time() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('select', nargs='*')
    ap.add_argument('--long', action='store_true')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) // 2))
    ap.add_argument('--cores', type=int, default=32, help='cores of a case that does not ask for 255 workers')
    ap.add_argument('--wall-scale', type=float, default=1.0)
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'gvm_apps', 'test_all'))
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    w = os.environ.get('W', os.path.join(ROOT, 'build', 'gvm_apps', 'img'))
    cases = T.CASES
    if a.select:
        cases = [c for c in cases if c['name'] in a.select or c['tags'] & set(a.select)]
    elif not a.long:
        cases = [c for c in cases if 'long' not in c['tags']]
    if a.list:
        for c in cases:
            print(c['name'])
        return 0
    os.makedirs(a.logs, exist_ok=True)
    subprocess.run(['make', '-s', '-C', os.path.join(ROOT, 'gpu_vm', 'cpu'), 'CC=gcc'], check=True)
    done, passed, failed, skipped = {}, [], [], []
    results = []
    with ThreadPoolExecutor(a.jobs) as ex:
        fut = {ex.submit(run_case, c, a, w): c for c in cases}
        for f in as_completed(fut):
            c = fut[f]
            r = f.result()
            if r is None:
                skipped.append(c['name'])
                print(f"SKIP {c['name']:28} (reads the host, or another image)", flush=True)
                continue
            done[c['name']] = r[0]
            results.append((c, r))
    for c, (text, err, secs) in results:
        why = T.check(c, text, err, False, done)
        print(f"{'PASS' if not why else 'FAIL'} {c['name']:28} {secs:5.0f} s" + ('' if not why else '  ' + '; '.join(why)), flush=True)
        (failed if why else passed).append(c['name'])
    print(f'{len(passed)} passed, {len(failed)} failed, {len(skipped)} skipped' + (f'; failed: {" ".join(failed)}' if failed else '')
          + f' (consoles in {a.logs})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
