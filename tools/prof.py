"""A profiler of the programs the machine runs, from rvc_cpu's RVC_PROF (docs/machine/profiling.md).

    python tools/prof.py run OUT [--exe EXE] -- RVC_CPU_ARGS...      a run with RVC_PROF=OUT
    python tools/prof.py report OUT [--elf PATH[=NM]] [--find NAME] [--top N] [--lines]
    python tools/prof.py deps OUT [--elf ...]                         what RVC_PROF_DEPS found
    python tools/prof.py doubles OUT [--elf ...] [--limit PERCENT]    the share in soft doubles and who calls them; exit 1 over the limit
    python tools/prof.py auto OUT [--elf ...] -- RVC_CPU_ARGS...     run, choose loops and calls, run again with
                                                                       RVC_PROF_DEPS on them, and say which are independent
    python tools/prof.py search --cmd "CMD" --param NAME=1,2,4 [--param NAME=16..64:16] [--check TEXT]
                                                                       every layout of a program's switches, ranked
    python tools/prof.py patterns                                     the library of slow patterns (tools/prof_patterns.ini)

The run's pcs are physical (a program without the MMU lies where the kernel put it): each ELF
given is placed by matching its code's words against the words the run executed. The kernel
(~/nm/k-rom/vmlinux in WSL) is added when it is there. --find NAME looks for an unstripped
NAME in the WSL build tree (~/nm/work/src).
"""
import argparse
import bisect
import collections
import configparser
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (the distribution's name: Ubuntu-24.04 once, Ubuntu on 28 October 2026)
WSL = next((p for p in (r'\\wsl.localhost\Ubuntu-24.04', r'\\wsl.localhost\Ubuntu') if os.path.isdir(p + r'\home')),
           r'\\wsl.localhost\Ubuntu')
# (the home of WSL's default user: /root where the distribution has no user of its own)
WSL_HOME = next((h for h in ('/home/micha', '/root') if os.path.isdir(WSL + h.replace('/', '\\') + r'\nm')), '/home/micha')
KERNEL = WSL + WSL_HOME.replace('/', '\\') + r'\nm\k-rom\vmlinux'
TOOLBIN = WSL_HOME + '/shaderemu-linux/toolchain/bin'
PATTERNS = os.path.join(ROOT, 'tools', 'prof_patterns.ini')
CORE0_TEXELS, WORKER_TEXELS = 384, 192   # what a core's write cache keeps a pass, about (AGENTS.md)


def to_wsl(path):
    p = os.path.abspath(path)
    if p.lower().startswith(WSL.lower()):
        return p[len(WSL):].replace('\\', '/')
    drive, rest = os.path.splitdrive(p)
    return '/mnt/' + drive[0].lower() + rest.replace('\\', '/')


def from_wsl(path):
    if path.startswith('~/'):
        path = WSL_HOME + path[1:]
    return WSL + path.replace('/', '\\') if path.startswith('/') else path


# ---------------------------------------------------------------- ELF

class Elf:
    """An ELF32's executable sections and function symbols; `base` is where the run had it."""

    def __init__(self, path, nm=None, name=None):
        self.path, self.name, self.base = path, name or os.path.basename(path), 0
        data = open(path, 'rb').read()
        if data[:4] != b'\x7fELF' or data[4] != 1:
            raise ValueError(f'{path}: not a 32-bit ELF')
        shoff, = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
        sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
        self.code = []    # (vaddr, words)
        funcs = []
        for sh in sections:
            name_, kind, flags, addr, off, size = sh[:6]
            if kind == 1 and flags & 4 and size >= 4:   # PROGBITS, executable
                self.code.append((addr, np.frombuffer(data[off:off + size // 4 * 4], dtype='<u4')))
            if kind == 2:   # SYMTAB
                strtab = sections[sh[6]]
                for at in range(off, off + size, 16):
                    nm_, value, sz, info, _, shndx = struct.unpack_from('<IIIBBH', data, at)
                    # (functions; and global symbols of no type, the assembler's: the kernel's memcpy and memset are
                    # NOTYPE, and their code was named after the C function before them)
                    if not value or not (info & 15 == 2 or (info & 15 == 0 and info >> 4 in (1, 2) and 0 < shndx < len(sections)
                                                             and sections[shndx][2] & 4)):
                        continue
                    end = data.index(b'\0', strtab[4] + nm_)
                    funcs.append((value, sz, data[strtab[4] + nm_:end].decode('latin-1')))
        if nm:
            for line in open(nm, encoding='latin-1'):
                p = line.split()
                if len(p) >= 3 and p[1] in 'tTwW':
                    funcs.append((int(p[0], 16), 0, p[2]))
        funcs.sort()
        self.funcs = funcs
        self.starts = [f[0] for f in funcs]
        self.lo = min((a for a, w in self.code), default=0)
        self.hi = max((a + 4 * len(w) for a, w in self.code), default=0)

    def word(self, vaddr):
        for a, w in self.code:
            if a <= vaddr < a + 4 * len(w):
                return int(w[(vaddr - a) >> 2])
        return None

    def func(self, vaddr):
        """(name, start) of the function at an ELF address, or None."""
        i = bisect.bisect_right(self.starts, vaddr) - 1
        if i < 0:
            return None
        start, size, n = self.funcs[i]
        if size and vaddr >= start + size and vaddr - start > 0x4000:
            return None
        if not size and vaddr - start > 0x4000:
            return None
        return n, start


def find_elf(name):
    """An unstripped ELF called NAME (or NAME_unstripped, NAME.elf) in the WSL build tree."""
    r = subprocess.run(['wsl', '--', 'bash', '-c',
                        f'find ~/nm/work/src -maxdepth 4 -type f \\( -name "{name}" -o -name "{name}_unstripped" -o -name "{name}.elf" \\) '
                        f'-size +8k 2>/dev/null | head -20'], capture_output=True, text=True)
    best = None
    for line in r.stdout.split():
        p = from_wsl(line)
        try:
            e = Elf(p)
        except (ValueError, OSError):
            continue
        if e.funcs and (best is None or len(e.funcs) > len(best.funcs)):
            best = e
    return best


# ---------------------------------------------------------------- the run's files

class Profile:
    def __init__(self, out):
        self.out = out
        self.meta = {}
        for line in open(os.path.join(out, 'meta.txt')):
            k, v = line.split()
            self.meta[k] = int(v)
        self.ticks = self.meta.get('ticks', 8192)
        rows = []
        for line in open(os.path.join(out, 'pcs.txt')):
            if line[0] == '#':
                continue
            c, table, pc, word, ex, lo, st, *more = line.split()
            nt, full = (int(more[0]), int(more[1])) if len(more) >= 2 else (0, 0)   # (rvc_cpu before 28 October 2026: no such columns)
            rows.append((int(c), int(pc, 16), int(word, 16), int(ex), int(lo), int(st), nt, full))
        a = np.array(rows, dtype=np.int64).reshape(-1, 8)
        self.cls, self.pc, self.word, self.exec, self.loads, self.stores, self.newtex, self.full = (a[:, i] for i in range(8))
        self.edges = []
        p = os.path.join(out, 'edges.txt')
        if os.path.exists(p):
            for line in open(p):
                if line[0] != '#':
                    c, a_, b, n, incl = line.split()
                    self.edges.append((int(c), int(a_, 16), int(b, 16), int(n), int(incl)))
        self.images = []

    def place(self, elfs, quiet=False):
        """Each ELF where its words match the executed ones (a program may be there twice)."""
        order = np.argsort(-self.exec)
        hot_pc, hot_word = self.pc[order], self.word[order]
        _, first = np.unique(hot_pc, return_index=True)   # (core 0's and the workers' rows of one pc: once)
        keep = np.sort(first)
        hot_pc, hot_word = hot_pc[keep][:60000], hot_word[keep][:60000]
        free = np.ones(len(hot_pc), dtype=bool)
        for e in elfs:
            if not e.code:
                continue
            allw = np.concatenate([w for a, w in e.code])
            alla = np.concatenate([a + 4 * np.arange(len(w), dtype=np.int64) for a, w in e.code])
            # (anchors are words rare in this ELF: a common one says nothing, and most hot pcs may be another program's)
            uw, cnt = np.unique(allw, return_counts=True)
            for copy in range(4):
                idx = np.nonzero(free)[0]
                if len(idx) < 8:
                    break
                found = None
                tried = 0
                for anchor in idx[:6000]:
                    w = hot_word[anchor]
                    k = np.searchsorted(uw, w)
                    if k >= len(uw) or uw[k] != w or cnt[k] > 4:
                        continue
                    where = np.nonzero(allw == w)[0]
                    tried += 1
                    if tried > 300:
                        break
                    for j in where:
                        base = int(hot_pc[anchor] - alla[j])
                        test = idx[:2000]
                        rel = hot_pc[test] - base
                        inside = (rel >= e.lo) & (rel < e.hi)
                        if inside.sum() < 8:
                            continue
                        pos = np.searchsorted(alla, rel[inside])
                        pos = np.minimum(pos, len(alla) - 1)
                        ok = (alla[pos] == rel[inside]) & (allw[pos] == hot_word[test][inside])
                        score = ok.mean()
                        if score >= 0.75 and ok.sum() >= 32 and (found is None or ok.sum() > found[1]):
                            found = (base, int(ok.sum()), score)
                    if found:
                        break
                if not found:
                    break
                base = found[0]
                img = Elf.__new__(Elf)
                img.__dict__.update(e.__dict__)
                img.base = base
                img.name = e.name if copy == 0 else f'{e.name}#{copy + 1}'
                self.images.append(img)
                inside = (hot_pc - base >= e.lo) & (hot_pc - base < e.hi)
                free &= ~inside
                if not quiet:
                    print(f'  {img.name}: at {base:#010x} (text {e.lo + base:#010x}..{e.hi + base:#010x}), '
                          f'{found[1]} of the hottest pcs match ({found[2]:.0%}'
                          f'{"" if found[2] > 0.97 else ": not quite the build that ran"})')
        self.place_pieces(elfs, hot_pc, hot_word, free, quiet)
        self.images.sort(key=lambda i: i.lo + i.base)
        self.starts_img = [i.lo + i.base for i in self.images]
        # a pc whose word is not the ELF's there is not named by it (a piece widened to whole functions, or a build
        # apart, would name another program's code: nxvideo's decoder was named musl's string_read)
        self.bad = set()
        pcs, first = np.unique(self.pc, return_index=True)
        words = self.word[first]
        img_of = np.array([-1 if (m := self.image_of(int(p))) is None else id(m) for p in pcs], dtype=np.int64)
        for img in self.images:
            sel = img_of == id(img)
            if not sel.any() or not img.code:
                continue
            allw = np.concatenate([w for a, w in img.code])
            alla = np.concatenate([a + 4 * np.arange(len(w), dtype=np.int64) for a, w in img.code])
            rel = pcs[sel].astype(np.int64) - img.base
            pos = np.minimum(np.searchsorted(alla, rel), len(alla) - 1)
            # (an immediate may differ: data or a callee moved in a build a little apart; the registers may not)
            ew, rw = allw[pos].astype(np.int64), words[sel].astype(np.int64)
            op = rw & 0x7f
            mask = np.where(np.isin(op, (0x17, 0x37, 0x6f)), 0xfff,
                            np.where(np.isin(op, (0x03, 0x13, 0x67)), 0xf8fff,
                                     np.where(np.isin(op, (0x23, 0x63)), 0x1fff07f, 0xffffffff)))
            ok = (alla[pos] == rel) & ((ew & mask) == (rw & mask))
            self.bad.update(int(p) for p in pcs[sel][~ok])
        if not quiet:
            named = img_of >= 0
            print(f'  {len(self.bad)} of {int(named.sum())} named pcs carry another word than the ELF\'s there: left unnamed')

    def place_pieces(self, elfs, hot_pc, hot_word, free, quiet):
        """An ELF built from sources a little apart from the run's (a function more in a library, a patch moved) has its
        code in a few stretches each at a base of its own: the hot pcs still unplaced vote for bases by the ELF's rare
        words, and each base keeps the runs of pcs whose words match there (32 or more), widened to whole functions."""
        for e in elfs:
            if not e.code:
                continue
            idx = np.nonzero(free)[0]
            if len(idx) < 32:
                break
            allw = np.concatenate([w for a, w in e.code])
            alla = np.concatenate([a + 4 * np.arange(len(w), dtype=np.int64) for a, w in e.code])
            order = np.argsort(allw, kind='stable')
            sw = allw[order]
            uw, start, cnt = np.unique(sw, return_index=True, return_counts=True)
            k = np.minimum(np.searchsorted(uw, hot_word[idx]), len(uw) - 1)
            rare = (uw[k] == hot_word[idx]) & (cnt[k] <= 2)
            a_idx, k = idx[rare], k[rare]
            bases = [hot_pc[a_idx] - alla[order[start[k]]]]
            two = cnt[k] == 2
            bases.append(hot_pc[a_idx[two]] - alla[order[start[k[two]] + 1]])
            ub, nv = np.unique(np.concatenate(bases), return_counts=True)
            top = np.argsort(-nv)[:24]
            pieces = matched = 0
            for base, v in zip(ub[top].tolist(), nv[top].tolist()):
                if v < 16:
                    break
                idx = np.nonzero(free)[0]
                idx = idx[np.argsort(hot_pc[idx])]
                rel = hot_pc[idx] - base
                inside = (rel >= e.lo) & (rel < e.hi)
                idx, rel = idx[inside], rel[inside]
                if len(idx) < 32:
                    continue
                pos = np.minimum(np.searchsorted(alla, rel), len(alla) - 1)
                ok = (alla[pos] == rel) & (allw[pos] == hot_word[idx])
                runs, cur, miss = [], [], 0
                for i in range(len(idx)):
                    if ok[i]:
                        cur.append(i)
                        miss = 0
                    elif cur:
                        miss += 1
                        if miss > 2:
                            runs.append(cur)
                            cur, miss = [], 0
                if cur:
                    runs.append(cur)
                for r in runs:
                    if len(r) < 32:
                        continue
                    lo, hi = int(rel[r[0]]), int(rel[r[-1]]) + 4
                    f = e.func(lo)
                    if f:
                        lo = f[1]
                    i = bisect.bisect_right(e.starts, hi - 4) - 1
                    if i >= 0 and e.funcs[i][1]:
                        hi = max(hi, e.funcs[i][0] + e.funcs[i][1])
                    img = Elf.__new__(Elf)
                    img.__dict__.update(e.__dict__)
                    img.base, img.lo, img.hi = base, lo, hi
                    img.name = e.name
                    self.images.append(img)
                    free &= ~((hot_pc - base >= lo) & (hot_pc - base < hi))
                    pieces += 1
                    matched += len(r)
            if pieces and not quiet:
                print(f'  {e.name}: {pieces} pieces at bases of their own, {matched} more of the hottest pcs match')

    def image_of(self, pc):
        if pc in getattr(self, 'bad', ()):
            return None
        i = bisect.bisect_right(self.starts_img, pc) - 1
        for j in range(i, max(i - 8, -1), -1):   # (pieces widened to whole functions may lie inside another)
            img = self.images[j]
            if img.lo <= pc - img.base < img.hi:
                return img
        return None

    def name(self, pc, offset=True):
        img = self.image_of(pc)
        if img:
            f = img.func(pc - img.base)
            if f:
                n, start = f
                if offset and pc - img.base != start:
                    return f'{img.name}:{n}+{pc - img.base - start:#x}'
                return f'{img.name}:{n}'
            return f'{img.name}:{pc - img.base:#x}'
        return f'?{pc & ~0xfff:#010x}'

    def func_key(self, pc):
        img = self.image_of(pc)
        if img:
            f = img.func(pc - img.base)
            if f:
                return f'{img.name}:{f[0]}', f[1] + img.base
            return f'{img.name}:?', img.base
        return f'?{pc & ~0xfff:#010x}', pc & ~0xfff


# ---------------------------------------------------------------- instructions

REG = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7',
       's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 't3', 't4', 't5', 't6']


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(w):
    """(mnemonic, class, immediate or branch offset) of an RV32IMAF word."""
    op, rd, f3, rs1, rs2, f7 = w & 0x7f, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31, w >> 25
    if op == 0x13:
        imm = sx(w >> 20, 12)
        if f3 == 5:
            return ('srai' if f7 == 0x20 else 'srli'), 'alu', rs2
        return ['addi', 'slli', 'slti', 'sltiu', 'xori', 'srli', 'ori', 'andi'][f3], 'alu', imm
    if op == 0x33:
        if f7 == 1:
            m = ['mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu'][f3]
            return m, ('div' if f3 >= 4 else 'mul'), 0
        if f7 == 0x20:
            return ('sub' if f3 == 0 else 'sra'), 'alu', 0
        return ['add', 'sll', 'slt', 'sltu', 'xor', 'srl', 'or', 'and'][f3], 'alu', 0
    if op == 0x03:
        return ['lb', 'lh', 'lw', '?', 'lbu', 'lhu', '?', '?'][f3], 'load', sx(w >> 20, 12)
    if op == 0x23:
        return ['sb', 'sh', 'sw', '?', '?', '?', '?', '?'][f3], 'store', sx(((w >> 25) << 5) | rd, 12)
    if op == 0x63:
        imm = sx(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) | (((w >> 8) & 0xf) << 1), 13)
        return ['beq', 'bne', '?', '?', 'blt', 'bge', 'bltu', 'bgeu'][f3], 'branch', imm
    if op == 0x6f:
        imm = sx(((w >> 31) << 20) | (((w >> 12) & 0xff) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3ff) << 1), 21)
        return ('j' if rd == 0 else 'jal'), ('jump' if rd == 0 else 'call'), imm
    if op == 0x67:
        if rd == 0 and rs1 == 1:
            return 'ret', 'ret', 0
        return ('jr' if rd == 0 else 'jalr'), ('jump' if rd == 0 else 'call'), sx(w >> 20, 12)
    if op == 0x37:
        return 'lui', 'alu', w >> 12
    if op == 0x17:
        return 'auipc', 'alu', w >> 12
    if op == 0x2f:
        return {0: 'amoadd', 1: 'amoswap', 2: 'lr', 3: 'sc'}.get(w >> 27, 'amo'), 'atomic', 0
    if op == 0x07:
        return 'flw', 'load', sx(w >> 20, 12)
    if op == 0x27:
        return 'fsw', 'store', sx(((w >> 25) << 5) | rd, 12)
    if op in (0x43, 0x47, 0x4b, 0x4f, 0x53):
        if op == 0x53 and f7 in (0x0c, 0x2c):
            return ('fdiv' if f7 == 0x0c else 'fsqrt'), 'fdiv', 0
        return 'f' + {0x53: 'op', 0x43: 'madd', 0x47: 'msub', 0x4b: 'nmsub', 0x4f: 'nmadd'}[op], 'float', 0
    if op == 0x73:
        if w == 0x10500073:
            return 'wfi', 'system', 0
        return ('csr' if f3 else 'ecall'), 'system', 0
    if op == 0x0f:
        return ('fence.i' if f3 == 1 else 'pause' if w == 0x0100000f else 'fence'), 'system', 0
    return '?', 'other', 0


# ---------------------------------------------------------------- the report's parts

def pct(a, b):
    return 100.0 * a / b if b else 0.0


def hot_functions(P, top, lines=False):
    out = {}
    for k in (0, 1):
        sel = P.cls == k
        total = int(P.exec[sel].sum())
        if not total:
            continue
        by = collections.Counter()
        mem = collections.Counter()
        for pc, ex, lo, st in zip(P.pc[sel], P.exec[sel], P.loads[sel], P.stores[sel]):
            f, _ = P.func_key(int(pc))
            by[f] += int(ex)
            mem[f] += int(lo) + int(st)
        who = 'core 0' if k == 0 else 'the workers'
        print(f'\n== where {who} ran: {total:,} instructions (by function, its own instructions)')
        progs = collections.Counter()
        for f, n in by.items():
            progs[f.split(':')[0]] += n
        print('   ' + ', '.join(f'{p} {pct(n, total):.1f}%' for p, n in progs.most_common(8)))
        for f, n in by.most_common(top):
            print(f'  {pct(n, total):5.1f}%  {n:>14,}  {mem[f] / n if n else 0:5.2f} mem/instr  {f}')
        out[k] = (total, by)
        if lines:
            hot_lines(P, sel, total, top)
    return out


def hot_lines(P, sel, total, top):
    """The hottest source lines (addr2line in WSL, on ELFs with debug information)."""
    order = np.argsort(-P.exec[sel])[:top * 8]
    pcs, ex = P.pc[sel][order], P.exec[sel][order]
    per_img = collections.defaultdict(list)
    for pc, n in zip(pcs, ex):
        img = P.image_of(int(pc))
        if img:
            per_img[img].append((int(pc), int(n)))
    lines = collections.Counter()
    for img, items in per_img.items():
        addrs = ' '.join(f'{pc - img.base:#x}' for pc, _ in items)
        r = subprocess.run(['wsl', '--', 'bash', '-c', f'{TOOLBIN}/riscv32-linux-addr2line -e {to_wsl(img.path)} {addrs}'],
                           capture_output=True, text=True)
        got = r.stdout.split('\n')
        for (pc, n), where in zip(items, got):
            where = where.strip()
            if where and not where.startswith('??'):
                lines[f'{img.name}:{where.split("/")[-1]}'] += n
    if lines:
        print('   hottest lines:')
        for l, n in lines.most_common(top):
            print(f'    {pct(n, total):5.1f}%  {l}')


def call_graph(P, top):
    for k in (0, 1):
        es = [e for e in P.edges if e[0] == k]
        if not es:
            continue
        total = int(P.exec[P.cls == k].sum())
        incl = collections.Counter()
        calls = collections.Counter()
        pairs = collections.Counter()
        for _, a, b, n, i in es:
            nb = P.name(b, False)
            if a and P.name(a, False) == nb:
                continue   # (a recursion: its outer call has it)
            incl[nb] += i
            calls[nb] += n
            pairs[(P.name(a, False) if a else '(top)', nb)] += i
        who = 'core 0' if k == 0 else 'the workers'
        print(f'\n== calls on {who}: inclusive instructions (a function that recurses counts each level)')
        for f, i in incl.most_common(top):
            print(f'  {pct(i, total):5.1f}%  {calls[f]:>10,} calls  {i / max(calls[f], 1):>12,.0f} a call  {f}')
        print('   heaviest caller -> callee:')
        for (a, b), i in pairs.most_common(top):
            print(f'  {pct(i, total):5.1f}%  {a} -> {b}')


def memory(P, top):
    path = os.path.join(P.out, 'mem.bin')
    if not os.path.exists(path):
        return
    raw = np.fromfile(path, dtype='<u4').reshape(-1, 1025)
    at = raw[:, 0].astype(np.int64)
    cnt = raw[:, 1:].reshape(-1, 4, 256).astype(np.int64)   # loads0, loads1, stores0, stores1
    l0, l1, s0, s1 = (cnt[:, i, :].sum(1) for i in range(4))
    print(f'\n== memory: {len(at)} pages of RAM touched; loads {l0.sum():,} (core 0) {l1.sum():,} (workers), '
          f'stores {s0.sum():,} {s1.sum():,}')
    texels_stored = (cnt[:, 2, :] + cnt[:, 3, :] > 0).sum(1)
    order = np.argsort(-(l0 + l1 + s0 + s1))[:top]
    print('   busiest pages (loads core0/workers, stores core0/workers, texels stored of 256):')
    for i in order:
        img = P.image_of(int(at[i]))
        what = f'  ({img.name} text)' if img else ''
        print(f'    {at[i]:#010x}  {l0[i]:>12,} {l1[i]:>12,}  {s0[i]:>11,} {s1[i]:>11,}  {texels_stored[i]:3d}{what}')
    # stores by the code that made them
    for k in (0, 1):
        sel = (P.cls == k) & (P.stores > 0)
        if not sel.any():
            continue
        by = collections.Counter()
        for pc, st in zip(P.pc[sel], P.stores[sel]):
            by[P.func_key(int(pc))[0]] += int(st)
        tot = sum(by.values())
        print(f'   stores of {"core 0" if k == 0 else "the workers"} by function ({tot:,}):')
        for f, n in by.most_common(min(top, 10)):
            print(f'    {pct(n, tot):5.1f}%  {f}')
    # what fills the write cache: the stores that took a texel new to their core's pass (changed bytes), and where a
    # store found it full (RVC_WCACHE: the pass ended there)
    for k in (0, 1):
        sel = (P.cls == k) & ((P.newtex > 0) | (P.full > 0))
        if not sel.any():
            continue
        by, fu = collections.Counter(), collections.Counter()
        for pc, nt, f in zip(P.pc[sel], P.newtex[sel], P.full[sel]):
            name = P.func_key(int(pc))[0]
            by[name] += int(nt)
            fu[name] += int(f)
        tot, tf = sum(by.values()), sum(fu.values())
        tl = os.path.join(P.out, 'timeline.bin')
        passes = os.path.getsize(tl) // 32 if os.path.exists(tl) else 0   # (the passes profiled: from --count-from on)
        print(f'   texels new to a pass of {"core 0" if k == 0 else "the workers"} by function ({tot:,}'
              f'{f", {tot / passes:.1f} a pass" if passes else ""}; the write cache entries they took):')
        for f, n in by.most_common(min(top, 12)):
            print(f'    {pct(n, tot):5.1f}%  {n:>12,}  {f}')
        if tf:
            print(f'   where {"core 0" if k == 0 else "the workers"} found the write cache full ({tf:,} passes ended so):')
            for f, n in fu.most_common(min(top, 8)):
                print(f'    {pct(n, tf):5.1f}%  {n:>10,}  {f}')
    path = os.path.join(P.out, 'wcache.txt')
    if os.path.exists(path):
        hist = {0: collections.Counter(), 1: collections.Counter()}
        for line in open(path):
            if line[0] != '#':
                k, b, n = map(int, line.split())
                hist[k][b] += n
        for k, limit in ((0, CORE0_TEXELS), (1, WORKER_TEXELS)):
            h = hist[k]
            n = sum(h.values())
            if not n:
                continue
            over = sum(v for b, v in h.items() if b * 16 >= limit)
            mean = sum((b * 16 + 8) * v for b, v in h.items()) / n
            print(f'   distinct texels a core stored a pass, {"core 0" if k == 0 else "workers"}: {n:,} core-passes, about {mean:.0f} '
                  f'on average, {pct(over, n):.1f}% at or over {limit} (the write cache keeps about that many)')


IDLE = ('arch_cpu_idle', 'default_idle_call', 'do_idle', 'cpu_startup_entry', 'cpuidle_idle_call', 'zeroed_idle')


def timeline(P, buckets=20):
    path = os.path.join(P.out, 'timeline.bin')
    t = np.fromfile(path, dtype='<u4').reshape(-1, 8).astype(np.int64)
    if not len(t):
        return
    n0, pc, flags, ran, full, wsum, d0, dmost = (t[:, i] for i in range(8))
    T = P.ticks
    passes = len(t)
    reasons = collections.Counter()
    lost = collections.Counter()
    for i in np.nonzero(n0 < T)[0]:
        name = P.name(int(pc[i]), False)
        fn = name.split(':')[-1]
        if flags[i] & 1:
            r = 'idle (kernel)' if fn in IDLE else 'wfi in ' + name
        elif fn in IDLE:
            r = 'idle (kernel)'
        elif fn == 'shaderemu_gpu_rest':
            r = 'rest (the program asked)'
        elif flags[i] & 2:
            r = 'ended at ' + name
        else:
            r = 'short at ' + name
        reasons[r] += 1
        lost[r] += T - int(n0[i])
    idle = reasons['idle (kernel)'] + reasons['rest (the program asked)']
    busy = passes - idle
    print(f'\n== passes: {passes:,}; core 0 ran {n0.sum():,} instructions; {int((n0 >= T).sum()):,} full, {idle:,} idle or rested; '
          f'core 0 ran {pct(n0.sum(), max(busy, 1) * T):.1f}% of the instructions the other passes could have')
    print('   what ended passes early (passes, instructions lost):')
    for r, n in sorted(reasons.items(), key=lambda kv: -lost[kv[0]])[:12]:
        print(f'    {n:>8,} {lost[r]:>14,}  {r}')
    cores = P.meta.get('cores', 1) - 1
    if wsum.sum():
        print(f'   workers: {wsum.sum():,} instructions; {pct(wsum.sum(), passes * T * max(cores, 1)):.1f}% of all workers\' passes, '
              f'{pct(wsum.sum(), max(ran.sum(), 1) * T):.1f}% of the passes they ran; most at once {ran.max()}')
    print(f'   over time ({buckets} parts): core 0 busy %, workers running (mean), worker utilization while running %')
    edges = np.linspace(0, passes, buckets + 1).astype(int)
    for a, b in zip(edges[:-1], edges[1:]):
        if b <= a:
            continue
        c0 = pct(n0[a:b].sum(), (b - a) * T)
        wr = ran[a:b].mean()
        wu = pct(wsum[a:b].sum(), max(ran[a:b].sum(), 1) * T)
        bar = '#' * int(c0 / 5)
        print(f'    {a:>8}-{b:<8} {c0:5.1f}% {bar:20s} {wr:6.1f} workers {wu:5.1f}%')


def loops(P, total0):
    """Loops of core 0 found by the branches back, with what their bodies ran."""
    sel = P.cls == 0
    pcs, ex, words = P.pc[sel], P.exec[sel], P.word[sel]
    order = np.argsort(pcs)
    pcs, ex, words = pcs[order], ex[order], words[order]
    cum = np.concatenate(([0], np.cumsum(ex)))
    found = []
    for i in range(len(pcs)):
        m, cls, imm = decode(int(words[i]))
        if cls in ('branch', 'jump') and m not in ('jr',) and imm < 0 and imm > -4096:
            head, tail = int(pcs[i]) + imm, int(pcs[i])
            a = np.searchsorted(pcs, head)
            body = int(cum[i + 1] - cum[a])
            trips = int(ex[i])
            if trips < 8 or body < total0 * 0.002:
                continue
            found.append({'head': head, 'tail': tail, 'body': body, 'trips': trips, 'per': body / trips, 'a': a, 'b': i + 1})
    found.sort(key=lambda l: -l['body'])
    for l in found:
        l['features'] = features(P, pcs, ex, words, l)
    return found


def features(P, pcs, ex, words, l):
    seq, f = [], collections.Counter()
    for j in range(l['a'], l['b']):
        m, cls, imm = decode(int(words[j]))
        seq.append(m)
        f[cls] += 1
        f['m_' + m] += 1
        if m == 'andi':
            f['andi_' + str(imm)] += 1
        if m in ('addi',) and imm in (1, -1):
            f['step1'] += 1
    return {'seq': ' '.join(seq), 'n': l['b'] - l['a'], 'loads': f['load'], 'stores': f['store'], 'calls': f['call'],
            'divs': f['div'] + f['fdiv'], 'floats': f['float'], 'branches': f['branch'], 'lbu': f['m_lbu'] + f['m_lb'],
            'sb': f['m_sb'], 'lw': f['m_lw'], 'sw': f['m_sw'], 'step1': f['step1'], 'andi255': f['andi_255'],
            'system': f['system'], 'per': l['per'], 'trips': l['trips'], 'atomics': f['atomic'],
            'adds': f['m_add'] + f['m_addi']}


# ---------------------------------------------------------------- the pattern library

def load_patterns():
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(PATTERNS, encoding='utf-8')
    pats = []
    for name in cp.sections():
        s = cp[name]
        pats.append({'name': name, 'kind': s.get('kind', 'loop'), 'seq': s.get('seq'), 'func': s.get('function'),
                     'when': s.get('when', 'True'), 'why': s.get('why', ''), 'instead': s.get('instead', ''), 'see': s.get('see', '')})
    return pats


SAFE = {'__builtins__': {}, 'min': min, 'max': max, 'abs': abs}


def match_loop(p, f):
    if p['kind'] != 'loop':
        return False
    if p['seq'] and not re.search(p['seq'], f['seq']):
        return False
    try:
        return bool(eval(p['when'], SAFE, dict(f)))
    except Exception:
        return False


def match_func(p, name, f):
    if p['kind'] != 'function' or not p['func'] or not re.search(p['func'], name.split(':')[-1]):
        return False
    try:
        return bool(eval(p['when'], SAFE, dict(f)))
    except Exception:
        return False


def patterns_report(P, found_loops, total0, top):
    pats = load_patterns()
    hits = []
    for l in found_loops[:top * 3]:
        for p in pats:
            if match_loop(p, l['features']):
                hits.append((l['body'], p, P.name(l['head']), f'{l["trips"]:,} trips of {l["per"]:.0f}'))
    # functions: by inclusive instructions a call, from the call edges
    incl, calls = collections.Counter(), collections.Counter()
    for k, a, b, n, i in P.edges:
        if k == 0:
            nb = P.name(b, False)
            incl[nb] += i
            calls[nb] += n
    for f, i in incl.most_common(200):
        feat = {'calls': calls[f], 'per': i / max(calls[f], 1), 'share': i / max(total0, 1), 'inclusive': i}
        for p in pats:
            if match_func(p, f, feat):
                hits.append((i, p, f, f'{calls[f]:,} calls of {feat["per"]:,.0f}'))
    hits.sort(key=lambda h: -h[0])
    print(f'\n== known slow patterns ({len(pats)} in {os.path.relpath(PATTERNS, ROOT)}): {len(hits)} places')
    for n, p, where, what in hits[:top]:
        print(f'  {pct(n, total0):5.1f}%  [{p["name"]}] {where}  ({what})')
        print(f'          {p["why"]}')
        print(f'          instead: {p["instead"]}' + (f'  ({p["see"]})' if p['see'] else ''))
    return hits


def candidates(P, found_loops, total0, most=10):
    """Loops and calls of core 0 worth asking RVC_PROF_DEPS about."""
    picks, heads = [], set()
    # (loops of one head, branched back to from several places: the widest, which the others are inside)
    for l in sorted(found_loops, key=lambda l: -l['tail']):
        if l['body'] >= total0 * 0.01 and l['trips'] >= 8 and l['features']['system'] == 0 and l['head'] not in heads:
            heads.add(l['head'])
            picks.append((l['body'], f'l:{l["head"]:x}-{l["tail"]:x}', P.name(l['head'])))
    seen = set()
    for k, a, b, n, i in sorted(P.edges, key=lambda e: -e[4]):
        if k != 0 or n < 16 or i < total0 * 0.01 or i / n < 2000 or i / n > total0 * 0.2 or b in seen:
            continue
        nm = P.name(b, False)
        if nm.startswith('?') or nm.startswith('vmlinux'):
            continue
        seen.add(b)
        picks.append((i, f'f:{b:x}', nm))
    picks.sort(key=lambda p: -p[0])
    out, have = [], set()
    for p in picks:
        if p[1] not in have:
            out.append(p)
            have.add(p[1])
    return out[:most]


# ---------------------------------------------------------------- dependences

KINDS = ['reads what an earlier one wrote', 'writes the bytes an earlier one wrote', 'writes beside what an earlier one wrote (one texel)',
         'writes what an earlier one read']


def deps_report(P, total0):
    path = os.path.join(P.out, 'deps.txt')
    if not os.path.exists(path):
        print('no deps.txt: run with RVC_PROF_DEPS (prof.py auto does)')
        return
    watches = []
    for line in open(path):
        w = line.split()
        if w[0] == 'watch':
            watches.append({'kind': w[1], 'lo': int(w[2], 16), 'hi': int(w[3], 16), 'groups': int(w[4]), 'inst': int(w[5]),
                            'instr': int(w[6]), 'with_raw': int(w[7]), 'count': list(map(int, w[8:16])), 'trunc': int(w[16]), 'pairs': []})
        elif w[0] == 'pair':
            watches[-1]['pairs'].append((int(w[1]), int(w[2], 16), int(w[3], 16), int(w[4])))
    print('\n== independence of iterations and calls (core 0; through memory, its own stack below its start left out)')
    for w in watches:
        what = ('loop at ' if w['kind'] == 'l' else 'calls of ') + P.name(w['lo'])
        if w['inst'] < 2 or w['inst'] < 1.5 * w['groups']:
            print(f'  {what}: about one instance a run ({w["inst"]:,} in {w["groups"]:,} runs): nothing to share out at once')
            continue
        per = w['instr'] / w['inst']
        share = pct(w['instr'], total0)
        c = w['count']
        raw, waw, wawt, war = c[0], c[1], c[2], c[3]
        fraw, fwaw = c[4], c[5]
        # a sum kept in memory: the same store place both read back and written again by the next instance
        stores_raw = {(a, b) for k, a, b, n in w['pairs'] if k == 0}
        acc = {b for k, a, b, n in w['pairs'] if k == 1 and a == b}
        reduction = acc and all(b in acc for a, b in stores_raw)
        if raw == 0 and waw == 0 and war == 0 and wawt == 0:
            verdict = 'INDEPENDENT: a candidate for workers (mcw_for over its instances)'
        elif raw == 0 and waw == 0 and war == 0:
            verdict = 'INDEPENDENT but neighbours write one 16 bytes: give each worker whole texels (mcw_for_opts.out_bytes)'
        elif reduction:
            verdict = 'A SUM carried in memory: a share for each worker and the sums added at the end'
        elif raw == 0:
            verdict = 'no reads of each other\'s results; writes overlap (order of the last write matters)'
        else:
            verdict = f'DEPENDENT: {pct(w["with_raw"], w["inst"]):.0f}% of instances read what an earlier one wrote'
        print(f'  {what}: {w["groups"]:,} runs, {w["inst"]:,} instances of {per:,.0f} instructions ({share:.1f}% of core 0)'
              f'{"; truncated" if w["trunc"] else ""}')
        print(f'      {verdict}')
        if fraw or fwaw:
            print(f'      in the frame above the stack pointer: {fraw:,} reads of an earlier one\'s, {fwaw:,} rewrites (locals kept in memory)')
        for k, a, b, n in sorted(w['pairs'], key=lambda p: -p[3])[:4]:
            if k & 3 == 2 and raw + waw + war:
                continue
            print(f'      {n:>10,} x {P.name(a)} {KINDS[k & 3]} at {P.name(b)}{" (frame)" if k & 4 else ""}')
        if raw == 0 and waw == 0 and war == 0 and share > 0:
            print(f'      with W workers about {share * (1 - 1 / 8):.1f}% of core 0 saved at 8, {share * (1 - 1 / 64):.1f}% at 64, '
                  f'less a pass or two a run ({w["groups"]:,} runs)')


# ---------------------------------------------------------------- commands

def load(args):
    P = Profile(args.out)
    elfs = []
    for spec in args.elf or []:
        path, _, nm = spec.partition('=')
        try:
            elfs.append(Elf(from_wsl(path), nm and from_wsl(nm)))
        except (ValueError, OSError) as e:
            print(f'  {spec}: {e}')
    for name in args.find or []:
        e = find_elf(name)
        if e:
            elfs.append(e)
        else:
            print(f'  {name}: no unstripped ELF of that name in ~/nm/work/src')
    if not args.no_kernel and os.path.exists(KERNEL):
        elfs.append(Elf(KERNEL, name='vmlinux'))
    print(f'{args.out}: {P.meta.get("passes", 0):,} passes of {P.ticks} instructions, {P.meta.get("cores", 1)} cores')
    P.place(elfs)
    return P


def cmd_report(args):
    P = load(args)
    hf = hot_functions(P, args.top, args.lines)
    total0 = hf.get(0, (0,))[0]
    call_graph(P, args.top)
    memory(P, args.top)
    timeline(P)
    ls = loops(P, total0)
    print(f'\n== loops of core 0 (by the instructions of their bodies, calls out of them not counted)')
    for l in ls[:args.top]:
        f = l['features']
        print(f'  {pct(l["body"], total0):5.1f}%  {l["trips"]:>12,} trips of {l["per"]:7.1f}  {P.name(l["head"])} .. +{l["tail"] - l["head"]:#x}'
              f'  ({f["loads"]} loads, {f["stores"]} stores, {f["calls"]} calls in {f["n"]})')
    patterns_report(P, ls, total0, args.top)
    picks = candidates(P, ls, total0)
    if picks:
        line = ','.join(p[1] for p in picks)
        open(os.path.join(args.out, 'watch.txt'), 'w').write(line + '\n')
        print(f'\n== worth asking about their independence (prof.py auto, or RVC_PROF_DEPS={line}):')
        for n, w, nm in picks:
            print(f'  {pct(n, total0):5.1f}%  {w:24s} {nm}')
    if os.path.exists(os.path.join(args.out, 'deps.txt')):
        deps_report(P, total0)
    return P


# What a double costs where there is no double hardware (rt/dfast.c, compiler-rt, soft-fp): loud, so that it is seen.
SOFT_DOUBLE = re.compile(r'__(add|sub|mul|div|neg|eq|ne|ge|gt|le|lt|unord|cmp)(df|tf|dc|tc)\d?(_slow)?'
                         r'|__(extend|trunc)(sf|df|tf)(df|sf|tf)2|__float(un)?(si|di|ti)(df|tf)|__fix(uns)?(df|tf)(si|di|ti)|__powidf2')


def double_name(name):
    return SOFT_DOUBLE.fullmatch(name.split(':')[-1].split('+')[0].strip()) is not None


def cmd_doubles(args):
    """Each side's instructions in soft doubles and who calls them; exit 1 when a share is over --limit (percent)."""
    P = load(args)
    worst, named = 0.0, 0
    for k in (0, 1):
        sel = P.cls == k
        total = int(P.exec[sel].sum())
        if not total:
            continue
        by = collections.Counter()
        for pc, ex in zip(P.pc[sel], P.exec[sel]):
            f, _ = P.func_key(int(pc))
            if ':' in f:
                named += int(ex)
            if double_name(f):
                by[f] += int(ex)
        soft = sum(by.values())
        share = pct(soft, total)
        worst = max(worst, share)
        who = 'core 0' if k == 0 else 'the workers'
        print(f'\n== {who}: {share:.1f}% in soft doubles, {soft:,} of {total:,} instructions'
              f' ({"OVER THE LIMIT" if share > args.limit else "ok"}, limit {args.limit:g}%)')
        for f, n in by.most_common(args.top):
            print(f'  {pct(n, total):5.1f}%  {n:>14,}  {f}')
        callers = collections.Counter()
        for c, a, b, n, i in P.edges:
            if c == k and a and double_name(P.name(b, False)) and not double_name(P.name(a, False)):
                callers[P.name(a, False)] += i
        if callers:
            print('   who calls them (inclusive):')
            for f, i in callers.most_common(args.top):
                print(f'  {pct(i, total):5.1f}%  {f}')
    if not named:
        print('\nNO NAMES: no ELF of the program was placed, so nothing could be counted (--find or --elf)')
        sys.exit(2)
    print(f'\nSOFT DOUBLES: {worst:.1f}% at most, limit {args.limit:g}%')
    if worst > args.limit:
        sys.exit(1)


def cmd_deps(args):
    P = load(args)
    total0 = int(P.exec[P.cls == 0].sum())
    deps_report(P, total0)


def rvc_cpu_run(exe, out, rest, env_extra):
    env = dict(os.environ)
    env['RVC_PROF'] = os.path.abspath(out)
    env.update(env_extra)
    if rest and rest[0] == '--':
        rest = rest[1:]
    t = time.time()
    r = subprocess.run([exe] + rest, env=env, capture_output=True, text=True, errors='replace')
    took = time.time() - t
    for line in r.stderr.splitlines():
        if line.startswith(('COUNT', '[prof]', '[cpu] ')) and 'passes' in line or line.startswith('COUNT'):
            print('  ' + line)
    if r.returncode:
        print('\n'.join('  ' + l for l in (r.stdout + r.stderr).strip().splitlines()[-6:]))
    print(f'  ({took:.1f} s, exit {r.returncode})')
    return r


def cmd_run(args):
    rvc_cpu_run(args.exe, args.out, args.rest, {})


def cmd_auto(args):
    print('-- the profile')
    rvc_cpu_run(args.exe, args.out, args.rest, {})
    P = cmd_report(args)
    w = os.path.join(args.out, 'watch.txt')
    if not os.path.exists(w):
        print('nothing worth a second run')
        return
    watch = open(w).read().strip()
    print(f'\n-- again with RVC_PROF_DEPS={watch}')
    rvc_cpu_run(args.exe, args.out, args.rest, {'RVC_PROF_DEPS': watch})
    deps_report(P, int(P.exec[P.cls == 0].sum()))


def parse_values(spec):
    if '..' in spec:
        a, _, rest = spec.partition('..')
        b, _, step = rest.partition(':')
        return [str(v) for v in range(int(a), int(b) + 1, int(step or 1))]
    return spec.split(',')


def cmd_search(args):
    import itertools
    from concurrent.futures import ThreadPoolExecutor
    names, values = [], []
    for p in args.param:
        n, _, v = p.partition('=')
        names.append(n)
        values.append(parse_values(v))
    combos = list(itertools.product(*values))
    print(f'{len(combos)} layouts of {", ".join(names)}')

    def one(combo):
        env = ' '.join(f'{n}={v}' for n, v in zip(names, combo))
        send = f"{args.pre}{'; ' if args.pre else ''}echo FL-''GO; {env} {args.cmd}; echo LX-''DONE\\n"
        log = os.path.join(tempfile.gettempdir(), 'prof_search_%d_%s.log' % (os.getpid(), '_'.join(combo)))
        if os.path.exists(log):
            os.remove(log)
        cmd = [args.exe, '--quiet', '--uart-log', log, '--expect', '/ # ', '--send', send, '--count-from', 'FL-GO', '--until', 'LX-DONE',
               '--seconds', str(args.seconds)]
        if args.image:
            cmd += ['--image', args.image]
        t = time.time()
        r = subprocess.run(cmd + args.more, capture_output=True, text=True, errors='replace')
        took = time.time() - t
        m = re.search(r'COUNT from "FL-GO": core 0 (\d+), all cores (\d+), (\d+) cores ran, the busiest worker (\d+), (\d+) passes, ([\d.]+) s',
                      r.stderr)
        ok = True
        if args.check:
            said = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
            ok = re.search(args.check, said.split("echo LX-''DONE", 1)[-1]) is not None
        if not m:
            return combo, None, took, False
        c0, allc, ran, busiest, passes, secs = m.groups()
        return combo, {'core0': int(c0), 'all': int(allc), 'cores': int(ran), 'busiest': int(busiest), 'passes': int(passes),
                       'seconds': float(secs)}, took, ok

    first = one(combos[0])
    est = first[2] * len(combos) / max(args.jobs, 1)
    print(f'  one run {first[2]:.1f} s: all of them about {est:.0f} s on {args.jobs} at once')
    if est > 300 and not args.yes:
        sys.exit('over five minutes: fewer values, or --yes')
    results = [first]
    with ThreadPoolExecutor(args.jobs) as ex:
        results += list(ex.map(one, combos[1:]))
    good = [r for r in results if r[1]]
    good.sort(key=lambda r: r[1][args.metric])
    print(f'{"layout":40s} {"passes":>8s} {"guest s":>8s} {"core 0":>12s} {"busiest":>12s} {"all cores":>13s} {"cores":>5s} check')
    for combo, m, took, ok in good:
        lay = ' '.join(f'{n}={v}' for n, v in zip(names, combo))
        print(f'{lay:40s} {m["passes"]:>8,} {m["seconds"]:>8.3f} {m["core0"]:>12,} {m["busiest"]:>12,} {m["all"]:>13,} {m["cores"]:>5} '
              f'{"ok" if ok else "FAILED"}')
    for combo, m, took, ok in results:
        if not m:
            print('no COUNT line (did not finish?):', ' '.join(f'{n}={v}' for n, v in zip(names, combo)))


def cmd_patterns(args):
    for p in load_patterns():
        print(f'[{p["name"]}] ({p["kind"]}) {p["why"]}\n    instead: {p["instead"]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='what', required=True)
    exe = os.path.join(ROOT, 'bin', 'rvc_cpu.exe')

    def common(p):
        p.add_argument('out')
        p.add_argument('--elf', action='append', help='an ELF of a program that ran (PATH or PATH=NMFILE; ~/... is WSL)')
        p.add_argument('--find', action='append', help='an unstripped program of this name in ~/nm/work/src')
        p.add_argument('--no-kernel', action='store_true')
        p.add_argument('--top', type=int, default=20)
        p.add_argument('--lines', action='store_true', help='the hottest source lines too (addr2line in WSL)')
        p.add_argument('--exe', default=os.environ.get('RVC_CPU', exe))

    p = sub.add_parser('run'); common(p); p.set_defaults(fn=cmd_run)
    p = sub.add_parser('report'); common(p); p.set_defaults(fn=cmd_report)
    p = sub.add_parser('deps'); common(p); p.set_defaults(fn=cmd_deps)
    p = sub.add_parser('doubles'); common(p); p.add_argument('--limit', type=float, default=5.0, help="percent of a side's instructions: over it, exit 1"); p.set_defaults(fn=cmd_doubles)
    p = sub.add_parser('auto'); common(p); p.set_defaults(fn=cmd_auto)
    p = sub.add_parser('search')
    p.add_argument('--cmd', required=True, help='the command typed at the prompt, after the switches')
    p.add_argument('--pre', default='', help='typed before the measured part (e.g. "nano-X -p & sleep 2")')
    p.add_argument('--param', action='append', required=True, help='NAME=a,b,c or NAME=lo..hi:step')
    p.add_argument('--metric', default='seconds', choices=['seconds', 'passes', 'core0', 'busiest', 'all'])
    p.add_argument('--check', help='a regular expression the console must show for a layout to count as right')
    p.add_argument('--image')
    p.add_argument('--seconds', type=float, default=120)
    p.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    p.add_argument('--yes', action='store_true')
    p.add_argument('--exe', default=os.environ.get('RVC_CPU', exe))
    p.add_argument('more', nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_search)
    p = sub.add_parser('patterns'); p.set_defaults(fn=cmd_patterns)
    # (what follows -- is rvc_cpu's, whatever it looks like)
    argv = sys.argv[1:]
    rest = argv[argv.index('--') + 1:] if '--' in argv and argv[0] in ('run', 'auto') else None
    a = ap.parse_args(argv[:argv.index('--')] if rest is not None else argv)
    if a.what in ('run', 'auto'):
        a.rest = rest or []
    for k in ('rest', 'more'):
        if getattr(a, k, None) and getattr(a, k)[0] == '--':
            setattr(a, k, getattr(a, k)[1:])
    a.fn(a)


if __name__ == '__main__':
    main()
