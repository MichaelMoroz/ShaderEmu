"""Helpers of hotcode.py, layout_map.py and autofdo.py: an ELF's text and functions, a gvm_cpu profile, its load base."""
import struct
from collections import Counter, defaultdict


def elf(fn):
    """((text address, text bytes), [(address, size, name)] of the functions, by address)"""
    b = open(fn, 'rb').read()
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', b, shoff + i * shentsize) for i in range(shnum)]
    stro = secs[shstrndx][4]

    def nm(o, base):
        e = b.index(b'\0', base + o)
        return b[base + o:e].decode()
    named = {nm(s[0], stro): s for s in secs}
    t = named['.text']
    st = named['.symtab']
    strs = secs[st[6]][4]
    funcs = []
    for o in range(st[4], st[4] + st[5], 16):
        name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', b, o)
        if info & 0xf == 2 and size:
            funcs.append((value, size, nm(name, strs)))
    funcs.sort()
    return (t[3], b[t[4]:t[4] + t[5]]), funcs


def read_prof(fn):
    """pc -> (words, core 0's runs, the workers' runs, {core: runs} when the profile has them)"""
    rows = {}
    for l in open(fn):
        if l.startswith('#'):
            continue
        p = l.split()
        if len(p) < 7:
            continue
        per = {}
        for x in p[7:]:
            k, v = x.split(':')
            per[int(k)] = int(v)
        rows[int(p[0], 16)] = ([int(x, 16) for x in p[1:5]], int(p[5]), int(p[6]), per)
    return rows


def find_base(rows, ta, tb):
    """the load base: the hottest bundles' words looked up in the text"""
    idx = defaultdict(list)
    for o in range(0, len(tb) - 15, 16):
        idx[tb[o:o + 16]].append(o)
    votes = Counter()
    for pc, r in sorted(rows.items(), key=lambda r: -(r[1][1] + r[1][2]))[:2000]:
        k = struct.pack('<4I', *r[0])
        if len(idx.get(k, ())) == 1:
            votes[pc - (ta + idx[k][0])] += 1
    return votes.most_common(1)[0][0]
