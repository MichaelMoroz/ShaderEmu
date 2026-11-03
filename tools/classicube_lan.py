"""ClassiCube played together on the LAN (docs/games/classicube.md, "Played together"): one rvc_cpu machine hosts its world, one
or two more join it with the stock client, over rvc_cpu's link at the world's rate. Each joiner, once its picture is steady,
places a block and takes it away; the host says each join (the level's bytes, gzipped on the workers) and every five seconds
the bytes a second each way; a joiner says when its block went and when the host's echo came back.

    python tools/classicube_lan.py [--joiners 1|2] [--rate 4000] [--delay 115] [--image DIR] [--cpu EXE] [--logs DIR]

Prints what each machine said of the LAN, and: join seconds (from a joiner's start to its level's end), the round trip of a
block, bytes a second; exit 1 when a joiner did not get the level or a block did not come back.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CC = 'mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; nano-X -p & sleep 2; '


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--joiners', type=int, default=1)
    ap.add_argument('--rate', type=int, default=4000)
    ap.add_argument('--delay', type=int, default=115)
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'images', 'nommu'))
    ap.add_argument('--cpu', default=os.path.join(ROOT, 'bin', 'rvc_cpu.exe'))
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'classicube_lan'))
    ap.add_argument('--port', type=int, default=47400)
    ap.add_argument('--seconds', type=int, default=240)
    ap.add_argument('--races', action='store_true', help='RVC_RACES (and RVC_RACES_NEAR=4) on every machine, a file each')
    a = ap.parse_args()
    os.makedirs(a.logs, exist_ok=True)
    for f in os.listdir(a.logs):
        if f.endswith('.log'):
            os.remove(os.path.join(a.logs, f))
    env = dict(os.environ, RVC_THREADS=env_threads())
    def races(name):
        return dict(env, RVC_RACES=os.path.join(a.logs, name + '.races'), RVC_RACES_NEAR='4') if a.races else env
    net = ['--net', str(a.port), '--net-rate', str(a.rate), '--net-delay', str(a.delay)]
    procs = []
    host_log = os.path.join(a.logs, 'host.log')
    procs.append(subprocess.Popen([a.cpu, '--quiet', '--uart-log', host_log, '--wall', str(a.seconds + 120), '--seconds', str(a.seconds),
                                   '--image', a.image, '--net-id', '1', *net, '--expect', '/ # ',
                                   '--send', CC + 'CLASSICUBE_LAN=host CLASSICUBE_LAN_LOG=1 CLASSICUBE_SEED=7 classicube </dev/null\\n'],
                                  env=races('host'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for j in range(a.joiners):
        log = os.path.join(a.logs, f'joiner{j + 2}.log')
        # (the host's world is made and served from some 40 s of the guest's; a joiner's own start takes as long)
        cmd = CC + f'sleep {20 + 10 * j}; CLASSICUBE_LAN_LOG=1 classicube Player{j + 2} lan 10.0.0.1 25565'
        procs.append(subprocess.Popen([a.cpu, '--quiet', '--uart-log', log, '--wall', str(a.seconds + 120), '--seconds', str(a.seconds),
                                       '--image', a.image, '--net-id', str(j + 2), *net, '--expect', '/ # ', '--send', cmd + '\\n',
                                       '--expect', 'classicube: steady', '--send', 'turn 0 250\\ndown LeftMouse\\n',
                                       '--expect', 'lan: block', '--send', 'up LeftMouse\\n'],
                                      env=races(f'joiner{j + 2}'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(1)
    for p in procs:
        p.wait()
    bad = 0
    for name in ['host'] + [f'joiner{j + 2}' for j in range(a.joiners)]:
        lines = [l.rstrip() for l in open(os.path.join(a.logs, name + '.log'), encoding='latin-1') if 'lan:' in l or 'steady' in l]
        print(f'--- {name}')
        for l in lines[:40]:
            print('   ', l)
        if name != 'host':
            began = [int(m.group(1)) for l in lines for m in [re.search(r'lan: connecting, (\d+) ms', l)] if m]
            whole = [int(m.group(1)) for l in lines for m in [re.search(r'lan: the level came whole, (\d+) ms', l)] if m]
            if began and whole:
                print(f'    joined in {(whole[0] - began[0]) / 1000:.1f} s (from connecting to the level whole)')
            sent = [int(m.group(1)) for l in lines for m in [re.search(r'lan: block \d+ sent .*?, (\d+) ms', l)] if m]
            came = [int(m.group(1)) for l in lines for m in [re.search(r'lan: block \d+ arrived .*?, (\d+) ms', l)] if m]
            if sent and came:
                print(f'    a block\'s round trip: {came[0] - sent[0]} ms')
            else:
                print('    no block came back')
                bad += 1
    host = open(host_log, encoding='latin-1').read()
    rates = [(int(o), int(i)) for n, o, i in re.findall(r'lan: (\d+) joined, (\d+) bytes a second out, (\d+) in', host) if int(n) == a.joiners]
    if rates:
        rates = rates[1:] or rates   # (the first five seconds with everybody in have the last level in them)
        print(f'with {a.joiners} joined: the host sends {sum(o for o, i in rates) / len(rates):.0f} bytes a second and takes '
              f'{sum(i for o, i in rates) / len(rates):.0f} (means of {len(rates)} five-second spans)')
    joins = re.findall(r'lan: (Player\d+) joined: the level (\d+) bytes, gzipped on the workers to (\d+)', host)
    for who, raw, packed in joins:
        print(f'{who}: level {raw} bytes as {packed} gzipped; at {a.rate} bytes a second about {int(packed) / a.rate:.1f} s on the link')
    if len(joins) < a.joiners:
        print(f'only {len(joins)} of {a.joiners} joined')
        bad += 1
    if a.races:
        for f in sorted(os.listdir(a.logs)):
            if f.endswith('.races'):
                undone = sum(1 for l in open(os.path.join(a.logs, f), encoding='latin-1') if l.startswith('undone'))
                print(f'{f}: {undone} undone')
                bad += undone != 0
    return 1 if bad else 0


def env_threads():
    return str(max(1, (os.cpu_count() or 4) // 3))


if __name__ == '__main__':
    sys.exit(main())
