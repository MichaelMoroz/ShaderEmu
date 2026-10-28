"""tools/test_all.py's cases on the graphics card: bin/rvc_harness.exe (D3D11, fxc2, experiments/rvc_opt) one at a
time through tools\\gpu_queue.ps1, each console checked by test_all's own rules (GPU-VM's userland/apps/card.py).

    python tools/test_card.py CASE_OR_TAG ... [--image build/images/nommu] [--max-wall 55] [--wall-scale 1]
    python tools/test_card.py games --list
    python tools/test_card.py mctest-3 lanetest fptest --warp --mark-ok --max-wall 0   # the WARP stamp (below)

The harness runs in real time: a case's wall is the guest's seconds it may take. The card boots in some 50 s, so
every case starts from a snapshot of the shell (made once on the processor, rvc_harness --cpu --save-state, as
tools/rv_compare does; --snapshot makes it again), its line typed as it resumes. A case is skipped when the card
cannot run it as rvc_cpu does: another image (`lazy`, `zero-tail`), the host's words changing as it runs (`--host-from`,
`--host-wheel`), or rvc_cpu's own devices (its `env`: RVC_SOUND's model and the like, which the card has for real).
--races is rvc_cpu's: a case here is checked by its own lines. Values the card gives otherwise (its floats are the
tick's) go in CARD, each with the runs that showed them.

The card runs a processor folder only when its warp_ok holds its stamp (its sources hashed, `rvc_harness --stamp`;
gpu_vm's tick.hlsl.warp_ok): --warp runs the cases on WARP, and --mark-ok writes the stamp when every one passed.
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import test_all as T   # noqa: E402

# the card's own values where they differ from rvc_cpu's (the same in two runs on the card)
CARD = {
    # the card's floats (the tick's), the same in two runs on 28 October 2026
    'hlsl': {'has': [r'hlsl_bench: compiled in \d+ ms: .* 0 spilled', r'hlsl_bench: sum a4b289dd']},
    'nxpath-balls': {'has': [r'pathstat: 160 x 120, 2 samples.* sum 11e09363']},
    # longer than one run may be (55 s; --parts runs them): mctest 255 was at its `calls` check, cc-bytes at its sixth program
    'mctest-255': {'long': True},
    'cc-bytes': {'long': True},
}
# options of rvc_cpu that rvc_harness takes as they are, with how many values each has
PASSED = {'--expect': 1, '--send': 1, '--host': 1, '--fixed-dt': 1}


def skip_why(c, a):
    if c.get('long') and not a.parts:
        return 'over 55 s on the card: --parts'
    if c['cmd'] is None or c['image']:
        return 'another image'
    if any(m in ('--host-from', '--host-wheel', '--until-times') for m in c['more']):
        return "the host's words change as it runs"
    if c['env']:
        return "rvc_cpu's own devices (" + ' '.join(c['env']) + ')'
    return None


def harness(a):
    return ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), a.harness, '--d3d11'] + (['--warp'] if a.warp else []) + ['--rvc', a.rvc,
            '--define', 'NO_PAGING', '--image', os.path.abspath(a.image), '--no-desktop', '--no-viz', '--no-stdin']


def run_locked(args):
    for _ in range(60):   # (the harness lock refuses a GPU harness beside CPU ones: wait for them)
        p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            return p
        time.sleep(20)
    return p


def make_snapshot(a):
    """The shell on the processor, a cold boot to the prompt, saved where the shader machine resumes it."""
    p = run_locked(harness(a) + ['--cpu', '--frames', '1000000', '--uart-log', a.snapshot_file + '.uart',
                                 '--input', 'echo SH""READY\\n', '--until', 'SHREADY', '--save-state', a.snapshot_file])
    if '--until matched' not in p.stderr or not os.path.exists(a.snapshot_file):
        raise SystemExit('the shell snapshot was not made: ' + ' '.join(p.stderr.strip().splitlines()[-3:]))
    print(f'the shell snapshot made: {a.snapshot_file}', flush=True)


def run(c, a):
    """One case: in one run, or with --parts in runs of --part-secs each, every one saving the machine at its end
    (rvc_harness --save-state) and the next resuming it, until --until or the case's wall (WARP is some 300
    thousand instructions a second: mctest's own checks alone are 50 s)."""
    log = os.path.join(a.logs, c['name'] + '.log')
    snap = os.path.join(a.logs, c['name'] + '.part')
    wall = c['wall'] * a.wall_scale
    if a.max_wall:
        wall = min(wall, a.max_wall)
    more, opts = list(c['more']), []
    while more:
        k = more.pop(0)
        n = PASSED.get(k)
        if n is None:
            raise SystemExit(f"{c['name']}: {k} is no option of rvc_harness's here (tools/test_card.py PASSED)")
        opts += [k] + more[:n]
        del more[:n]
    texts, errs, secs, left, part = [], '', 0.0, wall, 0
    while True:
        if os.path.exists(log):
            os.remove(log)
        now = min(left, a.part_secs) if a.parts else left
        args = harness(a) + ['--uart-log', log, '--seconds', f'{now:.0f}', '--until', c['until']]
        if part == 0:
            args += ['--load-state', a.snapshot_file, '--input', c['cmd'] + '; ' + T.DONE + chr(92) + 'n'] + opts
        else:   # (what was typed and sent is in the machine: of the case's options only its clock again)
            fixed = opts.index('--fixed-dt') if '--fixed-dt' in opts else -1
            args += ['--load-state', snap] + (opts[fixed:fixed + 2] if fixed >= 0 else [])
        if a.parts and now < left:
            args += ['--save-state', snap]
        t = time.time()
        p = run_locked(args)
        text = open(log, encoding='latin-1').read().replace(chr(13), '') if os.path.exists(log) else ''
        if '=== rvc_harness' in text:
            text = text[text.rfind('=== rvc_harness'):]
            text = text[text.find(chr(10)) + 1:] if part else text
        texts.append(text)
        errs += p.stderr
        queued = re.search(r'gpu_queue: waited (\d+) s', p.stderr)
        secs += time.time() - t - (int(queued[1]) if queued else 0)
        left -= now
        part += 1
        if '--until matched' in p.stderr or left < 1 or not a.parts or 'snapshot written' not in p.stderr:
            break
    if os.path.exists(snap):
        os.remove(snap)
    if part > 1:
        errs += f'[test_card] {part} parts' + chr(10)
    return ''.join(texts), errs, secs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('select', nargs='+', help='case names or tags, as test_all.py takes them')
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'images', 'nommu'))
    ap.add_argument('--harness', default=os.path.join(ROOT, 'bin', 'rvc_harness.exe'))
    ap.add_argument('--rvc', default='experiments/rvc_opt', help='the tick (its folder)')
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'test_card'))
    ap.add_argument('--wall-scale', type=float, default=1.0)
    ap.add_argument('--max-wall', type=float, default=55, help="a case's seconds at most (0: its wall)")
    ap.add_argument('--list', action='store_true', help='the cases chosen, and why one is skipped')
    ap.add_argument('--snapshot', action='store_true', help='make the shell snapshot again (rvc_harness --cpu)')
    ap.add_argument('--parts', action='store_true', help='each case in runs of --part-secs, the machine saved between')
    ap.add_argument('--part-secs', type=float, default=45)
    ap.add_argument('--warp', action='store_true', help="on WARP, D3D11's software device, not the card")
    ap.add_argument('--mark-ok', action='store_true', help="with --warp: when every case chosen passed, the tick's stamp "
                    "goes into its folder's warp_ok, and the card runs it (rvc_harness --stamp)")
    a = ap.parse_args()
    cases = [dict(c, **CARD.get(c['name'], {})) for c in T.CASES if c['name'] in a.select or c['tags'] & set(a.select)]
    if T.without_mmu(a.image):
        for c in cases:
            if c['name'] == 'cc-bytes':
                c['digest'] = [(c['digest'][0][0], T.CC_BYTES_NOMMU)]
    if a.list:
        for c in cases:
            print(f"{c['name']:28} {skip_why(c, a) or ''}")
        return 0
    os.makedirs(a.logs, exist_ok=True)
    a.snapshot_file = os.path.join(a.logs, 'shell.snap')
    if a.snapshot or not os.path.exists(a.snapshot_file):
        make_snapshot(a)
    done, failed, skipped, t0 = {}, [], [], time.time()
    for c in cases:
        why = skip_why(c, a)
        if why:
            skipped.append(c['name'])
            print(f"SKIP {c['name']:28} ({why})", flush=True)
            continue
        text, err, secs = run(c, a)
        done[c['name']] = text
        why = T.check(c, text, err, False, done)
        print(f"{'PASS' if not why else 'FAIL'} {c['name']:28} {secs:5.0f} s" + ('' if not why else '  ' + '; '.join(why)), flush=True)
        if why:
            failed.append(c['name'])
    ran = len(cases) - len(skipped)
    if a.mark_ok and a.warp and ran and not failed:
        p = subprocess.run([a.harness, '--rvc', a.rvc, '--stamp'], cwd=ROOT, capture_output=True, text=True)
        stamp = re.search(r'stamp ([0-9a-f]{16})', p.stdout)
        if not stamp:
            raise SystemExit('rvc_harness --stamp said no stamp: ' + p.stdout + p.stderr)
        with open(os.path.join(ROOT, a.rvc, 'warp_ok'), 'a') as f:
            f.write(f"{stamp[1]}  {time.strftime('%Y-%m-%d')}  WARP passed {ran} cases: {' '.join(done)}" + chr(10))
        print(f'{a.rvc}: stamp {stamp[1]} written to its warp_ok', flush=True)
    print(f'{ran - len(failed)} of {ran} passed on the card in {time.time() - t0:.0f} s, {len(skipped)} skipped' +
          (f'; failed: {" ".join(failed)}' if failed else '') + f' (consoles in {a.logs})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
