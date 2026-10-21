"""A game's instructions a frame on core 0 and on all cores, under rvc_cpu, and its real speed on
the shader machine (docs/machine/multicore.md, "Every core's share of a game's frame", "A game's speed").

    python tools/game_par.py GAME [--image DIR] [--log FILE] [--calls FILE]
    python tools/game_par.py all [--runs 3]     (a mean over three timings: --salt)
    python tools/game_par.py GAME --profile PC.LOG; python tools/pc_profile.py PC.LOG kernel=K.nm game=G.nm
    python tools/game_par.py GAME[,GAME...] --card [--image DIR ...]   (the graphics card, through gpu_queue)
    python tools/game_par.py quake3 --guest QUAKE3_SERVER=paced --calls FILE   (NAME=VALUE exported in the guest)

Quake III's line has its server's frames of the game a second and the passes each takes (the
window's passes over them; the server's worker is busy all the window), from the q3stat: lines.

Each game runs a fixed workload from power-on (bin/rvc_cpu.exe, no card). rvc_cpu --count-from
counts every core from when the console says the game's start mark until --until; the frames
in that window come from the game's own lines. Factor = all cores' instructions / core 0's.
Passes a frame is what decides a game's speed where a pass costs the same however early core 0
ends it (the world's fixed rounds a frame); rvc_cpu's guest seconds count a short pass as short.

--card runs the same workload under bin/rvc_harness.exe on the card, in real time (D3D11 +
fxc2, as the world), one image after another, and prints the game's own frames a second, passes
a second and a game frame, and core 0's, the workers' and all cores' millions of instructions a
second (the harness's STATS lines from the mark). It refuses while VRChat or SteamVR runs.
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DONE = "echo LX-''DONE"   # (the quotes keep the echo of the command from matching --until)
# the card's image: Linux without the MMU once made (linux/nommu/image.py), else the MMU's
DEFAULT_IMAGE = os.path.join(ROOT, 'build', 'images', 'nommu' if os.path.exists(os.path.join(ROOT, 'build', 'images', 'nommu', 'linux_payload.bin')) else 'linux')


def doom_frames(log):
    # every "doom: N frames" line after the mark is a window; the first began before it
    lines = re.findall(r'doom: (\d+) frames in', log)
    total = int(re.search(r'timed (\d+) gametics', log).group(1))
    return total - int(lines[0])


def lines_rate(pattern, most=None, skip=0):
    """(frames, ms) summed over the game's own "N frames in M ms" lines after the mark."""
    def rate(log):
        got = [(int(f), int(ms)) for f, ms in re.findall(pattern, log)][skip:][:most]
        return sum(f for f, _ in got), sum(ms for _, ms in got)
    return rate


def quake_server(log):
    got = re.findall(r'the server on a worker core: (\d+) frames of its own to these (\d+)', log)
    return f"server {sum(int(s) for s, _ in got)} frames to the client's {sum(int(c) for _, c in got)}" if got else ''


def q3_server(log):
    """(the server's frames of the game, the client's frames, ms) over the q3stat: windows after the mark that have both."""
    sf = cf = ms = 0
    for chunk in log.split('q3stat: frame ')[2:]:   # (the mark's own window is before it)
        f = re.match(r'\d+: (\d+) frames in (\d+) ms', chunk)
        m = re.search(r'q3stat: the server made (\d+) frames of the game', chunk)
        if f and m:
            sf, cf, ms = sf + int(m[1]), cf + int(f[1]), ms + int(f[2])
    return sf, cf, ms


# the first window after a level's first frame began while it loaded
QUAKE_RATE = lines_rate(r'quakestat: (\d+) frames in (\d+) ms', skip=1)

GAMES = {
    'doom': dict(
        what='-timedemo demo1, one tic a frame, from its first rate line to the end',
        cmd='nano-X -p & sleep 2; doom -1 -timedemo demo1',
        mark='doom: ', frames=doom_frames),
    'quake': dict(
        what='e1m5 played, the server on its worker, standing where the level starts, from its first frame to 70 s after the command',
        cmd='nano-X -p & sleep 2; QUAKE_STATS_MS=5000 quake +map e1m5 & sleep 70',
        mark="a level's first frame is frame", frames=lambda log: QUAKE_RATE(log[log.find("a level's first frame"):])[0], rate=QUAKE_RATE, extra=quake_server),
    'quake-timedemo': dict(
        what='+timedemo demo1, from the level\'s first frame to the timedemo\'s line (no server: not the game)',
        cmd='nano-X -p & sleep 2; quake +timedemo demo1', until=' fps',
        mark="a level's first frame is frame", frames=lambda log: int(re.search(r'(\d+) frames +[\d.]+ seconds', log)[1])),
    'quake3': dict(
        what='q3dm17 with one bot, fixedtime 33, put at 488 -968 378 looking north at frame 100, frames 300 to 700',
        cmd='nano-X -p & sleep 2; QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3',
        more=['--expect', 'q3stat: frame 100:', '--send', 'setviewpos 488 -968 378 90\\n'],
        mark='q3stat: frame 300:', until='q3stat: frame 700:', frames=lambda log: 400,
        rate=lines_rate(r'q3stat: frame \d+: (\d+) frames in (\d+) ms', 4), server=q3_server),
    'quake3-3bots': dict(
        what='q3dm17 with three bots, otherwise as quake3',
        cmd='nano-X -p & sleep 2; QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3 +addbot sarge 3 +addbot sarge 3',
        more=['--expect', 'q3stat: frame 100:', '--send', 'setviewpos 488 -968 378 90\\n'],
        mark='q3stat: frame 300:', until='q3stat: frame 700:', frames=lambda log: 400,
        rate=lines_rate(r'q3stat: frame \d+: (\d+) frames in (\d+) ms', 4), server=q3_server),
    'classicube': dict(
        what='seed 7, block physics off, walking and jumping into a hill, five 2 s windows from "steady"',
        cmd='mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; '
            'nano-X -p & sleep 2; CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
        more=['--expect', 'classicube: steady', '--send', 'turn 1200 0\\ndown W\\ndown Space\\n', '--until-times', '5'],
        mark='classicube: steady', until='ccstat: a frame:', frames=lambda log: sum(
            int(n) for n in re.findall(r'ccstat: (\d+) frames', log[log.find('classicube: steady'):])[:5]),
        rate=lines_rate(r'ccstat: (\d+) frames in (\d+) ms', 5),
        # the harness has no --until-times: the window ends at a line of the shell's own
        card=dict(cmd='mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; '
                      'nano-X -p & sleep 2; (sleep 75; echo CC-\'\'END) & CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
                  until='CC-END', more=['--expect', 'classicube: steady', '--send', 'turn 1200 0\\ndown W\\ndown Space\\n'])),
    'tdawn': dict(
        what='mission 3, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 '
            'TDAWN_MARK=301,1201 tdawn FROMINSTALL',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'ralert': dict(
        what='Soviet mission, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=1300 RALERT_MARK=301,1201 ralert',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'ralert-allies': dict(
        what='Allied mission, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_FRAMES=1300 RALERT_MARK=301,1201 ralert',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'nes': dict(
        what='Thwaite, the 1,000-frame run of docs/games/nes.md, the list by a worker, NES frames 100 to 1000',
        cmd='nano-X -p & sleep 2; NES_LIST=worker NES_SOUND=0 NES_MARK=100 NES_FRAMES=1000 NES_PRESS="20 start 50 start '
            '80 a 100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a 800 a 900 -right 1000 a 1100 +left 1300 a" nes thwaite',
        mark='nesmark', frames=lambda log: 900),
    'halflife': dict(
        what='hldemo1, standing where it starts, the fight at 31 to 45 s of the level (docs/games/halflife.md, "Speed")',
        cmd='nano-X -p & sleep 2; HL_MARK=${HL_MARK:-31,45} halflife +map hldemo1', wall=600,
        mark='hlmark: begins', until='hlmark: ends', frames=lambda log: int(re.search(r'hlmark: ends, (\d+) frames', log)[1])),
    'nxphys': dict(
        what='the wall (scene 1), 120 substeps; a frame here is a substep',
        cmd='nano-X -p & sleep 2; PHYS_SCENE=1 PHYS_STEPS=120 nxphys',
        mark='shares on ', frames=lambda log: int(re.search(r'physstat: done: \d+ boxes, (\d+) substeps', log)[1]),
        rate=lambda log: tuple(int(x) for x in re.search(r'physstat: done: \d+ boxes, (\d+) substeps in (\d+) ms', log).groups())),
}


def game_fps(g, text, frames, seconds):
    if 'rate' in g:
        f, ms = g['rate'](text)
        return f * 1000.0 / ms if ms else 0.0
    return frames / seconds if seconds else 0.0


def run_locked(args, **kw):
    """subprocess.run, again while the harness lock refuses it (CPU harnesses or one GPU harness at a time)."""
    for _ in range(60):
        p = subprocess.run(args, capture_output=True, text=True, errors='replace', **kw)
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            return p
        time.sleep(20)
    return p


def run(name, g, image, log, calls, salt, show, profile=None, plain=False, guest=()):
    exe = os.path.join(ROOT, 'bin', 'rvc_cpu.exe')
    if os.path.exists(log):
        os.remove(log)
    args = [exe, '--quiet', '--uart-log', log, '--wall', str(g.get('wall', 280)), '--expect', '/ # ',
            '--send', (f': {salt}; ' if salt else '') + ''.join(f'export {v}; ' for v in guest) + g['cmd'] + '; ' + DONE + '\\n', '--until', g.get('until', 'LX-DONE'), '--count-from', g['mark']] + g.get('more', [])
    if image:
        args += ['--image', image]
    if profile:   # (fine samples change the timing: a profile's counts are not the baseline's)
        args += ['--pc-log', profile, '--pc-from', g['mark'], '--ticks', '256', '--pass-ms', '0.0078']
    env = dict(os.environ)
    if not plain:   # the machine as the card has it: its sound card's clock, write caches, memory a pass old, its page walk
        env.update(RVC_SOUND='1', RVC_WCACHE='1', RVC_ADFAULT=os.environ.get('RVC_ADFAULT', '1'), RVC_RACES=log + '.races')
    env.update(g.get('env', {}))
    if calls:
        env['RVC_CALLS'] = calls
    if show:
        print(subprocess.list2cmdline(args))
    t = time.time()
    p = run_locked(args, cwd=ROOT, env=env)
    wall = time.time() - t
    err = p.stderr
    m = re.search(r'COUNT from "(?:[^"]*)": core 0 (\d+), all cores (\d+), (\d+) cores ran, the busiest worker (\d+), (\d+) passes, ([\d.]+) s', err)
    if not m or '--until matched' not in err:
        why = 'the mark was never said' if 'COUNT never' in err else 'it did not reach LX-DONE' if '--until NOT' in err else 'no COUNT line'
        print(f'{name}: not measured, {why} ({wall:.0f} s; console in {log})')
        return None
    c0, allc, ran, busiest, passes, secs = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5]), float(m[6])
    text = open(log, encoding='latin-1').read()
    text = text[text.rfind('=== rvc_cpu ==='):]
    frames = g['frames'](text)
    after = text[text.find(g['mark']):]
    r = dict(game=name, frames=frames, core0=c0 / frames, all=allc / frames, factor=allc / c0, workers=ran - 1,
             busiest=busiest / frames, seconds=secs, wall=wall, passes=passes / frames, fps=game_fps(g, after, frames, secs))
    extra = g['extra'](after) if 'extra' in g else ''
    s = re.search(r'\[cpu\] stops: .*; from the mark (\d+), (\d+), (\d+)', err)
    if s:   # (workers' faults from the mark, those on a clear accessed or written bit, core 0's on one)
        r.update(stops=int(s[1]), ad_stops=int(s[2]), ad_core0=int(s[3]))
        extra = (extra + '; ' if extra else '') + f"workers stopped {s[1]} times, {s[2]} on a page's written bit (core 0 {s[3]})"
    if 'server' in g:   # its frames of the game a second, and passes each (the client's passes a frame over the server's frames to its)
        sf, cf, ms = g['server'](after)
        if sf:
            r.update(server_fps=sf * 1000.0 / ms, server_passes=r['passes'] * cf / sf)
            extra = (extra + '; ' if extra else '') + f"the server {r['server_fps']:.1f} frames of the game a second, {r['server_passes']:.1f} passes each"
    print(f"{name}: {frames} frames ({g['what']}); core 0 {r['core0'] / 1e3:.1f}k a frame, all cores {r['all'] / 1e3:.1f}k, "
          f"factor {r['factor']:.2f}; {r['workers']} workers ran, the busiest {r['busiest'] / 1e3:.1f}k a frame; "
          f"{r['passes']:.2f} passes a frame; {secs:.1f} s of guest time, {r['fps']:.1f} frames a second of it"
          + (f"; {extra}" if extra else '') + f"; {wall:.0f} s here")
    return r


def vr_running():
    out = subprocess.run(['tasklist'], capture_output=True, text=True, errors='replace').stdout.lower()
    return [n for n in ('vrchat.exe', 'vrserver.exe') if n in out]


def card_folder(image):
    """The harness boots build/images/nommu of its working folder (build/images/linux without it, the MMU's):
    a folder of junctions, both to the image."""
    image = os.path.abspath(image or DEFAULT_IMAGE)
    work = os.path.join(ROOT, 'build', 'game_par', 'card_' + os.path.basename(image))
    links = [('experiments', os.path.join(ROOT, 'experiments')), ('harness', os.path.join(ROOT, 'harness')),
             (os.path.join('build', 'shadercache'), os.path.join(ROOT, 'build', 'shadercache')),
             (os.path.join('build', 'images', 'linux'), image), (os.path.join('build', 'images', 'nommu'), image)]
    os.makedirs(os.path.join(work, 'build', 'images'), exist_ok=True)
    for link, target in links:
        if not os.path.exists(os.path.join(work, link)):
            subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(work, link), target], capture_output=True, check=True)
    return work


def run_card(name, g, image, log, show, limit, rvc='experiments/rvc_opt'):
    if vr_running():
        print(f'{name}: not run on the card, {" and ".join(vr_running())} running')
        return None
    c = dict(g, **g.get('card', {}))
    more = [x for i, x in enumerate(c.get('more', [])) if x != '--until-times' and (i == 0 or c['more'][i - 1] != '--until-times')]
    if os.path.exists(log):
        os.remove(log)
    args = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'rvc_harness.exe'),
            '--d3d11', '--rvc', rvc, '--image', 'linux-net', '--no-desktop', '--no-stdin', '--uart-log', log,
            '--seconds', str(limit), '--expect', '/ # ', '--send', 'md5sum /usr/bin/*.bin /usr/bin/nxphys; ' + c['cmd'] + '; ' + DONE + '\\n',
            '--until', c.get('until', 'LX-DONE'), '--stats-from', c['mark']] + more
    if show:
        print(subprocess.list2cmdline(args))
    t = time.time()
    p = run_locked(args, cwd=card_folder(image))
    wall = time.time() - t
    err = p.stderr
    s = re.search(r'STATS seconds=([\d.]+) instructions=(\d+) ips=(\d+) frames=(\d+)', err)
    w = re.search(r'STATS workers: instructions=(\d+) ips=(\d+); all cores ips=(\d+)', err)
    queued = re.search(r'gpu_queue: waited (\d+) s', err)
    if not s or not w or '--until matched' not in err:
        print(f'{name}: not measured on the card ({wall:.0f} s; console in {log}): ' + ' '.join(err.strip().splitlines()[-2:]))
        return None
    secs, c0, passes = float(s[1]), int(s[2]), int(s[4])
    text = open(log, encoding='latin-1').read()
    text = text[text.rfind('=== rvc_harness'):]
    after = text[text.find(c['mark']):]
    frames = c['frames'](text) if 'rate' not in c else c['rate'](after)[0]
    r = dict(game=name, frames=frames, fps=game_fps(c, after, frames, secs), seconds=secs, passes_s=passes / secs,
             passes=0, core0=c0 / secs / 1e6, workers=int(w[2]) / 1e6, all=int(w[3]) / 1e6,
             wall=wall - (int(queued[1]) if queued else 0))
    r['passes'] = r['passes_s'] / r['fps'] if r['fps'] else 0   # (a game's lines may cover less than the window)
    extra = c['extra'](after) if 'extra' in c else ''
    if 'server' in c:
        sf, cf, ms = c['server'](after)
        if sf:
            r.update(server_fps=sf * 1000.0 / ms, server_passes=r['passes'] * cf / sf)
            extra = (extra + '; ' if extra else '') + f"the server {r['server_fps']:.1f} frames of the game a second, {r['server_passes']:.1f} passes each"
    binary = name.split('-')[0]   # (which image ran, by the guest's own sum of the game)
    md5 = re.search(r'([0-9a-f]{32})  /usr/bin/' + binary + r'(\.bin)?\s', text)
    extra = (extra + '; ' if extra else '') + (f"{binary} {md5[1][:8]}" if md5 else 'no md5sum line')
    print(f"{name} (card): {r['fps']:.1f} frames a second ({frames} frames, {secs:.1f} s from the mark); {r['passes_s']:.0f} passes "
          f"a second, {r['passes']:.2f} a frame; core 0 {r['core0']:.2f}M a second, workers {r['workers']:.2f}M, all cores "
          f"{r['all']:.2f}M" + (f"; {extra}" if extra else '') + f"; {r['wall']:.0f} s on the card")
    return r


def card_main(a):
    names = list(GAMES) if a.game == 'all' else a.game.split(',')
    images = a.image or [DEFAULT_IMAGE]
    rows = []
    for n in names:   # each game on every image in turn, so that the images' runs are close in time
        if n not in GAMES:
            sys.exit(f'no game {n}: ' + ', '.join(GAMES))
        for im in images:
            log = os.path.join(ROOT, 'build', 'game_par', f'card_{n}_{os.path.basename(os.path.abspath(im))}.log')
            r = run_card(n, GAMES[n], im, log, a.show, a.limit, a.rvc)
            if r:
                rows.append(dict(r, image=os.path.basename(os.path.abspath(im))))
    print('\n| Game | Image | Frames a second | Passes a second | Passes a frame | Core 0 M/s | Workers M/s | All cores M/s |')
    print('|---|---|---|---|---|---|---|---|')
    for r in rows:
        print(f"| {r['game']} | {r['image']} | {r['fps']:.1f} | {r['passes_s']:.0f} | {r['passes']:.2f} | {r['core0']:.2f} | "
              f"{r['workers']:.2f} | {r['all']:.2f} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', help='|'.join(GAMES) + '|all (with --card, several joined by commas)')
    ap.add_argument('--image', action='append', help='an image folder (default build/images/linux); with --card, may be given more than once')
    ap.add_argument('--log', help='the console (default build/game_par/GAME.log)')
    ap.add_argument('--calls', help='RVC_CALLS=FILE for tools/mc_candidates.py')
    ap.add_argument('--salt', type=int, default=0, help='N > 0: the command typed after ": N;", which shifts its timing')
    ap.add_argument('--runs', type=int, default=1, help='runs at salts SALT.. SALT+RUNS-1, and their mean')
    ap.add_argument('--profile', help="core 0's pc every 256 instructions from the mark, for tools/pc_profile.py")
    ap.add_argument('--show', action='store_true', help="print rvc_cpu's command line")
    ap.add_argument('--guest', action='append', default=[], help='NAME=VALUE exported in the guest before the game (rvc_cpu runs)')
    ap.add_argument('--plain', action='store_true', help='rvc_cpu without RVC_SOUND, RVC_WCACHE, RVC_ADFAULT and RVC_RACES (the counts before 15 October 2026)')
    ap.add_argument('--card', action='store_true', help='on the graphics card in real time, through tools/gpu_queue.ps1')
    ap.add_argument('--limit', type=int, default=240, help='with --card: seconds a run may take at most')
    ap.add_argument('--rvc', default='experiments/rvc_opt', help='with --card: the processor folder (e.g. a copy of experiments/rvc_lean)')
    a = ap.parse_args()
    if a.card:
        return card_main(a)
    image = a.image[-1] if a.image else None
    names = list(GAMES) if a.game == 'all' else [a.game]
    os.makedirs(os.path.join(ROOT, 'build', 'game_par'), exist_ok=True)
    rows = []
    for n in names:
        if n not in GAMES:
            sys.exit(f'no game {n}: ' + ', '.join(GAMES))
        log = a.log or os.path.join(ROOT, 'build', 'game_par', n + '.log')
        got = [r for r in (run(n, GAMES[n], image, log, a.calls, a.salt + k, a.show, a.profile, a.plain, a.guest) for k in range(a.runs)) if r]
        if got:
            mean = {k: sum(r[k] for r in got) / len(got) for k in ('core0', 'all', 'factor', 'workers', 'frames', 'passes', 'fps')}
            mean.update(game=n, spread=(max(r['core0'] for r in got) - min(r['core0'] for r in got)) / mean['core0'])
            rows.append(mean)
    if len(rows) > 1 or a.runs > 1:
        print(f'\n| Game | Frames | Core 0 a frame | All cores a frame | Factor | Workers | Passes a frame | Guest frames a second |' + (' Core 0 spread |' if a.runs > 1 else ''))
        print('|---|---|---|---|---|---|---|---|' + ('---|' if a.runs > 1 else ''))
        for r in rows:
            print(f"| {r['game']} | {r['frames']:.0f} | {r['core0'] / 1e3:.1f}k | {r['all'] / 1e3:.1f}k | {r['factor']:.2f} | {r['workers']:.0f} | {r['passes']:.2f} | {r['fps']:.1f} |"
                  + (f" {100 * r['spread']:.1f}% |" if a.runs > 1 else ''))


if __name__ == '__main__':
    main()
