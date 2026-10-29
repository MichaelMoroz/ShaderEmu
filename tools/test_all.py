"""Every program's own checks on the Linux image, under bin/rvc_cpu.exe, several at a time
(docs/machine/cpu-harness.md, "The test set"). One PASS or FAIL line a case, then a summary; exit 1 on
any failure.

    python tools/test_all.py                    # the default set (some 5 minutes at most)
    python tools/test_all.py games doom-1 ...   # cases by name or tag (machine, games, desktop, media, long, ...)
    python tools/test_all.py --long             # the default set and the long cases
    python tools/test_all.py --races            # RVC_RACES on every case: 0 undone (and RVC_RACES_NEAR=4's count said)
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
F10 = '\x1b[21~'   # (the console's TERM is linux)
CC = 'mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; ' + X
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
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


def case(name, tags, cmd, workers=False, until='LX-DONE', more=(), wall=240, image=None, env=None, **checks):
    return dict(name=name, tags=set(tags.split()), cmd=cmd, workers=workers, until=until, more=list(more), wall=wall,
                image=image, env=env or {}, **checks)


CASES = [
    # the machine (docs/machine/multicore.md; programs/mc/mctest.c)
    case('mctest-3', 'machine', 'mctest 3', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
    case('mctest-255', 'machine', 'mctest 255 30000', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
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
    case('libmedia-installed', 'machine media', 'ls -1 /usr/lib/libmedia.a /usr/include/media.h /usr/bin/media_test | cat',   # docs/system/media.md
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
         True, until='classicube: steady', count=[(r'^cctex: ', 42)], digest=[(r'^(cctex: .*)', 'e51865ae')]),   # (RAM in one texture: 42 by then)
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
          '--host-from', '24', '90000133,0,0,c0000000,84bea3da,0,44bea026,2600f8e4'], 'holds 58', r'seat: 1 .* holds 58 raw 1 menu 0'))],
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
    case('nxphys-kill', 'games nxphys', X + 'nxphys & sleep 5; killall -9 nxphys; sleep 1; mctest 15 2000', True, has=[r'mctest: PASS']),
    case('nxphys-none', 'games nxphys long', X + 'PHYS_WORKERS=0 PHYS_SCENE=1 PHYS_STEPS=120 nxphys', has=[r'sum b28b77b4']),
    # Micropolis (docs/games/micropolis.md): a city and two scenarios with a fixed seed, the sums of the build computer's run
    case('micropolis', 'games', 'MICROPOLIS_SUM=/usr/share/micropolis/cities/haight.cty,1000 nxmicropolis; '
         'MICROPOLIS_SUM=1,500 nxmicropolis; MICROPOLIS_SUM=8,1500 nxmicropolis',
         sums=[(r'micropolis: .* ticks: .* sum ([0-9a-f]+)', ['909517ac', 'e14d1a50', '75e86f73'])]),
    # OpenTTD (docs/games/openttd.md): two new games of test.cfg without a window, the sums of the native build's run
    case('openttd', 'games', 'cp /usr/share/openttd/test.cfg /tmp/t.cfg; OPENTTD_SUM=1 openttd.bin -v null:ticks=500 -s null -m null -g -G 1 -c /tmp/t.cfg; '
         'OPENTTD_SUM=1 openttd.bin -v null:ticks=2000 -s null -m null -g -G 2 -c /tmp/t.cfg',
         sums=[(r'openttd: \d+ ticks, .* sum ([0-9a-f]+)', ['9d6fc0a0', '66880c87'])]),
    # the viewport on worker cores (docs/games/openttd.md, "Checking a picture"): the play script paused, every picture
    # against one drawn whole, not a pixel apart; with the way back an odd move, and on the first core alone
    case('openttd-scroll', 'games openttd', X + 'OPENTTD_PLAY=1 OPENTTD_PLAY_ODD=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play round 1: .*checked (19\d|200), 0 differ \(0 pixels']),
    case('openttd-scroll-serial', 'games openttd', X + 'OPENTTD_PLAY=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 OPENTTD_WORKERS=0 openttd -g /usr/share/openttd/busy.sav',
         True, wall=400, has=[r'openttd: play round 1: .*checked (19\d|200), 0 differ \(0 pixels']),
    # started twice in one boot (docs/games/openttd.md, "At exit"): the first played with its ships' paths made ahead, the
    # second must find its sprites as they are (a worker left running at exit once wrote into the next program's pages)
    case('openttd-twice', 'games openttd', X + 'OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 openttd -g /usr/share/openttd/busy.sav; '
         'OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1 openttd -g /usr/share/openttd/busy.sav; '
         'OPENTTD_SUM=1 openttd -v null:ticks=1 -s null -m null -g /usr/share/openttd/busy.sav',
         True, wall=600, has=[r'openttd: play PL-DONE', r'openttd: play scroll: .* [1-9]\d* made again by workers',
                              r'openttd: play round 1: .*checked \d+, 0 differ \(0 pixels'],
         sums=[(r'openttd: 1 ticks, map 256 x 256, .* sum ([0-9a-f]+)', ['e49e18e3'])]),
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
    # libmedia and the players (docs/system/media.md, docs/apps/play.md, docs/apps/video.md)
    case('media-sound', 'media', '; '.join(f'media_test audio /usr/share/play-test.{e} /tmp/o.raw {w}' for e in ('flac', 'mp3', 'mod') for w in (0, 63)),
         True, wall=400, sums=[(r'audio /usr/share/play-test\.\w+: .* sum ([0-9a-f]+)', ['518c5ac8'] * 2 + ['aa07a2e5'] * 2 + ['c714a845'] * 2)]),
    case('media-mpeg', 'media', 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8', True, wall=400,
         has=[r"pictures' sum d124dc14", r"sound's sum 96a296a2"]),
    case('media-avi', 'media', 'media_test video "/root/Videos/Big Buck Bunny.avi" 16', True, wall=400,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
    case('media-avi-none', 'media long', 'media_test video "/root/Videos/Big Buck Bunny.avi" 0', wall=600,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
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
    # the IDE (docs/apps/ide.md, "Testing"): C built, run and debugged, JavaScript run, Python debugged,
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
    case('launch-note', 'desktop', 'export UI_LAUNCH=print; nx; sleep 4; nxkey 266,94 266,94; sleep 4', wall=120,
         has=[r'^launch: Calculator', r'^launch: a window shown after \d+ ms']),   # docs/system/nanox.md, "A program starting"
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
                   env=dict(os.environ, HALFLIFE_DATA='0'))
    return f'build/images/{name}'


# without the MMU a ROM file is mapped where it lies in the ROM, the next file's bytes after its end
# (16-byte romfs packing): what the pool hands out is checked on copies in tmpfs
ZERO_TAIL_NOMMU = ('zerotest 12 4; mkdir -p /tmp/t; find /usr/share /usr/bin /etc -maxdepth 1 -type f -size -256k | '
                   'while read f; do cp "$f" /tmp/t/; done; tailtest /tmp/t/*')
CC_BYTES_NOMMU = '4e0a84e8'   # libcompiler_rt.a's 64-bit division and double's sum, product and conversions of our own (rt/) and every global hidden in rv32-cc (nommu_hidden.h) (29 October 2026, latest); memset's range in the layout of RAM in one piece (2ecf76d) and mcwx's team job that lingered out posted with the round (28 October 2026, latest); memcpy's commit's copy from 4 KB and mcwx's calls round the row on RISC-V (28 October 2026, later); the devices at 0x80000000 (libmcw.a's and libgles.a's device addresses, uop.h's RAM test in memcpy and memset); libc.a's float libm (linux/userland/libm), memcpy and memset through the commit's copy, memmove and memcmp a word a turn, a double's printf in doubles; libmcw.a's mc_publish (28 October 2026)  # with libmcw.a's tagged answers (1ee0347) too it moves again: set anew from a build with both


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
    a = ap.parse_args()
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


if __name__ == '__main__':
    sys.exit(main())
