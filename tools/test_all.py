"""Every program's own checks on the Linux image, under bin/rvc_cpu.exe, several at a time
(docs/machine/cpu-harness.md, "The test set"). One PASS or FAIL line a case, then a summary; exit 1 on
any failure.

    python tools/test_all.py                    # the default set (some 5 minutes at most)
    python tools/test_all.py games doom-1 ...   # cases by name or tag (machine, games, desktop, media, long, ...)
    python tools/test_all.py --long             # the default set and the long cases
    python tools/test_all.py --races            # RVC_RACES on every case: 0 undone (and RVC_RACES_NEAR=4's count said)
    python tools/test_all.py --doubles          # each game's share in soft doubles under its limit (run again to go on)
    python tools/test_all.py --image DIR --jobs 8 --list

A case is the line typed at the guest's shell, what ends it, and what its console must say:
`has` lines (regular expressions), `sums` (a pattern's captures in order), `digest` (the first
8 hex of the md5 of every match, for long lists of state sums kept "as before" by the docs) and
`same` (another case's matches, which must be the same). The values are the docs' (each says
where) or, where a doc says only "as before", what the image of 15 October 2026 printed.
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DONE = "echo LX-''DONE"
X = 'nano-X -p & sleep 2; '
WM = 'export NXBAR_PRINT=1; nx; sleep 3; '   # the real desktop
WMC = 'export NXBAR_CLOSE_MS=1000; ' + WM   # a second for a close
F10 = '\x1b[21~'   # (the console's TERM is linux)
CC = 'mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; ' + X
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
# where place X, Y of a SW x SH game screen is on the display, the game's window (title T) shown scaled by one factor in the middle
# (docs/games/tdawn.md, "The window's size"): at SW SH X Y clicks there
WIN_AT = ('at() { SW=$1; SH=$2; X=$3; Y=$4; set -- $(nxwintest list | grep "$T"); P=${5%,*}; Q=${5#*,}; S=$6; A=$((${S%x*}-6)); '
          'B=$((${S#*x}-29)); if [ $((A*SH)) -le $((B*SW)) ]; then PW=$A; PH=$((A*SH/SW)); else PH=$B; PW=$((B*SW/SH)); fi; '
          'nxkey $((P+3+(A-PW)/2+X*PW/SW)),$((Q+26+(B-PH)/2+Y*PH/SH)); }; ')
Q3 = 'QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17'
NES = X + 'NES_SUM=1 NES_PPU=inline '
NES_RUNS = (('thwaite-120', 'NES_FRAMES=120 nes thwaite', '31a18295', 'afcb4a98'),
            ('thwaite-180', 'NES_FRAMES=180 NES_PRESS="20 start 50 start 80 a 100 left 110 a" nes thwaite', '2f58587d', '39c90e07'),
            ('nova-120', 'NES_FRAMES=120 nes nova', '3a2e5c47', 'd8a04406'))
STATE = r'(tic \d+ state [0-9a-f]+)'
# media files with no ending, an ending in capitals, or only an ending, all with the execute bit (docs/system/open.md)
KINDS_T = ('mkdir -p /tmp/t; cd /tmp/t; cp "/root/Music/Grieg - Hall of the Mountain King.ogg" song; cp /usr/share/play-test.wav tone; '
           'cp "/root/Videos/Big Buck Bunny.avi" film; cp /root/Pictures/Fox.png fox; cp /usr/share/play-test.flac A.FLAC; '
           'for e in tga pnm pbm oga vqa xyz; do echo x > a.$e; done; chmod +x *; cd /; ')


# Age of Empires' script at its window's size (tools/game_par.py has the same)
# Age of Empires' trial unpacked from its installer in the ROM before the window system starts: its licence asked on the terminal,
# answered (docs/system/install.md); the game reads the files in /tmp/aoe
AOE_UNPACK = 'if [ -d /usr/share/aoe/installed ]; then echo yes | aoe --unpack > /dev/null; fi; '
# Half-Life's licence answered on the terminal before the window system starts (docs/system/install.md): then the game starts at once
HL_LICENCE = 'if [ -f /usr/share/halflife/licence/eula.txt ]; then echo yes | halflife --licence > /dev/null; fi; '
AOE_640 = '3000 click 320 196;3000 click 320 138;3000 click 190 454;2000 click 289 58;1000 rclick 425 110;1000 click 290 155;1000 click 163 388'


def case(name, tags, cmd, workers=False, until='LX-DONE', more=(), wall=240, image=None, env=None, **checks):
    return dict(name=name, tags=set(tags.split()), cmd=cmd, workers=workers, until=until, more=list(more), wall=wall,
                image=image, env=env or {}, **checks)


# GitHub issue 1's server, as its reporter wrote it (its `test` printing what it serves), and what they typed
WEB_ISSUE = ("printf 'echo hello from the issue %s server\\\\n' shell > /tmp/test; (while :; do { printf 'HTTP/1.1 200 OK\\\\r\\\\n'; "
             "printf 'Content-Type: text/plain\\\\r\\\\n'; printf 'Connection: close\\\\r\\\\n'; printf '\\\\r\\\\n'; sh /tmp/test; } | nc -l -p 80 >/dev/null; done) & "
             "sleep 2; echo CURL $(curl -s http://127.0.0.1/); NXWEB_DUMP=1 nxweb http://127.0.0.1/ & sleep 15; killall nxweb; sleep 1; "
             "NXWEB_DUMP=1 nxweb http://10.0.0.11 & sleep 15; killall nxweb; sleep 1; NXWEB_DUMP=1 nxweb http://127.0.0.1:81/ & sleep 8; killall nxweb")
# thttpd's site: a page with a sheet, a picture and a form; a CGI that sets a cookie and sends the form on by a 303; one that shows it
WEB_SITE = ("mkdir -p /tmp/www/cgi-bin; cd /tmp/www; cp /root/Pictures/Fox.png .; chmod 644 Fox.png; dd if=/dev/zero of=a.bin bs=100 count=20 2>/dev/null; echo plain text > plain.txt; "
            "echo 'body{color:#800}' > page.css; echo '<title>Local</title><link rel=stylesheet href=page.css><h1>A page</h1><img src=Fox.png width=64 height=64>"
            "<form method=post action=cgi-bin/go.cgi><input name=who value=abc><input type=checkbox name=box checked><input type=submit></form>' > index.html; "
            "printf '#!/bin/sh\\\\nread -n $CONTENT_LENGTH q; echo POSTED $q > /dev/console; echo Status: 303 See; echo Location: /cgi-bin/show.cgi; echo Set-Cookie: sid=s1; echo\\\\n' > cgi-bin/go.cgi; "
            "printf '#!/bin/sh\\\\necho SAW $HTTP_COOKIE > /dev/console; echo Content-Type: text/html; echo; echo shown\\\\n' > cgi-bin/show.cgi; "
            "chmod 755 cgi-bin/*; thttpd -p 8080 -d /tmp/www -c '**.cgi' -u root -l /dev/null & ")
WEB_HTTP = ("echo '127.0.0.1 webhost' >> /etc/hosts; NXWEB_DUMP=1 NXWEB_PRINT=1 nxweb http://localhost:8080/ & sleep 25; nxkey tab x y z 1 tab tab enter; sleep 10; "
            "nxkey ctrl+l h t t p 'shift+;' / / l o c a l h o s t 'shift+;' 8 0 8 0 / n o n e enter; sleep 6; "
            "nxkey ctrl+l l o c a l h o s t 'shift+;' 8 0 8 0 / a . b i n enter; sleep 6; echo SAVED $(wc -c < /root/Downloads/a.bin); "
            "nxkey ctrl+l l o c a l h o s t 'shift+;' 8 0 8 0 / p l a i n . t x t enter; sleep 6; nxkey alt+left; sleep 3; killall nxweb; sleep 1; "
            "NXWEB_TIME=1 NXWEB_DUMP=1 nxweb http://webhost:8080/ & sleep 15; killall nxweb; sleep 1; NXWEB_DUMP=1 nxweb http://10.0.0.12:8080/ & sleep 15; killall nxweb; echo WEB-''ENDED\\n")

WEB_HEAD = "printf 'HTTP/1.1 200 OK\\\\r\\\\nContent-Type: text/plain\\\\r\\\\n\\\\r\\\\n'; seq 1 900; "
WEB_SLOW = ("(" + WEB_HEAD + "sleep 12; echo last) | nc -l -p 8081 > /dev/null & sleep 1; NXWEB_DUMP=1 NXWEB_PRINT=1 nxweb http://127.0.0.1:8081/ & sleep 7; nxkey pagedown; sleep 1; nxkey pagedown; sleep 12; killall nxweb; "
            "(" + WEB_HEAD + "sleep 30) | nc -l -p 8082 > /dev/null & sleep 1; NXWEB_DUMP=1 NXWEB_PRINT=1 nxweb http://127.0.0.1:8082/ & sleep 1; nxkey escape; sleep 5; killall nxweb; killall nc; "
            "printf 'HTTP/1.1 200 OK\\\\r\\\\nTransfer-Encoding: chunked\\\\r\\\\nContent-Type: text/html\\\\r\\\\n\\\\r\\\\n5\\\\r\\\\n<p>ab\\\\r\\\\n0\\\\r\\\\n\\\\r\\\\n' | nc -l -p 8083 > /dev/null & "
            "sleep 1; NXWEB_DUMP=1 nxweb http://127.0.0.1:8083/ & sleep 7; killall nxweb")

CASES = [
    # the machine (docs/machine/multicore.md; programs/mc/mctest.c)
    case('mctest-3', 'machine', 'mctest 3', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
    case('mctest-255', 'machine', 'mctest 255 30000', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
    # workers closed and opened again in one program (the counters mapped at each): as many every time, and rounds on them
    case('mctest-reopen', 'machine workers', 'mctest reopen; mctest reopen lend 100000', True, count=[(r'^mctest: open \d: 255 workers, sum 1409965408', 8), (r'^mctest: reopen PASS', 2)]),
    # lanes (docs/machine/lanes.md): none on rvc_cpu, so plain calls on every worker, compared with themselves;
    # one index a share longer than a pass, which a lane would leave to its worker
    case('lanetest', 'machine', 'lanetest 255 50 1 40; lanetest 31 40 3 20', True, count=[(r'^lanetest: PASS', 2)]),
    case('steptest', 'machine', 'steptest 31 3000', True, count=[(r'^steptest: PASS', 1)]),
    # a double's printf in doubles (linux/userland/vfprintf_double.py) against the long double's, and the host glibc's sum
    case('printf', 'machine', 'printf_check 500', has=[r'printf_check: 0 of 8000 differ, sum \S+, without %a a284a76e']),
    # the HLSL compiler in the machine (docs/system/hlsl.md): the fractal example compiled there and drawn on 31 workers'
    # lanes in steps, the host's sum (programs/hlsl/test/hlsl_test.c with the same arguments)
    case('hlsl', 'machine', 'HLSL_WORKERS=31 hlsl_bench /usr/share/example-fractal.hlsl 160 120 1 eye=1.9095,1.2344,-2.7911 '
         'forward=-0.53042,-0.3429,0.77531 right=0.82534,0,0.56464 up=-0.19362,0.9394,0.28301 size=160,120 turn=1,0,0,0,1,0,0,0,1',
         True, has=[r'hlsl_bench: compiled in \d+ ms: .* 0 spilled', r'hlsl_bench: sum 3614e8bb']),
    case('fptest', 'machine', 'fptest', has=[r'fptest: done, 0 of 15 instructions wrong']),   # docs/machine/fpu.md
    # zerotest and tailtest are in no image: --long builds them into build/images/test_all (docs/machine/boot.md)
    case('zero-tail', 'machine long', 'zerotest 12 4; tailtest /usr/share/* /usr/bin/* /etc/*', wall=400, image='zerotail',
         has=[r'zerotest: PASS', r'tailtest: PASS']),
    case('lazy', 'machine', None, image='lazy', until=' done', wall=60,   # docs/machine/boot.md: twenty lines ok
         has=[r'lazy: sum 1a4a1226'], count=[(r'^lazy: .* ok', 20)]),
    # cc's outputs, the same bytes (docs/system/cc.md): cc, cc -g and tcc -c of the four examples
    case('cc-bytes', 'machine', 'cd /tmp; for e in hello cube home shader; do cc /usr/share/example-$e.c -o $e.x; '
         'cc -g /usr/share/example-$e.c -o $e.g; tcc -c /usr/share/example-$e.c -o $e.o; done; md5sum *.x *.g *.o', wall=300,
         digest=[(r'^([0-9a-f]{32}  \S+)', '0527d60e')]),   # mcw.c with mcw_mmu_root: the linked programs
    # docs/machine/gpu.md, "Copies": a framebuffer object drawn as a list of its own and copied into its texture, a copy of
    # the window's corner, and SEGL_DEBUG saying the draw made after a copy on frame 5 (rvc_cpu draws no list: this is the
    # library's side; the pictures are rvc_harness --cpu's)
    case('gl-copies', 'machine', 'cd /tmp; cc /usr/share/cc-tests/glcopy.c -o glcopy; ' + X + '(SEGL_DEBUG=1 ./glcopy 2>&1 &) ; sleep 60',
         until='glcopy: done', wall=300, has=[r'^glcopy: framebuffer complete', r'^segl: a draw \(command 3\) after a copy in the same list', r'^glcopy: done'],
         never=[r'incomplete']),    case('libmedia-installed', 'machine media', 'ls -1 /usr/lib/libmedia.a /usr/include/media.h /usr/bin/media_test | cat',   # docs/system/media.md
         has=[r'^/usr/lib/libmedia\.a', r'^/usr/include/media\.h'], never=[r'No such file']),
    # unarc against busybox's unzip and tar (docs/system/open.md; one deflate stream on the workers, docs/system/media.md)
    case('unarc', 'machine media', 'cd /tmp; mkdir z1 z2 t1 t2; unarc zip /usr/share/classicube-default.zip z1 >/dev/null; '
         'unzip -q /usr/share/classicube-default.zip -d z2; tar czf u.tgz /usr/share/font-mono-1*.sef /usr/share/web-*.html 2>/dev/null; '
         'unarc tgz u.tgz t1 >/dev/null; tar xzf u.tgz -C t2; diff -r z1 z2 && echo ZIP-SAME; diff -r t1 t2 && echo TGZ-SAME', True,
         has=[r'^ZIP-SAME', r'^TGZ-SAME']),
    # Doom (docs/games/doom.md, "Checking it"): the demos' state sums, the view told and checked, the port at rest
    *[case(f'doom-{d}', 'games doom', X + f'DOOM_HASH_TICS=100 doom -1 -timedemo demo{d}', True,
           digest=[(STATE, s)]) for d, s in ((1, '0ab4df93'), (2, 'c5123ba8'), (3, 'df43029a'))],
    case('doom-told', 'games doom', X + 'DOOM_VIEW=told DOOM_VIEW_CHECK=1 DOOM_HASH_TICS=100 doom -1 -timedemo demo1', True,
         has=[r'doomcheck: 5000 views, 0 not as told'], digest=[(STATE, '0ab4df93')]),
    case('doom-rest', 'games doom', X + 'DOOM_PORT=rest DOOM_HASH_TICS=100 doom -1 -timedemo demo1', True, digest=[(STATE, '0ab4df93')]),
    # Quake (docs/games/quake.md, "Checking it"): the server's sums in three modes, particles, light, a walk, an exit
    *[case(f'quake-server-{m}', 'games quake', X + f'QUAKE_SERVER={m} QUAKE_SUM=10 quake +map e1m5 +host_framerate 0.05', True,
           until='server frame 610:', more=['--fixed-dt', '0.004'], digest=[(r'(server frame (?:[1-9]|[1-5]\d|60)0: .* state [0-9a-f]{8})', '52463017')])
      for m in ('inline', 'late', 'wait')],
    case('quake-particles', 'games quake', X + 'QUAKE_PARTICLES=wait QUAKE_PARTICLE_SUM=1 quake +timedemo demo1 +host_framerate 0.05', True,
         until=' fps', digest=[(r'(particles at frame \d+: [0-9a-f]+)', '9c67f307')]),
    case('quake-light', 'games quake', X + 'QUAKE_LIGHTPOINT=check quake +timedemo demo1', True, until=' fps',
         has=[r'0 not as made here \(QUAKE_LIGHTPOINT=check\)'], never=[r'[1-9]\d* not as made here']),
    case('quake-walk', 'games quake', X + 'QUAKE_TRACK=5 quake +map e1m1 +host_framerate 0.05', True, until='track 150:',
         more=['--expect', 'track 30:', '--send', '+forward\\n'], has=[r'track 140: .* at 480 687 24']),
    case('quake-exit', 'games quake', X + 'QUAKE_TRACK=5 QUAKE_TEST_EXIT=40 quake +map e1m1', True, until='track 400:',
         more=['--expect', 'track 80:', '--send', '+attack\\n'], wall=300, has=[r'maps/e1m2\.bsp']),
    # Quake III (docs/games/quake3.md, "The client's frame on three cores"): typed the same length; the
    # client's game on its worker against the first core differs early, as before, so it stays on its worker
    case('quake3-workers', 'games quake3', X + 'QUAKE3_CLIENT=worker QUAKE3_SCENE=worker QUAKE3_SIDE=worker QUAKE3_SUM=1 QUAKE3_SERVER=inline ' + Q3,
         True, until='q3stat: frame 600:', count=[(r'^q3sum: ', (500, 600))]),
    case('quake3-inline', 'games quake3', X + 'QUAKE3_CLIENT=worker QUAKE3_SCENE=inline QUAKE3_SIDE=inline QUAKE3_SUM=1 QUAKE3_SERVER=inline ' + Q3,
         True, until='q3stat: frame 600:', same=[(r'^q3sum: (\d+) (.*)', 'quake3-workers', 500)]),
    case('quake3-server', 'games quake3', X + 'QUAKE3_SEED=7 QUAKE3_SERVER=inline QUAKE3_CLIENT=inline QUAKE3_SCENE=inline QUAKE3_SUM=1 ' + Q3 +
         ' +addbot sarge 3 +wait 100 +setviewpos 488 -968 378 90', True, until='q3stat: frame 700:', count=[(r'^q3srvsum: ', 223)],
         digest=[(r'^(q3srvsum: .*)', 'aaa1379a')]),
    # QUAKE3_ENTER: the player enters at 1000 ms of the game, not when its load ends (docs/games/quake3.md)
    case('quake3-bots-serial', 'games quake3 long', X + 'QUAKE3_ENTER=1000 QUAKE3_BOTS=serial QUAKE3_SUM=1 QUAKE3_SEED=7 QUAKE3_SERVER=inline ' + Q3 + ' +addbot sarge 3' * 3,
         True, until='q3stat: frame 700:', wall=400, count=[(r'^q3srvsum: ', 223)]),
    case('quake3-bots', 'games quake3 long', X + 'QUAKE3_ENTER=1000 QUAKE3_BOTX=serial QUAKE3_SUM=1 QUAKE3_SEED=7 QUAKE3_SERVER=inline ' + Q3 + ' +addbot sarge 3' * 3,
         True, until='q3stat: frame 700:', wall=400, same=[(r'^(q3srvsum: .*)', 'quake3-bots-serial')]),
    # ClassiCube (docs/games/classicube.md, "Checking it"): 52 meshes, 68,964 vertices at the start; the pack's sums the raw pack's
    case('classicube-meshes', 'games classicube', CC + 'CLASSICUBE_MESH=inline CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^ccmesh: ', 52)], digest=[(r'^(ccmesh: .*)', '7f763849')]),
    case('classicube-meshes-workers', 'games classicube', CC + 'CLASSICUBE_MESH=worker CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^ccmesh: ', 52)], digest=[(r'^ccmesh: at (\d+ \d+ \d+), \d+ vertices, sums \S+ (\S+)', '1f9d723c', 'sorted')]),
    case('classicube-pack', 'games classicube', CC + 'CLASSICUBE_TEXTURE_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^cctex: \d+ x (?!26,)\d+, ', 30), (r'^cctex: \d+ x 26,', (10, 12))],
         digest=[(r'^(cctex: \d+ x (?!26,)\d+, .*)', '43d1cb6b')]),   # (RAM in one texture: 42 by then. The 26 high are lines of text, whose words and how many are made by steady go by how fast it starts: counted, not summed)
    case('classicube-kept', 'games classicube', CC + 'CLASSICUBE_CHECK_KEPT=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube', True,
         until='cccheck:', more=['--expect', 'classicube: steady', '--send', 'turn 1200 0\\ndown W\\ndown Space\\n'],
         has=[r'cccheck: the outline kept \d+ times, 0 of them wrong; the held block kept \d+ times, 0 wrong']),
    # docs/world/holodeck.md, "Played from the seat": a desktop sitter breaks by the right trigger and places by
    # the left one, at the view's middle; in a headset each hand's beam picks its own block (small blocks).
    # A button held as play starts is no press until let go: each is pressed once the world is loaded (20 s;
    # 18 s with 256 MB of RAM, whose pages the kernel takes 1.4 s longer to set up)
    *[case(f'classicube-seat-{n}', 'games classicube', CC + 'CLASSICUBE_SEAT=1 CLASSICUBE_SEED=7 classicube', True, until=' -> ', wall=60,
           more=['--host', b, '--host-from', '20', w], has=[h]) for n, b, w, h in (
        ('desk-break', '00000112,e0000000,0,0,0,0,0,0', '00000112,e0000000,0,0,0,0,40000000,0', r'ccblock: 32 40 31 2 -> 0'),
        ('desk-place', '00000112,e0000000,0,0,0,0,0,0', '00000112,e0000000,0,0,40000000,0,0,0', r'ccblock: 32 41 31 0 -> 1'),
        ('vr-break', '90000133,0,0,c0000000,04bea3da,0,04bea026,e0000000', '90000133,0,0,c0000000,04bea3da,0,44bea026,e0000000',
         r'ccblock: 34 40 28 2 -> 0'),
        ('vr-place', '90000133,0,0,c0000000,04bea3da,0,04bea026,e0000000', '90000133,0,0,c0000000,44bea3da,0,04bea026,e0000000',
         r'ccblock: 30 41 28 0 -> 1'))],
    # and the block in hand: a desktop's wheel, the right stick held up, the left grip opens the inventory
    *[case(f'classicube-seat-{n}', 'games classicube', CC + 'CLASSICUBE_SEAT=1 CLASSICUBE_SEED=7 classicube', True, until=u, wall=60,
           more=['--host', w] + m, has=[h]) for n, w, m, u, h in (
        ('desk-wheel', '00000112,e0000000,0,0,0,0,0,0', ['--host-wheel', '3'], 'slot 4', r'seat: 2 .* slot [5-8] holds \d+ raw 1'),
        ('vr-stick', '90000133,0,0,c0000000,04bea3da,7f000000,04bea026,e0000000', [], 'slot 3', r'seat: 1 .* slot 7 holds \d+ raw 1'),
        ('vr-grip', '90000133,0,0,c0000000,04bea3da,0,04bea026,e0000000', ['--host-from', '20', '90000133,0,0,c0000000,84bea3da,0,04bea026,e0000000'],
         'raw 0', r'seat: 1 .* slot 0 holds 1 raw 0 menu 1'),
        # the inventory on the sheet: the grip opens it, then the right hand's beam moves onto row 6 and its trigger picks a block
        ('vr-pick', '90000133,0,0,c0000000,04bea3da,0,04bea026,2400fc72',
         ['--host-from', '20', '90000133,0,0,c0000000,84bea3da,0,04bea026,2400fc72',
          '--host-from', '24', '90000133,0,0,c0000000,84bea3da,0,44bea026,2600f8e4'], 'holds 58 raw 1 menu 0', r'seat: 1 .* holds 58 raw 1 menu 0'))],
    # docs/games/classicube.md, "Sound": a dig sound on the card, and the tune decoded on workers into its stream
    case('classicube-sound', 'games classicube sound', CC + 'CLASSICUBE_SOUND_LOG=1 CLASSICUBE_SEAT=1 CLASSICUBE_SEED=7 classicube', True,
         until='ccmusic: 16 of', more=['--host', '00000112,e0000000,0,0,0,0,0,0', '--host-from', '20', '00000112,e0000000,0,0,0,0,40000000,0'],
         env={'RVC_SOUND': '1'},
         has=[r'ccsound: 59 sounds at', r'ccsound: dig_grass\d on voice \d+, volume 100, rate 80',
              r'ccmusic: /usr/share/classicube-music.mp3, 22050 Hz 2 ch']),
    # Tiberian Dawn and Red Alert (docs/games/tdawn.md, docs/games/ralert.md)
    case('tdawn-300', 'games tdawn', X + TD + 'TDAWN_SCEN=10 TDAWN_FRAMES=300 tdawn FROMINSTALL', True, has=[r'done at frame 300: SCG10EA, .* state f7650705']),
    case('tdawn-1300', 'games tdawn', X + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL', True, has=[r'done at frame 1300: SCG03EA, .* state 1a9a814b']),
    case('tdawn-paths', 'games tdawn', X + TD + 'TDAWN_SCEN=3 TDAWN_CHECK_PATHS=1 TDAWN_FRAMES=1300 tdawn FROMINSTALL', True,
         has=[r'searches not run again and \d+ another core.s, 0 of them wrong', r'edge walks made both ways, 0 of them wrong', r'state 1a9a814b']),
    case('tdawn-move', 'games tdawn long', X + TD + 'TDAWN_SCEN=3 TDAWN_MOVE=100,8,0 TDAWN_SUMS=1 TDAWN_FRAMES=600 tdawn FROMINSTALL', True, count=[(r'^tdsum: ', 600)]),
    case('tdawn-move-refresh', 'games tdawn long', X + TD + 'TDAWN_SCEN=3 TDAWN_MOVE=100,8,0 TDAWN_SUMS=1 TDAWN_REFRESH=1 TDAWN_FRAMES=600 tdawn FROMINSTALL', True,
         same=[(r'^(tdsum: .*)', 'tdawn-move')]),
    # docs/games/tdawn.md, "The window's size": the window made wide (792 x 300): the screen laid out at 528 x 200 (its height kept, the
    # picture scaled by 1.5); the sidebar opened (Tab), the window maximised (794 x 545, narrower than the game's shape): 320 x 220, as tall as
    # the window's shape (no black above and below), a click on a unit selects it and one on the sidebar's Repair presses it, both through that
    # scaling; put back, 528 x 200 and Repair at the right edge. WIN_AT says where a place of a SW x SH screen is on the display.
    case('tdawn-window', 'games tdawn desktop', WIN_AT + 'nx; sleep 3; (' + TD + 'TDAWN_SCEN=10 TDAWN_INPUT_LOG=1 TDAWN_RESIZE=792x300 tdawn FROMINSTALL '
         '2>&1 | grep -v tdstat &); T=Tiberian; sleep 60; nxkey tab; sleep 3; nxkey ctrl+f10; sleep 20; at 320 220 82 89; sleep 5; at 320 220 258 84; '
         'sleep 5; nxkey ctrl+f10; sleep 20; at 528 200 466 84; sleep 5; killall game', True, wall=600,
         has=[r'tdawn: the screen laid out at 528x200 \(the view 528x192\)', r'tdawn: the screen laid out at 320x220 \(the view 240x212\)',
              r'tdawn: 1 selected', r'tdawn: the screen laid out at 528x200 \(the view 448x192\)'],
         count=[(r'tdawn: the sidebar.s repair button pressed', 2)]),
    # docs/games/ralert.md, "The window's size": the same for Red Alert: wide, 1,056 x 400 (scaled by 0.75), a soldier selected and Repair pressed
    # at the right edge through the scaling; maximised, 640 x 440, as tall as the window's shape (the view 480 x 424), and Repair where it always was;
    # put back, 1,056 x 400 again (going wide moves the view to the map's left edge, as Set_View_Dimensions confines it: the clicks on the map
    # come first)
    case('ralert-window', 'games ralert desktop', WIN_AT + 'nx; sleep 3; (TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 TDAWN_INPUT_LOG=1 '
         'TDAWN_RESIZE=792x300 ralert 2>&1 | grep -v ralstat &); T="Red Alert"; sleep 90; at 1056 400 631 199; sleep 5; at 1056 400 924 158; sleep 5; '
         'nxkey ctrl+f10; sleep 25; at 640 440 508 158; sleep 5; nxkey ctrl+f10; sleep 25; killall game', True, wall=600,
         has=[r'ralert: the screen laid out at 1056x400 \(the view \d+x384\)', r'ralert: the screen laid out at 640x440 \(the view 480x424\)',
              r'ralert: [1-9]\d* selected'],
         count=[(r'ralert: the sidebar.s repair button pressed', 2), (r'ralert: the screen laid out at 1056x400', 2)]),
    # docs/games/tdawn.md, "A mission won": the demo's first mission won, its score screen, the next mission played
    case('tdawn-won', 'games tdawn', X + TD + 'TDAWN_SCEN=1 TDAWN_WIN=100 TDAWN_FRAMES=300 tdawn FROMINSTALL', True, wall=400,
         has=[r'^tdawn: score: ends', r'done at frame 300: SCG10EA, .* state 325b1830'], never=[r'Segmentation fault', r'out of memory']),
    *[case(f'ralert-{side}-{f}', 'games ralert', X + f'TDAWN_AUTO=1 RALERT_SIDE={side} RALERT_SEED=7 RALERT_FRAMES={f} ralert', True,
           has=[rf'done at frame {f}: .* state {s}']) for side, f, s in (('soviet', 300, '01e0dd60'), ('allies', 300, 'ea72a5d1'),
                                                                        ('soviet', 1300, 'eb6c2265'), ('allies', 1300, 'dadd663d'))],
    case('ralert-scans', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_CHECK_SCANS=1 RALERT_FRAMES=1300 ralert', True,
         has=[r'scan bits kept in \d+ frames of 1280, 0 of them wrongly'], never=[r'[1-9]\d* of them wrongly']),
    case('ralert-movie', 'games ralert', X + 'RALERT_MOVIE_CHECK=1 TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=20 ralert', True,
         wall=400, has=[r'movie check: 0 of 156 shared frames differ']),
    # docs/games/ralert.md, "A base destroyed": a base attacked to the atlas's filling (the build before crashed in it), and
    # the atlas's shapes forgotten every 50 scenes giving the same pictures
    case('ralert-attack', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 '
         'RALERT_FRAMES=3000 ralert', True, wall=400, has=[r'done at frame 3000: .* state dbb267c0']),
    # docs/games/ralert.md, "A unit's path searches": every search kept or made by another core, and every edge walk, made again
    case('ralert-paths', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 '
         'RALERT_CHECK_PATHS=1 RALERT_FRAMES=1500 ralert', True, wall=400,
         has=[r'searches not run again and [1-9]\d* another core.s, 0 of them wrong', r'edge walks made both ways, 0 of them wrong',
              r'done at frame 1500: .* state 520254a5']),
    *[case(f'ralert-atlas{tag}', 'games ralert', X + pre + 'RALERT_DRAW=here RALERT_REFRESH=1 TDAWN_SUMS=1 TDAWN_AUTO=1 RALERT_SIDE=allies '
           'RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 RALERT_FRAMES=300 ralert', True, **checks)
      for tag, pre, checks in (('', '', dict(count=[(r'^rasum: ', 300)])),
                               ('-forgotten', 'RALERT_ATLAS=50 ', dict(same=[(r'^(rasum: .*)', 'ralert-atlas')])))],
    # docs/games/ralert.md, "A mission won": won twice, the score screen's table and rows against the game's own, the
    # demo's menu again (it has no next mission: the build before asked for "out of memory"), then 300 frames
    case('ralert-won', 'games ralert', X + 'TDAWN_INTERPOLATION=check TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_WIN=100,2 '
         'RALERT_FRAMES=300 ralert', True, wall=400,
         has=[r'done at frame 300: SCG02EA\.INI, .* state ea72a5d1'],
         count=[(r"^ralert: the demo's menu again", 2), (r'^interpolation: 0 of 65536 entries differ', 2), (r'^interpolation: 0 of 399 rows differ', 22)],
         never=[r'^interpolation: [1-9]', r'Segmentation fault', r'out of memory']),
    # the Soviet side's won (docs/games/ralert.md, "A mission won"): its score screen's animation reads its frame table, at
    # an odd address, by bytes
    case('ralert-won-soviet', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_WIN=100,2 RALERT_FRAMES=300 ralert',
         True, wall=400, has=[r'done at frame 300: SCU01EA\.INI, .* state 01e0dd60'], count=[(r"^ralert: the demo's menu again", 2)],
         never=[r'Segmentation fault', r'out of memory']),
    # NES (docs/games/nes.md, "Testing"): RAM and picture with the lists on the first core and on a worker
    *[case(f'nes-{n}{tag}', 'games nes', NES + pre + c, True, sums=[(r"every frame's RAM ([0-9a-f]+)", [ram]), (r'done, picture ([0-9a-f]+)', [pic])])
      for n, c, ram, pic in NES_RUNS for tag, pre in (('', ''), ('-lists', 'NES_LIST=worker '))],
    case('nes-cells', 'games nes', X + 'NES_CELLS_CHECK=1 NES_LIST=worker NES_SOUND=0 NES_MARK=100 NES_FRAMES=1000 NES_PRESS="20 start 50 start 80 a '
         '100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a 800 a 900 -right 1000 a 1100 +left 1300 a" nes thwaite', True,
         has=[r'0 of 1000 frames differ']),
    case('nes-kill', 'games nes', 'mctest 3 120000; ' + X + 'nes nova & sleep 8; killall -9 nes.bin; sleep 1; mctest 3 120000', True,
         count=[(r'^mctest: PASS', 2)]),
    # nxphys (docs/apps/physics.md): the wall with every worker, with none, the tower, the heap, a throw
    *[case(f'nxphys-{n}', 'games nxphys', X + pre + 'nxphys', True, has=[rf'sum {s}'] + extra) for n, pre, s, extra in (
        ('wall', 'PHYS_COUNT=1 PHYS_SCENE=1 PHYS_STEPS=120 ', 'b28b77b4', [r'0 pages brought for workers']),
        ('wall-7', 'PHYS_WORKERS=7 PHYS_SCENE=1 PHYS_STEPS=120 ', 'b28b77b4', []),
        ('tower', 'PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300 ', '3d3402c2', [r'highest 6899 mm']),
        ('heap', 'PHYS_WORKERS=0 PHYS_SCENE=3 PHYS_BODIES=150 PHYS_STEPS=90 ', 'ed123738', []),
        ('scene-2', 'PHYS_WORKERS=0 PHYS_SCENE=2 PHYS_STEPS=60 ', '51fae5a5', []),
        ('throw', 'PHYS_SCENE=1 PHYS_LAUNCH=40:1 PHYS_STEPS=120 ', '8cccf9ba', []))],
    # nxchess (docs/games/chess.md): moves against the known numbers; the plain search's sum is the build computer's
    # (linux/chess/test/chess_host.c 6); on the workers no sum: nodes go by who was first, so the right moves
    case('chess-perft', 'games chess', 'nxchess perft; nxchess book', count=[(r'as known', 6)], has=[r'nxchess: perft PASS', r'nxchess: book PASS']),
    case('chess-plain', 'games chess', 'NXCHESS_WORKERS=0 nxchess search 6', sums=[(r'nxchess: sum ([0-9a-f]+)', ['6faa2f74'])]),
    case('chess-workers', 'games chess workers', 'nxchess solve 6000; mctest 15 2000', True, has=[r'nxchess: 6 of 6 right', r'mctest: PASS']),
    case('chess-15', 'games chess workers', 'NXCHESS_WORKERS=15 nxchess solve 12000', True, has=[r'nxchess: 6 of 6 right']),
    case('chess-level', 'games chess desktop', X + "NXCHESS_PRINT=1 nxchess & sleep 2; nxkey 150,56; sleep 1; nxkey 190,157; sleep 1; nxreg get nxchess.level; killall -9 nxchess; sleep 1; NXCHESS_PRINT=1 nxchess & sleep 2; killall -9 nxchess; nxreg unset nxchess.level",
         has=[r'(?m)^5\s*$', r'level 5, as 0']),
    case('chess-window', 'games chess desktop', X + "NXCHESS_PRINT=1 NXCHESS_BOOK=0 NXCHESS_MS=800 nxchess & sleep 2; nxkey 281,386 500 281,290; sleep 3; nxkey u; sleep 1; nxkey n; sleep 1; killall -9 nxchess; sleep 1; mctest 15 2000", True,
         has=[r'nxchess: played: 1 moves, last e4 e2e4', r'nxchess: engine: 2 moves', r'nxchess: undo: 0 moves', r'nxchess: new game: 0 moves', r'mctest: PASS']),
    case('nxphys-kill', 'games nxphys', X + 'nxphys & sleep 5; killall -9 nxphys; sleep 1; mctest 15 2000', True, has=[r'mctest: PASS']),
    case('nxphys-none', 'games nxphys long', X + 'PHYS_WORKERS=0 PHYS_SCENE=1 PHYS_STEPS=120 nxphys', has=[r'sum b28b77b4']),
    # Micropolis (docs/games/micropolis.md): a city and two scenarios with a fixed seed, the sums of the build computer's run
    case('micropolis', 'games', 'MICROPOLIS_SUM=/usr/share/micropolis/cities/haight.cty,1000 nxmicropolis; '
         'MICROPOLIS_SUM=1,500 nxmicropolis; MICROPOLIS_SUM=8,1500 nxmicropolis',
         sums=[(r'micropolis: .* ticks: .* sum ([0-9a-f]+)', ['909517ac', 'e14d1a50', '75e86f73'])]),
    # its window made larger shows more of the map ("The window"), and a controller: the left stick's notches pan the map, A pauses
    case('micropolis-window', 'games micropolis desktop', 'nx; sleep 3; MICROPOLIS_REPORT=1 nxmicropolis & sleep 50; nxkey ctrl+alt+up; sleep 40; killall nxmicropolis',
         True, wall=400, more=['--pointer-sweep', '0', '--pointer-circle', '400,300,0', '--scroll', '60,lx,1', '--scroll', '62,ly,-1',
                               '--pointer-button', '0x10080,64,64.5'],
         has=[r'nxmicropolis: the window is \d+x\d+, the map (7[0-9][0-9]|8[0-9][0-9]|9[0-6]\d)x\d+', r'nxmicropolis: pad scrolls the map to',
              r'nxmicropolis: pad pauses']),
    # OpenTTD (docs/games/openttd.md): two new games of test.cfg without a window, the sums of the native build's run
    case('openttd', 'games', 'cp /usr/share/openttd/test.cfg /tmp/t.cfg; OPENTTD_SUM=1 openttd.bin -v null:ticks=500 -s null -m null -g -G 1 -c /tmp/t.cfg; '
         'OPENTTD_SUM=1 openttd.bin -v null:ticks=2000 -s null -m null -g -G 2 -c /tmp/t.cfg',
         sums=[(r'openttd: \d+ ticks, .* sum ([0-9a-f]+)', ['9d6fc0a0', '66880c87'])]),
    # the viewport on worker cores (docs/games/openttd.md, "Checking a picture"): the play script paused, every picture
    # against one drawn whole, not a pixel apart; with the way back an odd move, and on the first core alone. The 8-bit
    # blitter's (OPENTTD_DRAW=cpu): rvc_cpu draws no GPU list, and the GPU's picture is checked under rvc_harness --cpu --warp
    # ("The picture on the GPU")
    # Age of Empires is on every image (docs/games/aoe.md); a private one made with AOE_IMAGE=0 has no data, says so and passes
    case('aoe', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_SIZE=800 AOE_VIEW=inline AOE_TICK_MS=33 AOE_SUM_EVERY=500 AOE_SUM_END=1000 '
         'AOE_SCRIPT="3000 click 400 245;3000 click 400 172;3000 click 237 568;2000 click 369 118;1000 rclick 505 170;1000 click 370 215;1000 click 163 508" aoe; else echo AOE-NO-DATA; fi',
         wall=120, has=[r'^(aoesum: update 1000 time 99900 objects 4357 sum 5032c153|AOE-NO-DATA)']),
    # the music on the card's clock (docs/games/aoe.md, "Music"): songs in the game's order, cut short at 4 s of the card's so that many end in the run, and the state
    # sum as with no card: when a song ends is the card's, and the game must not feel it
    case('aoe-music', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_MUSIC_LOG=1 AOE_MUSIC_CUT=4 AOE_VIEW=inline AOE_TICK_MS=33 AOE_SUM_EVERY=500 AOE_SUM_END=1000 '
         'AOE_SCRIPT="' + AOE_640 + '" aoe; else echo AOE-NO-DATA; fi',
         wall=180, env={'RVC_SOUND': '1'},
         has=[r'^(aoemus: play \S*open\.mid, 1505 notes|AOE-NO-DATA)', r'^(aoemus: play \S*music1\.mid, 3766 notes|AOE-NO-DATA)', r'^(aoemus: play \S*music2\.mid, 5388 notes|AOE-NO-DATA)',
              r'^(aoesum: update 1000 time 99900 objects 4357 sum 5032c153|AOE-NO-DATA)']),
    # the window it opens as, 640 x 480 (docs/games/aoe.md, "Windowed"): the same game by the same script, at this size's places
    case('aoe-640', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_VIEW=inline AOE_TICK_MS=33 AOE_SUM_EVERY=500 AOE_SUM_END=1000 '
         'AOE_SCRIPT="' + AOE_640 + '" aoe; else echo AOE-NO-DATA; fi',
         wall=120, has=[r'^(aoesum: update 1000 time 99900 objects 4357 sum 5032c153|AOE-NO-DATA)']),
    # the map by the right button ("Controls"): a click is an order, a drag moves the view a pixel for a pixel and gives none, the selection stays
    # (the order after the drag has 1 selected), the left button's box is a box, the pointer at the window's edge and held there moves nothing
    case('aoe-drag', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_TICK_MS=33 AOE_INPUT_LOG=1 AOE_SCRIPT="' + AOE_640.rsplit(';', 4)[0] + ';3000 ready;9000 view;9000 view;9000 view" aoe & '
         'while [ ! -e /tmp/aoe-ready ];do sleep 1;done;read OX OY </tmp/aoe-ready;sleep 2;nxkey $((OX+289)),$((OY+58));sleep 1;nxkey wheel:$((OX+639)),$((OY+200)):0;sleep 10;nxkey wheel:$((OX+300)),$((OY+200)):0;sleep 2;'
         'nxkey r:$((OX+425)),$((OY+110));sleep 3;nxkey rdrag:$((OX+400)),$((OY+200)):$((OX+200)),$((OY+100));sleep 4;nxkey r:$((OX+380)),$((OY+150));sleep 3;'
         'nxkey drag:$((OX+220)),$((OY+40)):$((OX+420)),$((OY+240));sleep 4;killall aoe.bin;else echo AOE-NO-DATA; fi', wall=300,
         has=[r'^(aoeinput: order 425 110 1 0|AOE-NO-DATA)', r'^(aoeinput: drag -200 -100 0 625|AOE-NO-DATA)', r'^(aoeinput: order 380 150 1 0|AOE-NO-DATA)', r'^(aoeinput: box 220 40 420 240|AOE-NO-DATA)',
              r'^(aoeview: (\d+ \d+) selected 1\s+(?:aoe: step.*\s+)?aoeview: \2 selected 1|AOE-NO-DATA)']),
    # a notch of the left hand's stick holds an arrow 120 ms over the window (docs/machine/input.md, "Game controls"), the pointer over it
    case('aoe-pad', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_TICK_MS=33 AOE_INPUT_LOG=1 AOE_SCRIPT="' + AOE_640.rsplit(';', 4)[0] + ';3000 ready" aoe & '
         'while [ ! -e /tmp/aoe-ready ];do sleep 1;done;sleep 30;killall aoe.bin;else echo AOE-NO-DATA; fi', wall=400,
         more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0'] + [x for t in range(40, 400, 10) for x in ('--scroll', f'{t},lx,1')],
         has=[r'^(aoe: pad holds key 39|AOE-NO-DATA)']),
    # the map dragged with the GPU's lists taken back late, as in a world (RVC_DEVICE_EVERY=6), the world in slices on the real clock
    # (docs/games/aoe.md, "Scrolls checked"): at each check the picture the view made by its scrolls against one drawn whole from the
    # same game, not a pixel apart; and the scroll's machine copy made by the first core
    case('aoe-scroll-check', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_DEBUG=1 AOE_SEED=1 AOE_SCRIPT="' + AOE_640 +
         ';1000 check;1000 rsweep 400 200;1500 check;1500 check;1500 check;1500 check;1500 check;22000 check;2000 quit" aoe 2>&1 | grep -aE "aoecheck|copy"; '
         'else echo AOE-NO-DATA; fi', wall=420, env={'RVC_DEVICE_EVERY': '6'},
         has=[r"^(aoe: the machine's copy by the first core|AOE-NO-DATA)", r'^(aoecheck: 0 pixels differ \(first at 0,20\), 0 of 7 checks|AOE-NO-DATA)'],
         never=[r'aoecheck: [1-9]']),
    # its window resized ("Windowed", 3 November 2026): the picture is stretched over it, and a click where a button is drawn lands on it,
    # maximised (Single Player at the main menu's 320,196) and tiled at the right half (Random Map at 320,138), by the window's size now;
    # each menu made again in the layout of the window's size first ("Two layouts": the main menu at 800 x 600, the single player menu at 640)
    case('aoe-resize', 'games aoe desktop', AOE_UNPACK + 'nx; sleep 3; ' + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_DEBUG=1 AOE_SCRIPT="3000 ready" aoe & '
         'while [ ! -e /tmp/aoe-size ];do sleep 1;done;sleep 2;nxkey ctrl+alt+up;'
         'until [ "$(cat /tmp/aoe-layout 2>/dev/null)" = 800 ];do sleep 1;done;sleep 2;read OX OY W H </tmp/aoe-size;echo SIZE $W $H;'
         'nxkey $((OX+320*W/640)),$((OY+196*H/480));sleep 8;nxkey ctrl+alt+right;'
         'until [ "$(cat /tmp/aoe-layout 2>/dev/null)" = 640 ];do sleep 1;done;sleep 2;read OX OY W H </tmp/aoe-size;echo SIZE $W $H;'
         'nxkey $((OX+320*W/640)),$((OY+138*H/480));sleep 8;killall aoe.bin;else echo AOE-NO-DATA; fi', wall=600,
         has=[r'^(SIZE (?!640 480)\d+ \d+|AOE-NO-DATA)', r'^(aoe: panel Single Player Menu|AOE-NO-DATA)', r'^(aoe: panel MP Setup Screen|AOE-NO-DATA)',
              r'^(aoe: Main Menu made again at 800 x 600|AOE-NO-DATA)', r'^(aoe: Single Player Menu made again at 640 x 480|AOE-NO-DATA)']),
    # the layout follows the window in play ("Two layouts"): maximised it is 800 x 548 (the window's shape), put back 640 x 480, the game goes on and a click in
    # the window's middle is the picture's middle in each; the menus before the game kept theirs
    case('aoe-layout', 'games aoe desktop', AOE_UNPACK + 'nx; sleep 3; if [ -f /tmp/aoe/Empires.dat ]; then AOE_TICK_MS=33 AOE_INPUT_LOG=1 '
         'AOE_STATS_MS=5000 AOE_SCRIPT="' + AOE_640.rsplit(';', 4)[0] + ';3000 ready" aoe & while [ ! -e /tmp/aoe-ready ];do sleep 1;done;sleep 2;'
         'nxkey ctrl+f10;sleep 12;read OX OY W H </tmp/aoe-size;echo SIZE $W $H;nxkey $((OX+W/2)),$((OY+H/2));sleep 6;'
         'nxkey ctrl+f10;sleep 12;read OX OY W H </tmp/aoe-size;echo SIZE $W $H;nxkey $((OX+W/2)),$((OY+H/2));sleep 6;killall aoe.bin;'
         'else echo AOE-NO-DATA; fi', wall=600,
         has=[r'^(aoe: layout 800 x 54\d at Game Screen \(from 640 x 480\)|AOE-NO-DATA)', r'^(aoeinput: press 1 (39\d|40\d) (26\d|27\d|28\d)|AOE-NO-DATA)',
              r'^(aoe: layout 640 x 480 at Game Screen \(from 800 x 54\d\)|AOE-NO-DATA)', r'^(aoeinput: press 1 (31\d|32\d) (23\d|24\d)|AOE-NO-DATA)']),
    # the trial's first start on the desktop (docs/system/install.md): its EULA read out of the installer in a window, Accept refused until
    # the text has been scrolled to its end, then accepted by Tab and Enter, the bar to the last of 105 files, and aoeprep's strings and faces
    case('aoe-install', 'games aoe desktop', X + 'if [ -d /usr/share/aoe/installed ]; then (sleep 6; nxkey a; sleep 1; nxkey pagedown; sleep 1; nxkey end; '
         'sleep 1; nxkey tab enter) & CABINST_PRINT=1 aoe --unpack; echo INSTALL=$?; ls /tmp/aoe | wc -l; else echo AOE-NO-DATA; fi', True, wall=300,
         has=[r'^(cabinst: licence eula\.txt: 5065 bytes, CRC-32 b81b406b|AOE-NO-DATA)', r'^(cabinst: licence shown, \d+ lines|AOE-NO-DATA)',
              r'^(cabinst: accept refused: the licence is not read to its end|AOE-NO-DATA)', r'^(cabinst: licence at line \d+ of \d+, its end|AOE-NO-DATA)',
              r'^(cabinst: accepted|AOE-NO-DATA)', r'^(cabinst: Age of Empires \(trial\): 10\d files linked from /usr/share/aoe/installed|AOE-NO-DATA)',
              r'^(aoeprep: 2276 strings .* 174 fonts|AOE-NO-DATA)', r"^(aoeprep: 1 menus' pictures brought to 640 x 480|AOE-NO-DATA)", r'^(INSTALL=0|AOE-NO-DATA)', r'^(1[01]\d|AOE-NO-DATA)']),
    # Jedi Outcast's demo, on images made with its installer only (docs/games/jk2.md, "Checking it"): its start played by a script of
    # keys from the menu (new game, a walk, a turn, a shot, the death and the autosave's load), and the build computer's sums;
    # no window, so the picture is nullgl's and the sound mixed into nothing (the card's pace would move the sums)
    # (its pak unpacked first, the licence answered on the terminal: no window system)
    case('jk2', 'games jk2 long', 'if [ -d /usr/share/jk2/installed ]; then JK2_KEYS="250:enter 330:at500,455 340:mouse1 1500:w+ 1700:w- '
         '1700:right+ 1730:right- 1740:w+ 1900:w- 1900:mouse1+ 2000:mouse1-" JK2_FRAMES=2400 JK2_SUM=200 JK2_SEED=7 jk2 +set fixedtime 50 '
         '+set com_maxfps 0; else echo JK2-NO-DATA; fi', wall=600, more=['--expect', 'Type yes', '--send', 'yes\\n'],
         has=[r'^.*(jk2sum: frame 800 time 41250 entities 645 sum b7b63862 draws cf778cae|JK2-NO-DATA)',
              r'^.*(jk2sum: frame 1200 time 61250 entities 641 sum 5496b62c draws 8d82b4ce|JK2-NO-DATA)',
              r'^.*(jk2sum: frame 1000 time 51250 entities 643 sum d946eeaa draws 7bf726e5|JK2-NO-DATA)']),
    # the level played with no input for 4,000 frames: host_sum.sh's nine lines (the last two of them here)
    # the demo's first start on the desktop (docs/system/install.md): LucasArts' licence read out of the installer in a window, Accept
    # refused until it is scrolled to its end, the pak unpacked into memory with the bar, and its CRC-32 7-Zip's (239abc19)
    # the menus' cursor is the pointer (docs/games/jk2.md, "Input"): the pointer's place in the window, not its travel, at the window's place
    case('jk2-pointer', 'games jk2 desktop long', X + 'if [ -d /usr/share/jk2/installed ]; then (sleep 6; nxkey pagedown; sleep 1; nxkey end; sleep 1; '
         'nxkey tab enter) & jk2 --unpack; JK2_POINTER_LOG=1 JK2_FRAMES=150 jk2; else echo JK2-NO-DATA; fi', True, wall=1800,
         more=['--pointer-sweep', '0', '--pointer-circle', '400,300,100'],
         has=[r'(jk2pointer: (\d+),(\d+) of the display to \d+,\d+ of the menu \(window at \d+,\d+, 640x480\)|JK2-NO-DATA)']),
    # the window maximised at the main menu: the renderer restarted at the window's size (docs/games/jk2.md, "The window's size"), then
    # New and a click on Begin Game where the stretched menu has it (500, 455 of 640 x 480) in the window's place: the level loads
    case('jk2-resize', 'games jk2 desktop long', 'nx; sleep 3; if [ -d /usr/share/jk2/installed ]; then (sleep 6; nxkey pagedown; sleep 1; '
         'nxkey end; sleep 1; nxkey tab enter) & jk2 --unpack; (JK2_POINTER_LOG=1 jk2 &); sleep 320; nxkey ctrl+alt+up; sleep 120; '
         'set -- $(nxwintest list | grep Jedi); P=${5%,*}; Q=${5#*,}; S=$6; W=${S%x*}; H=${S#*x}; echo PLACE $P $Q $W $H; nxkey enter; sleep 40; '
         'C=$((W-6)); D=$((H-29)); nxkey $((P+3+500*D/480+(C-640*D/480)/2)),$((Q+26+455*D/480)); sleep 200; killall jk2.bin; else echo JK2-NO-DATA; fi', True, wall=2400,
         has=[r'(jk2: the window is \d+x\d+: the renderer restarted at \d+x\d+|JK2-NO-DATA)', r'(^PLACE \d+ \d+ \d+ \d+|JK2-NO-DATA)',
              r'(jk2pointer: \d+,\d+ of the display to (49\d|50\d),(44\d|45\d|46\d) of the menu|JK2-NO-DATA)',
              r'(Server Initialization|SpawnServer|JK2-NO-DATA)']),
    case('jk2-install', 'games jk2 desktop', X + 'if [ -d /usr/share/jk2/installed ]; then (sleep 6; nxkey a; sleep 1; nxkey pagedown; sleep 1; nxkey end; '
         'sleep 1; nxkey tab enter) & CABINST_PRINT=1 CABINST_SUMS=1 jk2 --unpack; echo INSTALL=$?; else echo JK2-NO-DATA; fi', True, wall=300,
         has=[r'^(cabinst: licence Disk1/Install/license\.txt: 3529 bytes, CRC-32 4361c574|JK2-NO-DATA)', r'^(cabinst: licence shown, \d+ lines|JK2-NO-DATA)',
              r'^(cabinst: accept refused: the licence is not read to its end|JK2-NO-DATA)', r'^(cabinst: licence at line \d+ of \d+, its end|JK2-NO-DATA)',
              r'^(cabinst: accepted|JK2-NO-DATA)', r'^(cabinst: Jedi Outcast \(demo\): \d+ files linked from /usr/share/jk2/installed|JK2-NO-DATA)', r'^(INSTALL=0|JK2-NO-DATA)']),
    # Half-Life's licence (docs/system/install.md): Sierra's, as Uplink's installer showed it, in a window before the game, Accept refused
    # until it is read to its end, then accepted; nothing to link (the game's files lie in place)
    case('halflife-licence', 'games halflife desktop', X + 'if [ -f /usr/share/halflife/licence/eula.txt ]; then (sleep 6; nxkey a; sleep 1; '
         'nxkey pagedown; sleep 1; nxkey end; sleep 1; nxkey tab enter) & CABINST_PRINT=1 halflife --licence; echo LICENCE=$?; else echo HL-NO-DATA; fi',
         True, wall=300,
         has=[r'^(cabinst: licence eula\.txt: 8678 bytes, CRC-32 8676e153|HL-NO-DATA)', r'^(cabinst: licence shown, \d+ lines|HL-NO-DATA)',
              r'^(cabinst: accept refused: the licence is not read to its end|HL-NO-DATA)', r'^(cabinst: accepted|HL-NO-DATA)',
              r'^(cabinst: Half-Life: Uplink: licence accepted, nothing to link|HL-NO-DATA)', r'^(LICENCE=0|HL-NO-DATA)']),
    # the starters delete what they unpacked when the game ends (docs/system/install.md): 34 and 64 MB of RAM
    case('aoe-cleanup', 'games aoe', AOE_UNPACK + X + 'if [ -f /tmp/aoe/Empires.dat ]; then AOE_TICK_MS=33 AOE_SCRIPT="3000 quit" aoe > /dev/null; '
         'echo AOE=$?; ls -d /tmp/aoe 2>&1; else echo AOE-NO-DATA; fi', True, wall=300,
         has=[r'^(AOE=0|AOE-NO-DATA)', r'(/tmp/aoe: No such file|AOE-NO-DATA)']),
    case('jk2-cleanup', 'games jk2', 'if [ -d /usr/share/jk2/installed ]; then echo yes | jk2 --unpack > /dev/null; ls /tmp/jk2/base; '
         'JK2_FRAMES=60 jk2 +set fixedtime 50 < /dev/null > /dev/null; echo JK2=$?; ls -d /tmp/jk2 2>&1; else echo JK2-NO-DATA; fi', True, wall=300,
         has=[r'(assets0\.pk3|JK2-NO-DATA)', r'^(JK2=0|JK2-NO-DATA)', r'(/tmp/jk2: No such file|JK2-NO-DATA)']),
    case('jk2-sums', 'games jk2 long', 'if [ -d /usr/share/jk2/installed ]; then JK2_FRAMES=4000 JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 '
         '+set com_maxfps 0 +map demo; else echo JK2-NO-DATA; fi', wall=1200, more=['--expect', 'Type yes', '--send', 'yes\\n'],
         has=[r'^.*(jk2sum: frame 3200 time 161250 entities 639 sum 559c85a7 draws 810d9e76|JK2-NO-DATA)',
              r'^.*(jk2sum: frame 3600 time 181250 entities 639 sum 286e9b16 draws 274198c2|JK2-NO-DATA)']),
    # the same with the client's game on the copy the game gives it, what it writes given back by its fields (JK2_SPLIT=check: every word by difference) (docs/games/jk2.md, "The game's frame apart")
    case('jk2-split', 'games jk2 long', 'if [ -d /usr/share/jk2/installed ]; then JK2_SPLIT=copy JK2_FRAMES=4000 JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 '
         '+set com_maxfps 0 +map demo; else echo JK2-NO-DATA; fi', wall=1200, more=['--expect', 'Type yes', '--send', 'yes\\n'],
         has=[r'^.*(jk2sum: frame 3200 time 161250 entities 639 sum 559c85a7 draws 810d9e76|JK2-NO-DATA)',
              r'^.*(jk2sum: frame 3600 time 181250 entities 639 sum 286e9b16 draws 274198c2|JK2-NO-DATA)']),
    # the game's next frame made after the client's game has its copy, before the client's frame (JK2_SPLIT=ahead): other sums, the
    # build computer's in the same order (host_sum.sh with JK2_SPLIT=ahead) (docs/games/jk2.md, "The game's frame apart")
    case('jk2-ahead', 'games jk2 long', 'if [ -d /usr/share/jk2/installed ]; then JK2_SPLIT=ahead JK2_FRAMES=4000 JK2_SUM=400 JK2_SEED=7 jk2 +set fixedtime 50 '
         '+set com_maxfps 0 +map demo; else echo JK2-NO-DATA; fi', wall=1200, more=['--expect', 'Type yes', '--send', 'yes\\n'],
         has=[r'^.*(jk2sum: frame 3200 time 161250 entities 639 sum 1530a9f9 draws 9367bd09|JK2-NO-DATA)',
              r'^.*(jk2sum: frame 3600 time 181250 entities 639 sum ff07869a draws afc9cdbd|JK2-NO-DATA)']),
    case('openttd-scroll', 'games openttd', X + 'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_ODD=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play round 1: .*checked (19\d|200), 0 differ \(0 pixels']),
    case('openttd-scroll-serial', 'games openttd', X + 'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 OPENTTD_WORKERS=0 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play round 1: .*checked (19\d|200), 0 differ \(0 pixels']),
    # windows standing, moved and closed over the view standing still (docs/games/openttd.md, "Windows"), every picture against one drawn whole
    case('openttd-windows', 'games openttd', X + 'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=many OPENTTD_PLAY_MOVE=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav; '
         'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=vehicle OPENTTD_PLAY_MOVE=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play round 1: .*checked \d+, 0 differ \(0 pixels']),
    # docs/games/openttd.md, "The small map on the workers": the small map open while the game runs, its tiles' colours drawn in bands by every
    # core, and each picture of it drawn again by the first core alone: not a pixel differs
    case('openttd-map', 'games openttd', X + 'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=map OPENTTD_SMALLMAP=check '
         'openttd -g /usr/share/openttd/busy.sav', True, wall=400,
         has=[r'openttd: small map \d+x\d+ checked [1-9]\d* times', r'openttd: play round 1: '], never=[r'[1-9]\d* pixels differ', r' [1-9]\d* in all']),
    # and timed: the small map open, its tiles' colours by every core and by the first core alone (OPENTTD_SMALLMAP=core, upstream's way)
    *[case(f'openttd-map-time{tag}', 'games openttd long', X + f'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=map {pre}'
           'openttd -g /usr/share/openttd/busy.sav', True, wall=600, has=[r'openttd: play round 1: '])
      for tag, pre in (('', ''), ('-core', 'OPENTTD_SMALLMAP=core '))],
    # a window of a new vehicle hidden in a depot and the other small views, with light pictures, which OPENTTD_CHECK turns off (docs/games/openttd.md,
    # "Pictures the tick does not wait for"): its view followed a copy no sync had made, the map's north corner
    # a game saved uncompressed, as named LZMA, and with an encoder that cannot start, and each loaded: its state as it was saved
    case('openttd-save', 'games openttd', X + ' ; '.join(
        f'OPENTTD_DRAW=cpu {e} OPENTTD_SAVE_AT=8:/tmp/{n}.sav openttd -g /usr/share/openttd/busy.sav; OPENTTD_DRAW=cpu OPENTTD_LOAD_CHECK=/tmp/{n}.sav.sum openttd -g /tmp/{n}.sav'
        for n, e in (('plain', ''), ('packed', 'OPENTTD_SAVE_FORMAT=lzma'), ('fallback', 'OPENTTD_SAVE_FORMAT=lzma OPENTTD_SAVE_NOENCODER=1'))),
         True, wall=900, has=[r'saved ok, tag OTTN, ', r'saved ok, tag OTTX, ', r'LZMA at preset \d does not start, lzma_ret 5,[^~]*saved ok, tag OTTN, ',
             r'(?s)(: as saved.*){3}'], never=[r'NOT as saved']),
    case('openttd-depot', 'games openttd', X + 'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=depot openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: depot views: \d\d pictures, [1-9]\d* light, 0 off the vehicle, 0 stopped and hidden with their copies elsewhere; not black.* the least ([5-9]\d|100)%',
             # the vehicle started, sent back, and another already on the road with its window opened: where the view is in every picture, its copy, its pixels
             r'depot drive, out of the depot: 500 pictures, moved \d{2,}, 0 off the vehicle \(most \d+, last at \d+\), copy stale \d \(at most \d together\), shown [2-5]\d\d and not seen 0',
             r'depot drive, back to a depot: 500 pictures, moved \d{2,}, 0 off the vehicle \(most \d+, last at \d+\), copy stale \d \(at most \d together\), shown \d{2,} and not seen 0',
             r'depot drive, on the road: 300 pictures, moved \d{2,}, [0-2] off the vehicle \(most \d+, last at \d+\), copy stale \d \(at most \d together\), shown [23]\d\d and not seen 0']),
    # started twice in one boot (docs/games/openttd.md, "At exit"): the first played with its ships' paths made ahead, the
    # second must find its sprites as they are (a worker left running at exit once wrote into the next program's pages)
    case('openttd-twice', 'games openttd', X + 'OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 openttd -g /usr/share/openttd/busy.sav; '
         'OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav; '
         'OPENTTD_SUM=1 openttd -v null:ticks=1 -s null -m null -g /usr/share/openttd/busy.sav',
         True, wall=600, has=[r'openttd: play PL-DONE', r'openttd: play scroll: .* [1-9]\d* made again by workers',
                              r'openttd: play round 1: .*checked \d+, 0 differ \(0 pixels'],
         sums=[(r'openttd: 1 ticks, map 256 x 256, .* sum ([0-9a-f]+)', ['e49e18e3'])]),
    # chat lines over the scroll in a network game (docs/games/openttd.md, "Chat lines"): six lines added and ended one by one while it
    # scrolls, every picture against one drawn whole with the lines drawn again over it, on workers and on the first core alone; then
    # the GPU's blitter, whose lines are records of their own (rvc_cpu draws no list: its run counts the sets of lines and the lists that drew them)
    case('openttd-chat', 'games openttd', X + ' ; '.join(
        f'OPENTTD_HOST=1 OPENTTD_PLAY=1 OPENTTD_PLAY_PART=scroll OPENTTD_PLAY_CHAT=1 {e} openttd -g /usr/share/openttd/busy.sav' for e in (
            'OPENTTD_DRAW=cpu OPENTTD_PLAY_ODD=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1', 'OPENTTD_DRAW=cpu OPENTTD_PAUSE=1 OPENTTD_CHECK=1 OPENTTD_WORKERS=0', '')),
         True, wall=600, count=[(r'openttd: play chat: 6 lines added, 6 ended, [1-9]\d pictures with lines, 0 up at the end', 3),
                               (r'openttd: play round 1: 61 pictures .*checked 6\d, 0 differ \(0 pixels', 2)],
         has=[r'openttd: play chat: .* 0 up at the end; [1-9]\d* sets of lines made, [1-9]\d* lists drew them'], never=[r'not a network game']),
    # the same with light pictures (the game running, OPENTTD_CHECK=last: the part's last picture alone checked, after the lines ended)
    case('openttd-chat-light', 'games openttd', X + 'OPENTTD_HOST=1 OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=scroll OPENTTD_PLAY_CHAT=1 OPENTTD_CHECK=last '
         'openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play chat: 6 lines added, 6 ended, [1-9]\d pictures with lines, 0 up at the end',
                              r'openttd: play round 1: 61 pictures .*checked 1, 0 differ \(0 pixels'], never=[r'not a network game']),
    # and with the device taken back only every sixth pass, as a world draws a list once a frame (lists that do not wait for the GPU)
    case('openttd-chat-device', 'games openttd', X + 'OPENTTD_HOST=1 OPENTTD_DRAW=cpu OPENTTD_PLAY=1 OPENTTD_PLAY_PART=scroll OPENTTD_PLAY_CHAT=1 OPENTTD_CHECK=1 '
         'OPENTTD_PAUSE=1 openttd -g /usr/share/openttd/busy.sav', True, wall=600, env={'RVC_DEVICE_EVERY': '6'},
         has=[r'openttd: play chat: 6 lines added, 6 ended, [1-9]\d pictures with lines, 0 up at the end',
              r'openttd: play round 1: 61 pictures .*checked 6\d, 0 differ \(0 pixels'], never=[r'not a network game']),
    # the GPU's blitter: its lists made by the first core and the workers' records (with --races, what the workers store)
    case('openttd-gpu', 'games openttd', X + 'OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 OPENTTD_PAUSE=1 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play PL-DONE', r'openttd: play scroll: .* [1-9]\d* made again by workers']),
    # nxray and nxpath (docs/apps/raytrace.md)
    # (the scene's sines in floats since 20 October 2026: docs/apps/raytrace.md, "Lanes"; rvc_cpu has no lanes, the same sums plain)
    case('nxray', 'workers', X + 'RAY_FRAMES=4 nxray', True, sums=[(r'raystat: picture \d.* sum ([0-9a-f]+)', ['53ea472d', '226b5e92', '37366b32', '3ffdbe63'])],
         has=[r'with 255 workers']),
    case('nxray-none', 'workers long', X + 'RAY_WORKERS=0 RAY_FRAMES=4 nxray', wall=600,
         sums=[(r'raystat: picture \d.* sum ([0-9a-f]+)', ['53ea472d', '226b5e92', '37366b32', '3ffdbe63'])]),
    case('nxpath', 'workers', X + 'NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath', True, has=[r'd63313e9']),
    case('nxpath-none', 'workers long', X + 'NXPATH_CORES=0 NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath', wall=600, has=[r'd63313e9']),
    case('nxpath-check', 'workers', X + 'NXPATH_CHECK=500 nxpath', True, until='thousandths of a pixel',
         has=[r'nxpath: 0 of 500 rays met another object']),
    # nxpath's example scenes (docs/apps/raytrace.md, "Example scenes"): the same sum with every worker and with none
    *[case(f'nxpath-{n}{tag}', 'workers pathtrace', X + f'{pre}NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE={size} NXPATH_SAMPLES=2 nxpath {n}',
           not pre, has=[rf'pathstat: {size.replace("x", " x ")}, 2 samples.* sum {s}'])
      for n, size, s in (('cornell', '120x120', '78c07525'), ('cornell-balls', '120x120', 'b962c063'), ('balls', '160x120', '820f06bc'),
                         ('materials', '160x120', '5ecc7a24'), ('beer', '160x120', '3c29787e'), ('mirrors', '160x120', '2f211b98'),
                         ('glow', '160x120', '2858a999'))
      for tag, pre in (('', ''), ('-none', 'NXPATH_CORES=0 '))],
    # a scene saved and opened again is the same scene: the start's and two examples' sums
    *[case(f'nxpath-saved-{n or "start"}', 'workers pathtrace', X + f'NXPATH_WRITE=/tmp/s.scene NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=8x8 NXPATH_SAMPLES=1 nxpath {n}'
           f' && NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE={size} NXPATH_SAMPLES=2 nxpath /tmp/s.scene', True,
           has=[r'nxpath: /tmp/s\.scene written', rf'pathstat: {size.replace("x", " x ")}, 2 samples.* sum {s}'])
      for n, size, s in (('', '160x120', 'd63313e9'), ('cornell', '120x120', '78c07525'), ('materials', '160x120', '5ecc7a24'))],
    case('nxpath-cornell-check', 'workers pathtrace', X + 'NXPATH_CHECK=500 nxpath cornell', True, until='thousandths of a pixel',
         has=[r'nxpath: 0 of 500 rays met another object', r'at most 0 thousandths of a pixel']),
    # the picture smoothed, the lanes' job (docs/apps/raytrace.md, "The picture smoothed on the lanes"): its sum, and no texel
    # near a pass (rvc_cpu's lane has 8,192 instructions; each texel counted twice, the fewer kept)
    case('nxpath-smooth', 'workers pathtrace', X + 'NXPATH_SMOOTH=1 NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath', True,
         has=[r'pathstat: 160 x 120, 2 samples.* sum d63313e9', r'nxpath: smoothed 160 x 120 of 2 samples: sum f339cab5, \d+ instructions a texel on average, the most [1-2]\d{3}\s*$']),
    # Dear ImGui's frames (docs/apps/imgui.md): the triangles of 30 held frames with the demo window, the same whether the
    # first core copies them and makes the text, lines and shapes or the workers do ("Drawn later"), and as the library
    # unchanged made them (the sum of the committed 1.91.5 with the held picture's count of vertices)
    *[case(f'imdemo-sum{tag}', 'desktop imgui', X + f'IMGUI_SUM=1 IMDEMO_HOLD=1 IMDEMO_SHOW=1 IMDEMO_FRAMES=30 {pre}imdemo 2>&1 | grep imsum | md5sum', not pre,
           has=[r'^dba8c49bf512875113898e1c5fca5ead'])
      for tag, pre in (('', ''), ('-inline', 'IMGUI_COPY=inline '))],
    # libmedia and the players (docs/system/media.md, docs/apps/play.md, docs/apps/video.md)
    case('media-sound', 'media', '; '.join(f'media_test audio /usr/share/play-test.{e} /tmp/o.raw {w}' for e in ('flac', 'mp3', 'mod') for w in (0, 63)),
         True, wall=400, sums=[(r'audio /usr/share/play-test\.\w+: .* sum ([0-9a-f]+)', ['518c5ac8'] * 2 + ['aa07a2e5'] * 2 + ['c714a845'] * 2)]),
    case('media-mpeg', 'media', 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8', True, wall=400,
         has=[r"pictures' sum d124dc14", r"sound's sum 96a296a2"]),
    case('media-avi', 'media', 'media_test video "/root/Videos/Big Buck Bunny.avi" 16', True, wall=400,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
    case('media-avi-none', 'media long', 'media_test video "/root/Videos/Big Buck Bunny.avi" 0', wall=600,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
    # the converter (docs/apps/convert.md): the fox into every picture format and at half its size, the files' sums
    # (a picture's are media_test-host encode's on the build computer); FLAC and WAV, the FLAC decoded again to the MP3's
    # samples, the same file with no workers; the films into MJPEG AVI and read back (the MPEG's AVI the host's filmenc),
    # an animated GIF and every 48th picture
    case('convert-pictures', 'media desktop apps', 'cd /tmp; for f in png jpg gif bmp ppm tga; do nxconvert --to $f /usr/share/picture-fox.jpg /tmp/fox.$f; done; '
         'nxconvert --to jpg --scale 50 /usr/share/picture-fox.png /tmp/half.jpg; nxconvert --to png /usr/share/picture-fox.jpg /tmp/fox.png', True,
         sums=[(r'^nxconvert: made .* sum ([0-9a-f]+),', ['5b38ecc9', 'bd0b8027', '3cae98ae', '0a40b790', '57d14865', '397b512a', '5d2ed007'])],
         has=[r'^nxconvert: not made: a file of that name is there already']),
    case('convert-sound', 'media apps', 'cd /tmp; nxconvert --to flac /usr/share/play-test.mp3 /tmp/t.flac; nxconvert --to wav /usr/share/play-test.flac /tmp/t.wav; '
         'media_test audio /tmp/t.flac /tmp/a.raw 8; NXCONVERT_WORKERS=0 nxconvert --to flac /usr/share/play-test.mp3 /tmp/t0.flac; '
         'nxconvert --to flac "/root/Videos/Big Buck Bunny.avi" /tmp/b.flac; media_test audio /tmp/b.flac /tmp/b.raw 4', True, wall=400,
         sums=[(r'^nxconvert: made .* sum ([0-9a-f]+),', ['8ca12520', '6bcc8ea3', '8ca12520', '33990159']),
               (r'^audio /tmp/\w\.flac: .* sum ([0-9a-f]+)', ['aa07a2e5', 'caab0fc3'])]),
    case('convert-film', 'media apps', 'cd /tmp; nxconvert --to avi "/root/Videos/Big Buck Bunny.avi" /tmp/b.avi; MEDIA_TEST_QUIET=1 media_test video /tmp/b.avi 8; '
         'NXCONVERT_TIMES=1 nxconvert --to avi --quality 75 "/root/Videos/Big Buck Bunny.mpg" /tmp/m.avi; MEDIA_TEST_QUIET=1 media_test video /tmp/m.avi 8; '
         'nxconvert --to gif --scale 50 --every 4 "/root/Videos/Big Buck Bunny.mpg" /tmp/m.gif; '
         'nxconvert --to jpg --every 48 --quality 80 "/root/Videos/Big Buck Bunny.avi" /tmp/p.jpg; test -f /tmp/p-0002.jpg && echo P2-THERE', True, wall=600,
         sums=[(r'^nxconvert: made .* sum ([0-9a-f]+),', ['4e0789ff', '2c5a85f1', 'b10560b8', '13f75eba']),
               (r"pictures' sum ([0-9a-f]+)", ['26ebe6f9', '396b3d97']), (r"sound's sum ([0-9a-f]+)", ['6dd01fde', '96a296a2'])],
         has=[r'^nxconvert: made 2 JPEG pictures /tmp/p-0001\.jpg to p-0002\.jpg', r'^P2-THERE',
              r'^nxconvert: the sound \d+ ms, the pictures in 5 batches of 96']),
    # its window: the fox opened, PNG chosen and Convert pressed (the window's inside at 43,46), the file beside it
    case('convert-window', 'media desktop apps', 'cp /usr/share/picture-fox.jpg /tmp/f.jpg; ' + X + 'NXCONVERT_PRINT=1 nxconvert /tmp/f.jpg & sleep 8; '
         'nxkey 215,133; sleep 2; nxkey 390,341; sleep 25; ls -l /tmp/f.png; nxkey escape; sleep 2; pidof nxconvert; echo PIDOF=$?', True, wall=400,
         has=[r'^nxconvert: button PNG at 172,87', r'^nxconvert: button Convert at 347,295', r'^nxconvert: made /tmp/f\.png: PNG picture, 640 x 480, 227682 bytes, sum 5b38ecc9',
              r'^PIDOF=1']),
    case('nxplay', 'media', '; '.join(f'NXPLAY_SUM=1 nxplay /usr/share/play-test.{e}' for e in ('wav', 'flac', 'mod', 'mp3')) +
         '; NXPLAY_WORKERS=0 NXPLAY_HALF=0 NXPLAY_SUM=1 nxplay /usr/share/play-test.mp3', True, wall=600,
         sums=[(r'nxplay: sum ([0-9a-f]+ of \d+) samples', ['e5fd8f8c of 141120', '1a14a562 of 176400', '98e66905 of 1354752',
                                                            '809816e7 of 1594368', '809816e7 of 1594368'])]),
    case('nxvideo', 'media', X + 'NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo; NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo "/root/Videos/Big Buck Bunny.avi"',
         True, wall=400, sums=[(r'nxvideo: sum ([0-9a-f]+ of \d+) pictures', ['d124dc14 of 480', '3129e37f of 96'])]),
    case('nxvideo-quit', 'media', X + 'nxvideo & sleep 9; nxkey q; sleep 4; pidof nxvideo; echo PIDOF=$?; mctest 255 20000', True,
         has=[r'^PIDOF=1', r'mctest: PASS']),
    # docs/apps/video.md, "The window": a second start and Open... while a film plays, a smaller desktop, a codec not played,
    # a GL program while a film plays (refused in a window, the film on)
    case('nxvideo-second', 'media', X + 'nxvideo & sleep 10; nxvideo "/root/Videos/Big Buck Bunny.avi"; echo SECOND=$?; sleep 8; pidof nxvideo; echo PIDOF=$?',
         True, has=[r'^nxvideo: given to the player that is playing', r'^SECOND=0', r'^nxvideo: Big Buck Bunny\.avi, AVI 320 x 176', r'^PIDOF=0'],
         never=[r'the GPU is drawing for']),
    case('nxvideo-open', 'media', X + 'nxvideo & sleep 10; nxkey o; sleep 3; nxkey b enter; sleep 8; pidof nxvideo; echo PIDOF=$?', True,
         count=[(r'^nxvideo: Big Buck Bunny\.\w+, ', 2)], has=[r'^PIDOF=0'], never=[r'the GPU is drawing for']),
    case('nxvideo-fit', 'media', X + 'NXVIDEO_SCREEN=300x260 NXVIDEO_PRINT=1 NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo', True, wall=400,
         has=[r'^nxvideo: 320 x 176 shown at 280 x 154, the window 280 x 194', r'nxvideo: sum d124dc14 of 480 pictures']),
    case('nxvideo-codec', 'media', X + 'cp "/root/Videos/Big Buck Bunny.avi" /tmp/x.avi; printf FMP4 | dd of=/tmp/x.avi bs=1 seek=188 conv=notrunc 2>/dev/null; '
         'UI_ASK_PRINT=1 nxvideo /tmp/x.avi & sleep 6; killall nxvideo',
         has=[r'^ask: Video: x\.avi MPEG-4 Part 2 \(FMP4\) pictures: not played \(this plays MJPEG in AVI\)']),
    case('nxvideo-gpu-taken', 'media', X + 'nxvideo & sleep 10; UI_ASK_PRINT=1 RAY_FRAMES=2 nxray; echo RAY=$?; sleep 4; pidof nxvideo; echo PIDOF=$?; killall nxask',
         True, has=[r'^RAY=1', r'^ask: Notice: The GPU is drawing for nxvideo ', r'^PIDOF=0']),
    # the desktop's programs (docs/system/nanox.md, docs/machine/multicore.md, docs/apps/web.md)
    case('nxmon', 'desktop', X + 'NXMON_CHECK=1 nxmon & sleep 20; killall nxmon', True, has=[r'nxmon: 0 of 67200 pixels differ'],
         never=[r'nxmon: [1-9]\d* of 67200']),
    case('nxview', 'desktop media', 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm; '
         'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm', True,
         sums=[(r'nxview: .* sum ([0-9a-f]+) of', ['72ae6c0d', '95b8fd40'])]),
    case('nxpaint', 'desktop', X + 'NXPAINT_SCRIPT="new:200x120 fg:ff0000 tool:Rect size:2 drag:20,20:120,90 sum tool:Fill fg:00ff00 '
         'press:60,50 pixel:60,50 undo sum quit quit" nxpaint; NXPAINT_SCRIPT="new:40x30 saveas:/tmp/x sum open:/tmp/x.png sum quit quit" nxpaint',
         has=[r'pixel 60,50 is 00ff00', r'saved as /tmp/x\.png'], pairs=[r'nxpaint: (\d+ x \d+, sum [0-9a-f]+),']),
    case('nxask', 'desktop', X + "(nxkey 2500 n) & nxask 'Sure?' Yes No; echo ASK=$?", has=[r'^ASK=1']),
    case('nxpinball', 'desktop', X + 'PINBALL_DEMO=1800 nxpinball',   # docs/apps/pinball.md: three balls of a game played by itself
         has=[r'pinball: 1800 frames, 1 games, ball 3, high 0, score 18060 state 51ec1e4b']),
    # docs/apps/solitaire.md, "Checking it": deal 15 (tools/solitaire_deal.py) played by the pointer, the window's inside at 43,46
    # (a drag of 6c onto 7d; 5c picked, then put on 6h; a double click sends Ah up; the stock clicked and undone by the Undo
    # button; 8h dragged onto 5h, which it does not fit; closed, taken up again and undone once by the key)
    case('nxsolitaire', 'desktop', 'mkdir -p /persistence; ' + X + 'NXSOLITAIRE_PRINT=1 NXSOLITAIRE_DRAW=1 nxsolitaire 15 & sleep 6; '
         'nxkey drag:83,182:293,206 153,188 363,206 223,194 223,194 83,116 147,498 drag:503,218:153,182 alt+f4 2000; '
         'NXSOLITAIRE_PRINT=1 NXSOLITAIRE_DRAW=1 nxsolitaire & sleep 5; nxkey u 1000; killall nxsolitaire',
         has=[r'^nxsolitaire: dragged: moves 1, stock 24, waste 0, home 0 0 0 0, playing', r'^nxsolitaire: piles: waste:- .* t1:- \| .* t4:## ## ## 7d 6c \|',
              r'^nxsolitaire: picked t2, 1 cards', r'^nxsolitaire: placed: moves 2,', r'^nxsolitaire: piles: .* t2:5h \| t3:## ## Ah \| .* t5:## ## ## ## 6h 5c \|',
              r'^nxsolitaire: up: moves 3, stock 24, waste 0, home 1 0 0 0, playing', r'^nxsolitaire: piles: waste:- \| f1:Ah \| .* t3:## 6d \|',
              r'^nxsolitaire: stock: moves 4, stock 23, waste 1,', r'^nxsolitaire: piles: waste:2d \|', r'^nxsolitaire: undo: moves 3, stock 24, waste 0, home 1 0 0 0',
              r'^nxsolitaire: dropped back: moves 3,', r'^nxsolitaire: game kept', r'^nxsolitaire: resumed: moves 3, stock 24, waste 0, home 1 0 0 0, playing',
              r'^nxsolitaire: undo: moves 2, stock 24, waste 0, home 0 0 0 0, playing', r'^nxsolitaire: piles: waste:- .* t3:## ## Ah \|']),
    # every card but the queens and kings home: the Finish button plays them out; a queen sent up by a double click starts it by itself
    case('nxsolitaire-finish', 'desktop', 'mkdir -p /persistence; ' + X + 'NXSOLITAIRE_PRINT=2 NXSOLITAIRE_DEAL=nearly nxsolitaire & sleep 6; '
         'nxkey 279,498 3000; killall nxsolitaire nano-X; sleep 1; ' + X + 'NXSOLITAIRE_PRINT=2 NXSOLITAIRE_DEAL=nearly nxsolitaire & sleep 6; '
         'nxkey 83,212 83,212 3000; killall nxsolitaire',
         has=[r'^nxsolitaire: finish: moves 0,'], count=[(r'^nxsolitaire: finished: moves 8, stock 0, waste 0, home 13 13 13 13, won', 2)],
         sums=[(r'^nxsolitaire: statistics: (played \d+, won \d+, streak \d+)', ['played 1, won 1, streak 1', 'played 2, won 2, streak 2'])]),
    case('nxfiles', 'desktop', X + 'NXFILES_PRINT=1 nxfiles /usr/share & sleep 6; nxkey drag:643,86:343,196; sleep 4',
         has=[r'nxfiles: the box selected 8 ']),
    # docs/system/nanox.md, "A new name" and "Save to persistence": typed over the name, its ending kept; refused over 100 KB
    case('nxfiles-rename', 'desktop', X + 'mkdir -p /tmp/r/folder.d; echo x > /tmp/r/hello.txt; nxfiles /tmp/r & sleep 5; '
         'nxkey right right f2 x y z enter; sleep 2; ls /tmp/r | cat', has=[r'^xyz\.txt$', r'^folder\.d$'], never=[r'^hello\.txt$']),
    case('nxfiles-persist', 'desktop', X + 'mkdir -p /tmp/p /persistence; head -c 60000 /dev/urandom > /tmp/p/a.bin; '
         'head -c 50000 /dev/urandom > /tmp/p/b.bin; UI_ASK_PRINT=1 NXFILES_PRINT=1 nxfiles /tmp/p & sleep 5; '
         'nxkey right shift+f10 s enter; sleep 3; nxkey l; sleep 1; nxkey right shift+f10 s enter; sleep 3; ls /persistence | cat',
         has=[r'^nxfiles: saved to persistence: a\.bin', r'^a\.bin$',
              r'^ask: Not saved to persistence: b\.bin: 50\.0 KB, and /persistence holds 60\.\d KB already'], never=[r'^b\.bin$']),
    # what opens a file and its icon (docs/system/open.md, "Checking it"): by first bytes, else by ending in any case
    case('open-kinds', 'desktop media', KINDS_T + 'for f in /root/Music/* /root/Videos/* /root/Pictures/* /tmp/t/*; do open --dry "$f"; done',
         count=[(r'^open: /root/Music/.* is sound: nxplay ', 6), (r'^open: /root/Videos/.* is video: nxvideo ', 2),
                (r'^open: /root/Pictures/.* is picture: nxview ', 7)],
         has=[rf'^open: /tmp/t/{n} is {k}: {p} ' for n, k, p in (
             ('song', 'sound', 'nxplay'), ('tone', 'sound', 'nxplay'), ('A.FLAC', 'sound', 'nxplay'), ('a.oga', 'sound', 'nxplay'),
             ('film', 'video', 'nxvideo'), ('a.vqa', 'video', 'nxvideo'), ('fox', 'picture', 'nxview'), ('a.tga', 'picture', 'nxview'),
             ('a.pnm', 'picture', 'nxview'), ('a.pbm', 'picture', 'nxview'))] + [r'^open: /tmp/t/a\.xyz: unknown kind']),
    case('nxfiles-kinds', 'desktop media', KINDS_T + 'for d in /root/Music /root/Videos /root/Models /tmp/t /usr/bin; do nxfiles --kinds $d; done',
         has=[r'^nxfiles: Beethoven - Moonlight Sonata\.ogg: Sound, icon tune', r'^nxfiles: Big Buck Bunny\.avi: Video, icon video',
              r'^nxfiles: teapot\.obj: File, icon file', r'^nxfiles: a\.tga: Picture, icon picture', r'^nxfiles: a\.vqa: Video, icon video',
              r'^nxfiles: song: File, icon file', r'^nxfiles: nxplay: Program, icon nxplay', r'^nxfiles: nxgame: Program, icon nxgame'],
         count=[(r'^nxfiles: .*: Sound, ', 8)], never=[r'^nxfiles: (song|tone|film|fox|[^:]*\.(ogg|oga|flac|mp3|mod|wav|avi|mpg|vqa|tga|pnm|pbm|obj|ply|xyz|FLAC)): Program']),
    case('nxview-unnamed', 'desktop media', X + 'cp /root/Pictures/Earthrise.jpg /tmp/pic; NXVIEW_PRINT=1 nxview /tmp/pic & sleep 8; killall nxview',
         has=[r'^nxview: /tmp/pic, 480 x 480$']),
    case('nxweb', 'desktop', X + 'NXWEB_DUMP=1 nxweb & sleep 30; killall nxweb',   # tools/web_test.c gives the same
         has=[r'nxweb: /usr/share/web-index\.html: 122 nodes, 4 rules, 586 x 872; 77 texts \(1836 characters\), 2 boxes, 0 pictures, 0 fields']),
    # docs/apps/web.md, "Loading, and what did not come": each failure and why, a box for each picture
    case('nxweb-missing', 'desktop', X + 'NXWEB_DUMP=1 nxweb /usr/share/web-missing.html & sleep 20; killall nxweb', True,
         has=[r'nxweb: /usr/share/web-missing\.html: 36 nodes, 0 rules, 586 x 480; 10 texts \(525 characters\), 0 boxes, 5 pictures',
              r'nxweb: loading: Loading web-missing\.html: 0 of 1 picture \(5 not loaded\)',
              r'nxweb: not loaded: style sheet /usr/share/web-gone\.css: not found$',
              r'nxweb: not loaded: picture /usr/share/web-gone\.png: not found$', r'nxweb: not loaded: picture /usr/share/web-gone\.gif: not found$',
              r'nxweb: not loaded: picture /usr/share/web-test\.html: unsupported format', r'nxweb: /usr/share/web-missing\.html: all here, 5 not loaded']),
    # (no host answers rvc_cpu's fetches: the picture is given up after 30 s, its box laid out)
    case('nxweb-no-answer', 'desktop', X + 'NXWEB_DUMP=1 nxweb /usr/share/web-test.html & sleep 45; killall nxweb',
         has=[r'nxweb: loading: Loading web-test\.html: 0 of 1 picture$', r'nxweb: not loaded: picture https://example\.com/x\.png: no answer in 30 s',
              r'nxweb: /usr/share/web-test\.html: 88 nodes, 1 rules, 586 x 636; 40 texts \(520 characters\), 1 boxes, 1 pictures']),
    # a short first page ("No such page"): its strips were never made, nothing was shown
    # a server in the machine (docs/apps/web.md, "Addresses the machine reaches itself"): GitHub issue 1's shell loop,
    # then thttpd: by 127.0.0.1, by name, by the machine's LAN address, with a port, a form, a cookie, a redirect
    case('nxweb-issue', 'desktop web', X + WEB_ISSUE, more=['--net', '47310', '--net-id', '11'], wall=300,
         has=[r'nxweb: http: http://127\.0\.0\.1/: 200 text/plain, 34 bytes', r'nxweb: http: http://10\.0\.0\.11/?: 200 text/plain, 34 bytes',
              r'nxweb: http://10\.0\.0\.11/: \d+ nodes, \d+ rules, 586 x \d+; 1 texts \(33 characters\)', r'^CURL hello from the issue',
              r'nxweb: failed: http://127\.0\.0\.1:81/: 127\.0\.0\.1 \(127\.0\.0\.1\) refused the connection']),
    case('nxweb-http', 'desktop web', X + WEB_SITE + 'sleep 1', more=['--net', '47320', '--net-id', '12', '--expect', 'LX-DONE', '--send', WEB_HTTP], until='WEB-ENDED', wall=300,
         has=[r'nxweb: http: http://localhost:8080/: 200 text/html, \d+ bytes', r'nxweb: http: http://webhost:8080/: 200 text/html, \d+ bytes',
              r'nxweb: http: http://10\.0\.0\.12:8080/: 200 text/html, \d+ bytes',
              r'nxweb: http://localhost:8080/: \d+ nodes, 1 rules, 586 x \d+; \d+ texts \(\d+ characters\), \d+ boxes, 1 pictures, 3 fields',
              r'nxweb: http: http://localhost:8080/page\.css: 200 text/css', r'nxweb: http: http://localhost:8080/Fox\.png: 200 image/png',
              r'nxweb: the pictures waited for \d+ ms after the page.s text',
              r'nxweb: http: http://localhost:8080/cgi-bin/go\.cgi: 303', r'nxweb: http: http://localhost:8080/cgi-bin/show\.cgi: 200',
              r'^POSTED who=abcxyz1&box=on', r'^SAW sid=s1', r'nxweb: http: http://localhost:8080/none: 404',
              r'nxweb: saved http://localhost:8080/a\.bin as /root/Downloads/a\.bin, 2000 bytes', r'^SAVED 2000',
              r'nxweb: http: http://localhost:8080/plain\.txt: 200 text/plain']),
    # the page shown while it comes, and the window alive meanwhile (a key scrolls it; Escape keeps what came); chunks
    case('nxweb-slow', 'desktop web', X + WEB_SLOW, wall=180,
         has=[r'nxweb: http://127\.0\.0\.1:8081/: \d+ nodes.* 586 x \d+; \d+ texts', r'^nxweb: at http://127\.0\.0\.1:8081/, page 1 of 1, top [1-9]\d*, .*status "still loading"',
              r'nxweb: http: http://127\.0\.0\.1:8081/: 200 text/plain, 3\d\d\d bytes', r'nxweb: http: http://127\.0\.0\.1:8082/: 200 text/plain, \d+ bytes$',
              r'at http://127\.0\.0\.1:8082/.*status "stopped"', r'nxweb: http: http://127\.0\.0\.1:8083/: 200 text/html, 5 bytes, chunked',
              r'nxweb: http://127\.0\.0\.1:8083/: 4 nodes']),
    case('nxweb-short', 'desktop', X + 'NXWEB_TIME=1 nxweb /usr/share/web-nowhere.html & sleep 10; killall nxweb', has=[r'nxweb: the first picture \d+ ms after']),
    # the editor's reader (docs/system/nanox.md, "The editor's reader"): a page of every kind of Markdown as tools/web_test.c
    # --sections lays it out; a file of the documentation is read at once, another .md only with --read (or Ctrl+R)
    case('nxedit-read', 'desktop', X + 'NXEDIT_DUMP=1 nxedit --read /usr/share/md-test.md & sleep 10; killall nxedit; '
         'NXEDIT_DUMP=1 nxedit /usr/share/doc/shaderemu/README.md & sleep 10; killall nxedit; '
         'cp /usr/share/md-test.md /tmp/t.md; NXEDIT_DUMP=1 nxedit /tmp/t.md & sleep 6; killall nxedit',
         has=[r'^nxedit: read /usr/share/md-test\.md: 201 nodes, 526 x 1404; 89 texts \(855 characters, 4 links\), 59 boxes; 0 texts past the edge; sum e00ae1c0$',
              r'^nxedit: read /usr/share/doc/shaderemu/README\.md: \d+ nodes, 526 x \d+; \d+ texts \(\d+ characters, \d+ links\), \d+ boxes; 0 texts past the edge; sum'],
         never=[r'^nxedit: read /tmp/']),
    # the script languages (docs/system/scripts.md): QuickJS's pi example, MicroPython with its built-in modules
    # the benchmark (docs/apps/bench.md): every test's own check, the GPU's in a window of its own; some 30 s of the guest's
    case('nxperf', 'programs', X + 'nxperf --cli', True, has=[r'^nxperf: 61 results: 61 OK, 0 FAIL, 0 not run', r'^nxperf: score \d+ of 61 results', r'^nxperf: part GPU +\d+ \(5 results\)',
                                                                    r'^nxperf: beside it: RTX 3090, harness +\d+', r'^nxperf: beside it: 9950X3D, native +\d+'],
         never=[r'^nxperf: .* FAIL \(', r'Segmentation fault']),
    case('qjs', 'programs', "qjs /examples/pi.js 50; qjs --std -e 'print([1, 2, 3].map(x => x * x).join(), std.getenv(\"HOME\"))'",
         has=[r'^3\.14159265358979323846264338327950288419716939937510\r?$', r'^1,4,9 /root']),
    case('micropython', 'programs', "micropython -c 'import asyncio, json, os, re, random, time; print(json.dumps({\"a\": [1, 2]}), "
         "sum(range(1000)), re.sub(\"b+\", \"-\", \"abbbc\"), len(os.urandom(8))); exec(\"async def f():\" + chr(10) + \" return 42\"); print(asyncio.run(f()))'",
         has=[r'^\{"a": \[1, 2\]\} 499500 a-c 8\r?$', r'^42\r?$']),
    # the terminal programs on ncurses (docs/system/terminal.md)
    case('term-nano', 'programs', 'cd /tmp; nano t.txt; cat t.txt',
         more=['--expect', 'New File', '--send', 'hello nano\x0f', '--expect', 'Write', '--send', '\\r', '--expect', 'Wrote', '--send', '\x18'],
         wall=60, has=[r'hello nano\r?$']),
    case('term-mg', 'programs', 'cd /tmp; mg m.txt; cat m.txt',
         more=['--expect', 'C-h q', '--send', 'hello mg\\r\x18\x13', '--expect', 'Wrote', '--send', '\x18\x03'], wall=60, has=[r'hello mg\r?$']),
    case('term-htop', 'programs', 'htop -d 5; echo htop $?', more=['--expect', 'Quit', '--send', 'q'], wall=60, has=[r'^htop 0']),
    case('term-dialog', 'programs', 'dialog --inputbox Name 8 30 2>/tmp/a; echo dialog $?; cat /tmp/a; echo',
         more=['--expect', 'ancel', '--send', 'pterm\\r'], wall=60, has=[r'^dialog 0', r'^pterm']),
    case('term-sl', 'programs', 'sl; echo sl $?', wall=60, has=[r'sl 0\r?$']),
    case('term-file', 'programs', 'file /bin/busybox /usr/share/terminfo/v/vt100 /root/Models/bunny.ply', wall=60,
         has=[r'/bin/busybox: +BFLT executable', r'vt100: +Compiled terminfo entry "vt100"', r'bunny.ply: +PLY model']),
    case('term-mc', 'programs', 'cd /etc; mc; cat /tmp/x',   # a command line run in mc's subshell
         more=['--expect', 'Quit', '--send', 'echo sid=$MC_SID > /tmp/x\\r', '--expect', 'Quit', '--send', F10], wall=60, has=[r'^sid=\d+']),
    case('term-mcedit', 'programs', 'mcedit /tmp/e.txt; cat /tmp/e.txt; echo',
         more=['--expect', 'Quit', '--send', 'hello mcedit\x1b[[B', '--expect', 'ancel ]', '--send', '\\r' + F10], wall=60, has=[r'^hello mcedit']),
    # upstream's programs ported to the machine without an MMU (docs/system/ports.md)
    case('ports-pcre', 'programs', "echo foo123 bar45 | pcregrep -o '[0-9]+'; echo '/(a+)b/' > /tmp/p; echo aaab >> /tmp/p; pcretest /tmp/p",
         has=[r'^123$', r'^45$', r'^ 0: aaab', r'^ 1: aaa$']),
    case('ports-lockfile', 'programs', "lockfile-create /tmp/x && lockfile-check /tmp/x && echo LOCK''ED; lockfile-remove /tmp/x; "
         "lockfile-check /tmp/x || echo GO''NE; dotlockfile -l /tmp/y.lock && dotlockfile -u /tmp/y.lock && echo DOT''LOCK; mail-lock --help",
         has=[r'^LOCKED$', r'^GONE$', r'^DOTLOCK$', r'^usage: mail-lock']),
    case('ports-socat', 'programs', "socat -u TCP-LISTEN:5555,reuseaddr OPEN:/tmp/o,creat & sleep 1; echo socat-''ok | socat -u - TCP:127.0.0.1:5555; "
         "sleep 1; cat /tmp/o; filan -s | head -1; procan | head -1; "
         "socat -u TCP-LISTEN:5556,fork - & sleep 1; echo x | socat -u - TCP:127.0.0.1:5556; sleep 2",
         has=[r'^socat-ok$', r'^\s+0 tty /dev/hvc0', r'^process id = \d+', r'fork\(\): this machine has no MMU']),
    case('ports-thttpd-curl', 'programs', "mkdir -p /tmp/www/cgi-bin /tmp/www/sub; echo hello-''thttpd > /tmp/www/index.html; touch /tmp/www/sub/a.txt; "
         "f=/tmp/www/cgi-bin/t.cgi; echo '#!/bin/sh' > $f; echo 'echo Content-Type: text/plain; echo; echo cgi-ok $QUERY_STRING $REQUEST_METHOD' >> $f; "
         "echo '[ $REQUEST_METHOD = POST ] && head -c $CONTENT_LENGTH; true' >> $f; chmod +x $f; "
         "thttpd -p 8080 -d /tmp/www -c '**.cgi' -u root -l /dev/null & sleep 1; curl -s http://127.0.0.1:8080/; "
         "curl -s http://127.0.0.1:8080/sub/ | grep -o '>a.txt<'; curl -s 'http://127.0.0.1:8080/cgi-bin/t.cgi?q1'; "
         "curl -s -d posted-''body http://127.0.0.1:8080/cgi-bin/t.cgi; echo; curl -s -o /dev/null -w 'code %{http_code}' http://127.0.0.1:8080/none",
         has=[r'^hello-thttpd$', r'^>a\.txt<$', r'^cgi-ok q1 GET$', r'^cgi-ok POST$', r'^posted-body$', r'^code 404']),
    case('ports-dropbear', 'programs', "mkdir -p /root/.ssh; dropbearkey -t ed25519 -f /root/.ssh/id_dropbear | grep ^ssh- > /root/.ssh/authorized_keys; "
         "dropbear -R -p 2222 & sleep 2; ssh -y -p 2222 root@127.0.0.1 'echo ssh-''ok $(uname -m)'; echo file-''data > /tmp/f1; "
         "scp -P 2222 /tmp/f1 root@127.0.0.1:/tmp/f2; cat /tmp/f2; echo 'get /tmp/f2 /tmp/f3' > /tmp/b; sftp -P 2222 -b /tmp/b root@127.0.0.1; cat /tmp/f3",
         wall=300, has=[r'^ssh-ok riscv32$', r'^file-data$', r'^sftp> get /tmp/f2 /tmp/f3'], count=[(r'^file-data$', 2)]),
    case('ports-at', 'programs', "date -s '2026-10-18 12:00:00' > /dev/null; atd & sleep 1; echo 'echo at-''ran > /tmp/at.out' | at now; sleep 4; "
         "cat /tmp/at.out; echo true | at now + 1 hour; atq",
         has=[r'^at-ran$', r'^2\s+Sun Oct 18 13:00:00 2026 a root']),
    case('ports-iproute2', 'programs', 'ss -s | head -1; nstat -az | head -2; ifstat | grep "^lo "; genl ctrl list | grep nlctrl; lnstat -d | head -1; '
         'bridge link; tc -V; rtmon -V',
         has=[r'^Total: \d+', r'^IpInReceives\s+\d+', r'^lo\s+\d+', r'^Name: nlctrl', r'^/proc/net/stat/arp_cache:',
              r'^tc utility, iproute2-6\.16\.0', r'^rtmon utility, iproute2-6\.16\.0']),
    # the IDE (docs/apps/ide.md, "Checking it"): C built, run and debugged, JavaScript run, Python debugged,
    # each language's problems, the tree only with a project, and the storm of edits taken back
    case('nxide-c', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='build wait run wait mark:9 debug over over out into go quit quit' nxide",
         has=[r'^nxide\| 5 squared is 25, and the sum so far 55', r'^nxide: stopped at line 11, in square', r'^nxide: variable: result = 1',
              r'^nxide: stopped at line 20, in main'], count=[(r'^nxide: stopped at line', 6)]),
    case('nxide-js', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='example:pi run wait quit quit' nxide",
         has=[r'^nxide\| qjs --std .*/root/pi\.js', r'^nxide\| 3\.14159265358979323846264338327950288419716939937510582097494459',
              r'^nxide: the program ended, with 0']),
    case('nxide-py', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='example:primes mark:17 debug into over over out go wait quit quit' nxide",
         sums=[(r'^nxide: stopped at line (\d+), in (.*?)\r?$', ['17 the module', '7 sieve', '8 sieve', '9 sieve', '18 the module'])],
         has=[r'^nxide: variable: primes = \[2, 3, 5, 7, 11, .* 97\]', r'^nxide\| 25 primes below 100', r'^nxide: the program ended, with 0']),
    case('nxide-problems', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='example:pi line:118 type:nowhere();| run wait problems "
         "type:( wait wait problems quit quit quit' nxide; NXIDE_PRINT=1 NXIDE_SCRIPT='example:primes line:18 type:x_=_nowhere| run wait "
         "problems line:3 type:( wait wait problems quit quit quit' nxide",
         has=[r"^nxide: problem: line 118: ReferenceError: 'nowhere' is not defined", r"^nxide: problem: line 118: SyntaxError: expecting '\)'",
              r"^nxide: problem: line 18: NameError: name 'nowhere' isn't defined", r'^nxide: problem: line 3: SyntaxError: invalid syntax']),
    # a shader in the IDE (docs/apps/ide.md): hlslc's mistake marked at its line, and Run starts hlslview with it
    case('nxide-hlsl', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='example:fractal.hlsl line:36 type:nowhere();| wait wait problems "
         "quit quit quit' nxide; (sleep 15; killall hlslview) & NXIDE_PRINT=1 NXIDE_SCRIPT='example:fractal.hlsl run quit quit' nxide", True,
         has=[r'^nxide: problem: line 36: error: no function nowhere', r'^nxide: fractal\.hlsl: nothing wrong found',
              r'^nxide\| fractal: 20 pictures in', r'^nxide: the program was ended by signal 15']),
    case('nxide-tree', 'desktop ide', X + "mkdir -p /tmp/p/sub; echo 'print(1)' > /tmp/p/a_file_with_a_rather_long_name.py; echo x > /tmp/p/sub/b.js; "
         "NXIDE_PRINT=1 NXIDE_SCRIPT='project:/tmp/p noproject quit quit' nxide; NXIDE_PRINT=1 NXIDE_SCRIPT='tabs quit quit' nxide /tmp/p",
         has=[r'^nxide: project /tmp/p: 2 rows in its tree', r'^nxide: no project: no tree',
              r'^nxide: tab 0: /tmp/p/a_file_with_a_rather_long_name\.py \(shown\)']),
    case('nxide-storm', 'desktop ide', X + "NXIDE_PRINT=1 NXIDE_SCRIPT='storm:400 example:pi storm:400 example:primes storm:400 quit quit quit quit' nxide",
         count=[(r'^nxide: storm: 400 steps; \d+ changes taken back: the text is as it was', 3),
                (r'^nxide: storm: \d+ made again: the text is as the storm left it', 3)]),
    case('launch-note', 'desktop', 'export UI_LAUNCH=print; nx; sleep 4; nxkey 341,94 341,94; sleep 4', wall=120,
         has=[r'^launch: Calculator', r'^launch: a window shown after \d+ ms']),   # docs/system/nanox.md, "A program starting"
    # the desktop's fuzzer's short set (docs/system/fuzz.md; tools/app_fuzz.py for more): windows closed every way, a GPU program
    # after another, one refused with the first left open and its Notice; OpenTTD's X answered by a click on Yes (no process left)
    case('app-fuzz', 'desktop', 'nx; sleep 3; nxfuzz start:p:nxpaint wait:p:20 close:p:x start:g:glxgears wait:g:20 close:g:alt '
         'start:c:nxcalc wait:c:20 start:t:nxterm wait:t:20 rand:c:6:3 close:t:bar close:c:term start:e:nxedit wait:e:20 key:e:h,i close:e:x '
         'start:r:nxray wait:r:30 sleep:2000 close:r:kill start:g2:glxgears wait:g2:20 close:g2:x start:f:nxfiles,/root sleep:200 close:f:x end', True,
         wall=120, has=[r'^FZ-DONE 0', r'FZ closed g2'], never=[r'FZ-BAD']),
    case('app-fuzz-refused', 'desktop', 'nx; sleep 3; nxfuzz start:g:glxgears wait:g:20 start:r:nxray sleep:6000 wins close:g:term sleep:1000 wins start:i:imdemo wait:i:30 close:i:x end',
         True, wall=120, has=[r'the GPU is drawing for glxgears', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    # faults of the programs found going through them (docs/system/fuzz.md, "What it found"): each by its own window's keys and clicks
    case('apps-edit-triple', 'desktop apps', r"nx; sleep 3; (echo aa; echo bb; echo cc) > /tmp/e.txt; nxfuzz start:e:nxedit,/tmp/e.txt wait:e:20 click:e:90,28 click:e:90,28 drag:e:90,28,90,31 key:e:delete,ctrl+s close:e:x end; cat /tmp/e.txt", True,
         wall=120, has=[r'^FZ-DONE 0', r'^bb|^cc'], never=[r'FZ-BAD', r'^aa$']),
    case('apps-edit-move', 'desktop apps', r"nx; sleep 3; (echo A; echo B; echo C; echo D) > /tmp/e.txt; nxfuzz start:e:nxedit,/tmp/e.txt wait:e:20 key:e:ctrl+shift+down,ctrl+shift+down,ctrl+s close:e:x end; echo $(cat /tmp/e.txt)", True,
         wall=120, has=[r'^FZ-DONE 0', r'^B C A D']),
    case('apps-edit-huge-cut', 'desktop apps', r"nx; sleep 3; yes abcdefghi | head -c 2000000 > /tmp/e.txt; nxfuzz ask:60:100 start:e:nxedit,/tmp/e.txt wait:e:60 sleep:2000 key:e:ctrl+a,ctrl+x sleep:2000 key:e:ctrl+v sleep:2000 key:e:ctrl+s sleep:3000 close:e:x end; echo bytes=$(wc -c < /tmp/e.txt)", True,
         wall=240, has=[r'^FZ-DONE 0', r'^bytes=2000000']),
    case('apps-view-close', 'desktop apps', "nx; sleep 3; nxfuzz start:v:nxview,/tmp/no.png wait:v:20 sleep:1000 close:v:x end", True,
         wall=120, has=[r'^FZ-DONE 0', r'FZ closed v \(x\)'], never=[r'FZ-BAD']),
    case('apps-reg-full', 'desktop apps', "nxreg set a.kept 1; i=0; while [ $i -lt 400 ]; do nxreg set k.$i 0123456789012345678901234567890123456789012345678901234567890 || break; i=$((i+1)); done; echo full=$i; nxreg set a.kept 01234567890123456789012345678901234567890123456789012345678901234567890; echo set=$?; echo a=$(nxreg get a.kept)", True,
         wall=120, has=[r'^set=1', r'^a=1$']),
    case('apps-desk-stale', 'desktop apps wm', "nx; sleep 3; nxclock & c=$!; sleep 3; nxkey hold:795,588 drop:795,588 1000; kill $c; sleep 2; nxkey hold:795,588 drop:795,588 1000; sleep 1; ps | grep -c '[n]xbar' | sed s/^/bars=/", True,
         wall=120, has=[r'^bars=1']),
    case('apps-edit-empty', 'desktop apps', r"nx; sleep 3; : > /tmp/e.txt; nxfuzz start:e:nxedit,/tmp/e.txt wait:e:20 key:e:a,backspace,ctrl+s close:e:x end; echo bytes=$(wc -c < /tmp/e.txt)", True,
         wall=120, has=[r'^FZ-DONE 0', r'^bytes=0']),
    case('apps-open-self', 'desktop apps', r"nx; sleep 3; printf hello > /root/z.xyz; nxfuzz start:o:open,/root/z.xyz wait:o:20 sleep:500 click:o:230,74 sleep:1000 end; echo bytes=$(wc -c < /root/z.xyz)", True,
         wall=120, has=[r'^FZ-DONE 0', r'^bytes=5']),
    case('apps-calc-paste', 'desktop apps', r"nx; sleep 3; printf 1e+20 > /tmp/clipboard; nxfuzz start:c:nxcalc wait:c:20 key:c:ctrl+v,enter,ctrl+c close:c:x end; echo got=$(cat /tmp/clipboard)", True,
         wall=120, has=[r'^FZ-DONE 0', r'^got=.*e\+?20']),
    case('apps-calc-negative', 'desktop apps', r"nx; sleep 3; printf -- -5 > /tmp/clipboard; nxfuzz start:c:nxcalc wait:c:20 key:c:ctrl+v,enter,ctrl+c close:c:x end; echo got=$(cat /tmp/clipboard)", True,
         wall=120, has=[r'^FZ-DONE 0', r'^got=-5']),
    case('apps-menu-escape', 'desktop apps', "nx; sleep 3; nxfuzz start:a:nxplay wait:a:20 sleep:500 click:a:24,10 sleep:300 key:a:escape sleep:800 wins close:a:x end", True,
         wall=120, has=[r'^FZ-DONE 0', r'FZ closed a \(x\)', r'FZ win \d+ .*Music'], never=[r'FZ-BAD']),
    case('apps-video-small', 'desktop apps', "nx; sleep 3; echo R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7 | base64 -d > /tmp/s.gif; nxfuzz start:v:nxvideo,/tmp/s.gif wait:v:20 wins close:v:x end", True,
         wall=120, has=[r'^FZ-DONE 0', r'FZ win \d+ .* mapped at \S+ \d{3,}x'], never=[r'FZ-BAD']),
    case('apps-web-fifo', 'desktop apps', "nx; sleep 3; mkfifo /tmp/f; nxfuzz start:w:nxweb,/tmp/f wait:w:30 close:w:x end", True,
         wall=120, has=[r'^FZ-DONE 0', r'FZ closed w \(x\)'], never=[r'FZ-BAD']),
    case('apps-net-stalled', 'desktop apps', "nx; sleep 3; nxnet --serve & sleep 2; (sleep 60 | nc 127.0.0.1 7474) & sleep 2; a=$(date +%s); nxnet --ls 127.0.0.1 /root | head -2; echo took=$(( $(date +%s) - a ))", True,
         wall=120, has=[r'^took=\d$']),
    # a file of 2 MB through nxnet's server and --get on the loopback (docs/system/lan.md, "A file over the world's link":
    # two machines on rvc_cpu's --net-rate are its test, which test_all cannot run)
    case('nxnet-get', 'desktop apps', "nxnet --serve & sleep 2; head -c 2097152 /dev/urandom > /root/big; nxnet --get 127.0.0.1 /big /tmp/f; echo GET=$?; "
         '[ "$(md5sum < /root/big)" = "$(md5sum < /tmp/f)" ] && echo SAME; ls -l /tmp/f', wall=200, has=[r'^GET=0', r'^SAME', r' 2097152 .*/tmp/f']),
    case('apps-menus', 'desktop apps', "nx; sleep 3; nxfuzz start:e:nxedit wait:e:20 sleep:500 menus:e close:e:x end", True,
         wall=200, has=[r'^FZ-DONE 0', r'FZ menus e: 1\d menus, \d{2,3} rows'], never=[r'FZ-BAD']),
    # a folder level 1 leaves over the limit goes on to harder levels (docs/system/persist.md): no host answers here, so 404 is its having fit
    case('apps-persist-harder', 'desktop apps', 'mkdir -p /persistence; awk "BEGIN{srand(1);for(i=0;i<27500;i++)print int(rand()*1000000)}" > /persistence/t; nxpersist save; echo save=$?; rm /persistence/t', wall=300,
         has=[r'Compressing data harder', r'answer 404', r'^save=1']),
    case('apps-persist-big', 'desktop apps', "mkdir -p /persistence; dd if=/dev/zero of=/persistence/big bs=1M count=9 2>/dev/null; nxpersist save; echo save=$?; rm /persistence/big", wall=120, has=[r'^save=1']),
    case('apps-clock-kept', 'desktop apps', "nxreg set nxclock.alarm 09:30; nxreg set nxclock.size 500x420; nxreg set nxclock.24h 1; nxreg set nxclock.seconds 1; nxreg set nxclock.alarm_on 0; nxreg set nxclock.timer 300; nx; sleep 3; nxfuzz start:c:nxclock wait:c:20 wins close:c:x end", True,
         wall=120, has=[r'FZ win \d+ .* 50\dx4\d\d "', r'^FZ-DONE 0']),
    case('apps-icon-bad', 'desktop apps', "cp /usr/bin/nxreg /tmp/p; nxicon /tmp/p /root/Pictures/Fox.png; nxicon /tmp/p /nosuch; nxicon /tmp/p", wall=60, has=[r'/tmp/p has an icon of its own']),
    # nxray closed with one worker on a whole picture at the largest size: at the next tiles, not when the picture is whole
    case('apps-ray-one-worker-close', 'desktop apps', "export RAY_WORKERS=1 NANOX_SIZE=1024x768; nx; sleep 3; nxfuzz start:r:nxray wait:r:30 sleep:1000 key:r:r sleep:500 key:r:r sleep:1000 close:r:x end", True,
         wall=200, has=[r'FZ closed r \(x\)', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    case('wm-640-click', 'desktop wm', "export NANOX_SIZE=640x480 TDAWN_INPUT_LOG=1; nx; sleep 3; nxfuzz start:t:tdawn wait:t:90 sleep:30000 hclick:t:100,200 sleep:3000 close:t:kill start:r:ralert wait:r:90 sleep:30000 hclick:r:639,399 sleep:3000 close:r:kill", True,
         wall=280, has=[r'tdinput: button 1 down at 50,100 \(100,200 in the 640x400 window\)', r'tdinput: button 1 down at 639,399 \(639,399 in the 640x400 window\)']),
    case('apps-term-large', 'desktop apps', "nxreg set terminal.size 24; nx; sleep 3; nxfuzz start:t:nxterm wait:t:20 wins close:t:x end", True,
         wall=120, has=[r'FZ closed t \(x\)', r'^FZ-DONE 0', r'FZ win \d+ .* mapped at \S+ [0-7]\d\dx[0-5]\d\d "'], never=[r'FZ-BAD']),
    case('apps-micropolis-x-early', 'desktop apps micropolis', "nx; sleep 3; nxfuzz start:m:nxmicropolis wait:m:60 close:m:x end", True,
         wall=120, has=[r'FZ closed m \(x\) in \d{1,4} ms', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    # OpenTTD's other ways out (docs/games/openttd.md, "At exit"): the title's Exit, a link opened and the window then closed, the toolbar's Exit and Yes
    case('app-fuzz-openttd-exit', 'desktop games openttd', 'nx; sleep 3; nxfuzz ask:60:100 start:o:openttd wait:o:120 sleep:8000 click:o:329,345 sleep:3000 click:o:247,225 sleep:6000 tell:o:x sleep:1000 click:o:319,375 gone:o start:g:glxgears wait:g:20 close:g:x end', True,
         wall=240, has=[r'FZ closed o \(gone\)', r'^FZ-DONE 0'], never=[r'FZ-BAD', r'answered']),
    case('app-fuzz-openttd-menu', 'desktop games openttd', 'nx; sleep 3; nxfuzz ask:60:100 start:o:openttd,-g wait:o:120 sleep:40000 click:o:72,10 sleep:40000 click:o:77,68 sleep:40000 click:o:367,261 gone:o start:g:glxgears wait:g:20 close:g:x end', True,
         wall=240, has=[r'FZ closed o \(gone\)', r'^FZ-DONE 0'], never=[r'FZ-BAD', r'answered']),
    # a program that dies holding the GPU's lock word: the window system goes on (shaderemu_gpu_exiting_one)
    case('app-fuzz-lock', 'desktop', 'nx; sleep 3; nxfuzz start:l:nxfuzz,--die-with-gpu-lock sleep:500 start:g:glxgears wait:g:20 sleep:300 close:g:x end', True,
         wall=120, has=[r'FZ closed g', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    case('app-fuzz-openttd', 'desktop games openttd', 'nx; sleep 3; nxfuzz ask:40:100 start:o:openttd,-g,/usr/share/openttd/busy.sav wait:o:60 sleep:30000 '
         'tell:o:x sleep:2000 click:o:367,261 gone:o start:g:glxgears wait:g:20 close:g:x end', True, wall=240,
         has=[r'FZ closed o \(gone\)', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    # The window manager and the bar (docs/system/nanox.md, "The window manager and the bar"), on the real desktop, by keys and
    # the pointer; nxwintest list says each window's place and state, front first. tools/wm_shots.py has their pictures.
    case('wm-snap', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey hold:150,32 to:300,200 to:60,200 to:2,200 1000 drop:2,200 1000; nxwintest list; "
         "nxkey drag:100,12:400,200 1000; nxwintest list; nxkey hold:400,200 to:600,100 to:797,3 1000 drop:797,3 1000; nxwintest list; nxkey hold:600,12 to:400,100 to:400,0 1000 drop:400,0 1000; nxwintest list", wall=120,
         has=[r'^nxbar: snap preview 1', r'^nxbar: snap preview gone', r'^win 0 a 0,0 400x574 mapped tiled', r'^win 0 a 400,0 400x287 mapped tiled',
              r'^win 0 a 0,0 800x574 mapped maximised']),
    case('wm-keys', 'desktop wm', WM + "nxwintest plain a & sleep 2; L='nxwintest list'; nxkey ctrl+alt+up 800; $L; nxkey ctrl+alt+down 800; $L; nxkey ctrl+alt+right 800; $L; "
         "nxkey ctrl+alt+u 800; $L; nxkey ctrl+alt+k 800; $L; nxkey ctrl+alt+j 800; $L; nxkey ctrl+alt+i 800; $L; nxkey ctrl+alt+down 800; $L; nxkey ctrl+alt+shift+right ctrl+alt+shift+down 800; $L", wall=120,
         has=[r'^win 0 a 0,0 800x574 mapped maximised', r'^win 0 a 400,0 400x574 mapped tiled', r'^win 0 a 0,0 400x287 mapped tiled',
              r'^win 0 a 400,287 400x287 mapped tiled', r'^win 0 a 0,287 400x287 mapped tiled', r'^win 0 a 400,0 400x287 mapped tiled',
              r'^win 0 a 40,20 246x149 mapped', r'^win 0 a 56,36 246x149 mapped']),
    case('wm-double', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey 150,32 150,32 1000; nxwintest list; nxkey 150,12 150,12 1000; nxwintest list", wall=120,
         has=[r'^win 0 a 0,0 800x574 mapped maximised', r'^win 0 a 40,20 246x149 mapped focus']),
    case('wm-shade', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey ctrl+alt+s 800; nxwintest list; nxkey ctrl+alt+s 800; nxwintest list", wall=120,
         has=[r'^win 0 a 40,20 246x149 mapped shaded', r'^win 0 a 40,20 246x149 mapped focus']),
    case('wm-ontop', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey ctrl+alt+t 800; nxwintest plain b & sleep 2; nxwintest list", wall=120,
         has=[r'^win 0 a 40,20 246x149 mapped ontop', r'^win 1 b ']),
    case('wm-desktops', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey ctrl+alt+2 800; nxwintest plain b & sleep 2; nxwintest list; nxkey ctrl+alt+shift+3 800; nxwintest list; "
         "nxkey ctrl+alt+1 800; nxwintest list", wall=120,
         has=[r'^win \d a [\d,]+ 246x149 unmapped desk 0', r'^win \d b [\d,]+ 246x149 mapped focus desk 1', r'^win \d b [\d,]+ 246x149 unmapped desk 2', r'^win \d a [\d,]+ 246x149 mapped.* desk 0']),
    case('wm-switch', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxwintest plain b & sleep 2; nxwintest plain c & sleep 2; nxkey down:ctrl tab 500 tab 500 up:ctrl 800; nxwintest list; "
         "nxkey ctrl+tab 800; nxwintest list", wall=120,
         has=[r'^nxbar: switcher at b', r'^nxbar: switcher at a', r'^nxbar: switched to a', r'^win 0 a .* focus', r'^nxbar: switched to c', r'^win 0 c .* focus']),
    case('wm-menu', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey r:150,32 800 escape 500 ctrl+alt+space 800 down down enter 800; nxwintest list", wall=120,
         has=[r'^nxbar: the menu of a', r'^win 0 a 0,0 800x574 mapped maximised']),
    case('wm-remember', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey drag:150,32:350,132 800 alt+f4 2000; nxreg list | grep wm.place; nxwintest plain a & sleep 2; nxwintest list; nxreg set wm.remember 0; nxkey alt+f4 2000; nxwintest plain a & sleep 2; nxwintest list", wall=120,
         has=[r'^wm\.place\.nxwintest', r'^win 0 a 240,120 246x149 mapped', r'^win 0 a 120,60 246x149 mapped']),
    case('wm-group', 'desktop wm', WM + "nxwintest two a & sleep 2; nxwintest plain c & sleep 2; nxkey 180,586 800 enter 800; nxwintest list", wall=120,
         has=[r'^nxbar: buttons: \[c x3 active\]', r'^win \d (a|second) .* focus']),
    # a program that does not answer a close: asked, then ended: it took SIGTERM (ignore) or it took SIGKILL (stopped)
    case('wm-hang', 'desktop wm', WMC + "nxwintest hang h & P=$!; sleep 2; kill -STOP $P; nxkey alt+f4 3000 enter 4000; ps | grep -c 'nxwintes[t] hang'; nxwintest list", wall=120,
         has=[r'^nxbar: not responding: h', r'^nxbar: SIGTERM to h', r'^nxbar: SIGKILL to h', r'^0\s*$'], never=[r'^win \d h ']),
    case('wm-ignore', 'desktop wm', WMC + "nxwintest ignore i & sleep 2; nxkey alt+f4 3000 enter 2000; ps | grep -c 'nxwintes[t] ignore'; nxwintest list", wall=120,
         has=[r'^nxbar: not responding: i', r'^nxbar: SIGTERM to i', r'^0\s*$'], never=[r'^win \d i ']),
    case('wm-linger', 'desktop wm', WMC + "nxwintest linger l & sleep 2; nxkey alt+f4 4000 enter 2000; ps | grep -c 'nxwintes[t] linger'", wall=120,
         has=[r'^nxbar: outlives its window: l', r'^nxbar: SIGTERM to l', r'^0\s*$']),
    # a clean close asks nothing
    case('wm-close', 'desktop wm', WMC + "nxwintest plain p & sleep 2; nxkey alt+f4 4000; nxwintest list", wall=120,
         has=[r'^wins \d'], never=[r'^nxbar: not responding', r'^nxbar: outlives', r'^win \d p ']),
    # the close box takes the frame's corner, past its picture (wm_on_caption_box in wmaction.c): a press there closes
    case('wm-close-corner', 'desktop wm', WMC + "nxwintest plain p & sleep 2; nxwintest list; set -- $(nxwintest list | awk '/^win [0-9]+ p /{split($4,a,\",\"); split($5,b,\"x\"); print a[1]+b[1]-1\",\"a[2]+1}'); "
         "echo CORNER $1; nxkey $1 2000; nxwintest list", wall=120,
         has=[r'^CORNER \d+,\d+', r'^wins \d'], count=[(r'^win \d+ p ', 1)]),
    # and at the X's top left, where a beam's press lands short of its middle
    case('wm-close-box', 'desktop wm', WMC + "nxwintest plain p & sleep 2; nxwintest list; set -- $(nxwintest list | awk '/^win [0-9]+ p /{split($4,a,\",\"); split($5,b,\"x\"); print a[1]+b[1]-22\",\"a[2]+6}'); "
         "echo AT $1; nxkey $1 2000; nxwintest list", wall=120,
         has=[r'^AT \d+,\d+', r'^wins \d'], count=[(r'^win \d+ p ', 1)]),
    # and of a window without the keyboard (the desktop clicked first)
    case('wm-close-inactive', 'desktop wm', WMC + "nxwintest plain p & sleep 2; nxkey 600,300 1000; nxwintest list; set -- $(nxwintest list | awk '/^win [0-9]+ p /{split($4,a,\",\"); split($5,b,\"x\"); print a[1]+b[1]-22\",\"a[2]+6}'); "
         "echo AT $1; nxkey $1 2000; nxwintest list", wall=120,
         has=[r'^AT \d+,\d+', r'^wins \d'], count=[(r'^win \d+ p ', 1)]),
    # the Task Manager by its X (a program that closes on the window manager's request)
    case('wm-close-nxmon', 'desktop wm', WM + "nxmon & sleep 5; nxwintest list; set -- $(nxwintest list | awk '/ Monitor /{split($4,a,\",\"); split($5,b,\"x\"); print a[1]+b[1]-22\",\"a[2]+6}'); "
         "echo AT $1; nxkey $1 4000; nxwintest list; pidof nxmon || echo NXMON-GONE", wall=120,
         has=[r'^AT \d+,\d+', r'^NXMON-GONE']),
    # the same through the host's pointer, as in the world: held on the X (624,26), the left button from second 40 to 40.3
    case('wm-close-host', 'desktop wm', WM + "nxmon & sleep 5; nxwintest list; cut -d' ' -f1 /proc/uptime; while [ $(cut -d. -f1 /proc/uptime) -lt 45 ]; do sleep 1; done; "
         "nxwintest list; pidof nxmon || echo NXMON-GONE", wall=180,
         more=['--pointer-sweep', '0', '--pointer-circle', '624,26,4', '--pointer-button', '1,40,40.3'],
         has=[r'^NXMON-GONE']),
    # a button under the pointer is lit and put back when the pointer leaves (ui_hot_* in ui.h): Settings' tabs
    case('ui-hover', 'desktop wm', WM + "export UI_HOVER_PRINT=1; nxsettings & sleep 5; "
         "nxkey $(for y in 40 52 64 76 88 100 112 124; do echo at:200,$y at:260,$y; done) at:790,560 1000", wall=120,
         has=[r'^ui: lit \d+,6 \d+x22', r'^ui: unlit']),
    # a window opened under the pointer (in the middle at the start): no coming is told, the place is asked; Look's Classic
    case('ui-hover-under', 'desktop wm', WM + "export UI_HOVER_PRINT=1; nxsettings & sleep 5; nxkey right 1000 at:300,112 1000", wall=120,
         has=[r'^ui: lit 176,56 160x22']),
    # the bar's part under the pointer is lit (nxbar.c's bar_hover): Start, a window's button, desktop 2, and none off the bar
    case('bar-hover', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey at:20,587 800 at:250,587 800 at:520,587 800 at:400,300 1000", wall=120,
         has=[r'^nxbar: hot 0$', r'^nxbar: hot 200$', r'^nxbar: hot 301$', r'^nxbar: hot -1$']),
    # a caption's box under the host's pointer is lit and put back (wm_caption_hot): the pointer round the boxes of a
    # window at 40,20 246 wide, so moves into a box are heard through the quiet rectangle and out of it while one is lit
    case('wm-caption-hover', 'desktop wm', 'export NXWM_HOT_PRINT=1; ' + WM + "nxwintest plain a & sleep 2; nxwintest list; sleep 20", wall=120,
         more=['--pointer-sweep', '0', '--pointer-circle', '250,12,22'],
         has=[r'^win 0 a 40,20 246x', r'^nanowm: hot 1$', r'^nanowm: hot 2$', r'^nanowm: hot 3$', r'^nanowm: hot 0$']),
    # and a pointer that jumps and then stands still (rvc_cpu --pointer-at), over a program drawing in its window (glxgears at
    # 40,20, 306 wide, as a game): from the caption's title onto the minimise box, 50 ms later onto the close box and held there
    # 7 s. The close box stays lit: the last move is heard, the quiet rectangle never swallows it (7 November 2026, a report from
    # the world of the minimise box lit under a still pointer on the close box; the world's own pointer mapping was the cause)
    case('wm-caption-still', 'desktop wm', 'export NXWM_HOT_PRINT=1; ' + WM + "glxgears >/dev/null & sleep 4; nxwintest list; sleep 10", wall=120,
         more=['--pointer-at', '0,100,300', '--pointer-at', '14,200,30', '--pointer-at', '15,293,30', '--pointer-at', '15.05,335,30'],
         has=[r'^win 0 glxgears 40,20 306x'], count=[(r'^nanowm: hot 3$', 1), (r'^nanowm: hot 1$', 1)],
         never=[r'^nanowm: hot [02]$']),
    # the editors' text larger and smaller: Ctrl +, Ctrl and the wheel up, Ctrl 0; kept in the registry
    case('edit-zoom', 'desktop', WM + "echo hello > /tmp/z.c; nxedit /tmp/z.c & sleep 5; nxkey ctrl+= ctrl+= 800; echo Z1 $(nxreg get nxedit.zoom); "
         "nxkey down:ctrl wheel:300,200:1 up:ctrl 800; echo Z2 $(nxreg get nxedit.zoom); nxkey ctrl+- 800; echo Z3 $(nxreg get nxedit.zoom); "
         "nxkey ctrl+0 800; echo Z4 $(nxreg get nxedit.zoom); killall nxedit; nxreg unset nxedit.zoom", wall=120,
         has=[r'^Z1 2$', r'^Z2 3$', r'^Z3 2$', r'^Z4 0$']),
    case('ide-zoom', 'desktop', WM + "echo 'int main(void) { return 0; }' > /tmp/z.c; nxide /tmp/z.c & sleep 8; nxkey ctrl+= 800; echo Z1 $(nxreg get nxide.zoom); "
         "nxkey down:ctrl wheel:400,200:1 up:ctrl 800; echo Z2 $(nxreg get nxide.zoom); nxkey ctrl+0 800; echo Z3 $(nxreg get nxide.zoom); "
         "killall nxide; nxreg unset nxide.zoom", wall=120,
         has=[r'^Z1 1$', r'^Z2 2$', r'^Z3 0$']),
    # the Start menu's first level: the programs of start.first, the folders, the system's rows; a program started from it is
    # used recently; its right button's own menu takes Files off the first level
    case('start-menu', 'desktop wm', WM + "nxkey meta 1500 escape 800; nxkey meta 1500 enter; sleep 4; killall nxfiles; sleep 1; "
         "nxkey meta 1500 r:40,262 1000 60,294 1000; nxkey meta 1500 escape 500; echo FIRST $(nxreg get start.first); echo RECENT $(nxreg get start.recent); "
         "nxreg unset start.first; nxreg unset start.recent", wall=120,
         has=[r'^nxbar: start Files\|Editor\|Terminal\|Web\|Calculator\|Games\|Utilities\|Other\|Programming\|Settings\|Monitor\|Network\|Restart the desktop\|Save and shut down\|Shut down$',
              r'^nxbar: start Recently used\|Files\|Files\|Editor\|', r'^nxbar: own menu of nxfiles$',
              r'^nxbar: start Recently used\|Files\|Editor\|Terminal\|Web\|Calculator\|Games\|', r'^FIRST nxedit,nxterm,nxweb,nxcalc$', r'^RECENT nxfiles$']),
    # a dead client's windows go, with its button
    case('wm-kill', 'desktop wm', WM + "nxwintest two a & P=$!; sleep 2; nxwintest list; kill -9 $P; sleep 2; nxwintest list", wall=120,
         has=[r'^win 0 second', r'^nxbar: buttons: \s*$'], count=[(r'^win \d second', 1)]),
    # from the bar's pinned button (first at 49,586): a program on its way has a button, and after NXBAR_START_MS a question
    case('wm-slow', 'desktop wm', "nxreg set bar.pinned 'nxwintest slow 15'; export NXBAR_START_MS=4000; " + WM + "nxkey 49,586; sleep 2; nxwintest list; sleep 4; nxkey enter 500; sleep 14; nxwintest list; nxreg unset bar.pinned", wall=120,
         has=[r'^nxbar: starting ', r'^nxbar: slow to start', r'^win 0 slow '], never=[r'^nxbar: buttons: \[starting [^]]*\] \[starting']),
    # a program that crashes after its window: said so, with Start again (the first button) starting it anew
    case('wm-crash', 'desktop wm', "nxreg set bar.pinned 'nxwintest crash c'; export UI_LAUNCH=print; " + WM + "nxkey 49,586; sleep 8; nxwintest list; nxreg unset bar.pinned", wall=120,
         has=[r'^launch: Stopped working']),
    # the GPU's one drawer: a second program started from the bar is not a dead end; the question offers to close the first and starts it again
    case('wm-gpu', 'desktop wm', "nxreg set bar.pinned 'sh -c glxgears'; export UI_ASK_PRINT=1 UI_LAUNCH=print; " + WM + "glxgears >/dev/null & P=$!; sleep 3; nxkey 49,586; sleep 4; nxwintest list; nxkey enter; sleep 6; nxwintest list; "
         "kill -0 $P 2>/dev/null || echo FIRST-GONE; cat /proc/gpu | head -3; nxreg unset bar.pinned", wall=120,
         has=[r'^FIRST-GONE', r'^win \d .*gears']),
    # more of a mouse and the hands (docs/machine/input.md): the wheel tilted, the left hand's stick as a second wheel, the right hand's,
    # the side buttons and a hand's trigger, as rvc_cpu's --scroll and --pointer-button give them; a window that asked (GR_WMC_BUTTONS)
    # has them as button events of their own, evdev has REL_HWHEEL, REL_DIAL, REL_MISC and BTN_SIDE, BTN_EXTRA, BTN_TRIGGER_HAPPY3
    case('input-more', 'desktop', X + 'nxinput 6', wall=120,
         more=['--scroll', '5,x,2', '--scroll', '5.5,ly,-1', '--scroll', '5.6,lx,1', '--scroll', '6,ry,1', '--pointer-button', '0x419,6.5,7'],
         has=[r'^nx down 0x200 ', r'^nx down 0x2000 ', r'^nx down 0x8000 ', r'^nx down 0x20 ', r'^nx down 0xc00 ', r'^nx up 0xc00 ',
              r'^ev rel 6 2', r'^ev rel 7 -1', r'^ev rel 9 1', r'^ev rel 8 1', r'^ev key 0x113 1', r'^ev key 0x114 1', r'^ev key 0x2c2 1',
              r'^word 0x419', r'^nxinput done']),
    # and a window that did not ask has the usual buttons alone: most programs take any button that goes down for a click
    case('input-plain', 'desktop', X + 'nxinput 6 plain', wall=120,
         more=['--scroll', '5,x,2', '--scroll', '5.5,ly,-1', '--scroll', '6,ry,1', '--pointer-button', '0x419,6.5,7'],
         has=[r'^nx down 0x20 ', r'^nx down 0x1 ', r'^ev rel 6 2', r'^nxinput done'], never=[r'^nx (down|up) 0x(200|2000|c00|c01) ']),
    # the desktop's games played by a controller (docs/machine/input.md, "Game controls"): the hands' bits (0x80 told, 0x100 the left
    # trigger, 0x400 the right, 0x8000 B, 0x10000 A) and the sticks' notches, with the pointer over each window (WM places it at 40,20)
    case('pad-tetris', 'desktop games', X + 'NXTETRIS_PRINT=1 NXTETRIS_SEED=1 nxtetris & sleep 4; nxkey enter; sleep 10; killall nxtetris', wall=120,
         more=['--pointer-sweep', '0', '--pointer-circle', '400,300,0', '--scroll', '12,lx,-2', '--scroll', '12.5,ly,-1', '--scroll', '13,y,1',
               '--scroll', '13.3,x,-1', '--pointer-button', '0x80,7,16', '--pointer-button', '0x10000,14,14.3'],
         has=[r'^nxtetris: pad left: piece \d at 2,', r'^nxtetris: pad left: piece \d at 1,', r'^nxtetris: pad down: .* score 1$',
              r'^nxtetris: pad cw: .* turned 1,', r'^nxtetris: pad ccw: .* turned 0,', r'^nxtetris: pad drop: ']),
    # the C&C games: a notch of the left hand's stick is an arrow pressed and let go (the map scrolls a step) over Tiberian Dawn's window
    case('pad-tdawn', 'games tdawn', X + 'TDAWN_INPUT_LOG=1 ' + TD + 'TDAWN_SCEN=10 TDAWN_FRAMES=2000 tdawn FROMINSTALL', True, until='pad key 38 up', wall=300,
         more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0', '--scroll', '14,lx,-2', '--scroll', '15,ly,1'],
         count=[(r'tdinput: pad key 37 down', 2)], has=[r'tdinput: pad key 38 down']),
    case('pad-ralert', 'games ralert', X + 'TDAWN_INPUT_LOG=1 TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_FRAMES=2000 ralert', True,
         until='pad key 38 up', wall=300, more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0', '--scroll', '20,lx,-2', '--scroll', '21,ly,1'],
         count=[(r'tdinput: pad key 37 down', 2)], has=[r'tdinput: pad key 38 down']),
    # OpenTTD: a notch holds an arrow 120 ms (the main view scrolls); Quake: the jump button (A, bit 16) jumps
    case('pad-openttd', 'games openttd', X + 'OPENTTD_PAD_LOG=1 openttd -g /usr/share/openttd/busy.sav', True, until='pad pans down', wall=400,
         more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0', '--scroll', '60,lx,-1', '--scroll', '62,ly,-1', '--scroll', '64,ly,-1',
               '--scroll', '66,ly,-1'],
         has=[r'openttd: pad pans left', r'openttd: pad pans down']),
    case('pad-quake', 'games quake', X + 'QUAKE_TRACK=5 quake +map e1m1 +host_framerate 0.05', True, until='track 200:', wall=300,
         more=['--pointer-button', '0x10080,32,32.5'], has=[r'quake: track \d+: .* on ground 0']),
    # the NES: a notch of the left hand's stick holds the d-pad's way 9 frames, A and B are the pad's (the frames' state sums show the presses)
    case('pad-nes', 'games nes', X + 'NES_PAD_LOG=1 NES_FRAMES=900 nes thwaite', True, wall=300,
         more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0', '--scroll', '8,ly,-1', '--scroll', '9,lx,1',
               '--pointer-button', '0x10080,10,10.5'],
         has=[r'nes: pad holds down', r'nes: pad holds right']),
    # Doom: A or B uses (DOOM_PAD_LOG says the first); Quake III: the hands' buttons as its keys (QUAKE3_PAD_LOG); ClassiCube unseated: a
    # notch of the left hand's stick holds W, the jump button is Space
    case('pad-doom', 'games doom', X + 'DOOM_PAD_LOG=1 doom -warp 1 1 & sleep 40; killall doom.bin', True, wall=300,
         more=['--pointer-button', '0x10080,20,20.5'], has=[r'doom: pad uses']),
    # Quake from a holodeck's seat (docs/world/holodeck.md, "Transitions"; a desktop sitter's words, the stick pushed: it walks): the menu
    # opened and closed after 30 frames, a kill, a level change; tools/volume_frames.py on --volume-log: flat while the menu is up and
    # during the load, the world with its HUD again after each, never an empty world over a second
    case('quake-seated', 'games quake holodeck', X + 'QUAKE_TRACK=5 quake +map e1m1', True, until='track 400:', wall=600, volume=True,
         more=['--host', '00000112,0,0,0,0,00007f00,0,0', '--expect', 'track 40:', '--send', 'togglemenu; ' + 'wait; ' * 30 + 'togglemenu\\n',
               '--expect', 'track 80:', '--send', 'kill\\n', '--expect', 'track 160:', '--send', 'changelevel e1m2\\n'],
         has=[r'maps/e1m2\.bsp', r'volume_frames: .*flat screen', r'volume_frames: .*world \+ HUD \(at most \d+ drawn flat\)',
              r'volume_frames: frames: .*empty worlds over a second: 0', r'volume_frames: exit 0']),
    # Jedi Outcast from the seat (an image with the demo; docs/world/holodeck.md): the level loaded flat, then the sitter's stick walks
    # it ("seat: 2 ... move 127 0 0", JK2_SEAT=1), the world with its HUD, no empty world over a second
    case('jk2-seat', 'games jk2 holodeck', 'if [ -d /usr/share/jk2/installed ]; then jk2 --unpack; nano-X -p & sleep 2; JK2_SEAT=1 jk2 +map demo; '
         "else echo Type y''es; read x; echo JK2-NO-DATA move 127 0 $((0)); fi", until='move 127 0 0', wall=600, volume=True,   # (no data: what it waits for at once, not typed whole)
         more=['--expect', 'Type yes', '--send', 'yes\\n', '--host', '00000112,0,0,0,0,00007f00,0,0'],
         has=[r'(seat: 2 aim .* move 127 0 0|JK2-NO-DATA)', r'(volume_frames: exit 0|JK2-NO-DATA)']),
    # Half-Life from the seat (an image with Uplink's data): the menu at the level's second 10 (flat) closed by Escape at the guest's
    # 125 s, a kill at 20, a new level at 30 (HL_SCRIPT; the script's text split so that the shell's echo is not the marks it waits for)
    case('halflife-seated', 'games halflife holodeck', HL_LICENCE + X + 'if [ -f /usr/share/halflife/valve/pak0.PAK ]; then S=10:togglemenu; '
         'S="$S|20:kill|30:map hl"; HL_SCRIPT="${S}demo2|40:echo SEAT-$((1+1))" halflife -dev 1 +map hldemo1; else echo HL-NO-$((0))DATA; echo SEAT-$((1+1)); fi',
         True, until='SEAT-2', wall=900, volume=True,
         more=['--host', '00000112,0,0,0,0,00007f00,0,0', '--input-event', '125,80000001', '--input-event', '125.3,1'],
         has=[r'(Spawn Server: hldemo2|HL-NO-0DATA)', r'(script 20\.\d+: kill|HL-NO-0DATA)', r'(volume_frames: .*flat screen|HL-NO-0DATA)',
              r'(volume_frames: exit 0|HL-NO-0DATA)']),
    # Half-Life (an image with Uplink's data; else HL-NO-DATA): the hands' buttons as its keys, A the jump and B the use (HL_PAD_LOG)
    case('pad-halflife', 'games halflife', HL_LICENCE + X + 'if [ -f /usr/share/halflife/valve/pak0.PAK ]; then HL_PAD_LOG=1 halflife -dev 1 +map hldemo1 & '
         'sleep 150; killall halflife.bin; else echo HL-NO-DATA; fi', True, wall=900, more=['--pointer-button', '0x18080,120,120.5'],
         has=[r'(halflife: pad down 18080|HL-NO-DATA)']),
    case('pad-quake3', 'games quake3', X + 'QUAKE3_PAD_LOG=1 ' + Q3, True, until='pad down 10080', wall=400,
         more=['--pointer-button', '0x10080,45,45.5'], has=[r'quake3: pad down 10080']),
    case('pad-classicube', 'games classicube', CC + 'CLASSICUBE_PAD_LOG=1 CLASSICUBE_SEED=7 classicube', True, until='pad key', wall=300,
         more=['--pointer-sweep', '0', '--pointer-circle', '300,250,0', '--expect', 'classicube: steady', '--send', '\\n',
               '--scroll', '30,ly,1', '--pointer-button', '0x10080,32,32.5'],
         has=[r'classicube: pad walks W']),
    # ClassiCube's pause menu: a click on "Host on the LAN" (left column, a row under "Hotkeys...") hosts (docs/games/classicube.md, "Played together")
    case('classicube-lan-menu', 'games classicube', CC + 'CLASSICUBE_SEAT=1 CLASSICUBE_LAN_LOG=1 CLASSICUBE_SEED=7 classicube', True, until='Hosting on',
         wall=300, more=['--expect', 'classicube: steady', '--send', 'down Escape\\nup Escape\\n', '--expect', 'seat:',
                         '--send', 'point 160 340\\ndown LeftMouse\\nup LeftMouse\\n'],
         has=[r'lan: Hosting on the LAN at [\d.]+:25565']),
    case('pad-pinball', 'desktop games', X + 'NXPINBALL_PRINT=1 nxpinball & sleep 4; nxkey space; sleep 8; killall nxpinball', wall=120,
         more=['--pointer-button', '0x80,5,20', '--pointer-button', '0x100,8,8.5', '--pointer-button', '0x400,9,9.5', '--pointer-button', '0x10000,10,11'],
         has=[r'^nxpinball: left flipper held', r'^nxpinball: left flipper let go', r'^nxpinball: right flipper held', r'^nxpinball: plunger held',
              r'^nxpinball: plunger let go']),
    # 2048 played by itself, then by keys (linux/apps/nx2048.c); Life's generations on the workers and on the first core
    # alone, which must come to the same grid (linux/apps/nxlife.c)
    case('nx2048', 'desktop games', X + 'NX2048_PRINT=1 NX2048_SEED=3 NX2048_AUTO=60 nx2048 & sleep 4; nxkey left 300 up 300 right; sleep 2; '
         'killall nx2048', wall=120,
         has=[r'^nx2048: auto 60 moves: score \d+, tile \d+, free \d+, sum [0-9a-f]{8}$', r'^nx2048: left: score', r'^nx2048: up: score']),
    # (ten generations: on the first core alone one is about 2.4 million instructions, a second of the machine's)
    case('nxlife', 'desktop games', X + 'NXLIFE_SEED=7 NXLIFE_GENS=10 nxlife & sleep 8; killall nxlife', True, wall=120,
         has=[r'^nxlife: 10 generations, population \d+, sum [0-9a-f]{8}, [1-9]\d* workers']),
    case('nxlife-core0', 'desktop games', X + 'NXLIFE_CORES=0 NXLIFE_SEED=7 NXLIFE_GENS=10 nxlife & sleep 25; killall nxlife', wall=120,
         has=[r'^nxlife: 10 generations, .* 0 workers'],
         same=[(r'^nxlife: 10 generations, (population \d+, sum [0-9a-f]{8})', 'nxlife')]),
    case('pad-mines', 'desktop games', X + 'NXMINE_PRINT=1 NXMINE_SEED=5 nxmine & sleep 11; killall nxmine', wall=120,
         more=['--pointer-sweep', '0', '--pointer-circle', '120,150,0', '--pointer-button', '0x80,5,20', '--scroll', '8,lx,2', '--scroll', '8.3,ly,-1',
               '--pointer-button', '0x10000,9,9.3'],
         has=[r'^nxmine: open: 9x9 10 mines, 18 opened']),
    case('pad-chess', 'desktop games', X + 'NXCHESS_PRINT=1 NXCHESS_WORKERS=0 NXCHESS_MS=200 nxchess & sleep 14; killall nxchess', wall=120,
         more=['--pointer-sweep', '0', '--pointer-circle', '200,200,0', '--pointer-button', '0x80,5,20', '--pointer-button', '0x10000,6,6.3',
               '--pointer-button', '0x10000,7,7.3', '--scroll', '8,ly,2', '--pointer-button', '0x10000,9,9.3', '--pointer-button', '0x8000,13,13.3'],
         has=[r'^nxchess: played: 1 moves, last e4 e2e4', r'^nxchess: undo: 0 moves']),
    # a pointer that has no edge (a beam: docs/system/nanox.md, "A drag that leaves the screen"): its moves stop short of the edge and the button comes up there,
    # after a step that would have reached it. A mouse's slow last steps to the same place, or a wait there, do not snap
    case('wm-snap-beam', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey hold:150,32 to:400,100 to:690,100 to:750,100 drop:750,100 1000; nxwintest list", wall=120,
         has=[r'^win 0 a 400,0 400x574 mapped tiled']),
    case('wm-snap-mouse', 'desktop wm', WM + "nxwintest plain a & sleep 2; nxkey hold:150,32 to:400,100 to:740,100 to:745,100 to:750,100 drop:750,100 1000; nxwintest list; "
         "nxkey hold:750,100 to:690,100 to:750,100 1000 drop:750,100 1000; nxwintest list", wall=120,
         has=[r'^win 0 a \d+,\d+ 246x149 mapped'], never=[r'^win 0 a .*tiled']),
    # a window as wide as the screen keeps its frame and its button, the frame's sides off the screen (wmclients.c); closed by its X, Alt+F4 and the bar
    case('wm-640-game', 'desktop wm tdawn', 'export NANOX_SIZE=640x480; ' + WM + "nxfuzz ask:60:100 start:g:tdawn,FROMINSTALL wait:g:90 sleep:1000 wins close:g:x start:h:ralert,FROMINSTALL wait:h:90 sleep:1000 wins close:h:alt start:q:quake,+menu_main wait:q:60 sleep:1000 wins close:q:bar end", True, wall=240,
         has=[r'mapped at -3,0 646x429 "Tiberian Dawn"', r'mapped at -3,0 646x429 "Red Alert"', r'mapped at -3,0 646x454 "Quake"', r'FZ closed g \(x\)', r'FZ closed h \(alt\)', r'FZ closed q \(bar\)',
              r'^nxbar: buttons: \[Tiberian Dawn x1', r'^nxbar: buttons: \[Red Alert x1', r'^FZ-DONE 0'], never=[r'FZ-BAD']),
    # a title said while another window has the keyboard reaches the bar's button (GR_WM_TELL_TITLE)
    case('wm-retitle', 'desktop wm', WM + "echo 'BEGIN{printf \"%c]0;Retitled%c\",27,7}' > /tmp/a.awk; (echo '#!/bin/sh'; echo 'sleep 6'; echo 'awk -f /tmp/a.awk'; echo 'sleep 3') > /tmp/rt; chmod +x /tmp/rt; nxterm /tmp/rt & sleep 2; nxwintest plain b & sleep 8; killall nxterm", wall=120,
         has=[r'^nxbar: buttons: .*\[Retitled x1']),
    # a game the bar starts opens maximised (GR_WMC_MAXIMISE, wmclients.c), and those it starts; not with wm.maximise_games 0
    case('wm-game', 'desktop wm', "echo '#!/bin/sh' > /tmp/g; echo 'nxwintest plain g' >> /tmp/g; chmod +x /tmp/g; echo 'Games/Test game=/tmp/g' > /usr/share/nxapps.99-test; "
         "nxreg set bar.pinned /tmp/g; " + WM + "nxkey 49,586; sleep 4; nxwintest list; nxkey alt+f4 2000; nxreg set wm.maximise_games 0; "
         "nxkey 49,586; sleep 4; nxwintest list; nxreg unset bar.pinned; nxreg unset wm.maximise_games", wall=120,
         # (opened at its maximised size, SE_MAXIMISED: so the window manager's maximise is no resize)
         has=[r'^win 0 g 0,0 800x574 mapped maximised', r'^win 0 g [\d,]+ 246x149 mapped focus', r'^opened 794x545'],
         never=[r'^resized'], count=[(r'^opened', 1)]),
    # a desktop shortcut with game=1 (nxfiles --run, as a double click): the same, the file read again by its whole name
    case('wm-game-shortcut', 'desktop wm', "true; echo game=1 > /tmp/q.lnk; echo run=nxwintest plain q >> /tmp/q.lnk; " + WM +
         "nxfiles --run /tmp/q.lnk; sleep 4; nxwintest list", wall=90,
         has=[r'^win 0 q 0,0 800x574 mapped maximised', r'^opened 794x545'], never=[r'^resized']),
    case('wm-single', 'desktop wm', WM + "nxsettings & sleep 4; nxwintest plain b & sleep 2; nxwintest list; nxsettings; echo SECOND=$?; sleep 1; nxwintest list", wall=120,
         has=[r'^SECOND=0', r'^win 0 Settings .* focus'], count=[(r'^win \d Settings', 2)]),
]


def matches(text, pattern):
    return [m.group(0) if m.lastindex is None else ' '.join(m.groups()) for m in re.finditer(pattern, text, re.M)]


def digest(items):
    return hashlib.md5('\n'.join(items).encode()).hexdigest()[:8]


def check(c, text, err, races, done):
    """The reasons the case failed (an empty list if it passed)."""
    why = []
    if '--until matched' not in err:
        why.append(f'did not reach "{c["until"]}"')
    for p in c.get('has', []):
        if not re.search(p, text, re.M):
            why.append(f'no line /{p}/')
    for p in c.get('never', []):
        if re.search(p, text, re.M):
            why.append(f'a line /{p}/')
    for p, n in c.get('count', []):
        got = len(re.findall(p, text, re.M))
        if not (n[0] <= got <= n[1] if isinstance(n, tuple) else got == n):
            why.append(f'{got} lines /{p}/, not {n}')
    for p, want in c.get('sums', []):
        got = matches(text, p)
        if got != want:
            why.append(f'/{p}/ gave {got}, not {want}')
    for p, want, *how in c.get('digest', []):
        got = matches(text, p)
        d = digest(sorted(got) if how else got)
        if d != want:
            why.append(f'/{p}/: {len(got)} lines, digest {d}, not {want}')
    for p in c.get('pairs', []):   # every second match the one before it (an undo, a file read back)
        got = matches(text, p)
        if len(got) < 2 or any(got[i] != got[i + 1] for i in range(0, len(got) - 1, 2)):
            why.append(f'/{p}/ not in equal pairs: {got}')
    for p, other, *least in c.get('same', []):
        if other not in done:
            why.append(f'{other} was not run')
            continue
        if least:   # keyed by the first group: compared where both have the key
            a = {m[1]: m.groups()[1:] for m in re.finditer(p, text, re.M)}
            b = {m[1]: m.groups()[1:] for m in re.finditer(p, done[other], re.M)}
            both = [k for k in a if k in b]
            n = sum(a[k] == b[k] for k in both)
            if n != len(both) or n < least[0]:
                why.append(f'/{p}/: {n} of {len(both)} as in {other} (at least {least[0]})')
            continue
        a, b = matches(text, p), matches(done[other], p)
        if a != b:
            n = sum(x == y for x, y in zip(a, b))
            why.append(f'/{p}/: {n} of {max(len(a), len(b))} as in {other}')
    if races:   # (in sections, each call says its own)
        m = re.findall(r'(\d+) changes undone', err)
        if not m or sum(map(int, m)) != 0:
            why.append(f'RVC_RACES: {sum(map(int, m)) if m else "no"} changes undone')
    return why


def images(a):
    """The image folder of each kind of case: the one given, a private one with zerotest, the lazy test's."""
    out = {None: a.image}
    if any(c['image'] == 'zerotail' for c in a.run) and without_mmu(a.image):
        out['zerotail'] = zerotail_nommu()
    elif any(c['image'] == 'zerotail' for c in a.run):
        name = 'test_all'
        bin_ = os.path.join(ROOT, 'build', 'images', name, 'overlay', 'usr', 'bin')
        os.makedirs(bin_, exist_ok=True)
        repo = '/mnt/' + ROOT[0].lower() + ROOT[2:].replace('\\', '/')
        script = f'source {repo}/linux/userland/toolchain.sh && cd {repo} && ' + ' && '.join(
            f'rv32-cc -O2 linux/kernel/{t}.c -o build/images/{name}/overlay/usr/bin/{t}' for t in ('zerotest', 'tailtest'))
        subprocess.run(['wsl', '--', 'bash', '-c', script], check=True)
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'private_image.py'), name], check=True, capture_output=True)
        out['zerotail'] = f'build/images/{name}'
    if any(c['image'] == 'lazy' for c in a.run):
        folder = os.path.join(ROOT, 'build', 'test_all_lazy')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_lazy_image.py'), folder], check=True, capture_output=True)
        out['lazy'] = os.path.join(folder, 'build', 'images', 'linux')
    return out


def zerotail_nommu():
    """build/images/test_all without the MMU: the flat root with zerotest and tailtest, made flat."""
    name, built = 'test_all', os.path.join(ROOT, 'build', 'nommu', 'flatroot')
    flat = built if os.path.isdir(built) else os.path.join(ROOT, 'linux', 'prebuilt', 'nommu', 'root')
    root = os.path.join(ROOT, 'build', 'test_all-root')
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(flat, root, symlinks=True)
    repo = '/mnt/' + ROOT[0].lower() + ROOT[2:].replace('\\', '/')
    script = (f'export PATH=$HOME/nm/work/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH && cd {repo} && ' +
              ' && '.join(f'rv32-cc -O2 linux/kernel/{t}.c -o build/test_all-root/usr/bin/{t}' for t in ('zerotest', 'tailtest')) +
              ' && python3 linux/nommu/flat.py build/test_all-root/usr/bin')
    subprocess.run(['wsl', '-e', 'bash', '-c', script], check=True, capture_output=True)   # (-e: no shell between to expand $)
    subprocess.run([sys.executable, os.path.join(ROOT, 'linux', 'nommu', 'image.py'), name, root], check=True, capture_output=True,
                   env=dict(os.environ, HALFLIFE_DATA='0', AOE_IMAGE='0', JK2_IMAGE='0'))
    return f'build/images/{name}'


# without the MMU a ROM file is mapped where it lies in the ROM, the next file's bytes after its end
# (16-byte romfs packing): what the pool hands out is checked on copies in tmpfs
ZERO_TAIL_NOMMU = ('zerotest 12 4; mkdir -p /tmp/t; find /usr/share /usr/bin /etc -maxdepth 1 -type f -size -256k | '
                   'while read f; do cp "$f" /tmp/t/; done; tailtest /tmp/t/*')
CC_BYTES_NOMMU = '479cdac1'   # main of 6 November 2026 (192af201) built whole in ~/nm: cf92a103's libgles.a with libnano-X.a of 4210d01b (microwindows.patch, the buttons and bar lit under the pointer), which vram-gpu did not have when cf92a103 was set, latest; cf92a103: libgles.a with gpu_list_uploads (programs/common/gpu_uploads.h: every texture a list names in RAM copied into the VRAM by the list, the GPU's draws reading the VRAM alone; 5 November 2026), latest; a7f26442: b660af4f with libgles.a's volume words named by a worker's swap (seglSwapHand) and libtcc1.a's FILE symbols named /nm/work (linux/tcc/build.sh), built after setup.sh in two homes that differ only by their folder (~/agents/vram, ~/agents/vram2): every library and tcc the same bytes in both (5 November 2026); before it the runtime's objects kept the home's whole path, so the value moved with the build home: 25995171 is ~/nm's, f29737a1 another's, of the same sources; 39393209: main of 5 November 2026 (c6e3eb98) built whole in ~/nm after the pull: libgles.a's frame named for the volume display by the worker that ends it (gles.c's seglSwapHand, 9d502f89; its home said abddfba5 from the same sources), (still with ~/nm's paths); 25995171: main of 5 November 2026 (86585a04) built whole in ~/nm after the pull, libc's 8 KB memset and the GL changes (gles.c, gl.h, segl.h) together: f29737a1 came from a build home that is not on this computer, from the same library sources, latest; f29737a1: the merge of vram-gpu (its libgles.a's streams) with main (5 November 2026), latest; b9c7485e: libc's memset asks for the commit's fill from 8 KB, not 2 (5 November 2026); e2b6f8e1: libmcw.a's mcw_wait (bbea1ab8 alone) and libgles.a's destination-alpha passes (db2a890b alone) together (3 November 2026); mcw_wait serving only workers with something out (libmcw.a, 3 November 2026), latest; 93ad561a: shader.c and fractal.c follow a resized window (31 October 2026; what the examples take moved in the archives: pack.py), latest; 8f0167e6: mallocng keeping freed groups (malloc_keep.py, 31 October 2026, latest); 488c2419: elf2flt's relocations in the machine's order; 18ecf477: the build folder's name out of the libraries (-ffile-prefix-map in setup.sh's drivers, 31 October 2026): client.c's __FILE__ in libnano-X.a's strings went with the folder, /home/micha/bpcc 0650c8b3 and /home/micha/nm 1480ba8c from the same sources; before that libmcw.a with mcwx's lowest free worker (6af8737) too; before it f378920f: libmcw.a's worker entry writing the program's tag into its answer, with the toolchain below (29 October 2026, latest of all); libcompiler_rt.a's 64-bit division and double's sum, product and conversions of our own (rt/) and every global hidden in rv32-cc (nommu_hidden.h) (29 October 2026, latest); memset's range in the layout of RAM in one piece (2ecf76d) and mcwx's team job that lingered out posted with the round (28 October 2026, latest); memcpy's commit's copy from 4 KB and mcwx's calls round the row on RISC-V (28 October 2026, later); the devices at 0x80000000 (libmcw.a's and libgles.a's device addresses, uop.h's RAM test in memcpy and memset); libc.a's float libm (linux/userland/libm), memcpy and memset through the commit's copy, memmove and memcmp a word a turn, a double's printf in doubles; libmcw.a's mc_publish (28 October 2026)  # with libmcw.a's tagged answers (1ee0347) too it moves again: set anew from a build with both; libgles.a's streams in rows 3072 to 3328, the device's after them (gpu.md "Streams"; 5 November 2026), latest; 98dccb97: libgles.a's streams (a texture a draw names in GPU memory uploaded into the VRAM's last rows by every list that draws it, gpu.md "Streams"; 5 November 2026), latest; 6335e593: libgles.a's copies (glCopyTexSubImage2D, framebuffer objects, SEGL_DEBUG; 5 November 2026), latest; 8c39ad91: libgles.a's VRAM (5 November 2026: textures uploaded into the VRAM, seglVramLevels, the VRAM by slot for other programs' textures, gpu.md "VRAM"), latest; e2b6f8e1: libmcw.a's mcw_wait (bbea1ab8 alone) and libgles.a's destination-alpha passes (db2a890b alone) together (3 November 2026); mcw_wait serving only workers with something out (libmcw.a, 3 November 2026), latest; 93ad561a: shader.c and fractal.c follow a resized window (31 October 2026; what the examples take moved in the archives: pack.py), latest; 8f0167e6: mallocng keeping freed groups (malloc_keep.py, 31 October 2026, latest); 488c2419: elf2flt's relocations in the machine's order; 18ecf477: the build folder's name out of the libraries (-ffile-prefix-map in setup.sh's drivers, 31 October 2026): client.c's __FILE__ in libnano-X.a's strings went with the folder, /home/micha/bpcc 0650c8b3 and /home/micha/nm 1480ba8c from the same sources; before that libmcw.a with mcwx's lowest free worker (6af8737) too; before it f378920f: libmcw.a's worker entry writing the program's tag into its answer, with the toolchain below (29 October 2026, latest of all); libcompiler_rt.a's 64-bit division and double's sum, product and conversions of our own (rt/) and every global hidden in rv32-cc (nommu_hidden.h) (29 October 2026, latest); memset's range in the layout of RAM in one piece (2ecf76d) and mcwx's team job that lingered out posted with the round (28 October 2026, latest); memcpy's commit's copy from 4 KB and mcwx's calls round the row on RISC-V (28 October 2026, later); the devices at 0x80000000 (libmcw.a's and libgles.a's device addresses, uop.h's RAM test in memcpy and memset); libc.a's float libm (linux/userland/libm), memcpy and memset through the commit's copy, memmove and memcmp a word a turn, a double's printf in doubles; libmcw.a's mc_publish (28 October 2026)  # with libmcw.a's tagged answers (1ee0347) too it moves again: set anew from a build with both


def without_mmu(image):
    """An image of linux/nommu/image.py: its RAM starts at the kernel's 0x82000000."""
    dts = os.path.join(ROOT, image, 'dts.bin')
    return os.path.exists(dts) and struct.pack('>II', 0, 0x82000000) in open(dts, 'rb').read()


def run(c, a, image, logs):
    log = os.path.join(logs, c['name'] + '.log')
    # --sections: the case in calls of --section-secs each, rvc_cpu's snapshot (--save, --load: the console's place
    # and the script's with it) and what the calls before said kept between them
    snap, state = os.path.join(logs, c['name'] + '.snap'), os.path.join(logs, c['name'] + '.state')
    before = dict(seconds=0.0, part=0, err='')
    left = a.deadline - time.time() - 5   # (--sections: the call's time left, its snapshot's writing apart)
    if a.sections and left < 3:
        return 'later', 0, False, 0.0
    if a.sections and os.path.exists(state) and (os.path.exists(snap) or json.load(open(state)).get('ended')):
        before = json.load(open(state))
        if before.get('ended'):   # (ended in an earlier call, its check waiting for a case it is compared with)
            os.remove(state)
            return open(log, encoding='latin-1').read(), before['err'], a.races, before['seconds']
    else:
        for f in (log, state, snap):
            if os.path.exists(f):
                os.remove(f)
    # the case's wall (--wall-scale for a slower build or host), at most --max-wall, less what its sections took
    wall = c['wall'] * a.wall_scale
    if a.max_wall:
        wall = min(wall, a.max_wall)
    wall = max(1.0, wall - before['seconds'])
    args = [a.cpu, '--quiet', '--uart-log', log, '--wall', f'{wall:.0f}', '--image', image]
    if a.sections:
        args += ['--save', snap, '--save-after', f'{min(a.section_secs, left):.0f}'] + (['--load', snap] if before['part'] else [])
    if c['cmd'] is not None:
        args += ['--expect', '/ # ', '--send', c['cmd'] + '; ' + DONE + '\\n']
    args += ['--until', c['until']] + c['more']
    if c.get('volume'):   # (a seated case: the frames given a holodeck, judged by tools/volume_frames.py after the run)
        if os.path.exists(log + '.volume'):
            os.remove(log + '.volume')
        args += ['--volume-log', log + '.volume']
    env = dict(os.environ)
    env.pop('RVC_RACES', None)
    env.setdefault('RVC_THREADS', str(max(1, (os.cpu_count() or 4) // a.jobs)))   # the workers' host threads, all cases together
    if a.card_model:
        env.update(RVC_SOUND='1', RVC_WCACHE='1', RVC_ADFAULT='1')
    races = a.races   # (every case: nxpaint, nxweb, the IDE and others take workers though their cases do not say so)
    if races:
        env['RVC_RACES'] = log + '.races'
        # a case's part looked at (full speed to race_at's text, the check from there, the run's end at race_until's)
        for k, e in (('race_at', 'RVC_RACES_AT'), ('race_until', 'RVC_RACES_UNTIL')):
            if c.get(k):
                env[e] = c[k]
        # a texel another core changed other words of within 4 passes: on the card, whose passes end elsewhere, the
        # two may meet in one pass (Half-Life's statics, 632e982). Reported, not failed: a hand-over by a job word is one
        env.setdefault('RVC_RACES_NEAR', '4')
    env.update(c['env'])
    t = time.time()
    if c.get('checkpoint') and not a.sections:
        # a long case goes on from a checkpoint (rvc_cpu --save-at TEXT, then --load: docs/machine/cpu-harness.md), made once an
        # image (build/checkpoints, by the image's files' sizes and times) and its arguments: the first run is long, the rest minutes
        st = [os.stat(os.path.join(image, f)) for f in ('rootfs.bin', 'linux_payload.bin') if os.path.exists(os.path.join(image, f))]
        key = hashlib.sha1(repr([(s.st_size, int(s.st_mtime)) for s in st] + [c['cmd'], c['more'], c['checkpoint']]).encode()).hexdigest()[:10]
        ck = os.path.join(ROOT, 'build', 'checkpoints', '%s-%s.snap' % (c['name'], key))
        if not os.path.exists(ck):
            os.makedirs(os.path.dirname(ck), exist_ok=True)
            p = subprocess.run(args + ['--save', ck + '.part', '--save-at', c['checkpoint']], cwd=ROOT, env=env, capture_output=True,
                               text=True, errors='replace')
            if p.returncode == 5 and os.path.exists(ck + '.part'):
                os.replace(ck + '.part', ck)
            else:
                return (open(log, encoding='latin-1').read() if os.path.exists(log) else ''), p.stderr, races, time.time() - t
        args += ['--load', ck]
    for _ in range(60):   # the harness lock refuses a CPU harness beside a GPU one: wait for it
        p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            break
        time.sleep(20)
    secs = before['seconds'] + time.time() - t
    err = before['err'] + p.stderr
    if a.sections and p.returncode == 5 and os.path.exists(snap):   # (stopped at --save-after and saved: to go on)
        json.dump(dict(seconds=secs, part=before['part'] + 1, err=err), open(state, 'w'))
        return 'part', before['part'] + 1, races, secs
    for f in (state, snap):
        if os.path.exists(f):
            os.remove(f)
    text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
    if c.get('volume') and os.path.exists(log + '.volume'):
        # its runs (flat, world + HUD, an empty world) and its verdict, as lines of the console the checks read
        v = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'volume_frames.py'), log + '.volume'], capture_output=True,
                           text=True, errors='replace')
        text += '\nvolume_frames: ' + v.stdout.replace('\n', '\nvolume_frames: ') + '\nvolume_frames: exit %d\n' % v.returncode
    return text, err, races, secs


def futures_names(futures):
    return {c['name'] for c in futures.values()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('select', nargs='*', help='case names or tags (default: every case not tagged long)')
    ap.add_argument('--long', action='store_true', help='the long cases too')
    ap.add_argument('--races', action='store_true', help='RVC_RACES on every case: 0 changes undone')
    ap.add_argument('--card-model', action='store_true', help="rvc_cpu as the card: RVC_SOUND, RVC_WCACHE, RVC_ADFAULT")
    made = os.path.exists(os.path.join(ROOT, 'build', 'images', 'nommu', 'linux_payload.bin'))
    ap.add_argument('--image', default='build/images/nommu' if made else 'build/images/linux',
                    help='the image folder (default build/images/nommu once made, else build/images/linux)')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) // 2), help='cases at once')
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'test_all'), help='where the consoles go')
    ap.add_argument('--list', action='store_true', help='list the cases and stop')
    ap.add_argument('--cpu', default=os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), help='the rvc_cpu to run (default bin/rvc_cpu.exe)')
    ap.add_argument('--wall-scale', type=float, default=1.0, help="every case's wall times this (a slower build or host)")
    ap.add_argument('--max-wall', type=float, default=0, help="a case's wall at most (with --sections: its whole run's)")
    ap.add_argument('--sections', action='store_true',
                    help='a case in calls of --section-secs: run again to go on from its snapshot (each call under a minute)')
    ap.add_argument('--section-secs', type=float, default=40, help="a section's seconds (default 40)")
    ap.add_argument('--call-secs', type=float, default=52, help="with --sections, a call's seconds: no case starts or goes on past them")
    ap.add_argument('--doubles', action='store_true', help='each game under its limit of soft doubles, one at a time; run again to go on')
    a = ap.parse_args()
    if a.doubles:
        return doubles_round(a)
    done_before = {}
    if a.select:
        a.run = [c for c in CASES if c['name'] in a.select or c['tags'] & set(a.select)]
    else:
        a.run = [c for c in CASES if a.long or 'long' not in c['tags']]
    if a.list or not a.run:
        for c in a.run or CASES:
            print(f"{c['name']:28} {' '.join(sorted(c['tags']))}")
        return 0 if a.run else 1
    os.makedirs(a.logs, exist_ok=True)
    # --sections: the cases checked in the earlier calls of this round (sections.json), not run again, and their
    # consoles for a case compared with one; the round ends with the call that leaves no case to go on with
    progress_file = os.path.join(a.logs, 'sections.json')
    progress = {}
    if a.sections:
        try:
            progress = json.load(open(progress_file))
        except (OSError, ValueError):
            progress = {}
        for name in progress:
            f = os.path.join(a.logs, name + '.log')
            done_before[name] = open(f, encoding='latin-1').read() if os.path.exists(f) else ''
        for c in a.run:   # (and one not in this selection, ended before)
            for other in [rule[1] for rule in c.get('same', [])]:
                f = os.path.join(a.logs, other + '.log')
                if other not in [x['name'] for x in a.run] and os.path.exists(f):
                    done_before[other] = open(f, encoding='latin-1').read()
    if without_mmu(a.image):   # cc's programs there are flat ones (tcc -shared, elf2flt): other bytes
        for c in CASES:
            if c['name'] == 'cc-bytes':
                c['digest'] = [(c['digest'][0][0], CC_BYTES_NOMMU)]
            if c['name'] == 'zero-tail':
                c['cmd'] = ZERO_TAIL_NOMMU
    where = images(a)
    done, failed, t0 = dict(done_before), [], time.time()
    a.deadline = t0 + a.call_secs
    # the longest first, by the seconds each took the last time (the last case to start then ends soonest);
    # a case compared with another is checked once that one has ended
    times_file = os.path.join(a.logs, 'times.json')
    try:
        times = json.load(open(times_file))
    except (OSError, ValueError):
        times = {}
    order = sorted([c for c in a.run if c['name'] not in progress], key=lambda c: -times.get(c['name'], 60))
    waiting, parts = [], []

    def report(c, text, err, races, secs):
        why = check(c, text, err, races, done)
        near = re.findall(r'\[races\] (\d+) changes to a texel another core changed other words of within \d+ passes \(RVC_RACES_NEAR\)', err)
        near = sum(map(int, near))
        near = f'  (near: {near}, {c["name"]}.log.races)' if near else ''
        print(f"{'PASS' if not why else 'FAIL'} {c['name']:28} {secs:5.0f} s" + ('' if not why else '  ' + '; '.join(why)) + near, flush=True)
        if why:
            failed.append(c['name'])
        progress[c['name']] = why

    with ThreadPoolExecutor(a.jobs) as ex:
        futures = {ex.submit(run, c, a, where[c['image']], a.logs): c for c in order}
        for f in as_completed(futures):
            c = futures[f]
            text, err, races, secs = f.result()
            if text in ('part', 'later'):   # (--sections: saved, not ended; or not begun in this call)
                parts.append(c['name'])
                print(f"part {c['name']:28} {secs:5.0f} s: section {err} saved, run again to go on" if text == 'part' else
                      f"later {c['name']:27} (this call's time is up)", flush=True)
                continue
            done[c['name']] = text
            times[c['name']] = round(secs, 1)
            waiting.append((c, text, err, races, secs))
            for w in [w for w in waiting if all(s[1] in done or s[1] not in futures_names(futures) for s in w[0].get('same', []))]:
                waiting.remove(w)
                report(*w)
    for w in waiting:   # (compared with a case that was not run, or, with --sections, has not ended yet: checked then)
        if a.sections and any(s[1] in parts for s in w[0].get('same', [])):
            json.dump(dict(ended=True, err=w[2], seconds=w[4]), open(os.path.join(a.logs, w[0]['name'] + '.state'), 'w'))
            parts.append(w[0]['name'])
            print(f"later {w[0]['name']:27} (ended; checked once what it is compared with has)", flush=True)
            continue
        report(*w)
    try:
        json.dump(times, open(times_file, 'w'), indent=0, sort_keys=True)
    except OSError:
        pass
    if a.sections:   # (the whole round's: the earlier calls' checks too)
        failed = [name for name, why in progress.items() if why]
        if parts:
            json.dump(progress, open(progress_file, 'w'), indent=0)
        elif os.path.exists(progress_file):
            os.remove(progress_file)
    print(f'{len(a.run) - len(failed) - len(parts)} of {len(a.run)} passed in {time.time() - t0:.0f} s' + (f'; failed: {" ".join(failed)}' if failed else '') +
          (f'; {len(parts)} to go on (run again): {" ".join(parts)}' if parts else '') + f' (consoles in {a.logs})')
    return 1 if failed else 0


# --doubles: each game's share of its instructions in soft doubles (docs/machine/profiling.md, "Soft doubles"), over its
# limit a FAIL. game_par.py's workloads, profiled, one at a time on one thread; each limit a little over what it measured.
DOUBLES = {'doom': ('doom', 1.0), 'quake-timedemo': ('quake', 1.0), 'halflife': ('halflife', 6.0), 'nes': ('nes', 1.0),
           'classicube': ('classicube', 3.0), 'quake3': ('quake3', 3.0), 'nxphys': ('nxphys', 1.0)}


def doubles_round(a):
    """Stops before --call-secs and goes on from there when run again (doubles.json); exit 1 when any is over."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import game_par
    os.makedirs(a.logs, exist_ok=True)
    state_file, t0 = os.path.join(a.logs, 'doubles.json'), time.time()
    try:
        state = json.load(open(state_file))
    except (OSError, ValueError):
        state = {}
    prof = os.path.join(ROOT, 'tools', 'prof.py')
    for name, (elf, limit) in DOUBLES.items():
        if name in state:
            continue
        left = a.call_secs - (time.time() - t0)
        if left < 20:
            break
        g = dict(game_par.GAMES[name], **game_par.GAMES[name].get('cpu', {}))
        out = os.path.join(a.logs, 'doubles_' + name)
        send = g['cmd'] + '; ' + game_par.DONE + '\\n'
        args = ['--image', a.image, '--quiet', '--expect', '/ # ', '--send', send, '--count-from', g['mark'], '--until', g.get('until', 'LX-DONE'),
                '--wall', str(int(left - 12))] + list(g.get('more', []))
        env = dict(os.environ, **g.get('env', {}))
        begun = time.time()
        r = subprocess.run([sys.executable, prof, 'run', out, '--'] + args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace')
        if re.search(r'exit [1-9]', r.stdout + r.stderr):   # (the wall came first: too little of it to judge; the next call has the whole of its time)
            if not state and left >= a.call_secs - 1:
                state[name] = dict(ok=False, say='never reached its end in a whole call')
                print(f'FAIL doubles {name:21} {time.time() - begun:5.0f} s  never reached its end in a whole call', flush=True)
                continue
            break
        r = subprocess.run([sys.executable, prof, 'doubles', out, '--find', elf, '--limit', str(limit)], cwd=ROOT, capture_output=True, text=True, errors='replace')
        said = re.search(r'SOFT DOUBLES: .*', r.stdout)
        ok = r.returncode == 0
        top = re.findall(r'^\s+[\d.]+%\s+(\S+)$', r.stdout, re.M)[:2]
        say = said[0] if said else ' '.join(r.stdout.strip().splitlines()[-1:])[:100]
        state[name] = dict(ok=ok, say=say)
        print(f"{'PASS' if ok else 'FAIL'} doubles {name:21} {time.time() - begun:5.0f} s  {say}" + ('' if ok else '  callers: ' + ' '.join(top)), flush=True)
        json.dump(state, open(state_file, 'w'), indent=0)
    json.dump(state, open(state_file, 'w'), indent=0)
    to_go = [n for n in DOUBLES if n not in state]
    bad = [n for n, s in state.items() if not s['ok']]
    print(f'doubles: {len(state) - len(bad)} of {len(DOUBLES)} under their limits' + (f'; over: {" ".join(bad)}' if bad else '') +
          (f'; {len(to_go)} to go (run again): {" ".join(to_go)}' if to_go else ''))
    if not to_go:
        os.remove(state_file)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
