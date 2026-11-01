#!/usr/bin/env python3
"""Each load of the account under tools/prof.py on rvc_cpu: python prof_all.py [LOAD ...]. Writes logs/prof_LOAD/ and logs/prof_LOAD.txt."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
LOADS = {
    'boot': ('', 'x', ''),
    'gzip': ('seq 1 20000 > /tmp/g', 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null', 'busybox'),
    'jpeg': ('', 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm', 'nxview'),
    'png': ('', 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm', 'nxview'),
    'mp3': ('', 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63', 'media_test'),
    'mp3_1': ('', 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0', 'media_test'),
    'flac': ('', 'media_test audio /usr/share/play-test.flac /tmp/o.raw 63', 'media_test'),
    'mpeg': ('', 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8', 'media_test'),
    'avi': ('', 'media_test video "/root/Videos/Big Buck Bunny.avi" 16', 'media_test'),
    'jpeg_ns': ('', 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm', 'nxview'),
    'png_ns': ('', 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm', 'nxview'),
    'mpeg_ns': ('', 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.mpg" 8', 'media_test'),
    'avi_ns': ('', 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.avi" 16', 'media_test'),
    'tdawn': ('nano-X -p & sleep 2', 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL', 'tdawn'),
}
exe = os.path.join(MAIN, 'bin', 'rvc_cpu.exe')
image = os.path.join(MAIN, 'build', 'images', 'nommu')
for load in sys.argv[1:] or LOADS:
    pre, cmd, find = LOADS[load]
    out = os.path.join(HERE, 'logs', 'prof_' + load)
    if load == 'boot':
        args = ['--image', image, '--quiet', '--expect', '/ # ', '--send', "echo LX-''DONE\\n", '--until', 'LX-DONE', '--seconds', '60']
    else:
        send = (pre + '; ' if pre else '') + "echo FL-''GO; " + cmd + "; echo LX-''DONE\\n"
        args = ['--image', image, '--quiet', '--expect', '/ # ', '--send', send, '--count-from', 'FL-GO', '--until', 'LX-DONE', '--seconds', '400']
    r = subprocess.run([sys.executable, os.path.join(TREE, 'tools', 'prof.py'), 'run', out, '--exe', exe, '--'] + args, cwd=MAIN, capture_output=True, text=True, errors='replace')
    print(load, 'run', r.returncode, r.stdout[-200:], r.stderr[-200:], flush=True)
    rep = [sys.executable, os.path.join(TREE, 'tools', 'prof.py'), 'report', out, '--top', '40']
    if load == 'tdawn': rep += ['--elf', '~/nm/work/src/tdawn-build/vanillatd']
    elif find: rep += ['--find', find]
    r = subprocess.run(rep, cwd=MAIN, capture_output=True, text=True, errors='replace')
    open(out + '.txt', 'w', encoding='utf-8').write(r.stdout + r.stderr)
    print(load, 'report', r.returncode, len(r.stdout), flush=True)
