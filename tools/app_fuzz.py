"""The desktop's programs under `nx` on rvc_cpu, opened, used and closed in random and systematic
combinations; a plan is a list of nxfuzz's operations (linux/apps/nxfuzz.c), so that a failure replays
(docs/system/fuzz.md).

    python tools/app_fuzz.py list                         # the programs
    python tools/app_fuzz.py plan "start:a:nxpaint wait:a:30 rand:a:20:7 close:a:x end"   # one plan, its console printed
    python tools/app_fuzz.py single [APP ...]             # every program, closed every way, at once and after a use
    python tools/app_fuzz.py pairs [APP ...]              # every pair open together, closed in each order
    python tools/app_fuzz.py triples --light              # every triple (--light: of the small programs)
    python tools/app_fuzz.py random --seed 1 --count 200  # random plans of that seed; a plan's number replays it
    python tools/app_fuzz.py shrink "PLAN"                # a failing plan made as small as still fails the same way
    python tools/app_fuzz.py file PLANS.txt               # a plan a line
    python tools/app_fuzz.py smoke                        # the short set of tools/test_all.py

Common: --image DIR (default build/images/nommu), --jobs N (3), --threads N (8), --races (RVC_RACES), --wall S (per plan),
--card-model (RVC_SOUND, RVC_WCACHE, RVC_ADFAULT; on unless --no-card-model), --log DIR.

A plan fails by an FZ-BAD line (nxfuzz's oracles: a process left, a window left, the GPU held by a dead or
closed program, a window that never came, a hang) or by what the console says (kernel oops or panic, a segfault,
"a worker wanted", a Sys_Error, "Could not start", "One program at a time" from a program that has gone), or by
RVC_RACES undone, or by no FZ-DONE. A program that asks when it is closed (OpenTTD's question, an editor's
unsaved file) is answered, and is no failure unless it stays after the answer. Each result is a line of
results.jsonl in the log folder.
"""
import argparse, itertools, json, os, random, re, subprocess, sys, tempfile, threading, time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# tag: (command, seconds for its first window, weight). Light programs start in a fraction of a guest's second.
APPS = {
    # the small programs
    'tetris': ('nxtetris', 20, 'light'), 'mines': ('nxmine', 20, 'light'), 'pinball': ('nxpinball', 20, 'light'),
    'term': ('nxterm', 20, 'light'), 'calc': ('nxcalc', 20, 'light'), 'clock': ('nxclock', 20, 'light'),
    'eyes': ('nxeyes', 0, 'light'), 'gears': ('glxgears', 20, 'gpu'), 'edit': ('nxedit', 20, 'light'),
    'files': ('nxfiles /root', 20, 'light'), 'paint': ('nxpaint', 20, 'light'), 'web': ('nxweb /usr/share/web-test.html', 30, 'light'),
    'mon': ('nxmon', 20, 'light'), 'net': ('nxnet', 20, 'light'), 'settings': ('nxsettings', 20, 'light'),
    'ide': ('nxide', 30, 'light'), 'imdemo': ('imdemo', 30, 'gpu'),
    'ray': ('nxray', 30, 'workers'), 'path': ('nxpath', 40, 'workers'), 'play': ('nxplay', 20, 'light'), 'video': ('nxvideo', 20, 'gpu'),
    'phys': ('nxphys', 30, 'workers'), 'view': ('nxview /root/Pictures/Fox.png', 20, 'light'),
    # the games
    'doom': ('doom', 40, 'game'), 'tdawn': ('tdawn', 90, 'game'), 'ralert': ('ralert', 90, 'game'), 'quake': ('quake +menu_main', 60, 'game'),
    'quake3': ('quake3', 120, 'game'), 'halflife': ('halflife', 120, 'game'), 'micropolis': ('nxmicropolis', 60, 'game'),
    'openttd': ('openttd', 120, 'game'), 'classicube': ('classicube', 90, 'game'), 'thwaite': ('nes thwaite', 40, 'game'), 'nova': ('nes nova', 40, 'game'),
}
WAYS = ('x', 'alt', 'bar', 'term', 'twice')

# What the console must not say. (Each a program's own word for having broken, or the kernel's.)
NEVER = [
    (r'Kernel panic', 'kernel-panic'), (r'Oops', 'kernel-oops'), (r'Unable to handle kernel', 'kernel-oops'), (r'BUG: ', 'kernel-bug'),
    (r'WARNING: CPU', 'kernel-warn'), (r'segfault|Segmentation fault', 'segfault'), (r'a worker wanted', 'worker-wanted'),
    (r'Sys_Error', 'sys-error'), (r'Could not start', 'could-not-start'), (r'One program at a time|the GPU is drawing for', 'gpu-busy'),
    (r'stopped before its window', 'stopped-early'), (r'Illegal instruction', 'illegal-instruction'), (r'unhandled signal', 'signal'),
    (r'Bus error', 'bus-error'), (r'Aborted', 'aborted'), (r'an exception out of', 'exception'),
    (r'Crash encountered', 'crash'), (r'Fatal error|FatalError|fatal error', 'fatal'), (r'assert(ion)? failed|Assertion', 'assert'),
    (r'double free|invalid (free|pointer)', 'heap'), (r'Out of memory|invoked oom-killer|page allocation failure', 'oom'),
]


def flat(plan):
    return plan if isinstance(plan, str) else ' '.join(plan)


def start_ops(tag, wait=True):
    cmd, seconds, _ = APPS[tag]
    ops = [f'start:{tag}:{cmd.replace(" ", ",")}']
    if wait:   # (the eyes have no frame: no task of the desktop's, closed by a signal to their process)
        ops.append(f'wait:{tag}:{seconds}' if seconds else 'sleep:1000')
    return ops


class Runner:
    def __init__(self, a):
        self.a = a
        self.dir = os.path.join(ROOT, a.log or os.path.join('build', 'app_fuzz'))
        os.makedirs(self.dir, exist_ok=True)
        self.count = 0
        self.lock = threading.Lock()

    def run(self, plan, keep=False, wall=None, before='', env=None):
        """A plan under a new machine: what failed of it (a list of [kind, line]) and the console."""
        a = self.a
        with self.lock:
            self.count += 1
            n = self.count
        log = os.path.join(self.dir, f'c{os.getpid()}_{n}.log')
        races = log + '.races'
        e = dict(os.environ, RVC_THREADS=str(a.threads))
        if a.card_model:
            e.update(RVC_WCACHE='1', RVC_ADFAULT='1', RVC_SOUND='1')
        if a.races:
            e['RVC_RACES'] = races
        if env:
            e.update(env)
        line = f"{before}nx; sleep 3; nxfuzz {flat(plan)}; echo LX-''DONE\\n"
        cmd = [os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--image', a.image, '--quiet', '--uart-log', log, '--expect', '/ # ', '--send', line,
               '--until', 'LX-DONE', '--wall', str(wall or a.wall)]
        t = time.time()
        while True:
            try:
                r = subprocess.run(cmd, cwd=ROOT, env=e, capture_output=True, text=True, errors='replace', timeout=(wall or a.wall) + 60)
                code, tail = r.returncode, r.stdout[-3000:] + r.stderr[-3000:]
            except subprocess.TimeoutExpired:
                code, tail = -1, ''
            if code != 75:   # (refused beside a run on the card: it has done nothing, docs/machine/cpu-harness.md)
                break
            time.sleep(15)
        seconds = time.time() - t
        text = open(log, errors='replace').read() if os.path.exists(log) else ''
        console = text[text.find('nxfuzz'):]
        bad = [['bad', m] for m in re.findall(r'FZ-BAD ([^\r\n]*)', console)]
        for pattern, kind in NEVER:
            m = re.search(pattern, console)
            if m:
                s = console.rfind('\n', 0, m.start()) + 1
                bad.append([kind, console[s:console.find('\n', m.end())].strip()[:200]])
        # One program at a time draws: told to close one still open is as it should be, and then there is no window
        def open_at(name, at):
            for tag, command in re.findall(r'FZ started (\S+): process \d+: ([^\r\n]*)', console[:at]):
                if (name == 'another' or (name == 'game' and re.match(r'tdawn|ralert', command)) or name.replace('.bin', '') in command) and not re.search(r'FZ closed %s \(' % re.escape(tag), console[:at]):
                    return True
            return False
        told = list(re.finditer(r'(?:GPU is drawing for|close) ((?!it\b)\S+)(?: program)?(?: \(process| first)', console))
        if told and all(open_at(m.group(1), m.start()) for m in told):
            bad = [b for b in bad if b[0] != 'gpu-busy' and not (b[0] == 'bad' and b[1].startswith('no-window'))]
        if 'FZ-DONE' not in console:
            at = re.findall(r'FZ op ([^\r\n]*)', console)
            bad.append(['wall', 'no FZ-DONE in the wall, last op ' + (at[-1] if at else '?')])
        m = re.search(r'FZ memory: free\+cached (\d+) KB at the start, (\d+) KB now', console)
        if m and int(m.group(1)) - int(m.group(2)) > 8192:
            bad.append(['memory', m.group(0)])
        if a.races:
            m = re.search(r'(\d+) (?:changes? )?undone', tail)
            if m and int(m.group(1)):
                bad.append(['races', m.group(0)])
        if not keep and not bad:
            for f in (log, races):
                if os.path.exists(f):
                    os.remove(f)
        res = dict(plan=flat(plan), bad=bad, seconds=round(seconds, 1), log=os.path.relpath(log, ROOT), code=code)
        with self.lock, open(os.path.join(self.dir, 'results.jsonl'), 'a') as f:
            f.write(json.dumps(res) + '\n')
        return res, console


def all_plans(runner, plans, jobs):
    """Each plan, at most `jobs` at a time; failures said as they come. The number of failures."""
    bad = done = 0
    t = time.time()
    with ThreadPoolExecutor(jobs) as pool:
        futures = {pool.submit(runner.run, p): p for p in plans}
        for f in as_completed(futures):
            res, _ = f.result()
            done += 1
            if res['bad']:
                bad += 1
                print(f"FAIL {res['plan']}\n     {res['log']}: " + '; '.join(f'{k}: {v}' for k, v in res['bad'][:6]), flush=True)
            if done % 50 == 0:
                print(f'... {done} of {len(plans)}, {bad} failed, {time.time() - t:.0f} s', flush=True)
    print(f'{len(plans)} plans, {bad} failed, {time.time() - t:.0f} s')
    return bad


def single_plans(apps):
    for tag in apps:
        for way in WAYS if APPS[tag][1] else ('term',):
            yield [*start_ops(tag), f'close:{tag}:{way}', 'end']     # as soon as there is a window
            yield [f'start:{tag}:{APPS[tag][0].replace(" ", ",")}', 'sleep:300', f'close:{tag}:{way}', 'end']  # while it starts
            yield [*start_ops(tag), 'sleep:3000', f'rand:{tag}:12:{len(tag)}', f'close:{tag}:{way}', 'end']


def pair_plans(apps, group=2, ways=('x', 'alt', 'term')):
    for k, combo in enumerate(itertools.combinations(apps, group)):
        ops = [o for t in combo for o in start_ops(t)]
        order = combo[::-1] if k % 2 else combo
        ops.append('sleep:1000')
        ops += [f'close:{t}:{ways[(k + i) % len(ways)]}' for i, t in enumerate(order)]
        ops += [*start_ops('gears'), 'close:gears:x', 'end'] if 'gears' not in combo else ['end']
        yield ops


def random_plan(r, apps, length=None):
    """Random operations over at most five programs at a time; the same generator and number the same plan."""
    ops, open_tags, serial = [], [], 0
    for _ in range(length or r.randint(4, 14)):
        choice = r.random()
        if (choice < 0.35 or not open_tags) and len(open_tags) < 5:
            base = r.choice(apps)
            serial += 1
            tag = f'{base}{serial}'
            cmd = APPS[base][0].replace(' ', ',')
            ops.append(f'start:{tag}:{cmd}')
            if APPS[base][1] and r.random() < 0.85:
                ops.append(f'wait:{tag}:{APPS[base][1]}')
            open_tags.append(tag)
        elif choice < 0.6 and open_tags:
            tag = r.choice(open_tags)
            ops.append(f'rand:{tag}:{r.randint(1, 15)}:{r.randint(0, 9999)}')
        elif choice < 0.85 and open_tags:
            tag = r.choice(open_tags)
            ops.append(f'close:{tag}:{r.choice(WAYS)}')
            open_tags.remove(tag)
        else:
            ops.append(f'sleep:{r.choice((100, 500, 2000, 8000))}')
    ops.append('end')
    return ops


def kinds(res):
    return {k for k, _ in res['bad']}


def shrink(runner, plan):
    """Operations taken away while the same kinds of failure stay: first by program, then one by one."""
    ops = plan.split() if isinstance(plan, str) else list(plan)
    res, _ = runner.run(ops)
    want = kinds(res)
    if not want:
        print('it does not fail: ' + flat(ops))
        return ops

    def still(candidate):
        r, _ = runner.run(candidate)
        return kinds(r) & want

    tags = sorted({o.split(':')[1] for o in ops if o.startswith('start:')})
    for tag in tags:
        candidate = [o for o in ops if not (':' in o and o.split(':')[1] == tag)]
        if len(candidate) < len(ops) and still(candidate):
            ops = candidate
    i = 0
    while i < len(ops):
        if ops[i] == 'end':
            i += 1
            continue
        candidate = ops[:i] + ops[i + 1:]
        if still(candidate):
            ops = candidate
        else:
            i += 1
    print(f'shrunk ({", ".join(sorted(want))}): ' + flat(ops))
    return ops


SMOKE = [
    'start:p:nxpaint wait:p:20 close:p:x start:g:glxgears wait:g:20 close:g:alt end',
    'start:c:nxcalc wait:c:20 start:t:nxterm wait:t:20 rand:c:6:3 close:t:bar close:c:term end',
    'start:g:glxgears start:g2:imdemo sleep:100 end',
    'start:e:nxedit wait:e:20 key:e:h,i close:e:x end',
    'start:r:nxray wait:r:30 sleep:2000 close:r:kill start:g:glxgears wait:g:20 close:g:x end',
]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('what')
    p.add_argument('args', nargs='*')
    p.add_argument('--image', default='build/images/nommu')
    p.add_argument('--jobs', type=int, default=3)
    p.add_argument('--threads', type=int, default=8)
    p.add_argument('--wall', type=int, default=120)
    p.add_argument('--races', action='store_true')
    p.add_argument('--no-card-model', dest='card_model', action='store_false')
    p.add_argument('--log')
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--count', type=int, default=100)
    p.add_argument('--light', action='store_true')
    p.add_argument('--no-games', action='store_true')
    a = p.parse_args()
    r = Runner(a)
    apps = a.args or list(APPS)
    if a.light:
        apps = [t for t in apps if APPS[t][2] == 'light']
    if a.no_games:
        apps = [t for t in apps if APPS[t][2] != 'game']
    if a.what == 'list':
        for t, (cmd, s, w) in APPS.items():
            print(f'{t:12s} {w:8s} {s:4d} s  {cmd}')
    elif a.what == 'plan':
        res, console = r.run(' '.join(a.args), keep=True)
        print(console)
        print(res)
        return 1 if res['bad'] else 0
    elif a.what == 'single':
        return 1 if all_plans(r, list(single_plans(apps)), a.jobs) else 0
    elif a.what == 'pairs':
        return 1 if all_plans(r, list(pair_plans(apps, 2)), a.jobs) else 0
    elif a.what == 'triples':
        return 1 if all_plans(r, list(pair_plans(apps, 3)), a.jobs) else 0
    elif a.what == 'random':
        rng = random.Random(a.seed)
        return 1 if all_plans(r, [random_plan(rng, apps) for _ in range(a.count)], a.jobs) else 0
    elif a.what == 'shrink':
        shrink(r, ' '.join(a.args))
    elif a.what == 'file':   # a plan a line
        return 1 if all_plans(r, [l.strip() for l in open(a.args[0]) if l.strip()], a.jobs) else 0
    elif a.what == 'smoke':
        return 1 if all_plans(r, SMOKE, a.jobs) else 0
    else:
        p.error('what: list, plan, single, pairs, triples, random, shrink, smoke')
    return 0


if __name__ == '__main__':
    sys.exit(main())
