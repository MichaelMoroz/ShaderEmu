"""A part's profile (prof_parts.py) summed for docs/plans/riscv-profile.md: core 0's instructions and the write cache's
entries by kind of code (the program, libc's copies and strings, the rest of libc, libm and the soft floats, the
kernel, the window server, the window system's client library), the top functions, the passes ended full and where,
the plan's slow forms (GOT loads, soft doubles, byte copies), and the guest's seconds of the window.

    python tools/rv_compare/prof_summary.py PART [PART ...] [--dir build/prof_parts] [--json OUT]

The ELFs are the build of the before image's sources (ELFS, a folder of unstripped programs) and the kernel's vmlinux
(KERNEL); prof.py places them by their words."""
import argparse, collections, contextlib, io, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SE, 'tools'))
import numpy as np  # noqa: E402
import prof  # noqa: E402

WSLH = os.environ.get('PROF_BUILD', prof.WSL + r'\home\micha\ph')   # HOME of the before image's sources built privately
ELFS = os.environ.get('PROF_ELFS', WSLH + r'\elfs')       # unstripped ELFs of the before image's programs
KERNEL = os.environ.get('PROF_KERNEL', WSLH + r'\k\out\vmlinux')
NAMES = os.path.join(ELFS, 'names.json')               # musl's libm and libc objects' symbols, compiler-rt's (make_names)

# a part's programs (ELF names in ELFS): the window server, the bar and busybox are always offered
PROGRAMS = {
    'doom': ['doom'], 'doom-attract': ['doom'], 'quake': ['quake'], 'quake-server': ['quake'], 'quake-timedemo': ['quake'],
    'quake3': ['quake3'], 'quake3-server': ['quake3'], 'quake3-menu': ['quake3'], 'halflife': ['halflife'],
    'classicube': ['classicube'], 'tdawn-menu': ['tdawn'], 'tdawn': ['tdawn'], 'ralert': ['ralert'], 'nes': ['nes'],
    'nes-nova': ['nes'], 'nxphys': ['nxphys'], 'pinball': ['nxpinball'], 'micropolis': ['nxmicropolis'],
    'openttd': ['openttd'], 'openttd-title': ['openttd'], 'video-mpg': ['nxvideo'], 'video-avi': ['nxvideo'],
    'decode-avi': ['media_test'], 'nxray': ['nxray'], 'nxpath': ['nxpath'], 'nxweb': ['nxweb'], 'nxedit': ['nxedit'],
    'nxide': ['nxide', 'qjs'], 'desktop': ['nxcalc', 'nxpersist', 'nxkey'], 'nxview': ['nxview'], 'nxpaint': ['nxpaint'],
    'nxfiles': ['nxfiles'], 'memory': ['tcc'], 'memory-kernel': [], 'cc': ['tcc'], 'qjs': ['qjs'], 'python': ['micropython'],
    'gzip': [], 'htop': ['htop'], 'tetris': ['nxtetris'], 'mines': ['nxmine'], 'terminal': ['nxterm'], 'clock': ['nxclock'],
    'monitor': ['nxmon'], 'network': ['nxnet'], 'settings': ['nxsettings'], 'eyes': ['nxeyes'], 'glxgears': ['glxgears'],
    'imgui': ['imdemo'], 'music': ['nxplay'], 'example-cube': ['nxide', 'tcc'],
}
ALWAYS = ['nano-X', 'nxbar', 'busybox']
DESKTOP = ['nxkey', 'nxpersist']

COPY = re.compile(r'^(memcpy|memmove|memset|memcmp|bcmp|bzero|str\w+|stp\w+|__stpcpy|__strchrnul|wmem\w+|__memcpy\w*)$')
SOFT = re.compile(r'^__(add|sub|mul|div|neg)[sdt]f3$|^__(fix|fixuns|float|floatun)\w*[sdt]i$|^__(extend|trunc)\w+f2$|'
                  r'^__(eq|ne|lt|le|gt|ge|unord|cmp)[sdt]f2$|^__(div|mod|udiv|umod|mul)di3$|^__udivmoddi4$|^__(ashl|ashr|lshr)di3$')


def make_names(sysroot_lib):
    """{'libm': [...], 'libc': [...], 'rt': [...]}: musl's math objects' symbols, its other objects', compiler-rt's"""
    import subprocess
    out = {'libm': set(), 'libc': set(), 'rt': set()}
    r = subprocess.run(['wsl', '--', 'bash', '-c', f'{prof.TOOLBIN}/riscv32-linux-nm -A --defined-only {sysroot_lib}/libc.a '
                        f'{sysroot_lib}/libcompiler_rt.a'], capture_output=True, text=True)
    math = set(os.listdir(WSLH + r'\nm\work\src\musl\src\math'))
    math = {os.path.splitext(m)[0] for m in math}
    for line in r.stdout.splitlines():
        m = re.match(r'(.*?):(.*?\.(?:o|lo)):\s*(?:[0-9a-f]+\s+)?([TtWw])\s+(\S+)$', line)
        if not m:
            continue
        lib, obj, sym = m[1], os.path.splitext(m[2])[0], m[4]
        if 'compiler_rt' in lib:
            out['rt'].add(sym)
        elif obj in math:
            out['libm'].add(sym)
        else:
            out['libc'].add(sym)
    return {k: sorted(v) for k, v in out.items()}


def kind_of(img, fn, names):
    if img is None:
        return 'unknown'
    if img == 'vmlinux':
        return 'kernel'
    if img == 'nano-X':
        return 'window server'
    if SOFT.match(fn) or fn in names['rt']:
        return 'soft float, 64-bit'
    if fn in names['libm']:
        return 'libm'
    if COPY.match(fn):
        return 'libc copy/fill/str'
    if fn in names['libc']:
        return 'libc other'
    if re.match(r'^(Gr\w+|nx\w+|_nx\w+|ReadBlock|TypedReadBlock|GrFlush)$', fn):
        return 'window client'
    return 'program'


def got_loads(P, img):
    """core 0's executions of `auipc r; lw r, lo(r)` whose address is in the ELF's .got: an extern global reached by a load
    more under -fPIE (the plan's 2b)"""
    data = open(img.path, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    names = secs[shstrndx]
    got = None
    for s in secs:
        end = data.index(b'\0', names[4] + s[0])
        if data[names[4] + s[0]:end] == b'.got':
            got = (s[3], s[3] + s[5])
    if not got:
        return 0, 0
    sel = (P.cls == 0)
    pcs = {int(p): (int(w), int(e)) for p, w, e in zip(P.pc[sel], P.word[sel], P.exec[sel])}
    n = sites = 0
    for pc, (w, ex) in pcs.items():
        if w & 0x707f != 0x2003:   # lw
            continue
        rd, rs1, imm = (w >> 7) & 31, (w >> 15) & 31, prof.sx(w >> 20, 12)
        prev = img.word(pc - img.base - 4)
        if prev is None or prev & 0x7f != 0x17 or (prev >> 7) & 31 != rs1:
            continue
        target = (pc - 4 - img.base) + prof.sx(prev >> 12, 20) * 4096 + imm
        if got[0] <= (target & 0xffffffff) < got[1]:
            n += ex
            sites += 1
    return n, sites


def summarize(part, d, names, top=8):
    out = os.path.join(d, f'{part}.prof')
    err = open(os.path.join(out, 'stderr.txt'), errors='replace').read()
    m = re.search(r'COUNT from "[^"]*": core 0 (\d+), all cores (\d+), (\d+) cores ran, the busiest worker (\d+), (\d+) passes, ([\d.]+) s', err)
    if not m:
        return {'part': part, 'error': 'no COUNT'}
    c0, allc, ran, busiest, passes, gsec = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5]), float(m[6])
    P = prof.Profile(out)
    elfs = []
    for n in PROGRAMS.get(part, []) + ALWAYS + (DESKTOP if part in ('desktop',) else []):
        p = os.path.join(ELFS, n)
        if os.path.exists(p):   # (NAME.nm beside a program whose ELF keeps few symbols: micropython's, from its map)
            elfs.append(prof.Elf(p, p + '.nm' if os.path.exists(p + '.nm') else None, name=n))
    if os.path.exists(KERNEL):
        elfs.append(prof.Elf(KERNEL, name='vmlinux'))
    with contextlib.redirect_stdout(io.StringIO()) as placed:
        P.place(elfs)
    res = {'part': part, 'guest_s': gsec, 'core0': c0, 'all': allc, 'cores_ran': ran, 'busiest': busiest, 'passes': passes,
           'placed': placed.getvalue().strip().splitlines()}
    for k, who in ((0, 'core0'), (1, 'workers')):
        sel = P.cls == k
        by, nt, fu, kinds, kt = (collections.Counter() for _ in range(5))
        for pc, ex, n_, f_ in zip(P.pc[sel], P.exec[sel], P.newtex[sel], P.full[sel]):
            img = P.image_of(int(pc))
            key, _ = P.func_key(int(pc))
            fn = key.split(':', 1)[1] if img else key
            kind = kind_of(img.name.split('#')[0] if img else None, fn, names)
            by[key] += int(ex)
            nt[key] += int(n_)
            fu[key] += int(f_)
            kinds[kind] += int(ex)
            kt[kind] += int(n_)
        tot, ttot = sum(by.values()), sum(nt.values())
        res[who] = {'instr': tot, 'newtex': ttot, 'full': sum(fu.values()),
                    'kinds': {k_: round(100 * v / tot, 1) for k_, v in kinds.most_common()} if tot else {},
                    'kinds_tex': {k_: round(100 * v / ttot, 1) for k_, v in kt.most_common()} if ttot else {},
                    'top': [(f, round(100 * v / tot, 1)) for f, v in by.most_common(top)] if tot else [],
                    'top_tex': [(f, round(100 * v / ttot, 1)) for f, v in nt.most_common(top)] if ttot else [],
                    'top_full': [(f, v) for f, v in fu.most_common(5) if v]}
    tl = os.path.join(out, 'timeline.bin')
    t = np.fromfile(tl, dtype='<u4').reshape(-1, 8).astype(np.int64) if os.path.exists(tl) else np.zeros((0, 8), np.int64)
    res['profiled_passes'] = len(t)
    res['core0_distinct_mean'] = round(float(t[:, 6].mean()), 1) if len(t) else 0
    busy = t[:, 6] > 0
    res['core0_distinct_busy_mean'] = round(float(t[busy, 6].mean()), 1) if busy.any() else 0
    res['core0_instr_pass_mean'] = round(float(t[:, 0].mean()), 0) if len(t) else 0
    # soft floats called from the program (the plan's libm item): their share of core 0, and the GOT loads
    res['got'] = {}
    for img in P.images:
        if img.name in ('vmlinux',):
            continue
        n, sites = got_loads(P, img)
        if n:
            res['got'][img.name] = {'loads': n, 'sites': sites, 'pct_core0': round(100 * n / max(c0, 1), 2)}
    # the patterns prof.py finds (byte copies and the rest), core 0's loops
    with contextlib.redirect_stdout(io.StringIO()) as pat:
        ls = prof.loops(P, res['core0']['instr'])
        prof.patterns_report(P, ls, res['core0']['instr'], 10)
    res['patterns'] = [l for l in pat.getvalue().splitlines() if l.strip()][:14]
    res['jobs'] = jobs(P, os.path.join(d, f'{part}.calls', 'calls.txt'))
    return res


def jobs(P, path, least=20000):
    """RVC_CALLS of the same command (prof_parts.py --mode calls: the same programs at the same places): core 0's calls
    of a program's functions, named by the profile's placing, by instructions a call (a job is worth a worker from some
    tens of thousands: docs/machine/multicore.md, "Finding the work")"""
    if not os.path.exists(path):
        return []
    total, rows = 0, []
    for line in open(path):
        w = line.split()
        if w[:1] == ['ctx']:
            total += int(w[2])
        elif w[:1] == ['fn']:
            t, calls, incl, most = int(w[2], 16), int(w[3]), int(w[4]), int(w[5])
            img = P.image_of(t)
            if not img or img.name == 'vmlinux' or not calls:
                continue
            f = img.func(t - img.base)
            if not f or f[1] != t - img.base or f[0] in ('main', '_start', '__libc_start_main', '_start_c', 'libc_start_main_stage2'):
                continue
            rows.append((incl / calls, f'{img.name}:{f[0]}', calls, incl, most))
    rows.sort(reverse=True)
    return [dict(fn=n, calls=c, per=round(p_), most=m, pct_all=round(100 * i / max(total, 1), 2)) for p_, n, c, i, m in rows
            if p_ >= least][:12]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('parts', nargs='+')
    ap.add_argument('--dir', default=os.path.join(SE, 'build', 'prof_parts'))
    ap.add_argument('--json')
    a = ap.parse_args()
    if not os.path.exists(NAMES):
        json.dump(make_names('/home/micha/ph/nm/work/sysroot/lib'), open(NAMES, 'w'))
    names = {k: set(v) for k, v in json.load(open(NAMES)).items()}
    rs = []
    for p in a.parts:
        try:
            r = summarize(p, a.dir, names)
        except Exception as e:   # (a part whose run failed: said, the rest go on)
            r = {'part': p, 'error': repr(e)}
        rs.append(r)
        print(json.dumps(r), flush=True)
    if a.json:
        with open(a.json, 'a') as f:
            for r in rs:
                f.write(json.dumps(r) + '\n')


if __name__ == '__main__':
    main()
