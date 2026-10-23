"""A sample profile for clang (-fprofile-sample-use, LLVM's text format) and a linker order file from a gvm_cpu
--profile of a program built with -g -fdebug-info-for-profiling (gpu_vm/llvm/README.md, "Profile-guided builds").
Run in WSL (llvm-symbolizer).

    python3 gpu_vm/tick/gamebench_rv/autofdo.py PROF ELF OUT.prof OUT.order [--cores all|0|workers]

Each bundle the profile counted is symbolized with its inline stack (llvm-symbolizer --inlines, linkage names): a
line's samples are the most of any bundle on it (a bundle is one line's: the line table's last .loc in it), under the
call sites of the functions inlined there; a call bundle adds its callee as a call target. The order file lists the
functions that ran, the most bundles a byte first (lld --symbol-ordering-file: GVM_ORDER_FILE).
"""
import bisect, json, os, subprocess, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hotcode_lib import elf, find_base, read_prof

SYM = os.path.expanduser(os.environ.get('LLVM_BIN', '~/gpu_vm/llvm-build/bin')) + '/llvm-symbolizer'


def base_disc(d):
    if d & 1: return 0
    d >>= 1
    return (((d >> 1) & 0xfe0) | (d & 0x1f)) if d & 0x20 else d & 0x1f


class Node:
    __slots__ = ('body', 'calls', 'kids', 'head')

    def __init__(self):
        self.body = {}                    # (offset, disc) -> samples
        self.calls = defaultdict(lambda: defaultdict(int))   # (offset, disc) -> {callee: count}
        self.kids = {}                    # ((offset, disc), callee name) -> Node
        self.head = 0

    def total(self):
        return sum(self.body.values()) + sum(k.total() for k in self.kids.values())


def key_str(k):
    return f'{k[0]}' + (f'.{k[1]}' if k[1] else '')


def write(f, name, n, ind):
    tot = n.total()
    if ind == 0: f.write(f'{name}:{tot}:{n.head}\n')
    for k in sorted(n.body, key=lambda k: k):
        tg = ''.join(f' {c}:{v}' for c, v in sorted(n.calls.get(k, {}).items()))
        f.write(f'{" " * (ind + 1)}{key_str(k)}: {n.body[k]}{tg}\n')
    for (k, callee), kid in sorted(n.kids.items(), key=lambda x: x[0]):
        if kid.total() == 0: continue
        f.write(f'{" " * (ind + 1)}{key_str(k)}: {callee}:{kid.total()}\n')
        write(f, callee, kid, ind + 1)


def main():
    args = list(sys.argv[1:])
    cores = 'all'
    if '--cores' in args:
        i = args.index('--cores'); cores = args[i + 1]; del args[i:i + 2]
    prof, elff, outp, outo = args[:4]
    (ta, tb), funcs = elf(elff)
    rows = read_prof(prof)
    base = find_base(rows, ta, tb)
    lo, hi = ta + base, ta + base + len(tb)
    cnt = {}
    for pc, r in rows.items():
        c = r[1] if cores == '0' else r[2] if cores == 'workers' else r[1] + r[2]
        if c and lo <= pc < hi: cnt[pc - base] = (c, r[0][3])
    addrs = sorted(cnt)
    p = subprocess.run([SYM, '--obj=' + elff, '--inlines', '--no-demangle', '--output-style=JSON'],
                       input=''.join(f'0x{a:x}\n' for a in addrs), capture_output=True, text=True)
    lines = p.stdout.splitlines()
    assert len(lines) == len(addrs), (len(lines), len(addrs), p.stderr[:500])
    starts = [f[0] for f in funcs]
    by_addr = {f[0]: f for f in funcs}
    top = defaultdict(Node)
    unknown = 0
    for a, l in zip(addrs, lines):
        c, ctl = cnt[a]
        fr = json.loads(l).get('Symbol', [])
        fr = [x for x in fr if x.get('FunctionName') and x['FunctionName'] != '??']
        if not fr:
            unknown += c; continue
        out = fr[-1]
        n = top[out['FunctionName']]
        if a in by_addr: n.head = max(n.head, c)
        # outermost to innermost: each frame but the innermost is a call site of the next one
        for i in range(len(fr) - 1, 0, -1):
            x = fr[i]
            k = (max(x['Line'] - x['StartLine'], 0), base_disc(x.get('Discriminator', 0)))
            n = n.kids.setdefault((k, fr[i - 1]['FunctionName']), Node())
        x = fr[0]
        if not x['Line']:   # (line 0: the compiler's own, no line to give it)
            continue
        k = (max(x['Line'] - x['StartLine'], 0), base_disc(x.get('Discriminator', 0)))
        n.body[k] = max(n.body.get(k, 0), c)
        if (ctl >> 7) & 15 == 0 and (ctl >> 11) & 7 == 6:   # a call: its callee
            t = (a + ((ctl if ctl < 0x80000000 else ctl - (1 << 32)) >> 14) * 16) & 0xffffffff
            i = bisect.bisect_right(starts, t) - 1
            if i >= 0 and funcs[i][0] == t and not funcs[i][2].startswith('__gpuvm_thunk'):
                n.calls[k][funcs[i][2]] += c
    with open(outp, 'w') as f:
        for name in sorted(top, key=lambda n: -top[n].total()):
            if top[name].total(): write(f, name, top[name], 0)
    # the order: functions by bundles a byte
    fb = defaultdict(int)
    for a, (c, _) in cnt.items():
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < funcs[i][0] + funcs[i][1]: fb[i] += c
    with open(outo, 'w') as f:
        for i in sorted(fb, key=lambda i: -fb[i] / max(funcs[i][1], 16)):
            if not funcs[i][2].startswith('__gpuvm_thunk'): f.write(funcs[i][2] + '\n')
    print(f'{len(top)} functions with samples, {len(fb)} in the order; {unknown} samples with no line')


main()
