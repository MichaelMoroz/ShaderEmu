"""tools/test_all.py's cases on gpu_vm (gpu_vm/userland/apps/README.md, "Checks"): each case's line typed into
image.sh's machine on gvm_cpu, its console checked by test_all's own rules (has, sums, digest, same ...). Run in WSL:

    python3 gpu_vm/userland/apps/test_all.py [CASE or TAG ...] [--jobs N] [--cores N] [--list] [--save-refs]

gvm_cpu has the devices' words but draws no picture (frames are counted, as rvc_cpu does), so a case that types into
a window or reads the host (`--host`, `--fixed-dt`, `--host-wheel`) is skipped and said so. A case's `--expect`/`--send`
pairs are typed when their text appears. The kernel, ROM and device tree are image.sh's (W, build/gvm_apps/img).

Float results are not compared bit for bit (the user, 22 October 2026: the card's special functions are close enough):
where a RISC-V case keeps an exact sum of a float picture or sound, the gpu_vm case has the program write it and
`gvmcmp` (in the image) compares it with a reference that gvm_cpu made (`--save-refs`: build/gvm_apps/refs, which
image.sh puts in /usr/share/gvm-refs), by its PSNR or SNR; a float number is compared within a tolerance (NEAR).
"""
import argparse, os, re, select, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
# (tools/test_all.py by its path: this file has its name)
import importlib.util   # noqa: E402
_spec = importlib.util.spec_from_file_location('riscv_test_all', os.path.join(ROOT, 'tools', 'test_all.py'))
T = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(T)

PROMPT = re.compile(rb'@gpuvm:[^\n]*?# ')
UNSUPPORTED = ('--host', '--host-from', '--host-wheel', '--fixed-dt', '--pointer-sweep')
OUT = '/tmp/gvm-output'
REFS_IN_IMAGE = '/usr/share/gvm-refs'

# gpu_vm's own expected values where the RISC-V case's differ for a reason that is not a fault
OVERRIDES = {
    # a game played by itself whose ball runs on floats: gpu_vm's single-precision results (flushed denormals, the
    # card's sqrt) send it another way; the same with libm's exact math and with -fapprox-func
    'nxpinball': dict(has=[r'pinball: 1800 frames, 1 games, ball 3, high 0, score 18060 state 51ec1e4b']),
    # the textures made before "steady" depend on the machine's speed (gvm_cpu makes the world in its first stats
    # period: 8 loading texts fewer); its first 30 are the RISC-V machine's
    'classicube-pack': dict(count=[(r'^cctex: ', (30, 42))], digest=[]),
}

# float numbers of a line within a tolerance: (regex with groups, the reference values, tolerances: a fraction of the
# value, or an absolute amount when given as ('abs', n))
PHYS = r'highest (\d+) mm, deepest (\d+) mm, fastest (\d+) mm/s, (\d+) without room'
PHYS_TOL = (0.02, ('abs', 10), 0.15, ('abs', 3))
NEAR = {
    'nxphys-wall': [(PHYS, (4758, 7, 4335, 10), PHYS_TOL)],
    'nxphys-wall-7': [(PHYS, (4758, 7, 4335, 10), PHYS_TOL)],
    'nxphys-none': [(PHYS, (4758, 7, 4335, 10), PHYS_TOL)],
    'nxphys-tower': [(PHYS, (6899, 0, 0, 0), (0.01, ('abs', 10), ('abs', 50), ('abs', 1)))],
    'nxphys-heap': [(PHYS, (1484, 15, 4036, 0), PHYS_TOL)],
    'nxphys-scene-2': [(PHYS, (6890, 96, 12815, 0), PHYS_TOL)],
    'nxphys-throw': [(PHYS, (4749, 4, 10484, 9), PHYS_TOL)],
}


def gpu_vm_case(c):
    """A case as gpu_vm checks it (the module's text)."""
    c = dict(c)
    c.update(OVERRIDES.get(c['name'], {}))
    cmd, has, name = c['cmd'] or '', list(c.get('has', [])), c['name']
    # nxpath's pictures (docs/apps/raytrace.md: a sum each), by PSNR
    if name.startswith('nxpath') and 'NXPATH_AUTO=1' in cmd and any(' sum ' in h or '02f213e9' in h for h in has):
        c['cmd'] = cmd.replace('NXPATH_AUTO=1', 'NXPATH_SAVE=%s NXPATH_AUTO=1' % OUT)
        c['has'] = [h.split(' sum ')[0] if ' sum ' in h else r'^pathstat: ' for h in has]
        c['output'] = ('ppm', 35.0)
    # nxray's four pictures: the last one (RAY_SAVE, gpu_vm's), and four lines with a sum each
    if name in ('nxray', 'nxray-none'):
        c['cmd'] = cmd.replace('RAY_FRAMES=4', 'RAY_SAVE=%s RAY_FRAMES=4' % OUT)
        c['sums'], c['count'] = [], [(r'raystat: picture \d.* sum ([0-9a-f]+)', 4)]
        c['output'] = ('ppm', 35.0)
    # the HLSL compiler's fractal (the IR run with the card's functions)
    if name == 'hlsl':
        c['cmd'] = 'HLSL_PPM=%s ' % OUT + cmd
        c['has'] = [h for h in has if 'sum' not in h] + [r'hlsl_bench: sum [0-9a-f]{8}']
        c['output'] = ('ppm', 35.0)
    # libmedia's sounds: FLAC and MOD decoders are integers (their sums), MP3's is floats (SNR)
    if name == 'media-sound':
        c['sums'] = [(r'audio /usr/share/play-test\.(?:flac|mod): .* sum ([0-9a-f]+)', ['518c5ac8'] * 2 + ['c714a845'] * 2)]
        c['cmd'] = cmd + '; media_test audio /usr/share/play-test.mp3 %s 63' % OUT
        c['output'] = ('pcm16', 30.0)
    if name == 'nxplay':
        c['sums'] = [(r'nxplay: sum ([0-9a-f]+ of \d+) samples', ['e5fd8f8c of 141120', '1a14a562 of 176400', '98e66905 of 1354752'])]
        c['count'] = [(r'nxplay: sum [0-9a-f]+ of 1594368 samples', 2)]
    if name in NEAR:
        c['near'] = NEAR[name]
        c['has'] = [h for h in has if not h.startswith('sum ')]
    return c


def output_cmd(c, a):
    """What follows a case's line: its output written to the console (--save-refs) or compared with its reference."""
    if not c.get('output'):
        return ''
    if a.save_refs:
        return '; echo OUT-BEGIN; base64 %s; echo OUT-END' % OUT
    return '; gvmcmp %s %s %s/%s' % (c['output'][0], OUT, REFS_IN_IMAGE, c['name'])


def judge(c, text, a):
    """The lines the gpu_vm checks add to a console (and the patterns they must have)."""
    extra = ''
    if c.get('output'):
        c['has'] = c['has'] + [r'^output: [\d.]+ dB, at most \d+ apart$']
        if a.save_refs:
            import base64
            m = re.search(r'OUT-BEGIN\n(.*?)OUT-END', text, re.S)
            data = base64.b64decode(re.sub(r'[^A-Za-z0-9+/=]', '', m.group(1))) if m else b''
            if data:
                open(os.path.join(a.refs, c['name']), 'wb').write(data)
                extra += '\noutput: 99.0 dB, at most 0 apart\n'
        else:
            m = re.search(r'^gvmcmp: ([\d.]+) dB, at most (\d+) apart', text, re.M)
            if m:
                extra += '\noutput: %s dB, at most %s apart%s\n' % (m[1], m[2], '' if float(m[1]) >= c['output'][1] else ' (too far)')
    for p, want, tol in c.get('near', []):
        m = re.search(p, text, re.M)
        ok = bool(m)
        for k, w in enumerate(want if m else ()):
            got, t = float(m.group(k + 1)), tol[k]
            ok = ok and abs(got - w) <= (t[1] if isinstance(t, tuple) else t * abs(w))
        c['has'] = c['has'] + [r'^near: as the reference$']
        extra += '\nnear: %s\n' % ('as the reference' if ok else 'not near: %s' % (m.group(0) if m else 'no line'))
    return text + extra


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
            p.stdin.write(b'cd /; ' + c['cmd'].encode() + output_cmd(c, a).encode() + b'; ' + T.DONE.encode() + b'\n'); sent = 2
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
    text = judge(c, text, a)
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
    ap.add_argument('--refs', default=os.path.join(ROOT, 'build', 'gvm_apps', 'refs'), help="the outputs' references (gvm_cpu's)")
    ap.add_argument('--save-refs', action='store_true', help='keep the outputs made as the references (then image.sh again)')
    a = ap.parse_args()
    w = os.environ.get('W', os.path.join(ROOT, 'build', 'gvm_apps', 'img'))
    cases = [gpu_vm_case(c) for c in T.CASES]
    if a.select:
        cases = [c for c in cases if c['name'] in a.select or c['tags'] & set(a.select)]
    elif not a.long:
        cases = [c for c in cases if 'long' not in c['tags']]
    if a.list:
        for c in cases:
            print(c['name'])
        return 0
    os.makedirs(a.logs, exist_ok=True)
    os.makedirs(a.refs, exist_ok=True)
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
